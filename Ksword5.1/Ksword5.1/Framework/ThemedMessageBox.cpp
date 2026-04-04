#include "ThemedMessageBox.h"

#include "../theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDialogButtonBox>
#include <QEvent>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSize>
#include <QStyle>
#include <QTextEdit>
#include <QWidget>

namespace
{
    // kThemedMessageBoxObjectName 作用：
    // - 作为全局消息框样式表的唯一对象名锚点；
    // - 避免误伤普通 QDialog/QWidget。
    constexpr const char* kThemedMessageBoxObjectName = "KswordThemedMessageBox";

    // messageBoxWindowColor 作用：返回消息框主背景色。
    QColor messageBoxWindowColor(const bool darkModeEnabled)
    {
        return darkModeEnabled ? QColor(16, 21, 28) : QColor(248, 251, 255);
    }

    // messageBoxSurfaceColor 作用：返回消息框内部面板色。
    QColor messageBoxSurfaceColor(const bool darkModeEnabled)
    {
        return darkModeEnabled ? QColor(23, 29, 38) : QColor(255, 255, 255);
    }

    // messageBoxBorderColor 作用：返回消息框边框色。
    QColor messageBoxBorderColor(const bool darkModeEnabled)
    {
        return darkModeEnabled ? QColor(45, 62, 82) : QColor(198, 215, 234);
    }

    // messageBoxTextColor 作用：返回消息框主文本色。
    QColor messageBoxTextColor(const bool darkModeEnabled)
    {
        return darkModeEnabled ? QColor(237, 244, 255) : QColor(20, 33, 51);
    }

    // messageBoxSecondaryTextColor 作用：返回说明文本色。
    QColor messageBoxSecondaryTextColor(const bool darkModeEnabled)
    {
        return darkModeEnabled ? QColor(183, 197, 216) : QColor(81, 97, 116);
    }

    // messageBoxSecondaryButtonColor 作用：返回次级按钮底色。
    QColor messageBoxSecondaryButtonColor(const bool darkModeEnabled)
    {
        return darkModeEnabled ? QColor(21, 27, 36) : QColor(255, 255, 255);
    }

