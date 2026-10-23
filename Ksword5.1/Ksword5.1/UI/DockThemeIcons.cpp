#include "../Framework.h"
#include "DockThemeIcons.h"

#include "../include/ads/DockAreaTitleBar.h"
#include "../include/ads/DockAreaWidget.h"
#include "../include/ads/DockContainerWidget.h"
#include "../include/ads/DockManager.h"
#include "../include/ads/DockWidgetTab.h"
#include "../include/ads/DockWidget.h"
#include "../include/ads/AutoHideDockContainer.h"
#include "../include/ads/IconProvider.h"
#include "../theme.h"

#include <QAbstractButton>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QPointer>
#include <QSet>
#include <array>

namespace ks::ui
{
    namespace
    {
        // IconDescriptor 保存上游按钮角色与其标准轮廓，避免依赖第三方私有实现。
        struct IconDescriptor
        {
            ads::eIcon iconId;                  // ADS 图标提供器中的角色编号。
            QStyle::StandardPixmap shape;       // Qt 提供的原始图标轮廓。
            ads::TitleBarButton titleButton;    // 标题栏公开按钮编号。
        };

        // 六种角色与 ADS 5.1.1 的 setButtonIcon 调用一致；标签关闭单独按名称处理。
        constexpr std::array<IconDescriptor, 6> kIconDescriptors{{
            { ads::TabCloseIcon, QStyle::SP_TitleBarCloseButton, ads::TitleBarButtonClose },
            { ads::AutoHideIcon, QStyle::SP_DialogOkButton, ads::TitleBarButtonAutoHide },
            { ads::DockAreaMenuIcon, QStyle::SP_TitleBarUnshadeButton, ads::TitleBarButtonTabsMenu },
            { ads::DockAreaUndockIcon, QStyle::SP_TitleBarNormalButton, ads::TitleBarButtonUndock },
            { ads::DockAreaCloseIcon, QStyle::SP_TitleBarCloseButton, ads::TitleBarButtonClose },
            { ads::DockAreaMinimizeIcon, QStyle::SP_TitleBarMinButton, ads::TitleBarButtonMinimize }
        }};

        // tintPixmap 作用：保留标准图标透明度和轮廓，仅替换可见像素的主题前景色。
        // source 为标准图标位图；foreground 为目标颜色；返回相同尺寸和 DPR 的图标。
        QPixmap tintPixmap(const QPixmap& source, const QColor& foreground)
        {
            // tinted 为独立副本，禁止修改 Qt 样式缓存中的原始标准图标。
            QPixmap tinted = source.copy();
            if (tinted.isNull())
            {
                return tinted;
            }

            // painter 采用 SourceIn，仅填充原有非透明像素，保留抗锯齿 alpha。
            QPainter painter(&tinted);
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(tinted.rect(), foreground);
            return tinted;
        }

