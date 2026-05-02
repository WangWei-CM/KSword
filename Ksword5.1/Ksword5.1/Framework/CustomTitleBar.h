#pragma once

// ============================================================
// CustomTitleBar.h
// 作用说明：
// 1) 提供主窗口自绘标题栏（左信息、中命令输入、右控制按钮）；
// 2) 提供置顶/最小化/最大化/关闭等交互信号；
// 3) 支持深浅色主题切换和“33251 -> WangWei_CM”用户名特判展示。
// ============================================================

#include "../Framework.h"

#include <QPoint>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QGridLayout;
class QHBoxLayout;
class QMouseEvent;
class QResizeEvent;

namespace ks::ui
{
    // ============================================================
    // CustomTitleBar
    // 说明：
    // - 该类只负责标题栏 UI 与事件转发，不直接控制主窗口行为；
    // - 具体窗口动作（置顶/最小化/命令执行）由 MainWindow 接收信号后处理。
    // ============================================================
    class CustomTitleBar final : public QWidget
    {
        Q_OBJECT

    public:
        // 构造函数：
        // - 作用：创建并初始化自绘标题栏；
        // - 调用：MainWindow 构造时创建后通过 setMenuWidget 挂载；
        // - 传入 parentWidget：Qt 父控件；
        // - 传出：无。
        explicit CustomTitleBar(QWidget* parentWidget = nullptr);

        // 析构函数：
        // - 作用：默认析构即可，子控件由 Qt 父子关系自动回收。
        ~CustomTitleBar() override = default;

        // setPinnedState：
        // - 作用：同步图钉按钮显示状态（空心/实心）；
        // - 调用：MainWindow 每次置顶状态变化后调用；
        // - 传入 pinnedState：true=置顶，false=非置顶；
        // - 传出：无。
        void setPinnedState(bool pinnedState);

        // setCaptureProtectionState：
        // - 作用：同步截屏屏蔽按钮显示状态（眼睛/闭眼）；
        // - 调用：MainWindow 每次 SetWindowDisplayAffinity 状态变化后调用；
        // - 传入 protectedState：true=已屏蔽截屏，false=允许截屏；
        // - 传出：无。
        void setCaptureProtectionState(bool protectedState);

        // setMaximizedState：
        // - 作用：同步最大化按钮图标（最大化/还原）；
        // - 调用：MainWindow 在窗口状态变化时调用；
        // - 传入 maximizedState：true=当前已最大化；
        // - 传出：无。
        void setMaximizedState(bool maximizedState);

        // setDarkModeEnabled：
        // - 作用：切换标题栏深浅色主题样式；
        // - 调用：MainWindow::applyAppearanceSettings 内调用；
        // - 传入 darkModeEnabled：true=深色；
        // - 传出：无。
        void setDarkModeEnabled(bool darkModeEnabled);

        // isPointInDraggableRegion：
        // - 作用：判断标题栏某点是否属于“可拖动区域”；
        // - 调用：MainWindow::nativeEvent(HTCAPTION 命中测试)；
        // - 传入 localPos：相对标题栏左上角坐标；
        // - 传出：true=可作为拖动区域，false=命中交互控件。
        bool isPointInDraggableRegion(const QPoint& localPos) const;

        // titleBarHeight：
        // - 作用：返回标题栏固定高度，供主窗口命中测试复用。
        int titleBarHeight() const;

        // setCustomRightWidget：
        // - 作用：在右侧控制按钮前插入一个自定义控件（例如权限状态按钮组）；
        // - 调用：MainWindow 初始化权限按钮后调用；
        // - 传入 customRightWidget：要挂载的自定义控件，传 nullptr 表示移除；
        // - 传出：无。
        void setCustomRightWidget(QWidget* customRightWidget);

    signals:
        // requestTogglePinned：
        // - 作用：请求切换置顶状态；
        // - 触发：点击图钉按钮时触发。
        void requestTogglePinned();

        // requestToggleCaptureProtection：
        // - 作用：请求切换主窗口截屏屏蔽状态；
        // - 触发：点击标题栏眼睛按钮时触发。
        void requestToggleCaptureProtection();

        // requestMinimizeWindow：
        // - 作用：请求最小化主窗口；
        // - 触发：点击最小化按钮时触发。
        void requestMinimizeWindow();

        // requestToggleMaximizeWindow：
        // - 作用：请求最大化/还原主窗口；
        // - 触发：点击最大化按钮或双击可拖动区域时触发。
        void requestToggleMaximizeWindow();

        // requestCloseWindow：
        // - 作用：请求关闭主窗口；
        // - 触发：点击关闭按钮时触发。
        void requestCloseWindow();

        // commandSubmitted：
        // - 作用：提交命令行文本给主窗口执行；
        // - 触发：命令输入框按下回车时触发；
        // - 传入 commandText：用户输入的命令文本（未执行前文本）。
        void commandSubmitted(const QString& commandText);

    protected:
        // resizeEvent：
        // - 作用：窗口尺寸变化时保持中间输入框宽度=标题栏宽度的 1/3。
        void resizeEvent(QResizeEvent* resizeEventPointer) override;

        // mousePressEvent：
        // - 作用：记录标题栏左键按下状态，为后续拖动/双击判定提供起点；
        // - 说明：不在按下瞬间直接拖动，避免抢占双击序列。
        void mousePressEvent(QMouseEvent* mouseEventPointer) override;

        // mouseMoveEvent：
        // - 作用：在拖动阈值达到后发起系统拖动；
        // - 说明：最大化状态下会先恢复到窗口化，再继续进入拖动。
        void mouseMoveEvent(QMouseEvent* mouseEventPointer) override;

