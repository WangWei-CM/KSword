#include "ThemedMessageBox.h"
#include "./ToolbarMetrics.h"
#include "WindowChrome.h"
#include <QTimer>

#include "../Internationalization/LanguageManager.h"
#include "../theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGridLayout>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMargins>
#include <QPointer>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSize>
#include <QSizePolicy>
#include <QStyle>
#include <QTextEdit>
#include <QWidget>
#include <QWindow>

#include <algorithm>

namespace
{
    // kThemedMessageBoxObjectName 作用：
    // - 作为全局消息框样式表的唯一对象名锚点；
    // - 避免误伤普通 QDialog/QWidget。
    constexpr const char* kThemedMessageBoxObjectName = "KswordThemedMessageBox";

    // kThemePolishingPropertyName 作用：
    // - 标记当前消息框是否正处于主题重写过程中；
    // - 用于阻断 StyleChange/Polish 事件递归回调导致的重入崩溃。
    constexpr const char* kThemePolishingPropertyName = "ksword_theme_polishing";

    // kThemeModePropertyName 作用：
    // - 缓存上次应用到消息框的深浅色模式；
    // - 避免同一主题下重复 setStyleSheet/setPalette 触发额外样式事件。
    constexpr const char* kThemeModePropertyName = "ksword_theme_dark_mode";

    // 消息框尺寸常量：
    // - kMessageBoxMinWidth：消息框允许的最小宽度；
    // - kMessageBoxPreferredWidth：默认偏好的可读宽度；
    // - kMessageBoxHardMaxWidth：消息框宽度硬上限，避免超宽；
    // - kMessageBoxScreenMargin：与屏幕边缘的安全留白；
    // - kMessageLabelHorizontalReserve：文本区需要预留给图标和内边距的水平空间。
    constexpr int kMessageBoxMinWidth = 360;
    constexpr int kMessageBoxPreferredWidth = 520;
    constexpr int kMessageBoxHardMaxWidth = 820;
    constexpr int kMessageBoxScreenMargin = 96;
    constexpr int kMessageLabelHorizontalReserve = 148;
    // computeMessageBoxMaxWidth 作用：
    // - 按当前消息框所在屏幕计算安全最大宽度；
    // - 防止长文本导致消息框溢出屏幕可用区域。
    int computeMessageBoxMaxWidth(const QMessageBox* messageBox)
    {
        QScreen* targetScreen = messageBox != nullptr ? messageBox->screen() : nullptr;
        if (targetScreen == nullptr && qApp != nullptr)
        {
            targetScreen = qApp->primaryScreen();
        }
        if (targetScreen == nullptr)
        {
            return kMessageBoxPreferredWidth;
        }

        const int availableWidth = std::max(
            targetScreen->availableGeometry().width() - kMessageBoxScreenMargin,
            kMessageBoxMinWidth);
        const int cappedAvailableWidth = std::min(availableWidth, kMessageBoxHardMaxWidth);
        return std::max(cappedAvailableWidth, kMessageBoxMinWidth);
    }

    // polishMessageTextLabel 作用：
    // - 统一消息框文本标签的自动换行与宽度约束；
    // - 解决长文本把对话框强行撑宽的问题。
    void polishMessageTextLabel(QLabel* targetLabel, const int maxTextWidth)
    {
        if (targetLabel == nullptr)
        {
            return;
        }

        targetLabel->setWordWrap(true);
        targetLabel->setMinimumWidth(0);
        targetLabel->setMaximumWidth(std::max(maxTextWidth, 220));
        targetLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        targetLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        targetLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }

