#pragma once
#include "./PageNavigationCatalog.h"
#include "./ThemeBinding.h"
#include "../Internationalization/LanguageManager.h"
#include <QPointer>
#include <QTabWidget>
#include <QIcon>

namespace ks::ui
{
    // 同一份定义同时用于真实addTab、当前语言cold目录与稳定导航ID。
    struct PageNavigationTabDefinition
    {
        const char* id;
        const char* parentId;
        const char* languageKey;
        const char* sourceTitle;
        bool initiallyCurrent = false;
    };
    inline PageNavigationDescriptor PageNavigationTabDescriptor(const PageNavigationTabDefinition& definition)
    {
        const QString source = QString::fromUtf8(definition.sourceTitle);
        const QString key = QString::fromLatin1(definition.languageKey);
        return {QString::fromLatin1(definition.id), QString::fromLatin1(definition.parentId),
            key.isEmpty() ? ks::i18n::sourceText(source) : ks::i18n::contextText(key, source), {}, true, true,
            definition.initiallyCurrent};
    }

    // 只持有由明确工厂登记的Tab/page弱身份；不扫描祖先之外的widget树。
    class PageNavigationTabCatalog
    {
        struct Entry { QPointer<QTabWidget> tabs; QPointer<QWidget> page; };
        QHash<QString, Entry> m_entries;
    public:
        void add(const PageNavigationTabDefinition& definition, QTabWidget* tabs, QWidget* page, const QIcon& icon = {})
        {
            const auto descriptor = PageNavigationTabDescriptor(definition);
            tabs->addTab(page, icon, descriptor.title);
            if (definition.languageKey[0] != '\0')
                ks::i18n::LanguageManager::instance().bindTab(tabs, page,
                    QString::fromLatin1(definition.languageKey), QString::fromUtf8(definition.sourceTitle));
            m_entries.insert(descriptor.id, {tabs, page});
        }
        void remove(const QString& id) { m_entries.remove(id); }
        PageNavigationCatalog describe(PageNavigationCatalog schema) const
        {
            for (auto& descriptor : schema)
            {
                const auto entry = m_entries.value(descriptor.id);
                const int index = entry.tabs && entry.page ? entry.tabs->indexOf(entry.page) : -1;
                if (index < 0) { descriptor.visible = false; descriptor.enabled = false; descriptor.current = false; continue; }
                descriptor.title = entry.tabs->tabText(index);
                descriptor.visible = entry.tabs->isTabVisible(index);
                // 内容busy可临时disabled，但不代表实际Tab按钮失去导航权限。
                descriptor.enabled = entry.tabs->isTabEnabled(index);
                descriptor.current = entry.tabs->currentIndex() == index;
            }
            return schema;
        }
        QWidget* activate(const QString& id, const PageNavigationCatalog& schema) const
        {
            const auto resolved = ResolvePageNavigationCatalog(schema, QString());
            if (!resolved) return nullptr;
            QHash<QString, PageNavigationDescriptor> pages;
            for (const auto& descriptor : *resolved) pages.insert(descriptor.page.id, descriptor.page);
            const auto requested = pages.constFind(id);
            if (requested == pages.cend() || !requested->visible || !requested->enabled) return nullptr;
            QVector<Entry> chain;
            for (QString key = id; !key.isEmpty(); key = pages.value(key).parentId)
            {
                if (!m_entries.contains(key)) return nullptr;
                chain.prepend(m_entries.value(key));
            }
            // 在第一次setCurrent前复制整条弱链；回调可销毁原catalog宿主。
            QPointer<QWidget> target = chain.constLast().page;
            for (const auto& entry : chain)
            {
                if (!entry.tabs || !entry.page) return nullptr;
                const int index = entry.tabs->indexOf(entry.page);
                if (index < 0 || !entry.tabs->isTabVisible(index) || !entry.tabs->isTabEnabled(index)) return nullptr;
                PrepareWidgetThemeBindingsForShow(entry.page);
                if (!entry.tabs || !entry.page) return nullptr;
                const int preparedIndex = entry.tabs->indexOf(entry.page);
                if (preparedIndex < 0 || !entry.tabs->isTabVisible(preparedIndex)
                    || !entry.tabs->isTabEnabled(preparedIndex)) return nullptr;
                entry.tabs->setCurrentWidget(entry.page);
                if (!entry.tabs || !entry.page || entry.tabs->currentWidget() != entry.page) return nullptr;
                const int selectedIndex = entry.tabs->indexOf(entry.page);
                if (selectedIndex < 0 || !entry.tabs->isTabVisible(selectedIndex)
                    || !entry.tabs->isTabEnabled(selectedIndex)) return nullptr;
                PrepareWidgetThemeBindingsForShow(entry.page);
                if (!entry.tabs || !entry.page || entry.tabs->currentWidget() != entry.page) return nullptr;
            }
            // earlier currentChanged/theme回调也可改变已通过的父页状态，返回前再复核整链。
            for (const auto& entry : chain)
            {
                if (!entry.tabs || !entry.page || entry.tabs->currentWidget() != entry.page) return nullptr;
                const int index = entry.tabs->indexOf(entry.page);
                if (index < 0 || !entry.tabs->isTabVisible(index) || !entry.tabs->isTabEnabled(index)) return nullptr;
            }
            return target.data();
        }
    };
}
