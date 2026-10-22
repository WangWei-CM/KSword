#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <optional>
#include <mutex>
#include <utility>

class QWidget;
namespace ads { class CDockWidget; }

namespace ks::ui
{
    // Identical normalization for real controls and their cold search values.
    inline QString StripUiSearchMnemonicMarkers(const QString& sourceText)
    {
        QString strippedText;
        strippedText.reserve(sourceText.size());
        for (int charIndex = 0; charIndex < sourceText.size(); ++charIndex)
        {
            const QChar currentChar = sourceText.at(charIndex);
            if (currentChar == QLatin1Char('&'))
            {
                if (charIndex + 1 < sourceText.size() && sourceText.at(charIndex + 1) == QLatin1Char('&'))
                {
                    strippedText.append(QLatin1Char('&'));
                    ++charIndex;
                }
                continue;
            }
            strippedText.append(currentChar);
        }
        return strippedText;
    }

    // A cold factory's actual searchable text, with the real target's stable
    // object name. These are body records, never extra navigation pages.
    struct PageSearchBodyText
    {
        QString text;
        QString targetObjectName;
        QStringList groupTitles;
        bool mnemonic = false;
    };
    // 工厂与目录共同持有的纯值记录；provider只读同源元数据/能力快照，不创建业务UI。
    // id必须在同一个Dock内稳定且唯一，parentId为空表示Dock直属入口。
    struct PageNavigationDescriptor
    {
        QString id;
        QString parentId;
        QString title;
        QStringList searchTexts;
        bool visible = true;
        bool enabled = true;
        bool current = false;
        QVector<PageSearchBodyText> coldBodyTexts;
        bool coldBodyComplete = false;
    };
    using PageNavigationCatalog = QVector<PageNavigationDescriptor>;
    using PageNavigationCatalogProvider = std::function<std::optional<PageNavigationCatalog>(ads::CDockWidget*)>;
    // 只有已证明覆盖原冷页正文的source才能跳过旧搜索准备；纯导航目录不自动具备此能力。
    using PageNavigationCatalogSearchCompleteness = std::function<bool(ads::CDockWidget*)>;
    // 仅接受激活后调用；导航器须复核能力/可见状态、按外到内选页并返回实际页面shell。
    using PageNavigationCatalogNavigator = std::function<QWidget*(ads::CDockWidget*, const QString&)>;