        // mouseReleaseEvent：
        // - 作用：结束一次标题栏按压/拖动候选状态，避免残留状态影响下次交互。
        void mouseReleaseEvent(QMouseEvent* mouseEventPointer) override;

        // mouseDoubleClickEvent：
        // - 作用：双击标题栏可拖动区域时请求最大化/还原。
        void mouseDoubleClickEvent(QMouseEvent* mouseEventPointer) override;

    private:
        // initializeUi：
        // - 作用：构建标题栏控件树与布局；
        // - 调用：构造函数内调用。
        void initializeUi();

        // initializeConnections：
        // - 作用：绑定按钮点击和回车提交信号；
        // - 调用：构造函数内调用。
        void initializeConnections();

        // updateVisualState：
        // - 作用：刷新图标、文本与样式（含主题和置顶状态）；
        // - 调用：状态变化时统一调用。
        void updateVisualState();

        // updateCommandLineWidth：
        // - 作用：把命令输入框宽度调整为标题栏可用宽度的 1/3。
        void updateCommandLineWidth();

        // updateUserBadgeWidth：
        // - 作用：根据当前展示用户名重新计算用户名徽标宽度；
        // - 调用：用户名文本或主题字体刷新后调用；
        // - 传入 displayUserNameText：按钮实际显示的用户名文本；
        // - 传出：无。
        void updateUserBadgeWidth(const QString& displayUserNameText);

        // tryStartWindowSystemMove：
        // - 作用：向宿主窗口发起一次系统级拖动；
        // - 调用：mouseMoveEvent 在达到拖动阈值后调用；
        // - 传入 globalPoint：当前鼠标全局坐标；
        // - 传出：true=系统已接管拖动，false=拖动未启动。
        bool tryStartWindowSystemMove(const QPoint& globalPoint);

        // restoreWindowFromMaximizedForDrag：
        // - 作用：最大化窗口开始拖动时，先恢复为窗口化并把窗口放到鼠标下方；
        // - 调用：mouseMoveEvent 检测到“最大化态拖动”时调用；
        // - 传入 hostWindowWidget：标题栏所属顶层窗口；
        // - 传入 globalPoint：当前鼠标全局坐标；
        // - 传出：无。
        void restoreWindowFromMaximizedForDrag(QWidget* hostWindowWidget, const QPoint& globalPoint);

        // resolveCompileDateText：
        // - 作用：把编译日期格式化为 yyyy-MM-dd；
        // - 调用：初始化左侧标题文本时调用；
        // - 传入：无；
        // - 传出：格式化日期字符串。
        QString resolveCompileDateText() const;

        // resolveCurrentUserNameText：
        // - 作用：读取当前用户名（环境变量 + WinAPI 回退）；
        // - 调用：初始化用户按钮时调用；
        // - 传入：无；
        // - 传出：用户名文本。
        QString resolveCurrentUserNameText() const;

    private:
        QWidget* m_leftWidget = nullptr;          // m_leftWidget：左侧信息区容器（图标+标题+用户名）。
        QHBoxLayout* m_leftLayout = nullptr;      // m_leftLayout：左侧信息区布局。
        QLabel* m_appIconLabel = nullptr;         // m_appIconLabel：程序图标标签。
        QLabel* m_titleTextLabel = nullptr;       // m_titleTextLabel：标题文本（含编译日期）。
        QPushButton* m_userBadgeButton = nullptr; // m_userBadgeButton：用户名展示按钮（禁用态仅展示）。

        QLineEdit* m_commandLineEdit = nullptr;   // m_commandLineEdit：标题栏中间命令输入框。

        QWidget* m_rightWidget = nullptr;         // m_rightWidget：右侧按钮区容器。
        QHBoxLayout* m_rightLayout = nullptr;     // m_rightLayout：右侧按钮区布局。
        QWidget* m_customRightWidget = nullptr;   // m_customRightWidget：右侧控制按钮前的自定义扩展控件。
        QPushButton* m_captureProtectionButton = nullptr; // m_captureProtectionButton：截屏屏蔽切换按钮。
        QPushButton* m_pinButton = nullptr;       // m_pinButton：置顶切换图钉按钮。
        QPushButton* m_minButton = nullptr;       // m_minButton：最小化按钮。
        QPushButton* m_maxButton = nullptr;       // m_maxButton：最大化/还原按钮。
        QPushButton* m_closeButton = nullptr;     // m_closeButton：关闭按钮。

        QGridLayout* m_rootLayout = nullptr;      // m_rootLayout：标题栏主布局（左/中/右三段）。

        QString m_rawUserNameText;                // m_rawUserNameText：读取到的原始用户名。
        bool m_isSpecialUser = false;             // m_isSpecialUser：是否命中“33251”特判。
        bool m_captureProtectionEnabled = false;  // m_captureProtectionEnabled：当前是否启用截屏屏蔽。
        bool m_isPinned = false;                  // m_isPinned：当前置顶状态。
        bool m_isMaximized = false;               // m_isMaximized：当前窗口是否最大化。
        bool m_darkModeEnabled = false;           // m_darkModeEnabled：当前是否深色主题。
        bool m_dragCandidateActive = false;       // m_dragCandidateActive：当前是否处于标题栏拖动候选状态。
        bool m_dragInProgress = false;            // m_dragInProgress：当前是否已把拖动交给系统处理。
        QPoint m_dragPressLocalPos;               // m_dragPressLocalPos：本次按下时相对标题栏左上角的坐标。
        QPoint m_dragPressGlobalPos;              // m_dragPressGlobalPos：本次按下时的全局屏幕坐标。
    };
}