        // makeThemeIcon 作用：由 Qt 原始轮廓生成正常、悬停、选中和禁用主题图标。
        // manager 为获取平台样式的上下文；shape 为轮廓编号；返回可直接设置的 QIcon。
        QIcon makeThemeIcon(ads::CDockManager* manager, const QStyle::StandardPixmap shape)
        {
            // standard 为平台原图；图形从主题色派生，同时对三种背景偏移底面校准。
            const QIcon standard = manager->style()->standardIcon(shape, nullptr, manager);
            const QColor normal = KswordTheme::DockTabGlyphColor(KswordTheme::DockTabState::Inactive);
            const QColor active = KswordTheme::DockTabGlyphColor(KswordTheme::DockTabState::Hover);
            const QColor selected = KswordTheme::DockTabGlyphColor(KswordTheme::DockTabState::Active);
            const QColor disabled = KswordTheme::DockTabGlyphColor(KswordTheme::DockTabState::Inactive, true);
            QIcon themed; // themed 汇集常用尺寸与所有状态，避免 Qt 用固定双色图标兜底。

            // extent 为逻辑图标尺寸；分别保留 1x/2x/3x 位图以覆盖高 DPI 显示器。
            for (const int extent : { 16, 24, 32 })
            {
                for (const int density : { 1, 2, 3 })
                {
                    // source 是平台在对应物理尺寸下生成的轮廓，DPR 让布局保持逻辑尺寸。
                    const QSize physicalSize(extent * density, extent * density); // 对应 DPI 的物理画布。
                    QPixmap source = standard.pixmap(physicalSize);
                    if (!source.isNull() && source.size() != physicalSize)
                    {
                        // 原生样式可能只给固定 16px 位图，先等比放大再设置 DPR，避免逻辑轮廓缩小。
                        source = source.scaled(physicalSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    }
                    source.setDevicePixelRatio(density);
                    // On/Off 颜色相同，保留两种状态但共用同一组着色结果。
                    const std::array<QPixmap, 4> tinted{{tintPixmap(source, normal),
                        tintPixmap(source, active), tintPixmap(source, selected), tintPixmap(source, disabled)}};
                    constexpr std::array<QIcon::Mode, 4> modes{{QIcon::Normal, QIcon::Active,
                        QIcon::Selected, QIcon::Disabled}};
                    for (const QIcon::State state : { QIcon::Off, QIcon::On })
                    {
                        for (std::size_t index = 0; index < modes.size(); ++index)
                        {
                            themed.addPixmap(tinted[index], modes[index], state);
                        }
                    }
                }
            }
            return themed;
        }

        class DockIconCache final : public QObject
        {
        public:
            explicit DockIconCache(ads::CDockManager* manager) : QObject(manager) {}
            QPointer<QStyle> style;
            qint64 paletteKey = 0;
            QString iconTheme;
            std::array<QColor, 4> colors;
            std::array<QIcon, 6> icons;
            bool ready = false;
        };

        DockIconCache* iconCache(ads::CDockManager* manager)
        {
            // 缓存由 manager 直接拥有；只检查该明确宿主的直接 QObject 子对象。
            for (QObject* child : manager->children())
            {
                if (auto* cache = dynamic_cast<DockIconCache*>(child))
                {
                    return cache;
                }
            }
            return new DockIconCache(manager);
        }

        void setManagedIcon(QAbstractButton* button, const QIcon& icon)
        {
            if (button == nullptr)
            {
                return;
            }
            if (!button->property("ksword_theme_icon_managed").toBool())
            {
                button->setProperty("ksword_theme_icon_managed", true);
            }
            if (button->icon().cacheKey() != icon.cacheKey())
            {
                button->setIcon(icon);
            }
        }

        void refreshArea(ads::CDockAreaWidget* area, const std::array<QIcon, 6>& icons,
            QSet<ads::CDockAreaTitleBar*>& visited)
        {
            ads::CDockAreaTitleBar* titleBar = area != nullptr ? area->titleBar() : nullptr;
            if (titleBar == nullptr || visited.contains(titleBar))
            {
                return;
            }
            visited.insert(titleBar);
            for (std::size_t index = 1; index < kIconDescriptors.size(); ++index)
            {
                setManagedIcon(titleBar->button(kIconDescriptors[index].titleButton), icons[index]);
            }
        }
    }

