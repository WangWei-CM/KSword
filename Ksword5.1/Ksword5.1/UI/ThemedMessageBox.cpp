#include "ThemedMessageBox.h"

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

    // kMessageBoxTitleBarObjectName 作用：
    // - 标记 QMessageBox 内部自绘标题栏；
    // - 用于重复刷新时定位已有标题栏，避免重复插入。
    constexpr const char* kMessageBoxTitleBarObjectName = "KswordThemedMessageBoxTitleBar";

    // kMessageBoxTitleBarInstalledPropertyName 作用：
    // - 标记 QMessageBox 根布局已经为悬浮标题栏预留过顶部空白；
    // - 避免 Palette/StyleChange 多次触发后重复累加顶部边距。
    constexpr const char* kMessageBoxTitleBarInstalledPropertyName = "ksword_message_box_titlebar_margin_reserved";

    // kMessageBoxOriginalMarginPropertyName 作用：
    // - 记录 QMessageBox 原始布局边距；
    // - 后续主题刷新时以原始值计算顶部避让距离，避免反复叠加。
    constexpr const char* kMessageBoxOriginalMarginLeftPropertyName = "ksword_message_box_original_margin_left";
    constexpr const char* kMessageBoxOriginalMarginTopPropertyName = "ksword_message_box_original_margin_top";
    constexpr const char* kMessageBoxOriginalMarginRightPropertyName = "ksword_message_box_original_margin_right";
    constexpr const char* kMessageBoxOriginalMarginBottomPropertyName = "ksword_message_box_original_margin_bottom";

    // kMessageBoxTitleLabelObjectName 作用：
    // - 标记自绘标题栏中的标题文本控件；
    // - 每次 polish 时同步 QMessageBox 当前 windowTitle。
    constexpr const char* kMessageBoxTitleLabelObjectName = "KswordThemedMessageBoxTitleLabel";

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
    constexpr int kMessageBoxOuterBorderWidth = 1;
    constexpr int kMessageTitleBarHeight = 34;
    constexpr int kMessageLogoSize = 22;
    constexpr int kMessageTitleBarContentGap = 8;
    constexpr int kMessageTitleBarLeftMargin = 10;
    constexpr int kMessageTitleBarRightMargin = 4;
    constexpr int kMessageTitleBarItemSpacing = 8;

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

    // MessageBoxTitleBar 作用：
    // - 作为 QMessageBox 的自绘标题栏；
    // - 负责左侧 Logo/标题展示以及鼠标拖动窗口；
    // - 不提供最小化、最大化或关闭入口，消息框只能通过业务按钮结束。
    class MessageBoxTitleBar final : public QWidget
    {
    public:
        // 构造函数：
        // - 参数 ownerMessageBox：所属 QMessageBox；
        // - 处理逻辑：创建 Logo 和标题文本，关闭/最小化/最大化入口统一禁用；
        // - 返回值：无。
        explicit MessageBoxTitleBar(QMessageBox* ownerMessageBox)
            : QWidget(ownerMessageBox),
              m_ownerMessageBox(ownerMessageBox)
        {
            setObjectName(QString::fromLatin1(kMessageBoxTitleBarObjectName));
            setFixedHeight(kMessageTitleBarHeight);
            setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            setAttribute(Qt::WA_StyledBackground, true);

            m_logoLabel = new QLabel(this);
            m_logoLabel->setFixedSize(kMessageLogoSize, kMessageLogoSize);
            m_logoLabel->setAlignment(Qt::AlignCenter);
            m_logoLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            const QIcon applicationIcon = resolveApplicationIcon(ownerMessageBox);
            if (!applicationIcon.isNull())
            {
                // titlePixmap 用途：按标题栏固定尺寸取图，避免把启动页 MainLogo 当作窗口图标。
                const QPixmap titlePixmap = applicationIcon.pixmap(QSize(kMessageLogoSize, kMessageLogoSize));
                m_logoLabel->setPixmap(titlePixmap);
            }

            m_titleLabel = new QLabel(this);
            m_titleLabel->setObjectName(QString::fromLatin1(kMessageBoxTitleLabelObjectName));
            m_titleLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
            m_titleLabel->setMinimumWidth(0);
            m_titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            m_titleLabel->setTextFormat(Qt::PlainText);
            m_titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);

            updateTitleText();
            syncChildGeometry();
        }

        // updateTitleText 作用：
        // - 同步 QMessageBox 当前标题文本；
        // - 若业务未设置标题，则使用应用名兜底。
        void updateTitleText()
        {
            QString titleText = m_ownerMessageBox != nullptr ? m_ownerMessageBox->windowTitle().trimmed() : QString();
            if (titleText.isEmpty() && qApp != nullptr)
            {
                titleText = qApp->applicationDisplayName().trimmed();
                if (titleText.isEmpty())
                {
                    titleText = qApp->applicationName().trimmed();
                }
            }
            if (titleText.isEmpty())
            {
                titleText = QStringLiteral("Ksword");
            }
            if (m_titleLabel != nullptr)
            {
                m_fullTitleText = titleText;
                m_titleLabel->setToolTip(titleText);
                syncChildGeometry();
            }
        }

        // syncChildGeometry 作用：
        // - 手工计算 Logo 和标题位置；
        // - 标题只使用当前标题栏可见宽度，避免长标题把控件布局推到窗口外；
        // - 返回值：无。
        void syncChildGeometry()
        {
            const int titleBarWidth = std::max(width(), 0);
            const int titleBarHeight = std::max(height(), kMessageTitleBarHeight);

            // logoVisible 用途：极窄异常宽度下优先保留标题栏可读性，Logo 允许隐藏。
            const int minimumWidthForLogo =
                kMessageTitleBarLeftMargin +
                kMessageLogoSize +
                kMessageTitleBarRightMargin;
            const bool logoVisible = titleBarWidth >= minimumWidthForLogo;
            const int logoX = kMessageTitleBarLeftMargin;
            const int logoY = std::max((titleBarHeight - kMessageLogoSize) / 2, 0);
            if (m_logoLabel != nullptr)
            {
                m_logoLabel->setVisible(logoVisible);
                m_logoLabel->setGeometry(logoX, logoY, kMessageLogoSize, kMessageLogoSize);
            }

            // titleX/titleRight 用途：标题只吃标题栏安全区域内的剩余空间，空间不足时直接省略。
            const int titleX = logoVisible
                ? (kMessageTitleBarLeftMargin + kMessageLogoSize + kMessageTitleBarItemSpacing)
                : kMessageTitleBarLeftMargin;
            const int titleRight = std::max(titleBarWidth - kMessageTitleBarRightMargin, titleX);
            const int titleWidth = std::max(titleRight - titleX, 0);
            if (m_titleLabel != nullptr)
            {
                m_titleLabel->setGeometry(titleX, 0, titleWidth, titleBarHeight);

                // elidedTitle 用途：Qt6 下 QLabel 仍可能按完整文本给出较大 sizeHint，手工省略可消除挤压来源。
                const QFontMetrics titleFontMetrics(m_titleLabel->font());
                const QString elidedTitle = titleWidth > 0
                    ? titleFontMetrics.elidedText(m_fullTitleText, Qt::ElideRight, titleWidth)
                    : QString();
                m_titleLabel->setText(elidedTitle);
            }
        }

        // sizeHint 作用：给 Qt6 样式系统一个稳定标题栏建议尺寸。
        // 返回值：固定高度、最小消息框宽度的建议尺寸。
        QSize sizeHint() const override
        {
            return QSize(kMessageBoxMinWidth, kMessageTitleBarHeight);
        }

        // minimumSizeHint 作用：声明能容纳 Logo 与安全边距的最小标题栏尺寸。
        // 返回值：Logo 加左右安全边距后的最小尺寸。
        QSize minimumSizeHint() const override
        {
            return QSize(
                kMessageTitleBarLeftMargin + kMessageLogoSize + kMessageTitleBarRightMargin,
                kMessageTitleBarHeight);
        }

    protected:
        // resizeEvent 作用：
        // - Qt6 对话框在 adjustSize/show/高 DPI 切换时会多次更新子窗口尺寸；
        // - 每次尺寸变化后重新截断标题，避免沿用旧布局结果；
        // - 返回值：无。
        void resizeEvent(QResizeEvent* resizeEventPointer) override
        {
            QWidget::resizeEvent(resizeEventPointer);
            syncChildGeometry();
        }

        // mousePressEvent 作用：
        // - 记录标题栏拖动起点；
        // - 返回值：无，只更新拖动状态。
        void mousePressEvent(QMouseEvent* mouseEventPointer) override
        {
            if (mouseEventPointer != nullptr && mouseEventPointer->button() == Qt::LeftButton)
            {
                m_dragCandidateActive = true;
                m_dragInProgress = false;
                m_dragPressGlobalPos = mouseEventPointer->globalPosition().toPoint();
                if (QWidget* hostWidget = window())
                {
                    m_windowPressTopLeft = hostWidget->frameGeometry().topLeft();
                }
                mouseEventPointer->accept();
                return;
            }
            QWidget::mousePressEvent(mouseEventPointer);
        }

        // mouseMoveEvent 作用：
        // - 达到拖动阈值后发起系统窗口拖动；
        // - 若平台不支持 startSystemMove，则回退手工 move。
        void mouseMoveEvent(QMouseEvent* mouseEventPointer) override
        {
            if (mouseEventPointer != nullptr
                && m_dragCandidateActive
                && (mouseEventPointer->buttons() & Qt::LeftButton))
            {
                const QPoint currentGlobalPos = mouseEventPointer->globalPosition().toPoint();
                const int dragDistance = (currentGlobalPos - m_dragPressGlobalPos).manhattanLength();
                QWidget* hostWidget = window();
                if (hostWidget != nullptr && dragDistance >= QApplication::startDragDistance())
                {
                    if (!m_dragInProgress)
                    {
                        m_dragInProgress = true;
                        QWindow* hostWindowHandle = hostWidget->windowHandle();
                        if (hostWindowHandle != nullptr && hostWindowHandle->startSystemMove())
                        {
                            mouseEventPointer->accept();
                            return;
                        }
                    }

                    hostWidget->move(m_windowPressTopLeft + currentGlobalPos - m_dragPressGlobalPos);
                    mouseEventPointer->accept();
                    return;
                }
            }
            QWidget::mouseMoveEvent(mouseEventPointer);
        }

        // mouseReleaseEvent 作用：
        // - 清理拖动候选状态；
        // - 返回值：无。
        void mouseReleaseEvent(QMouseEvent* mouseEventPointer) override
        {
            m_dragCandidateActive = false;
            m_dragInProgress = false;
            QWidget::mouseReleaseEvent(mouseEventPointer);
        }

    private:
        QPointer<QMessageBox> m_ownerMessageBox; // m_ownerMessageBox：所属消息框。
        QLabel* m_logoLabel = nullptr;           // m_logoLabel：左侧应用图标控件。
        QLabel* m_titleLabel = nullptr;          // m_titleLabel：标题文本控件。
        QString m_fullTitleText;                 // m_fullTitleText：未省略的完整标题文本。
        bool m_dragCandidateActive = false;      // m_dragCandidateActive：是否处于拖动候选。
        bool m_dragInProgress = false;           // m_dragInProgress：是否已经开始拖动。
        QPoint m_dragPressGlobalPos;             // m_dragPressGlobalPos：按下时全局坐标。
        QPoint m_windowPressTopLeft;             // m_windowPressTopLeft：按下时窗口左上角。
    };

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
        const QString outerBorderColorText = KswordTheme::PrimaryBlueHex;
        const QString textColorText = messageBoxTextColor(darkModeEnabled).name(QColor::HexRgb);
        const QString secondaryTextColorText = messageBoxSecondaryTextColor(darkModeEnabled).name(QColor::HexRgb);
        const QString secondaryButtonColorText = messageBoxSecondaryButtonColor(darkModeEnabled).name(QColor::HexRgb);
        const QString secondaryButtonHoverColorText = messageBoxSecondaryButtonHoverColor(darkModeEnabled).name(QColor::HexRgb);

        return QStringLiteral(
            "QMessageBox#%1{"
            "  background-color:%2;"
            "  color:%3;"
            "  border:%13px solid %14;"
            "  border-radius:0px;"
            "}"
            "QMessageBox#%1 QWidget{"
            "  background:transparent;"
            "  color:%3;"
            "}"
            "QMessageBox#%1 QWidget#%10{"
            "  background-color:%11;"
            "  color:%3;"
            "  border-bottom:1px solid %4;"
            "}"
            "QMessageBox#%1 QLabel#%12{"
            "  color:%3;"
            "  font-size:13px;"
            "  font-weight:700;"
            "}"
            "QMessageBox#%1 QLabel#qt_msgbox_label{"
            "  font-size:14px;"
            "  font-weight:700;"
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
            "  border:1px solid %4;"
            "  border-radius:0px;"
            "  padding:4px 10px;"
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
            "  color:#FFFFFF;"
            "  border:1px solid %8;"
            "  font-weight:700;"
            "}"
            "QMessageBox#%1 QPushButton[ksword_primary=\"true\"]:hover{"
            "  background:__MESSAGE_PRIMARY_HOVER__;"
            "  border-color:__MESSAGE_PRIMARY_HOVER__;"
            "}"
            "QMessageBox#%1 QPushButton[ksword_primary=\"true\"]:pressed{"
            "  background:%9;"
            "  border-color:%9;"
            "}"
            "QMessageBox#%1 QTextEdit{"
            "  background:__MESSAGE_SURFACE__;"
            "  color:%3;"
            "  border:1px solid %4;"
            "  border-radius:0px;"
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
            .arg(QString::fromLatin1(kMessageBoxTitleBarObjectName))
            .arg(surfaceColorText)
            .arg(QString::fromLatin1(kMessageBoxTitleLabelObjectName))
            .arg(kMessageBoxOuterBorderWidth)
            .arg(outerBorderColorText)
            .replace(QStringLiteral("__MESSAGE_SURFACE__"), surfaceColorText)
            .replace(QStringLiteral("__MESSAGE_PRIMARY_HOVER__"), KswordTheme::PrimaryBlueSolidHoverHex());
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

    // findMessageBoxGridLayout 作用：
    // - QMessageBox 内部通常使用 QGridLayout；
    // - 返回该布局后才能统一调整内容边距，为悬浮标题栏预留安全区域。
    QGridLayout* findMessageBoxGridLayout(QMessageBox* messageBox)
    {
        if (messageBox == nullptr)
        {
            return nullptr;
        }
        return qobject_cast<QGridLayout*>(messageBox->layout());
    }

    // propertyIntOrFallback 作用：
    // - 从 QObject 动态属性读取整数；
    // - 属性不存在或不可转换时返回 fallbackValue；
    // - 返回值：解析后的整数边距值。
    int propertyIntOrFallback(
        const QObject* objectPointer,
        const char* propertyName,
        const int fallbackValue)
    {
        if (objectPointer == nullptr || propertyName == nullptr)
        {
            return fallbackValue;
        }

        bool conversionOk = false;
        const int propertyValue = objectPointer->property(propertyName).toInt(&conversionOk);
        if (!conversionOk)
        {
            return fallbackValue;
        }
        return propertyValue;
    }

    // reserveMessageBoxTitleBarTopMargin 作用：
    // - 不再把标题栏作为 QGridLayout 的第 0 行布局项；
    // - 仅给根布局顶部增加固定避让距离，确保标题栏不遮挡正文；
    // - 参数 messageBox：需要调整的消息框；gridLayout：消息框根网格布局；
    // - 返回值：无。
    void reserveMessageBoxTitleBarTopMargin(QMessageBox* messageBox, QGridLayout* gridLayout)
    {
        if (messageBox == nullptr || gridLayout == nullptr)
        {
            return;
        }

        if (!messageBox->property(kMessageBoxTitleBarInstalledPropertyName).toBool())
        {
            // originalMargins 用途：保存 Qt 原生 QMessageBox 内容边距，避免破坏按钮/正文布局。
            const QMargins originalMargins = gridLayout->contentsMargins();
            messageBox->setProperty(kMessageBoxOriginalMarginLeftPropertyName, originalMargins.left());
            messageBox->setProperty(kMessageBoxOriginalMarginTopPropertyName, originalMargins.top());
            messageBox->setProperty(kMessageBoxOriginalMarginRightPropertyName, originalMargins.right());
            messageBox->setProperty(kMessageBoxOriginalMarginBottomPropertyName, originalMargins.bottom());
            messageBox->setProperty(kMessageBoxTitleBarInstalledPropertyName, true);
        }

        // currentMargins 用途：当属性缺失时提供兜底，通常只在异常热重载场景出现。
        const QMargins currentMargins = gridLayout->contentsMargins();
        const int originalLeft = propertyIntOrFallback(
            messageBox,
            kMessageBoxOriginalMarginLeftPropertyName,
            currentMargins.left());
        const int originalTop = propertyIntOrFallback(
            messageBox,
            kMessageBoxOriginalMarginTopPropertyName,
            currentMargins.top());
        const int originalRight = propertyIntOrFallback(
            messageBox,
            kMessageBoxOriginalMarginRightPropertyName,
            currentMargins.right());
        const int originalBottom = propertyIntOrFallback(
            messageBox,
            kMessageBoxOriginalMarginBottomPropertyName,
            currentMargins.bottom());
        const int reservedTop = originalTop + kMessageTitleBarHeight + kMessageTitleBarContentGap;

        gridLayout->setContentsMargins(originalLeft, reservedTop, originalRight, originalBottom);
    }

    // syncMessageBoxTitleBarGeometry 作用：
    // - 让自绘标题栏始终覆盖 QMessageBox 顶部边框内侧完整宽度；
    // - 修复短标题时标题栏只按文本 sizeHint 收缩到左上角的问题；
    // - 同时避免标题栏压住 1px 主题蓝色外描边；
    // - 参数 messageBox：所属消息框；titleBar：自绘标题栏；
    // - 返回值：无。
    void syncMessageBoxTitleBarGeometry(QMessageBox* messageBox, QWidget* titleBar)
    {
        if (messageBox == nullptr || titleBar == nullptr)
        {
            return;
        }

        // availableWidth 用途：优先使用消息框当前宽度，显示前宽度不足时使用最小宽度兜底。
        const int availableWidth = std::max(messageBox->width(), messageBox->minimumWidth());
        // titleBarWidth 用途：扣掉左右外描边，避免标题栏遮住蓝色窗口边线。
        const int titleBarWidth = std::max(availableWidth - (kMessageBoxOuterBorderWidth * 2), 0);
        titleBar->setGeometry(
            kMessageBoxOuterBorderWidth,
            kMessageBoxOuterBorderWidth,
            titleBarWidth,
            kMessageTitleBarHeight);
        titleBar->setMinimumWidth(0);
        titleBar->setMaximumWidth(QWIDGETSIZE_MAX);
        titleBar->raise();

        if (MessageBoxTitleBar* typedTitleBar = dynamic_cast<MessageBoxTitleBar*>(titleBar))
        {
            // syncChildGeometry 用途：几何同步后立即刷新标题省略，防止 Qt6 延迟 resize 期间短暂错位。
            typedTitleBar->syncChildGeometry();
        }
    }

    // ensureCustomMessageBoxTitleBar 作用：
    // - 给 QMessageBox 安装自绘标题栏；
    // - 隐藏系统标题栏，左侧显示应用 Logo，不提供最小化/最大化/关闭按钮。
    void ensureCustomMessageBoxTitleBar(QMessageBox* messageBox)
    {
        if (messageBox == nullptr)
        {
            return;
        }

        // windowFlags 用途：Qt6 下集中写回窗口 flags，避免单独 setWindowFlag 在 show 前后被平台样式补回按钮。
        Qt::WindowFlags windowFlags = messageBox->windowFlags();
        windowFlags = (windowFlags & ~Qt::WindowType_Mask) | Qt::Dialog;
        windowFlags |= Qt::FramelessWindowHint;
        windowFlags &= ~Qt::WindowSystemMenuHint;
        windowFlags &= ~Qt::WindowMinimizeButtonHint;
        windowFlags &= ~Qt::WindowMaximizeButtonHint;
        windowFlags &= ~Qt::WindowMinMaxButtonsHint;
        windowFlags &= ~Qt::WindowCloseButtonHint;
        windowFlags &= ~Qt::WindowContextHelpButtonHint;
        if (messageBox->windowFlags() != windowFlags)
        {
            // setWindowFlags 在 Qt6 中可能隐藏已显示窗口；仅在 flags 真实变化时写回，并恢复可见状态。
            const bool messageBoxWasVisible = messageBox->isVisible();
            messageBox->setWindowFlags(windowFlags);
            if (messageBoxWasVisible)
            {
                messageBox->show();
            }
        }

        QGridLayout* gridLayout = findMessageBoxGridLayout(messageBox);
        if (gridLayout == nullptr)
        {
            return;
        }

        reserveMessageBoxTitleBarTopMargin(messageBox, gridLayout);

        QWidget* titleBarWidget = messageBox->findChild<QWidget*>(
            QString::fromLatin1(kMessageBoxTitleBarObjectName),
            Qt::FindDirectChildrenOnly);
        MessageBoxTitleBar* titleBar = dynamic_cast<MessageBoxTitleBar*>(titleBarWidget);
        if (titleBar == nullptr)
        {
            titleBar = new MessageBoxTitleBar(messageBox);
        }
        titleBar->updateTitleText();
        syncMessageBoxTitleBarGeometry(messageBox, titleBar);
        titleBar->show();
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
            if (eventType == QEvent::Close)
            {
                // Close 事件不能在全局主题器里拦截：
                // - QMessageBox 的标准按钮会经由 QDialog::done()/accept()/reject() 收束；
                // - Qt6 在收束过程中仍可能投递 Close 事件来结束本轮 exec()；
                // - 若这里 ignore()，按钮点击已经发生，但窗口不关闭，调用方也拿不到返回值；
                // - 标题栏关闭入口已经通过 FramelessWindowHint/WindowCloseButtonHint 移除，
                //   全局主题器只负责外观，不再改变消息框关闭语义。
                return QObject::eventFilter(watchedObject, eventObject);
            }

            if (eventType == QEvent::Resize)
            {
                // Resize 事件只同步悬浮标题栏几何，不重新套完整样式；
                // 这样可以避免 adjustSize 触发 Resize 后再次进入 polish 流程。
                QWidget* titleBarWidget = messageBox->findChild<QWidget*>(
                    QString::fromLatin1(kMessageBoxTitleBarObjectName),
                    Qt::FindDirectChildrenOnly);
                syncMessageBoxTitleBarGeometry(messageBox, titleBarWidget);
                return QObject::eventFilter(watchedObject, eventObject);
            }

            if (eventType == QEvent::Polish ||
                eventType == QEvent::Show ||
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

            if (messageBox->property(kThemePolishingPropertyName).toBool())
            {
                return;
            }

            const bool darkModeEnabled = KswordTheme::IsDarkModeEnabled();
            const bool themeAlreadyApplied =
                messageBox->objectName() == QString::fromLatin1(kThemedMessageBoxObjectName) &&
                messageBox->property(kThemeModePropertyName).toBool() == darkModeEnabled;

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
            const QString targetStyleSheet = buildMessageBoxStyleSheet(darkModeEnabled);
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
            ensureCustomMessageBoxTitleBar(messageBox);

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
            messageBox->adjustSize();
            QWidget* titleBarWidget = messageBox->findChild<QWidget*>(
                QString::fromLatin1(kMessageBoxTitleBarObjectName),
                Qt::FindDirectChildrenOnly);
            syncMessageBoxTitleBarGeometry(messageBox, titleBarWidget);
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
                pushButton->setMinimumHeight(28);
                pushButton->setMaximumHeight(30);
                pushButton->setMinimumWidth(primaryButton ? 86 : 76);
                pushButton->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
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