    // messageBoxSecondaryButtonHoverColor 作用：返回次级按钮悬停底色。
    QColor messageBoxSecondaryButtonHoverColor(const bool darkModeEnabled)
    {
        return darkModeEnabled ? QColor(27, 37, 49) : QColor(234, 244, 255);
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
            "  border:1px solid %4;"
            "  border-radius:10px;"
            "}"
            "QMessageBox#%1 QWidget{"
            "  background:transparent;"
            "  color:%3;"
            "}"
            "QMessageBox#%1 QLabel#qt_msgbox_label{"
            "  font-size:15px;"
            "  font-weight:700;"
            "  min-width:340px;"
            "  padding:4px 6px 2px 6px;"
            "}"
            "QMessageBox#%1 QLabel#qt_msgbox_informativelabel{"
            "  color:%5;"
            "  font-size:13px;"
            "  padding:0 6px 6px 6px;"
            "}"
            "QMessageBox#%1 QLabel#qt_msgboxex_icon_label{"
            "  min-width:46px;"
            "  padding:6px 8px 0 2px;"
            "}"
            "QMessageBox#%1 QDialogButtonBox{"
            "  border-top:1px solid %4;"
            "  margin-top:10px;"
            "  padding-top:10px;"
            "}"
            "QMessageBox#%1 QPushButton{"
            "  background:%6;"
            "  color:%3;"
            "  border:1px solid %4;"
            "  border-radius:6px;"
            "  padding:6px 14px;"
            "  min-width:96px;"
            "  min-height:32px;"
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
            "  color:#FFFFFF;"
            "  border:1px solid %8;"
            "  font-weight:700;"
            "}"
            "QMessageBox#%1 QPushButton[ksword_primary=\"true\"]:hover{"
            "  background:#2E8BFF;"
            "  border-color:#2E8BFF;"
            "}"
            "QMessageBox#%1 QPushButton[ksword_primary=\"true\"]:pressed{"
            "  background:%9;"
            "  border-color:%9;"
            "}"
            "QMessageBox#%1 QTextEdit{"
            "  background:%10;"
            "  color:%3;"
            "  border:1px solid %4;"
            "  border-radius:6px;"
            "  padding:8px;"
            "  selection-background-color:%8;"
            "  selection-color:#FFFFFF;"
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
            .arg(surfaceColorText);
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
        messageBoxPalette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
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
        case QMessageBox::Apply:
            return QStringLiteral(":/Icon/process_start.svg");
        case QMessageBox::Retry:
        case QMessageBox::Reset:
            return QStringLiteral(":/Icon/process_refresh.svg");
        case QMessageBox::Close:
        case QMessageBox::Cancel:
        case QMessageBox::No:
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

    // standardButtonToolTip 作用：为标准按钮补充悬停释义。
    QString standardButtonToolTip(const QMessageBox::StandardButton standardButton)
    {
        switch (standardButton)
        {
        case QMessageBox::Ok: return QStringLiteral("确认当前提示并继续");
        case QMessageBox::Yes: return QStringLiteral("同意并继续执行当前操作");
        case QMessageBox::No: return QStringLiteral("拒绝本次操作并返回");
        case QMessageBox::Cancel: return QStringLiteral("取消并关闭当前消息框");
        case QMessageBox::Close: return QStringLiteral("关闭当前消息框");
        case QMessageBox::Abort: return QStringLiteral("立即中止当前流程");
        case QMessageBox::Retry: return QStringLiteral("重新尝试当前操作");
        case QMessageBox::Ignore: return QStringLiteral("忽略本次问题并继续");
        case QMessageBox::Open: return QStringLiteral("打开目标资源");
        case QMessageBox::Save: return QStringLiteral("保存当前内容");
        case QMessageBox::Apply: return QStringLiteral("应用当前改动");
        case QMessageBox::Reset: return QStringLiteral("恢复到初始状态");
        case QMessageBox::Discard: return QStringLiteral("丢弃当前未保存内容");
        case QMessageBox::Help: return QStringLiteral("查看当前提示的帮助信息");
        default: return QStringLiteral("执行该按钮对应的消息框动作");
        }
    }

    // GlobalMessageBoxStyler 作用：
    // - 作为 QApplication 全局事件过滤器，拦截所有 QMessageBox；
    // - 在消息框显示/主题变化时统一执行样式重写。
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
                eventType == QEvent::PaletteChange ||
                eventType == QEvent::ApplicationPaletteChange ||
                eventType == QEvent::StyleChange)
            {
                polishMessageBox(messageBox);
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

            const bool darkModeEnabled = KswordTheme::IsDarkModeEnabled();
            const QPalette sourcePalette = (qApp != nullptr) ? qApp->palette() : messageBox->palette();
            messageBox->setObjectName(QString::fromLatin1(kThemedMessageBoxObjectName));
            messageBox->setAttribute(Qt::WA_StyledBackground, true);
            messageBox->setAutoFillBackground(true);
            messageBox->setPalette(buildMessageBoxPalette(sourcePalette, darkModeEnabled));
            messageBox->setMinimumWidth(500);
            messageBox->setStyleSheet(buildMessageBoxStyleSheet(darkModeEnabled));

            QLabel* mainTextLabel = messageBox->findChild<QLabel*>(QStringLiteral("qt_msgbox_label"));
            if (mainTextLabel != nullptr)
            {
                mainTextLabel->setWordWrap(true);
                mainTextLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            }

            QLabel* informativeLabel = messageBox->findChild<QLabel*>(QStringLiteral("qt_msgbox_informativelabel"));
            if (informativeLabel != nullptr)
            {
                informativeLabel->setWordWrap(true);
                informativeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            }

            QTextEdit* detailTextEdit = messageBox->findChild<QTextEdit*>();
            if (detailTextEdit != nullptr)
            {
                detailTextEdit->setReadOnly(true);
                detailTextEdit->setMinimumHeight(160);
            }

            polishButtons(messageBox);
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
                pushButton->setMinimumHeight(32);
                pushButton->setMinimumWidth(primaryButton ? 108 : 96);
                pushButton->setToolTip(standardButtonToolTip(standardButton));

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
