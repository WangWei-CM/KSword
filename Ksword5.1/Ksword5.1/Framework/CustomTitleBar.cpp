#include "CustomTitleBar.h"

#include "../theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDate>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QStringList>
#include <QWidget>
#include <QWindow>

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
    constexpr int kTitleBarHeight = 30;
    constexpr int kControlButtonWidth = 32;
    constexpr int kControlButtonHeight = 24;
    constexpr int kControlIconSize = 16;
    constexpr int kUserBadgeMinWidth = 82;
    constexpr int kCommandLineMinWidth = 210;
    constexpr int kCommandLineMaxWidth = 760;
    constexpr int kAppIconSize = 18;

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

    // resolveApplicationPreviewIcon：
    // - 作用：优先从可执行文件路径提取系统壳层图标；
    // - 目标：让左上角图标与资源管理器中文件预览保持一致；
    // - 传出：成功返回可执行文件图标，失败返回空图标。
    QIcon resolveApplicationPreviewIcon()
    {
        // executablePathText 用途：保存当前进程可执行文件绝对路径。
        const QString executablePathText = QCoreApplication::applicationFilePath();
        if (executablePathText.trimmed().isEmpty())
        {
            return QIcon();
        }

        // executableFileInfo 用途：描述当前可执行文件路径，供壳层图标提供器读取。
        const QFileInfo executableFileInfo(executablePathText);
        if (!executableFileInfo.exists())
        {
            return QIcon();
        }

        // iconProvider 用途：向系统壳层查询与资源管理器一致的文件图标。
        QFileIconProvider iconProvider;
        return iconProvider.icon(executableFileInfo);
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

    void CustomTitleBar::setCustomRightWidget(QWidget* customRightWidget)
    {
        if (m_rightLayout == nullptr)
        {
            return;
        }

        if (m_customRightWidget == customRightWidget)
        {
            return;
        }

        if (m_customRightWidget != nullptr)
        {
            m_rightLayout->removeWidget(m_customRightWidget);
            m_customRightWidget->hide();
        }

        // m_customRightWidget 用途：保存右侧扩展控件实例，便于后续替换或移除。
        m_customRightWidget = customRightWidget;
        if (m_customRightWidget == nullptr)
        {
            return;
        }

        if (m_customRightWidget->parentWidget() != m_rightWidget)
        {
            m_customRightWidget->setParent(m_rightWidget);
        }
        m_customRightWidget->setVisible(true);
        m_rightLayout->insertWidget(0, m_customRightWidget, 0, Qt::AlignVCenter);
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

    void CustomTitleBar::mousePressEvent(QMouseEvent* mouseEventPointer)
    {
        if (mouseEventPointer != nullptr
            && mouseEventPointer->button() == Qt::LeftButton
            && isPointInDraggableRegion(mouseEventPointer->position().toPoint()))
        {
            // m_dragCandidateActive 用途：标记本次左键按下可进入标题栏拖动候选状态。
            m_dragCandidateActive = true;
            // m_dragInProgress 用途：新的按压序列开始时重置“系统拖动已启动”标记。
            m_dragInProgress = false;
            // m_dragPressLocalPos 用途：保存按下时的标题栏局部坐标，供恢复窗口时计算相对位置。
            m_dragPressLocalPos = mouseEventPointer->position().toPoint();
            // m_dragPressGlobalPos 用途：保存按下时的全局坐标，供拖动阈值判断与恢复定位使用。
            m_dragPressGlobalPos = mouseEventPointer->globalPosition().toPoint();
            mouseEventPointer->accept();
            return;
        }

        m_dragCandidateActive = false;
        m_dragInProgress = false;
        QWidget::mousePressEvent(mouseEventPointer);
    }

    void CustomTitleBar::mouseMoveEvent(QMouseEvent* mouseEventPointer)
    {
        if (mouseEventPointer != nullptr
            && m_dragCandidateActive
            && !m_dragInProgress
            && (mouseEventPointer->buttons() & Qt::LeftButton))
        {
            // currentLocalPos 用途：记录本次 move 时相对标题栏左上角的坐标。
            const QPoint currentLocalPos = mouseEventPointer->position().toPoint();
            // dragDistance 用途：判断当前移动是否达到系统拖动阈值，避免点击被误判为拖动。
            const int dragDistance = (currentLocalPos - m_dragPressLocalPos).manhattanLength();
            if (dragDistance >= QApplication::startDragDistance())
            {
                // currentGlobalPos 用途：当前鼠标全局坐标，供恢复窗口和发起系统拖动复用。
                const QPoint currentGlobalPos = mouseEventPointer->globalPosition().toPoint();
                QWidget* hostWindowWidget = window();
                if (hostWindowWidget != nullptr)
                {
                    const bool hostWindowMaximized =
                        hostWindowWidget->isMaximized()
                        || ((hostWindowWidget->windowState() & Qt::WindowMaximized) != 0);
                    if (hostWindowMaximized)
                    {
                        restoreWindowFromMaximizedForDrag(hostWindowWidget, currentGlobalPos);
                    }

                    if (tryStartWindowSystemMove(currentGlobalPos))
                    {
                        m_dragCandidateActive = false;
                        m_dragInProgress = true;
                        mouseEventPointer->accept();
                        return;
                    }
                }
            }
        }

        QWidget::mouseMoveEvent(mouseEventPointer);
    }

    void CustomTitleBar::mouseReleaseEvent(QMouseEvent* mouseEventPointer)
    {
        m_dragCandidateActive = false;
        m_dragInProgress = false;
        QWidget::mouseReleaseEvent(mouseEventPointer);
    }

    void CustomTitleBar::mouseDoubleClickEvent(QMouseEvent* mouseEventPointer)
    {
        if (mouseEventPointer != nullptr
            && mouseEventPointer->button() == Qt::LeftButton
            && isPointInDraggableRegion(mouseEventPointer->position().toPoint()))
        {
            m_dragCandidateActive = false;
            m_dragInProgress = false;
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
        m_rootLayout->setContentsMargins(6, 1, 12, 1);
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
        m_rightLayout->setContentsMargins(0, 0, 2, 0);
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

        QIcon appIcon = resolveApplicationPreviewIcon();
        if (appIcon.isNull() && window() != nullptr)
        {
            appIcon = window()->windowIcon();
        }
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

    bool CustomTitleBar::tryStartWindowSystemMove(const QPoint& globalPoint)
    {
        Q_UNUSED(globalPoint);

        // hostWindowWidget 用途：获取标题栏所属顶层窗口，后续向系统发起窗口拖动。
        QWidget* hostWindowWidget = window();
        if (hostWindowWidget == nullptr)
        {
            return false;
        }

        // hostWindowHandle 用途：Qt 提供的顶层原生窗口句柄封装。
        QWindow* hostWindowHandle = hostWindowWidget->windowHandle();
        if (hostWindowHandle != nullptr && hostWindowHandle->startSystemMove())
        {
            return true;
        }

#ifdef Q_OS_WIN
        // hostWindowHandleValue 用途：Win32 兜底拖动链路所需窗口句柄。
        const HWND hostWindowHandleValue = reinterpret_cast<HWND>(hostWindowWidget->winId());
        if (hostWindowHandleValue != nullptr && ::IsWindow(hostWindowHandleValue) != FALSE)
        {
            ::ReleaseCapture();
            ::SendMessageW(
                hostWindowHandleValue,
                WM_SYSCOMMAND,
                static_cast<WPARAM>(SC_MOVE | HTCAPTION),
                0);
            return true;
        }
#endif

        return false;
    }

    void CustomTitleBar::restoreWindowFromMaximizedForDrag(
        QWidget* hostWindowWidget,
        const QPoint& globalPoint)
    {
        if (hostWindowWidget == nullptr)
        {
            return;
        }

        // restoredGeometry 用途：读取窗口最大化前的正常几何信息，供拖下还原时复用。
        QRect restoredGeometry = hostWindowWidget->normalGeometry();
        if (!restoredGeometry.isValid()
            || restoredGeometry.width() <= 0
            || restoredGeometry.height() <= 0)
        {
            restoredGeometry = hostWindowWidget->geometry();
        }

        const int windowWidth = std::max(1, hostWindowWidget->width());
        // horizontalRatio 用途：保存按下点在整窗宽度中的比例，恢复后让鼠标仍落在相近位置。
        const double horizontalRatio = std::clamp(
            static_cast<double>(m_dragPressLocalPos.x()) / static_cast<double>(windowWidth),
            0.0,
            1.0);
        // restoredLeft 用途：计算恢复为窗口化后窗口左上角 X 坐标。
        int restoredLeft = globalPoint.x() - static_cast<int>(restoredGeometry.width() * horizontalRatio);
        // restoredTopOffset 用途：让窗口恢复后标题栏仍贴近鼠标，而不是直接跳到屏幕顶端。
        const int restoredTopOffset = std::clamp(m_dragPressLocalPos.y(), 12, 24);
        // restoredTop 用途：计算恢复为窗口化后窗口左上角 Y 坐标。
        int restoredTop = globalPoint.y() - restoredTopOffset;

        // screenObject 用途：找到当前鼠标所在屏幕，避免恢复后窗口跑出可用工作区。
        QScreen* screenObject = QGuiApplication::screenAt(globalPoint);
        if (screenObject == nullptr && hostWindowWidget->windowHandle() != nullptr)
        {
            screenObject = hostWindowWidget->windowHandle()->screen();
        }
        if (screenObject != nullptr)
        {
            const QRect availableGeometry = screenObject->availableGeometry();
            restoredLeft = std::clamp(
                restoredLeft,
                availableGeometry.left(),
                availableGeometry.right() - restoredGeometry.width() + 1);
            restoredTop = std::clamp(
                restoredTop,
                availableGeometry.top(),
                availableGeometry.bottom() - restoredGeometry.height() + 1);
        }

        hostWindowWidget->showNormal();
        hostWindowWidget->setGeometry(
            restoredLeft,
            restoredTop,
            restoredGeometry.width(),
            restoredGeometry.height());
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
