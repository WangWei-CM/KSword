#pragma once

#include <functional>
#include <QPointer>

class QWidget;
class QAbstractSpinBox;
class QLineEdit;

namespace ks::ui
{
    // 独立窗口可明确保留自身 palette；普通控件允许主题函数更新其主题角色。
    enum class ThemePalettePolicy
    {
        FollowApplication,
        PreserveLocal
    };

    // BindWidgetTheme：登记控件完整的主题角色刷新函数；同一控件再次登记会替换旧函数。
    // widget 管理绑定生命周期，refresh 必须使用明确的主题角色，并完整刷新自身局部样式。
    // 构造期登记及主题变化分片排队；完整页面挂载可局部消费首帧请求，Popup 保留延迟约束。
    // 返回 false 表示空参数、没有 QApplication 或调用线程不属于 GUI 线程。
    // PreserveLocal 会在重建 QSS 后恢复调用前的局部 palette，防止 Qt repolish 丢失独立底色。
    bool BindWidgetTheme(QWidget* widget, std::function<void()> refresh,
        ThemePalettePolicy palettePolicy = ThemePalettePolicy::FollowApplication);

    // 仅在构造完成、最终父级建立后准备当前显示分支；不扫描模型或预加载隐藏 Tab/Popup。
    // 与原分片消费同一待刷新标记，已准备项不会在旧队列中重复执行。
    void PrepareWidgetThemeBindingsForShow(QWidget* scope);

    // 惰性工厂退出时统一准备已完成的可见分支；覆盖早退，不在构造开始或隐藏页调用。
    class ScopedWidgetThemePreparation final
    {
    public:
        explicit ScopedWidgetThemePreparation(QWidget* scope);
        ~ScopedWidgetThemePreparation();
        ScopedWidgetThemePreparation(const ScopedWidgetThemePreparation&) = delete;
        ScopedWidgetThemePreparation& operator=(const ScopedWidgetThemePreparation&) = delete;

    private:
        QPointer<QWidget> m_scope; // 工厂或回调关闭宿主时自动失效。
    };

    // 为数值控件及其内部编辑器显式绑定可读主题色，避免父级 QSS 把数字染成底色。
    // 只替换拥有的无线框颜色片段，保留 value/suffix、页面样式尺寸、步进按钮与编辑状态。
    bool BindSpinBoxTheme(QAbstractSpinBox* spinBox);

    // 页面明确登记独立单行字段，绑定固定输入层和各状态的文字/占位色。
    // 不设置搜索语义、尺寸、验证器或文本；内部 Combo/Spin 编辑器不调用此入口。
    bool BindLineEditTheme(QLineEdit* field);

    // 仅为页面明确登记的表格搜索框绑定无线框底色、文字和占位提示色。
    // 保留现有 padding/尺寸、输入内容、具体 placeholder、验证器和过滤信号。
    // 透明父 palette 回退主题表面色；普通输入框和数值控件不自动接入。
    bool BindSearchFieldTheme(QLineEdit* searchField);

    // HasWidgetThemeBinding：只判断该控件自身是否声明完整主题刷新，不跨到其未登记子控件。
    // 旧色值补偿据此跳过明确绑定的控件；其余存量控件仍沿用原有兼容路径。
    bool HasWidgetThemeBinding(const QWidget* widget);

    // RefreshWidgetThemeBindings：合并调度所有已登记控件，供主题种子更新完成后调用。
    // 不枚举 QApplication::allWidgets，不同步调用控件刷新，不改变全局或独立窗口 palette。
    void RefreshWidgetThemeBindings();
}