    // resolveApplicationIcon 作用：
    // - 优先从当前进程 EXE 读取 Win32 原生应用程序图标；
    // - 其次复用 QApplication 或宿主窗口已经设置的 windowIcon；
    // - 返回值：可用于标题栏和窗口图标的 QIcon，全部来源失败时返回空图标。
    QIcon resolveApplicationIcon(const QMessageBox* messageBox)
    {
        // executablePathText 用途：定位当前运行的 Ksword5.1.exe。
        const QString executablePathText = QCoreApplication::applicationFilePath();
        if (!executablePathText.trimmed().isEmpty())
        {
            // executableFileInfo 用途：交给 QFileIconProvider 从 EXE 资源中解析系统应用图标。
            const QFileInfo executableFileInfo(executablePathText);
            if (executableFileInfo.exists())
            {
                // iconProvider 用途：使用 Windows Shell 相同的图标解析路径，避免误用 MainLogo。
                QFileIconProvider iconProvider;
                const QIcon executableIcon = iconProvider.icon(executableFileInfo);
                if (!executableIcon.isNull())
                {
                    return executableIcon;
                }
            }
        }

        if (qApp != nullptr)
        {
            // applicationIcon 用途：兼容未来 main.cpp 显式设置 QApplication 默认图标的场景。
            const QIcon applicationIcon = QApplication::windowIcon();
            if (!applicationIcon.isNull())
            {
                return applicationIcon;
            }
        }

        // parentWidget 用途：向上查找业务父窗口，兼容局部窗口单独设置图标的场景。
        const QWidget* parentWidget = messageBox != nullptr ? messageBox->parentWidget() : nullptr;
        while (parentWidget != nullptr)
        {
            const QIcon parentWindowIcon = parentWidget->windowIcon();
            if (!parentWindowIcon.isNull())
            {
                return parentWindowIcon;
            }
            parentWidget = parentWidget->parentWidget();
        }

        // messageBoxIcon 用途：最后保留 QMessageBox 自身 windowIcon，避免完全无图标。
        const QIcon messageBoxIcon = messageBox != nullptr ? messageBox->windowIcon() : QIcon();
        if (!messageBoxIcon.isNull())
        {
            return messageBoxIcon;
        }

        return QIcon();
    }

    // messageBoxWindowColor 作用：返回消息框主背景色。
    QColor messageBoxWindowColor(const bool darkModeEnabled)
    {
        Q_UNUSED(darkModeEnabled);
        return KswordTheme::WindowColor();
    }

    // messageBoxSurfaceColor 作用：返回消息框内部面板色。
    QColor messageBoxSurfaceColor(const bool darkModeEnabled)
    {
        Q_UNUSED(darkModeEnabled);
        return KswordTheme::SurfaceColor();
    }

    // messageBoxBorderColor 作用：返回消息框内部控件分隔线颜色。
    QColor messageBoxBorderColor(const bool darkModeEnabled)
    {
        Q_UNUSED(darkModeEnabled);
        return KswordTheme::BorderColor();
    }

    // messageBoxTextColor 作用：返回消息框主文本色。
    QColor messageBoxTextColor(const bool darkModeEnabled)
    {
        Q_UNUSED(darkModeEnabled);
        return KswordTheme::TextPrimaryColor();
    }

    // messageBoxSecondaryTextColor 作用：返回说明文本色。
    QColor messageBoxSecondaryTextColor(const bool darkModeEnabled)
    {
        Q_UNUSED(darkModeEnabled);
        return KswordTheme::TextSecondaryColor();
    }

    // messageBoxSecondaryButtonColor 作用：返回次级按钮底色。
    QColor messageBoxSecondaryButtonColor(const bool darkModeEnabled)
    {
        Q_UNUSED(darkModeEnabled);
        return KswordTheme::SurfaceAltColor();
    }

    // messageBoxSecondaryButtonHoverColor 作用：返回次级按钮悬停底色。
    QColor messageBoxSecondaryButtonHoverColor(const bool darkModeEnabled)
    {
        Q_UNUSED(darkModeEnabled);
        return KswordTheme::SurfaceMutedColor();
    }

