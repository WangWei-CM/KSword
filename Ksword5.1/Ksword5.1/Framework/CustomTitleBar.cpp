#include "CustomTitleBar.h"

#include "../theme.h"

#include <QApplication>
#include <QDate>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QStringList>
#include <QWidget>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <iterator>

namespace
{
    // 标题栏尺寸常量：
    // - kTitleBarHeight：标题栏固定高度；
    // - kControlButtonWidth：右上角控制按钮固定宽度；
    // - kControlButtonHeight：右上角控制按钮固定高度；
    // - kControlIconSize：右上角控制按钮图标尺寸；
    // - kUserBadgeMinWidth：用户名徽标的最小宽度；
    // - kCommandLineMinWidth：命令输入框最小宽度；
    // - kCommandLineMaxWidth：命令输入框最大宽度；
    // - kAppIconSize：左侧应用图标绘制尺寸。
    constexpr int kTitleBarHeight = 34;
    constexpr int kControlButtonWidth = 26;
    constexpr int kControlButtonHeight = 20;
    constexpr int kControlIconSize = 12;
    constexpr int kUserBadgeMinWidth = 82;
    constexpr int kCommandLineMinWidth = 210;
    constexpr int kCommandLineMaxWidth = 760;
    constexpr int kAppIconSize = 16;

    // widgetBelongsTo：
    // - 作用：判断某个命中控件是否属于指定祖先控件分支；
    // - 调用：isPointInDraggableRegion 内部用于区分可拖拽区和交互控件区；
    // - 传入 widgetObject：命中控件；
    // - 传入 expectedAncestor：期望祖先控件；
    // - 传出：true=属于该祖先分支。
    bool widgetBelongsTo(const QWidget* widgetObject, const QWidget* expectedAncestor)
    {
        if (widgetObject == nullptr || expectedAncestor == nullptr)
        {
            return false;
        }

        const QWidget* cursorWidget = widgetObject;
        while (cursorWidget != nullptr)
        {
            if (cursorWidget == expectedAncestor)
            {
                return true;
            }
            cursorWidget = cursorWidget->parentWidget();
        }
        return false;
    }
}

namespace ks::ui
{
    CustomTitleBar::CustomTitleBar(QWidget* parentWidget)
        : QWidget(parentWidget)
    {
        initializeUi();
        initializeConnections();
        updateVisualState();
    }

    void CustomTitleBar::setPinnedState(const bool pinnedState)
    {
        m_isPinned = pinnedState;
        updateVisualState();
    }

    void CustomTitleBar::setMaximizedState(const bool maximizedState)
    {
        m_isMaximized = maximizedState;
        updateVisualState();
    }

    void CustomTitleBar::setDarkModeEnabled(const bool darkModeEnabled)
    {
        m_darkModeEnabled = darkModeEnabled;
        updateVisualState();
    }

    bool CustomTitleBar::isPointInDraggableRegion(const QPoint& localPos) const
    {
        if (!rect().contains(localPos))
        {
            return false;
        }

        // hitWidget 用于识别当前命中的子控件，避免交互控件也被当成拖动区。
        QWidget* hitWidget = childAt(localPos);
        if (hitWidget == nullptr)
        {
            return true;
        }

        if (widgetBelongsTo(hitWidget, m_commandLineEdit))
        {
            return false;
        }
        if (widgetBelongsTo(hitWidget, m_rightWidget))
        {
            return false;
        }
        if (widgetBelongsTo(hitWidget, m_userBadgeButton))
        {
            return false;
        }

        return true;
    }

    int CustomTitleBar::titleBarHeight() const
    {
        return kTitleBarHeight;
    }

    void CustomTitleBar::resizeEvent(QResizeEvent* resizeEventPointer)
    {
        QWidget::resizeEvent(resizeEventPointer);
        updateCommandLineWidth();
    }

    void CustomTitleBar::mouseDoubleClickEvent(QMouseEvent* mouseEventPointer)
    {
        if (mouseEventPointer != nullptr
            && mouseEventPointer->button() == Qt::LeftButton
            && isPointInDraggableRegion(mouseEventPointer->pos()))
        {
            emit requestToggleMaximizeWindow();
            mouseEventPointer->accept();
            return;
        }

        QWidget::mouseDoubleClickEvent(mouseEventPointer);
    }