    // 同一个生产前景补偿入口供自有标签工厂与离屏回归使用。
    void ApplyDockTabTextColor(QWidget* tabWidget, const bool activeTab, const bool hovered)
    {
        if (tabWidget == nullptr)
        {
            return;
        }

        const auto state = activeTab ? KswordTheme::DockTabState::Active
            : (hovered ? KswordTheme::DockTabState::Hover : KswordTheme::DockTabState::Inactive);
        const QColor finalTextColor = KswordTheme::DockTabTextColor(state);
        const QString finalTextColorName = finalTextColor.name(QColor::HexRgb).toUpper();

        if (tabWidget->property("kswordDockTabTextColor").toString() != finalTextColorName)
        {
            QPalette tabPalette = tabWidget->palette();
            tabPalette.setColor(QPalette::WindowText, finalTextColor);
            tabPalette.setColor(QPalette::Text, finalTextColor);
            tabPalette.setColor(QPalette::ButtonText, finalTextColor);
            tabWidget->setPalette(tabPalette);
            tabWidget->setProperty("kswordDockTabTextColor", finalTextColorName);
        }

        const QString labelStyle = QStringLiteral(
            "color:%1 !important;"
            "background-color:transparent !important;"
            "background:transparent !important;"
            "font-weight:%2;")
            .arg(finalTextColorName)
            .arg(activeTab ? QStringLiteral("700") : QStringLiteral("600"));

        const QList<QLabel*> labelChildren = tabWidget->findChildren<QLabel*>();
        for (QLabel* labelWidget : labelChildren)
        {
            if (labelWidget == nullptr)
            {
                continue;
            }

            QPalette labelPalette = labelWidget->palette();
            labelPalette.setColor(QPalette::WindowText, finalTextColor);
            labelPalette.setColor(QPalette::Text, finalTextColor);
            labelPalette.setColor(QPalette::ButtonText, finalTextColor);
            labelWidget->setPalette(labelPalette);

            if (labelWidget->styleSheet() != labelStyle)
            {
                labelWidget->setStyleSheet(labelStyle);
            }
        }
    }

    void RefreshDockThemeIcons(ads::CDockManager* dockManager)
    {
        if (dockManager == nullptr)
        {
            return;
        }

        DockIconCache* cache = iconCache(dockManager);
        const std::array<QColor, 4> colors{{
            KswordTheme::DockTabGlyphColor(KswordTheme::DockTabState::Inactive),
            KswordTheme::DockTabGlyphColor(KswordTheme::DockTabState::Hover),
            KswordTheme::DockTabGlyphColor(KswordTheme::DockTabState::Active),
            KswordTheme::DockTabGlyphColor(KswordTheme::DockTabState::Inactive, true)}};
        const qint64 paletteKey = dockManager->palette().cacheKey();
        const QString iconTheme = QIcon::themeName() + QLatin1Char('|') + QIcon::fallbackThemeName();
        if (!cache->ready || cache->style != dockManager->style() || cache->paletteKey != paletteKey
            || cache->iconTheme != iconTheme || cache->colors != colors)
        {
            for (std::size_t index = 0; index < kIconDescriptors.size(); ++index)
            {
                cache->icons[index] = makeThemeIcon(dockManager, kIconDescriptors[index].shape);
            }
            cache->style = dockManager->style();
            cache->paletteKey = paletteKey;
            cache->iconTheme = iconTheme;
            cache->colors = colors;
            cache->ready = true;
        }
        for (std::size_t index = 0; index < kIconDescriptors.size(); ++index)
        {
            if (ads::CDockManager::iconProvider().customIcon(kIconDescriptors[index].iconId).cacheKey()
                != cache->icons[index].cacheKey())
            {
                ads::CDockManager::iconProvider().registerCustomIcon(
                    kIconDescriptors[index].iconId, cache->icons[index]);
            }
        }

        // 只走 ADS 公开拓扑；隐藏业务内容不参与标题栏和关闭按钮发现。
        QSet<ads::CDockAreaTitleBar*> visited;
        for (ads::CDockContainerWidget* container : dockManager->dockContainers())
        {
            if (container == nullptr)
            {
                continue;
            }
            for (int index = 0; index < container->dockAreaCount(); ++index)
            {
                refreshArea(container->dockArea(index), cache->icons, visited);
            }
            for (ads::CAutoHideDockContainer* autoHide : container->autoHideWidgets())
            {
                if (autoHide != nullptr)
                {
                    refreshArea(autoHide->dockAreaWidget(), cache->icons, visited);
                }
            }
        }
        const auto docks = dockManager->dockWidgetsMap();
        for (ads::CDockWidget* dock : docks)
        {
            ads::CDockWidgetTab* tab = dock != nullptr ? dock->tabWidget() : nullptr;
            if (tab != nullptr)
            {
                setManagedIcon(tab->findChild<QAbstractButton*>(QStringLiteral("tabCloseButton"),
                    Qt::FindDirectChildrenOnly), cache->icons[0]);
            }
        }
    }
}