    using PageNavigationCatalogSource = std::function<PageNavigationCatalog(QWidget*)>;
    using PageNavigationCatalogTarget = std::function<QWidget*(QWidget*, const QString&)>;
    struct PageNavigationCatalogRegistration
    {
        PageNavigationCatalogSource source;
        PageNavigationCatalogTarget target;
        bool searchComplete = false;
    };
    // 仅登记EXE内同源工厂的无QObject捕获函数；不在静态初始化阶段求语言/能力或创建UI。
    struct PageNavigationCatalogRegistry
    {
        std::mutex mutex;
        QHash<QString, PageNavigationCatalogRegistration> factories;
    };
    inline PageNavigationCatalogRegistry& PageNavigationFactories()
    {
        static PageNavigationCatalogRegistry registry;
        return registry;
    }
    inline bool RegisterPageNavigationCatalog(const QString& dockKey,
        PageNavigationCatalogSource source, PageNavigationCatalogTarget target, bool searchComplete = false)
    {
        if (dockKey.isEmpty() || !source || !target) return false;
        auto& registry = PageNavigationFactories();
        std::lock_guard<std::mutex> lock(registry.mutex);
        if (registry.factories.contains(dockKey)) return false;
        registry.factories.insert(dockKey, {std::move(source), std::move(target), searchComplete});
        return true;
    }
    inline std::optional<PageNavigationCatalog> RegisteredPageNavigationCatalog(
        const QString& dockKey, QWidget* existing)
    {
        PageNavigationCatalogSource source;
        {
            auto& registry = PageNavigationFactories();
            std::lock_guard<std::mutex> lock(registry.mutex);
            const auto found = registry.factories.constFind(dockKey);
            if (found == registry.factories.cend()) return std::nullopt;
            source = found->source;
        }
        return source(existing); // provider在锁外只读元数据/已有状态。
    }
    inline bool RegisteredPageNavigationCatalogSearchComplete(const QString& dockKey)
    {
        auto& registry = PageNavigationFactories();
        std::lock_guard<std::mutex> lock(registry.mutex);
        const auto found = registry.factories.constFind(dockKey);
        return found != registry.factories.cend() && found->searchComplete;
    }
    inline QWidget* NavigateRegisteredPageNavigationCatalog(
        const QString& dockKey, QWidget* existing, const QString& id)
    {
        if (existing == nullptr) return nullptr;
        PageNavigationCatalogTarget target;
        {
            auto& registry = PageNavigationFactories();
            std::lock_guard<std::mutex> lock(registry.mutex);
            const auto found = registry.factories.constFind(dockKey);
            if (found == registry.factories.cend()) return nullptr;
            target = found->target;
        }
        return target(existing, id);
    }
    template<class Dock>
    bool RegisterTypedPageNavigationCatalog(const QString& dockKey, bool searchComplete = false)
    {
        return RegisterPageNavigationCatalog(dockKey,
            [](QWidget* existing) { return Dock::navigationCatalog(dynamic_cast<Dock*>(existing)); },
            [](QWidget* existing, const QString& id) -> QWidget*
            {
                auto* dock = dynamic_cast<Dock*>(existing);
                return dock != nullptr ? dock->activateNavigationDescriptor(id) : nullptr;
            }, searchComplete);
    }

    struct ResolvedPageNavigationDescriptor
    {
        PageNavigationDescriptor page;
        QStringList path;
        int depth = 0;
    };

    // 解析所有父链后输出原工厂顺序；无效schema返回nullopt，调用方保留旧真实目录回退。
    inline std::optional<QVector<ResolvedPageNavigationDescriptor>> ResolvePageNavigationCatalog(
        const PageNavigationCatalog& catalog, const QString& dockTitle)
    {
        QHash<QString, int> ids;
        for (int index = 0; index < catalog.size(); ++index)
        {
            const auto& page = catalog[index];
            if (page.id.isEmpty() || ids.contains(page.id) || page.title.isEmpty()) return std::nullopt;
            if (page.coldBodyComplete)
                for (const auto& text : page.coldBodyTexts)
                    if (text.targetObjectName.isEmpty()) return std::nullopt;
            ids.insert(page.id, index);
        }
        QVector<ResolvedPageNavigationDescriptor> result;
        result.reserve(catalog.size());
        for (const auto& page : catalog)
        {
            ResolvedPageNavigationDescriptor resolved;
            resolved.page = page;
            QStringList path;
            QSet<QString> visited;
            const PageNavigationDescriptor* ancestor = &page;
            bool visible = true;
            bool enabled = true;
            bool current = true;
            while (ancestor != nullptr)
            {
                if (visited.contains(ancestor->id)) return std::nullopt;
                visited.insert(ancestor->id);
                path.prepend(ancestor->title);
                visible = visible && ancestor->visible;
                enabled = enabled && ancestor->enabled;
                current = current && ancestor->current;
                if (ancestor->parentId.isEmpty()) break;
                const auto parent = ids.constFind(ancestor->parentId);
                if (parent == ids.cend()) return std::nullopt;
                ancestor = &catalog[*parent];
            }
            path.prepend(dockTitle);
            resolved.path = std::move(path);
            resolved.depth = static_cast<int>(resolved.path.size()) - 1;
            resolved.page.visible = visible;
            resolved.page.enabled = enabled;
            resolved.page.current = current;
            result.append(std::move(resolved));
        }
        return result;
    }
}