    void CustomTitleBar::initializeUi()
    {
        setObjectName(QStringLiteral("ksCustomTitleBar"));
        setFixedHeight(kTitleBarHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAttribute(Qt::WA_StyledBackground, true);

        m_rootLayout = new QGridLayout(this);
        m_rootLayout->setContentsMargins(6, 2, 6, 2);
        m_rootLayout->setHorizontalSpacing(6);
        m_rootLayout->setVerticalSpacing(0);
        m_rootLayout->setColumnStretch(0, 1);
        m_rootLayout->setColumnStretch(1, 1);
        m_rootLayout->setColumnStretch(2, 1);

        // 左侧信息区：程序图标 + 固定标题文本 + 用户名徽标。
        m_leftWidget = new QWidget(this);
        m_leftLayout = new QHBoxLayout(m_leftWidget);
        m_leftLayout->setContentsMargins(0, 0, 0, 0);
        m_leftLayout->setSpacing(4);

        m_appIconLabel = new QLabel(m_leftWidget);
        m_appIconLabel->setFixedSize(kAppIconSize, kAppIconSize);
        m_appIconLabel->setAlignment(Qt::AlignCenter);

        m_titleTextLabel = new QLabel(m_leftWidget);
        m_titleTextLabel->setText(QStringLiteral("Ksword5.1(%1)").arg(resolveCompileDateText()));
        m_titleTextLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

        m_rawUserNameText = resolveCurrentUserNameText();
        m_isSpecialUser = m_rawUserNameText.compare(QStringLiteral("33251"), Qt::CaseSensitive) == 0;
        m_userBadgeButton = new QPushButton(m_leftWidget);
        m_userBadgeButton->setEnabled(false);
        m_userBadgeButton->setFocusPolicy(Qt::NoFocus);
        m_userBadgeButton->setMinimumWidth(kUserBadgeMinWidth);
        m_userBadgeButton->setFixedHeight(18);
        m_userBadgeButton->setCursor(Qt::ArrowCursor);

        m_leftLayout->addWidget(m_appIconLabel, 0);
        m_leftLayout->addWidget(m_titleTextLabel, 0);
        m_leftLayout->addWidget(m_userBadgeButton, 0);

        // 中间命令输入框：输入后回车即可触发“新控制台执行命令”。
        m_commandLineEdit = new QLineEdit(this);
        m_commandLineEdit->setPlaceholderText(QStringLiteral("输入命令后回车：将使用 cmd /K 在新控制台执行"));
        m_commandLineEdit->setClearButtonEnabled(true);
        m_commandLineEdit->setFixedHeight(22);

        // 右侧控制区：图钉 + 最小化 + 最大化/还原 + 关闭。
        m_rightWidget = new QWidget(this);
        m_rightLayout = new QHBoxLayout(m_rightWidget);
        m_rightLayout->setContentsMargins(0, 0, 0, 0);
        m_rightLayout->setSpacing(1);

        m_pinButton = new QPushButton(m_rightWidget);
        m_minButton = new QPushButton(m_rightWidget);
        m_maxButton = new QPushButton(m_rightWidget);
        m_closeButton = new QPushButton(m_rightWidget);

        m_pinButton->setObjectName(QStringLiteral("ksTitlePinButton"));
        m_minButton->setObjectName(QStringLiteral("ksTitleMinButton"));
        m_maxButton->setObjectName(QStringLiteral("ksTitleMaxButton"));
        m_closeButton->setObjectName(QStringLiteral("ksTitleCloseButton"));

        const std::array<QPushButton*, 4> controlButtons = {
            m_pinButton,
            m_minButton,
            m_maxButton,
            m_closeButton
        };
        for (QPushButton* buttonObject : controlButtons)
        {
            if (buttonObject == nullptr)
            {
                continue;
            }

            buttonObject->setFixedSize(kControlButtonWidth, kControlButtonHeight);
            buttonObject->setCursor(Qt::PointingHandCursor);
            buttonObject->setFocusPolicy(Qt::NoFocus);
            m_rightLayout->addWidget(buttonObject, 0);
        }

        m_pinButton->setToolTip(QStringLiteral("切换窗口置顶状态"));
        m_minButton->setToolTip(QStringLiteral("最小化主窗口"));
        m_maxButton->setToolTip(QStringLiteral("最大化或还原主窗口"));
        m_closeButton->setToolTip(QStringLiteral("关闭主窗口"));

        m_rootLayout->addWidget(m_leftWidget, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);
        m_rootLayout->addWidget(m_commandLineEdit, 0, 1, Qt::AlignCenter);
        m_rootLayout->addWidget(m_rightWidget, 0, 2, Qt::AlignRight | Qt::AlignVCenter);

        updateCommandLineWidth();
    }

    void CustomTitleBar::initializeConnections()
    {
        connect(m_pinButton, &QPushButton::clicked, this, [this]() {
            emit requestTogglePinned();
        });
        connect(m_minButton, &QPushButton::clicked, this, [this]() {
            emit requestMinimizeWindow();
        });
        connect(m_maxButton, &QPushButton::clicked, this, [this]() {
            emit requestToggleMaximizeWindow();
        });
        connect(m_closeButton, &QPushButton::clicked, this, [this]() {
            emit requestCloseWindow();
        });
        connect(m_commandLineEdit, &QLineEdit::returnPressed, this, [this]() {
            const QString commandText = m_commandLineEdit->text().trimmed();
            if (commandText.isEmpty())
            {
                return;
            }
            emit commandSubmitted(commandText);
        });
    }

    void CustomTitleBar::updateVisualState()
    {
        const QString titleBarBackgroundText = m_darkModeEnabled
            ? QStringLiteral("#0B0F14")
            : QStringLiteral("#F8FBFF");
        const QString titleBarBorderText = m_darkModeEnabled
            ? QStringLiteral("#24364D")
            : QStringLiteral("#BFD4EB");
        const QString titleTextColorText = m_darkModeEnabled
            ? QStringLiteral("#E7F2FF")
            : QStringLiteral("#143B68");
        const QString commandBackgroundText = m_darkModeEnabled
            ? QStringLiteral("#111924")
            : QStringLiteral("#FFFFFF");
        const QString commandTextColorText = m_darkModeEnabled
            ? QStringLiteral("#EAF3FF")
            : QStringLiteral("#133A67");
        const QString commandBorderText = m_darkModeEnabled
            ? QStringLiteral("#355375")
            : QStringLiteral("#9FC3E8");

        const QString normalBadgeBackgroundText = m_darkModeEnabled
            ? QStringLiteral("#10161F")
            : QStringLiteral("#FFFFFF");
        const QString normalBadgeTextColorText = KswordTheme::PrimaryBlueHex;
        const QString specialBadgeBackgroundText = KswordTheme::PrimaryBlueHex;
        const QString specialBadgeTextColorText = m_darkModeEnabled
            ? QStringLiteral("#FFFFFF")
            : QStringLiteral("#0D1624");

        const QString userBadgeBackgroundText = m_isSpecialUser
            ? specialBadgeBackgroundText
            : normalBadgeBackgroundText;
        const QString userBadgeTextColorText = m_isSpecialUser
            ? specialBadgeTextColorText
            : normalBadgeTextColorText;

        const QString titleBarStyleSheetText = QStringLiteral(
            "#ksCustomTitleBar{"
            "  background:%1;"
            "  border-bottom:1px solid %2;"
            "}"
            "#ksCustomTitleBar QLabel{"
            "  color:%3;"
            "  font-size:12px;"
            "  font-weight:600;"
            "}"
            "#ksCustomTitleBar QLineEdit{"
            "  background:%4;"
            "  color:%5;"
            "  border:1px solid %6;"
            "  border-radius:3px;"
            "  padding:1px 6px;"
            "  font-size:12px;"
            "}"
            "#ksCustomTitleBar QPushButton{"
            "  background:transparent;"
            "  color:%3;"
            "  border:none;"
            "  border-radius:3px;"
            "  font-size:11px;"
            "}"
            "#ksCustomTitleBar QPushButton#ksTitlePinButton:hover,"
            "#ksCustomTitleBar QPushButton#ksTitleMinButton:hover,"
            "#ksCustomTitleBar QPushButton#ksTitleMaxButton:hover{"
            "  background:#2E8BFF;"
            "}"
            "#ksCustomTitleBar QPushButton#ksTitlePinButton:pressed,"
            "#ksCustomTitleBar QPushButton#ksTitleMinButton:pressed,"
            "#ksCustomTitleBar QPushButton#ksTitleMaxButton:pressed{"
            "  background:#1F78D0;"
            "}"
            "#ksCustomTitleBar QPushButton#ksTitleCloseButton:hover{"
            "  background:#E2554E;"
            "}"
            "#ksCustomTitleBar QPushButton#ksTitleCloseButton:pressed{"
            "  background:#C7433B;"
            "}"
            "#ksCustomTitleBar QPushButton:disabled{"
            "  background:%7;"
            "  color:%8;"
            "  border:1px solid %6;"
            "  padding:0 6px;"
            "  font-weight:700;"
            "}")
            .arg(titleBarBackgroundText)
            .arg(titleBarBorderText)
            .arg(titleTextColorText)
            .arg(commandBackgroundText)
            .arg(commandTextColorText)
            .arg(commandBorderText)
            .arg(userBadgeBackgroundText)
            .arg(userBadgeTextColorText);
        setStyleSheet(titleBarStyleSheetText);

        // 图标与按钮文案同步：
        // - 图钉根据置顶状态切换空心/实心；
        // - 最大化按钮根据窗口状态切换最大化/还原图标；
        // - 用户按钮根据“33251”特判切换显示名。
        m_pinButton->setIcon(QIcon(
            m_isPinned
            ? QStringLiteral(":/Icon/titlebar_pin_fill.svg")
            : QStringLiteral(":/Icon/titlebar_pin_line.svg")));
        m_pinButton->setToolTip(m_isPinned
            ? QStringLiteral("取消窗口置顶")
            : QStringLiteral("置顶窗口"));
        m_pinButton->setIconSize(QSize(kControlIconSize, kControlIconSize));

        m_minButton->setIcon(QIcon(QStringLiteral(":/Icon/titlebar_minimize.svg")));
        m_minButton->setIconSize(QSize(kControlIconSize, kControlIconSize));

        m_maxButton->setIcon(QIcon(
            m_isMaximized
            ? QStringLiteral(":/Icon/titlebar_restore.svg")
            : QStringLiteral(":/Icon/titlebar_maximize.svg")));
        m_maxButton->setToolTip(m_isMaximized
            ? QStringLiteral("还原主窗口")
            : QStringLiteral("最大化主窗口"));
        m_maxButton->setIconSize(QSize(kControlIconSize, kControlIconSize));

        m_closeButton->setIcon(QIcon(QStringLiteral(":/Icon/titlebar_close.svg")));
        m_closeButton->setIconSize(QSize(kControlIconSize, kControlIconSize));

        const QString displayUserNameText = m_isSpecialUser
            ? QStringLiteral("WangWei_CM")
            : m_rawUserNameText;
        m_userBadgeButton->setText(displayUserNameText);
        m_userBadgeButton->setToolTip(QStringLiteral("当前用户：%1").arg(m_rawUserNameText));

        QIcon appIcon = window() != nullptr ? window()->windowIcon() : QIcon();
        if (appIcon.isNull())
        {
            appIcon = QApplication::windowIcon();
        }
        if (appIcon.isNull())
        {
            appIcon = QIcon(QStringLiteral(":/Image/Resource/Logo/MainLogo.png"));
        }
        m_appIconLabel->setPixmap(appIcon.pixmap(kAppIconSize, kAppIconSize));
    }

    void CustomTitleBar::updateCommandLineWidth()
    {
        if (m_commandLineEdit == nullptr)
        {
            return;
        }

        const int commandLineWidth = std::clamp(width() / 3, kCommandLineMinWidth, kCommandLineMaxWidth);
        m_commandLineEdit->setFixedWidth(commandLineWidth);
    }

    QString CustomTitleBar::resolveCompileDateText() const
    {
        // __DATE__ 典型格式为 "Apr 11 2026"，这里统一转换为 yyyy-MM-dd。
        const QString rawCompileDateText = QString::fromLatin1(__DATE__);
        const QStringList datePartList = rawCompileDateText.split(' ', Qt::SkipEmptyParts);
        if (datePartList.size() != 3)
        {
            return QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"));
        }

        const QString monthTokenText = datePartList.at(0);
        const int dayValue = datePartList.at(1).toInt();
        const int yearValue = datePartList.at(2).toInt();

        const std::array<std::pair<const char*, int>, 12> monthMappingList = {
            std::pair<const char*, int>{ "Jan", 1 },
            std::pair<const char*, int>{ "Feb", 2 },
            std::pair<const char*, int>{ "Mar", 3 },
            std::pair<const char*, int>{ "Apr", 4 },
            std::pair<const char*, int>{ "May", 5 },
            std::pair<const char*, int>{ "Jun", 6 },
            std::pair<const char*, int>{ "Jul", 7 },
            std::pair<const char*, int>{ "Aug", 8 },
            std::pair<const char*, int>{ "Sep", 9 },
            std::pair<const char*, int>{ "Oct", 10 },
            std::pair<const char*, int>{ "Nov", 11 },
            std::pair<const char*, int>{ "Dec", 12 }
        };

        int monthValue = 0;
        for (const auto& monthEntry : monthMappingList)
        {
            if (monthTokenText.compare(QString::fromLatin1(monthEntry.first), Qt::CaseInsensitive) == 0)
            {
                monthValue = monthEntry.second;
                break;
            }
        }

        const QDate compileDateValue(yearValue, monthValue, dayValue);
        if (!compileDateValue.isValid())
        {
            return QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"));
        }
        return compileDateValue.toString(QStringLiteral("yyyy-MM-dd"));
    }

    QString CustomTitleBar::resolveCurrentUserNameText() const
    {
        const QString envUserNameText = qEnvironmentVariable("USERNAME").trimmed();
        if (!envUserNameText.isEmpty())
        {
            return envUserNameText;
        }

        wchar_t userNameBuffer[256] = {};
        DWORD bufferLength = static_cast<DWORD>(std::size(userNameBuffer));
        if (::GetUserNameW(userNameBuffer, &bufferLength) != FALSE)
        {
            const QString apiUserNameText = QString::fromWCharArray(userNameBuffer).trimmed();
            if (!apiUserNameText.isEmpty())
            {
                return apiUserNameText;
            }
        }

        return QStringLiteral("UnknownUser");
    }
}