    // buildMessageBoxStyleSheet 作用：
    // - 生成 QMessageBox 专属样式表；
    // - 统一背景、标签、详情文本框与按钮的视觉风格。
    QString buildMessageBoxStyleSheet(const bool darkModeEnabled)
    {
        const QString windowColorText = messageBoxWindowColor(darkModeEnabled).name(QColor::HexRgb);
        const QString surfaceColorText = messageBoxSurfaceColor(darkModeEnabled).name(QColor::HexRgb);
        const QString borderColorText = messageBoxBorderColor(darkModeEnabled).name(QColor::HexRgb);
        const QString textColorText = messageBoxTextColor(darkModeEnabled).name(QColor::HexRgb);
        const QString secondaryTextColorText = messageBoxSecondaryTextColor(darkModeEnabled).name(QColor::HexRgb);
        const QString secondaryButtonColorText = messageBoxSecondaryButtonColor(darkModeEnabled).name(QColor::HexRgb);
        const QString secondaryButtonHoverColorText = messageBoxSecondaryButtonHoverColor(darkModeEnabled).name(QColor::HexRgb);

        return QStringLiteral(
            "QMessageBox#%1{"
            "  background-color:%2;"
            "  color:%3;"
            "  border:0;"
            "  border-radius:5px;"
            "}"
            "QMessageBox#%1 QWidget{"
            "  background:transparent;"
            "  color:%3;"
            "}"
            "QMessageBox#%1 QLabel#qt_msgbox_label{"
            "  font-size:14px;"
            "  font-weight:500;"
            "  min-width:0px;"
            "  padding:2px 6px 2px 6px;"
            "}"
            "QMessageBox#%1 QLabel#qt_msgbox_informativelabel{"
            "  color:%5;"
            "  font-size:13px;"
            "  min-width:0px;"
            "  padding:0 6px 4px 6px;"
            "}"
            "QMessageBox#%1 QLabel#qt_msgboxex_icon_label{"
            "  min-width:46px;"
            "  padding:6px 8px 0 2px;"
            "}"
            "QMessageBox#%1 QDialogButtonBox{"
            "  border-top:1px solid %4;"
            "  margin-top:6px;"
            "  padding-top:6px;"
            "}"
            "QMessageBox#%1 QPushButton{"
            "  background:%6;"
            "  color:%3;"
            "  border:0;"
            "  border-radius:5px;"
            "  padding:0px 12px;"
            "  min-width:76px;"
            "  min-height:28px;"
            "  font-weight:600;"
            "}"
            "QMessageBox#%1 QPushButton:hover{"
            "  background:%7;"
            "  border-color:%4;"
            "}"
            "QMessageBox#%1 QPushButton:pressed{"
            "  background:%7;"
            "  border-color:%4;"
            "}"
            "QMessageBox#%1 QPushButton[ksword_primary=\"true\"]{"
            "  background:%8;"
            "  color:%10;"
            "  border:0;"
            "  font-weight:500;"
            "}"
            "QMessageBox#%1 QPushButton[ksword_primary=\"true\"]:hover{"
            "  background:__MESSAGE_PRIMARY_HOVER__;"
            "  border-color:__MESSAGE_PRIMARY_HOVER__;"
            "  color:__MESSAGE_PRIMARY_HOVER_TEXT__;"
            "}"
            "QMessageBox#%1 QPushButton[ksword_primary=\"true\"]:pressed{"
            "  background:%9;"
            "  border-color:%9;"
            "  color:%10;"
            "}"
            "QMessageBox#%1 QTextEdit{"
            "  background:__MESSAGE_SURFACE__;"
            "  color:%3;"
            "  border:0;"
            "  border-radius:5px;"
            "  padding:8px;"
            "  selection-background-color:%8;"
            "  selection-color:%10;"
            "}")
            .arg(QString::fromLatin1(kThemedMessageBoxObjectName))
            .arg(windowColorText)
            .arg(textColorText)
            .arg(borderColorText)
            .arg(secondaryTextColorText)
            .arg(secondaryButtonColorText)
            .arg(secondaryButtonHoverColorText)
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBluePressedHex)
            .arg(KswordTheme::OnAccentHex())
            .replace(QStringLiteral("__MESSAGE_SURFACE__"), surfaceColorText)
            .replace(QStringLiteral("__MESSAGE_PRIMARY_HOVER__"), KswordTheme::PrimaryBlueSolidHoverHex())
            .replace(QStringLiteral("__MESSAGE_PRIMARY_HOVER_TEXT__"),
                KswordTheme::OnAccentHex(KswordTheme::PrimaryBlueSolidHoverColor()));
    }

    // buildMessageBoxPalette 作用：
    // - 为消息框构建稳定 palette，避免某些子控件仍回退系统白底；
    // - 与样式表配合，解决深色模式下文本和底色冲突。
    QPalette buildMessageBoxPalette(const QPalette& basePalette, const bool darkModeEnabled)
    {
        QPalette messageBoxPalette = basePalette;
        messageBoxPalette.setColor(QPalette::Window, messageBoxWindowColor(darkModeEnabled));
        messageBoxPalette.setColor(QPalette::Base, messageBoxSurfaceColor(darkModeEnabled));
        messageBoxPalette.setColor(QPalette::AlternateBase, messageBoxWindowColor(darkModeEnabled));
        messageBoxPalette.setColor(QPalette::Text, messageBoxTextColor(darkModeEnabled));
        messageBoxPalette.setColor(QPalette::WindowText, messageBoxTextColor(darkModeEnabled));
        messageBoxPalette.setColor(QPalette::Button, messageBoxSecondaryButtonColor(darkModeEnabled));
        messageBoxPalette.setColor(QPalette::ButtonText, messageBoxTextColor(darkModeEnabled));
        messageBoxPalette.setColor(QPalette::ToolTipBase, messageBoxSurfaceColor(darkModeEnabled));
        messageBoxPalette.setColor(QPalette::ToolTipText, messageBoxTextColor(darkModeEnabled));
        messageBoxPalette.setColor(QPalette::Highlight, KswordTheme::PrimaryBlueColor);
        messageBoxPalette.setColor(QPalette::HighlightedText, KswordTheme::OnAccentColor());
        return messageBoxPalette;
    }

    // standardButtonIconPath 作用：为标准按钮选择统一图标。
    QString standardButtonIconPath(const QMessageBox::StandardButton standardButton)
    {
        switch (standardButton)
        {
        case QMessageBox::Ok:
        case QMessageBox::Yes:
        case QMessageBox::Open:
        case QMessageBox::Save:
        case QMessageBox::SaveAll:
        case QMessageBox::Apply:
        case QMessageBox::YesToAll:
            return QStringLiteral(":/Icon/process_start.svg");
        case QMessageBox::Retry:
        case QMessageBox::Reset:
        case QMessageBox::RestoreDefaults:
            return QStringLiteral(":/Icon/process_refresh.svg");
        case QMessageBox::Close:
        case QMessageBox::Cancel:
        case QMessageBox::No:
        case QMessageBox::NoToAll:
        case QMessageBox::Abort:
        case QMessageBox::Discard:
            return QStringLiteral(":/Icon/log_cancel_track.svg");
        case QMessageBox::Help:
        case QMessageBox::Ignore:
            return QStringLiteral(":/Icon/process_details.svg");
        default:
            return QString();
        }
    }

    // standardButtonText 作用：让所有 QMessageBox 标准按钮使用 KSword 语言包。
    QString standardButtonText(const QMessageBox::StandardButton standardButton)
    {
        switch (standardButton)
        {
        case QMessageBox::Ok: return ks::i18n::contextText(QStringLiteral("messagebox.button.ok"), QStringLiteral("确定"));
        case QMessageBox::Save: return ks::i18n::contextText(QStringLiteral("messagebox.button.save"), QStringLiteral("保存"));
        case QMessageBox::SaveAll: return ks::i18n::contextText(QStringLiteral("messagebox.button.save_all"), QStringLiteral("全部保存"));
        case QMessageBox::Open: return ks::i18n::contextText(QStringLiteral("messagebox.button.open"), QStringLiteral("打开"));
        case QMessageBox::Yes: return ks::i18n::contextText(QStringLiteral("messagebox.button.yes"), QStringLiteral("是"));
        case QMessageBox::YesToAll: return ks::i18n::contextText(QStringLiteral("messagebox.button.yes_to_all"), QStringLiteral("全部是"));
        case QMessageBox::No: return ks::i18n::contextText(QStringLiteral("messagebox.button.no"), QStringLiteral("否"));
        case QMessageBox::NoToAll: return ks::i18n::contextText(QStringLiteral("messagebox.button.no_to_all"), QStringLiteral("全部否"));
        case QMessageBox::Abort: return ks::i18n::contextText(QStringLiteral("messagebox.button.abort"), QStringLiteral("中止"));
        case QMessageBox::Retry: return ks::i18n::contextText(QStringLiteral("messagebox.button.retry"), QStringLiteral("重试"));
        case QMessageBox::Ignore: return ks::i18n::contextText(QStringLiteral("messagebox.button.ignore"), QStringLiteral("忽略"));
        case QMessageBox::Close: return ks::i18n::contextText(QStringLiteral("messagebox.button.close"), QStringLiteral("关闭"));
        case QMessageBox::Cancel: return ks::i18n::contextText(QStringLiteral("messagebox.button.cancel"), QStringLiteral("取消"));
        case QMessageBox::Discard: return ks::i18n::contextText(QStringLiteral("messagebox.button.discard"), QStringLiteral("放弃"));
        case QMessageBox::Help: return ks::i18n::contextText(QStringLiteral("messagebox.button.help"), QStringLiteral("帮助"));
        case QMessageBox::Apply: return ks::i18n::contextText(QStringLiteral("messagebox.button.apply"), QStringLiteral("应用"));
        case QMessageBox::Reset: return ks::i18n::contextText(QStringLiteral("messagebox.button.reset"), QStringLiteral("重置"));
        case QMessageBox::RestoreDefaults: return ks::i18n::contextText(QStringLiteral("messagebox.button.restore_defaults"), QStringLiteral("恢复默认值"));
        default: return QString();
        }
    }

    // standardButtonToolTip 作用：为标准按钮补充悬停释义。
    QString standardButtonToolTip(const QMessageBox::StandardButton standardButton)
    {
        switch (standardButton)
        {
        case QMessageBox::Ok: return ks::i18n::contextText(QStringLiteral("messagebox.button.ok.tooltip"), QStringLiteral("确认当前提示并继续"));
        case QMessageBox::Yes: return ks::i18n::contextText(QStringLiteral("messagebox.button.yes.tooltip"), QStringLiteral("同意并继续执行当前操作"));
        case QMessageBox::YesToAll: return ks::i18n::contextText(QStringLiteral("messagebox.button.yes_to_all.tooltip"), QStringLiteral("对全部项目同意并继续"));
        case QMessageBox::No: return ks::i18n::contextText(QStringLiteral("messagebox.button.no.tooltip"), QStringLiteral("拒绝本次操作并返回"));
        case QMessageBox::NoToAll: return ks::i18n::contextText(QStringLiteral("messagebox.button.no_to_all.tooltip"), QStringLiteral("对全部项目拒绝并返回"));
        case QMessageBox::Cancel: return ks::i18n::contextText(QStringLiteral("messagebox.button.cancel.tooltip"), QStringLiteral("取消并关闭当前消息框"));
        case QMessageBox::Close: return ks::i18n::contextText(QStringLiteral("messagebox.button.close.tooltip"), QStringLiteral("关闭当前消息框"));
        case QMessageBox::Abort: return ks::i18n::contextText(QStringLiteral("messagebox.button.abort.tooltip"), QStringLiteral("立即中止当前流程"));
        case QMessageBox::Retry: return ks::i18n::contextText(QStringLiteral("messagebox.button.retry.tooltip"), QStringLiteral("重新尝试当前操作"));
        case QMessageBox::Ignore: return ks::i18n::contextText(QStringLiteral("messagebox.button.ignore.tooltip"), QStringLiteral("忽略本次问题并继续"));
        case QMessageBox::Open: return ks::i18n::contextText(QStringLiteral("messagebox.button.open.tooltip"), QStringLiteral("打开目标资源"));
        case QMessageBox::Save: return ks::i18n::contextText(QStringLiteral("messagebox.button.save.tooltip"), QStringLiteral("保存当前内容"));
        case QMessageBox::SaveAll: return ks::i18n::contextText(QStringLiteral("messagebox.button.save_all.tooltip"), QStringLiteral("保存全部内容"));
        case QMessageBox::Apply: return ks::i18n::contextText(QStringLiteral("messagebox.button.apply.tooltip"), QStringLiteral("应用当前改动"));
        case QMessageBox::Reset: return ks::i18n::contextText(QStringLiteral("messagebox.button.reset.tooltip"), QStringLiteral("恢复到初始状态"));
        case QMessageBox::RestoreDefaults: return ks::i18n::contextText(QStringLiteral("messagebox.button.restore_defaults.tooltip"), QStringLiteral("恢复产品默认设置"));
        case QMessageBox::Discard: return ks::i18n::contextText(QStringLiteral("messagebox.button.discard.tooltip"), QStringLiteral("丢弃当前未保存内容"));
        case QMessageBox::Help: return ks::i18n::contextText(QStringLiteral("messagebox.button.help.tooltip"), QStringLiteral("查看当前提示的帮助信息"));
        default: return ks::i18n::contextText(QStringLiteral("messagebox.button.custom.tooltip"), QStringLiteral("执行该按钮对应的消息框动作"));
        }
    }

    class GlobalMessageBoxStyler final : public QObject
    {
    public:
        // 构造函数作用：绑定 QObject 父对象，跟随 QApplication 生命周期。
        explicit GlobalMessageBoxStyler(QObject* parentObject)
            : QObject(parentObject)
        {
        }

        // eventFilter 作用：监听 QMessageBox 的显示与样式刷新时机。
        bool eventFilter(QObject* watchedObject, QEvent* eventObject) override
        {
            QMessageBox* messageBox = qobject_cast<QMessageBox*>(watchedObject);
            if (messageBox == nullptr || eventObject == nullptr)
            {
                return QObject::eventFilter(watchedObject, eventObject);
            }

            const QEvent::Type eventType = eventObject->type();
            if (eventType == QEvent::Polish ||
                eventType == QEvent::Show ||
                eventType == QEvent::LanguageChange ||
                eventType == QEvent::PaletteChange ||
                eventType == QEvent::ApplicationPaletteChange ||
                eventType == QEvent::StyleChange)
            {
                // 主题应用过程中会同步触发 StyleChange/PaletteChange；
                // 若不在这里短路，会递归再次进入 polishMessageBox。
                if (messageBox->property(kThemePolishingPropertyName).toBool())
                {
                    return QObject::eventFilter(watchedObject, eventObject);
                }
                // 显示之后再计算文本尺寸，避免 Qt 的首次布局覆盖主题的几何约束。
                // context 绑定消息框寿命；提前关闭时回调自动取消。
                if (!messageBox->property("ksword_message_theme_pending").toBool())
                {
                    messageBox->setProperty("ksword_message_theme_pending", true);
                    QTimer::singleShot(0, messageBox, [this, messageBox]()
                    {
                        messageBox->setProperty("ksword_message_theme_pending", false);
                        polishMessageBox(messageBox);
                    });
                }
            }

            return QObject::eventFilter(watchedObject, eventObject);
        }

        // polishMessageBox 作用：把统一主题应用到单个 QMessageBox。
        void polishMessageBox(QMessageBox* messageBox) const
        {
            if (messageBox == nullptr)
            {
                return;
            }

            if (messageBox->property(kThemePolishingPropertyName).toBool())
            {
                return;
            }

            const bool darkModeEnabled = KswordTheme::IsDarkModeEnabled();
            const QString targetStyleSheet = buildMessageBoxStyleSheet(darkModeEnabled);
            const bool themeAlreadyApplied =
                messageBox->objectName() == QString::fromLatin1(kThemedMessageBoxObjectName) &&
                messageBox->property(kThemeModePropertyName).toBool() == darkModeEnabled &&
                messageBox->styleSheet() == targetStyleSheet;

            // currentPolishingStateResetter 作用：
            // - 保证任意提前 return 或异常路径都能清除“正在应用主题”标记；
            // - 避免消息框永久停留在“重入保护开启”状态。
            struct PolishingStateResetter
            {
                QMessageBox* targetMessageBox = nullptr; // targetMessageBox：需要在析构时清理属性的消息框。

                ~PolishingStateResetter()
                {
                    if (targetMessageBox != nullptr)
                    {
                        targetMessageBox->setProperty(kThemePolishingPropertyName, false);
                    }
                }
            };

            messageBox->setProperty(kThemePolishingPropertyName, true);
            PolishingStateResetter currentPolishingStateResetter{ messageBox };

            const QPalette sourcePalette = (qApp != nullptr) ? qApp->palette() : messageBox->palette();
            // dialogMaxWidth 用于按屏幕可用宽度限制消息框，避免超宽导致视觉拥挤。
            const int dialogMaxWidth = computeMessageBoxMaxWidth(messageBox);
            // textMaxWidth 用于约束主文本和说明文本宽度，确保自动换行可靠生效。
            const int textMaxWidth = std::max(dialogMaxWidth - kMessageLabelHorizontalReserve, 220);
            messageBox->setObjectName(QString::fromLatin1(kThemedMessageBoxObjectName));
            messageBox->setAttribute(Qt::WA_StyledBackground, true);
            messageBox->setAutoFillBackground(true);
            messageBox->setMinimumWidth(kMessageBoxMinWidth);
            messageBox->setMaximumWidth(dialogMaxWidth);
            messageBox->setProperty(kThemeModePropertyName, darkModeEnabled);
            // 保留原生标题栏及关闭语义，圆角/阴影交由系统；不在 Show 中重建窗口句柄。
            if (messageBox->windowIcon().isNull())
            {
                messageBox->setWindowIcon(resolveApplicationIcon(messageBox));
            }

            // 相同主题重复进入时，只刷新按钮与文本可选属性，避免重复 setStyleSheet 造成样式风暴。
            if (!themeAlreadyApplied)
            {
                messageBox->setPalette(buildMessageBoxPalette(sourcePalette, darkModeEnabled));
                if (messageBox->styleSheet() != targetStyleSheet)
                {
                    messageBox->setStyleSheet(targetStyleSheet);
                }
            }

            QLabel* mainTextLabel = messageBox->findChild<QLabel*>(QStringLiteral("qt_msgbox_label"));
            polishMessageTextLabel(mainTextLabel, textMaxWidth);

            QLabel* informativeLabel = messageBox->findChild<QLabel*>(QStringLiteral("qt_msgbox_informativelabel"));
            polishMessageTextLabel(informativeLabel, textMaxWidth);

            QTextEdit* detailTextEdit = messageBox->findChild<QTextEdit*>();
            if (detailTextEdit != nullptr)
            {
                detailTextEdit->setReadOnly(true);
                detailTextEdit->setMinimumHeight(160);
            }

            polishButtons(messageBox);
            // QMessageBox::updateSize 会重新 setFixedSize，单设窗口 minimumWidth 会被覆盖。
            // 把最低可读宽度纳入真实网格的尺寸提示；零高 spacer 不增加可见空白行。
            if (auto* grid = qobject_cast<QGridLayout*>(messageBox->layout());
                grid && !grid->property("ksword_message_width_reserved").toBool())
            {
                const QMargins margins = grid->contentsMargins();
                grid->addItem(new QSpacerItem(kMessageBoxMinWidth - margins.left() - margins.right(),
                    0, QSizePolicy::Minimum, QSizePolicy::Fixed),
                    grid->rowCount(), 0, 1, qMax(1, grid->columnCount()));
                grid->setProperty("ksword_message_width_reserved", true);
            }
            messageBox->adjustSize();
            ks::ui::ApplyWindowChrome(messageBox);
        }

    private:
        // polishButtons 作用：统一处理消息框按钮图标、悬停释义与主次按钮样式。
        void polishButtons(QMessageBox* messageBox) const
        {
            const QList<QAbstractButton*> buttonList = messageBox->buttons();
            for (QAbstractButton* abstractButton : buttonList)
            {
                QPushButton* pushButton = qobject_cast<QPushButton*>(abstractButton);
                if (pushButton == nullptr)
                {
                    continue;
                }

                const QMessageBox::StandardButton standardButton = messageBox->standardButton(pushButton);
                const QMessageBox::ButtonRole buttonRole = messageBox->buttonRole(pushButton);
                const bool primaryButton =
                    buttonRole == QMessageBox::AcceptRole ||
                    buttonRole == QMessageBox::YesRole ||
                    buttonRole == QMessageBox::ApplyRole;

                pushButton->setProperty("ksword_primary", primaryButton);
                pushButton->setCursor(Qt::PointingHandCursor);
                ks::ui::NormalizeToolbarControl(pushButton);
                pushButton->setMinimumWidth(primaryButton ? 86 : 76);
                pushButton->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
                const QString localizedButtonText = standardButtonText(standardButton);
                if (!localizedButtonText.isEmpty())
                {
                    pushButton->setText(localizedButtonText);
                }
                if (standardButton != QMessageBox::NoButton)
                {
                    pushButton->setToolTip(standardButtonToolTip(standardButton));
                }
                else if (pushButton->toolTip().isEmpty())
                {
                    pushButton->setToolTip(pushButton->text());
                }

                const QString iconPath = standardButtonIconPath(standardButton);
                if (!iconPath.isEmpty())
                {
                    pushButton->setIcon(QIcon(iconPath));
                    pushButton->setIconSize(QSize(16, 16));
                }

                QStyle* widgetStyle = pushButton->style();
                if (widgetStyle != nullptr)
                {
                    widgetStyle->unpolish(pushButton);
                    widgetStyle->polish(pushButton);
                }
            }
        }
    };

    // globalMessageBoxStylerInstance 作用：返回全局唯一主题器实例。
    GlobalMessageBoxStyler* globalMessageBoxStylerInstance()
    {
        static QPointer<GlobalMessageBoxStyler> stylerInstance;
        if (stylerInstance == nullptr && qApp != nullptr)
        {
            stylerInstance = new GlobalMessageBoxStyler(qApp);
        }
        return stylerInstance.data();
    }
}

namespace ks::ui
{
    void InstallGlobalMessageBoxTheme(QApplication* appInstance)
    {
        if (appInstance == nullptr)
        {
            return;
        }

        GlobalMessageBoxStyler* stylerInstance = globalMessageBoxStylerInstance();
        if (stylerInstance == nullptr)
        {
            return;
        }

        appInstance->installEventFilter(stylerInstance);
        RefreshGlobalMessageBoxTheme();
    }

    void RefreshGlobalMessageBoxTheme()
    {
        GlobalMessageBoxStyler* stylerInstance = globalMessageBoxStylerInstance();
        if (stylerInstance == nullptr || qApp == nullptr)
        {
            return;
        }

        const QWidgetList topLevelWidgetList = qApp->topLevelWidgets();
        for (QWidget* topLevelWidget : topLevelWidgetList)
        {
            QMessageBox* messageBox = qobject_cast<QMessageBox*>(topLevelWidget);
            if (messageBox == nullptr)
            {
                continue;
            }

            stylerInstance->polishMessageBox(messageBox);
        }
    }
}
