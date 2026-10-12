#pragma once

// ============================================================
// ThemeColorRemap.h
// 作用：
// 1) 修复“静态主题 token 在控件构造期被烧成固定 #RRGGBB”导致的主题不跟随问题；
// 2) 主题种子变化前后各采集一次全部静态 token 取值，得到“旧值 -> 新值”映射；
// 3) 用该映射重写全部存量控件 styleSheet 中的过期颜色，无需改动上千处调用点。
//
// 背景：theme.h 有两类 token。palette(...) 形式的动态 token 由 Qt 每次绘制时解析，
// 天然跟随主题；而 *ColorHex()、AccentHex()、语义色等静态 token 在调用那一瞬间就
// 固化成 #RRGGBB。页面构造期写入的 styleSheet 因此会永久停在构造时的主题配色，
// 且控件自身 styleSheet 优先级高于祖先全局 QSS，重建全局样式块也救不回来。
// ============================================================

#include <QList>
#include <QColor>
#include <QPalette>
#include <QString>
#include <functional>

class QObject;

namespace ks::ui
{
    // ThemeColorSnapshot 作用：一次主题切换前的静态 token 取值快照。
    // colorTexts 与内部 token 表按下标一一对应，不含语义信息，只用于建立旧新映射。
    struct ThemeColorSnapshot
    {
        QList<QString> colorTexts;
    };

    // CaptureThemeColorSnapshot 作用：按当前主题种子求值全部静态 token。
    // 调用方式：必须在 SetDarkModeEnabled / SetPrimaryAccentColor / SetMainBackgroundColor
    // 更新种子之前调用，否则采到的就是新值，映射为空。
    ThemeColorSnapshot CaptureThemeColorSnapshot();

    // ThemeColorRemapResult 作用：报告一次重映射的覆盖面，供主题刷新链路记录日志。
    struct ThemeColorRemapResult
    {
        // mappedColorCount：真正发生变化并参与重写的颜色条目数。
        int mappedColorCount = 0;
        // ambiguousColorCount：旧值相同但新值不同的冲突条目数，按静态 token 登记顺序取优先值。
        int ambiguousColorCount = 0;
        // inspectedWidgetCount：扫描过的、styleSheet 非空的控件数。
        int inspectedWidgetCount = 0;
        // rewrittenWidgetCount：实际被改写 styleSheet 的控件数。
        int rewrittenWidgetCount = 0;
        // rewrittenPaletteWidgetCount：实际被改写局部 QPalette 的控件数。
        int rewrittenPaletteWidgetCount = 0;
        // scheduledWidgetCount：异步入口本次登记的待检查控件数，不代表已经完成改写。
        int scheduledWidgetCount = 0;
    };

    // RemapStaleThemeColors 作用：把存量控件 styleSheet 里的旧 token 颜色改写成新值。
    // 输入：主题种子更新之前采集的快照。
    // 处理：重新求值同一批 token 得到新值，构造旧->新映射，一次扫描重写
    //       #RRGGBB、#AARRGGBB 与 rgb()/rgba() 三种写法；rgba 的 alpha 原样保留。
    //       同时改写控件自己设过的 QPalette：显式 setPalette 会固化 resolve 标记，
    //       之后 QApplication::setPalette 再也影响不到这些角色，表格的底色、文字色和
    //       选中色因此会停在构造时的主题。只动确实设过的 group/role，alpha 保持不变。
    // 返回：本次重映射的统计结果。
    // 同步兼容入口：保留值验证等现有消费者；主程序换肤使用下面的 Schedule 入口。
    ThemeColorRemapResult RemapStaleThemeColors(const ThemeColorSnapshot& previousSnapshot);

    // ScheduleStaleThemeColorRemap：主程序换肤使用的分片入口，必须在 GUI 线程调用。
    // 返回值只有映射/排队统计；真正扫描和写入结果在最新任务完成后由 completion 报告。
    // completionContext 限定回调寿命；控件通过 QPointer 跟踪，途中销毁安全跳过。
    // 新请求替代旧调度，但未处理的旧颜色映射链保留，连续 A→B→C 不遗漏 A 色。
    // 控件的局部 QSS/显式 palette 偏离排队基线后不改写该字段，保留业务接管。
    // 完整 TablePresentation 块单独保留当前值，只比较/改写其外的业务 QSS；
    // 这类表格的 Base/AlternateBase 由组件拥有，其余显式角色逐画刷保护并保留旧色转换链。
    // 单个 Qt 样式调用不能中断；其余工作按时间片排队，不调用 processEvents。
    ThemeColorRemapResult ScheduleStaleThemeColorRemap(
        const ThemeColorSnapshot& previousSnapshot,
        QObject* completionContext,
        std::function<void(const ThemeColorRemapResult&)> completion = {});

    // RemapStaleThemeColorsInText 作用：对单段样式文本执行同一套颜色重写。
    // 供构造期缓存了样式文本、又不在 QApplication::allWidgets() 范围内的调用方复用。
    QString RemapStaleThemeColorsInText(
        const ThemeColorSnapshot& previousSnapshot,
        const QString& styleText);

    // 纯值重映射接口：修复尚未返还控件的保存快照，不触发 setPalette/repolish。
    // previousSnapshot 是该值捕获时的主题；颜色 alpha、画刷类型和 palette 继承标记不变。
    QColor RemapStaleThemeColor(const ThemeColorSnapshot& previousSnapshot, const QColor& color);
    QPalette RemapStaleThemeColorsInPalette(
        const ThemeColorSnapshot& previousSnapshot,
        const QPalette& palette);
}
