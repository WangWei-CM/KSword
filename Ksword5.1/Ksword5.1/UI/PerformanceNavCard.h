#pragma once

// ============================================================
// PerformanceNavCard.h
// 作用：
// 1) 提供“任务管理器风格”左侧性能导航卡片；
// 2) 卡片展示标题、副标题与缩略折线；
// 3) 支持单折线/双折线两种缩略图模式，供 HardwareDock 利用率页复用。
// ============================================================

#include "../Framework.h"

#include <QColor>
#include <QVector>
#include <QWidget>

class QPaintEvent;

class PerformanceNavCard final : public QWidget
{
public:
    // 构造函数作用：初始化默认颜色、采样容量和控件属性。
    // 参数 parent：Qt 父控件指针。
    explicit PerformanceNavCard(QWidget* parent = nullptr);

    // setTitleText 作用：设置卡片主标题文本（如 CPU/内存/磁盘）。
    // 参数 titleText：标题字符串。
    void setTitleText(const QString& titleText);

    // setSubtitleText 作用：设置卡片副标题文本（如“51% 2.88 GHz”）。
    // 参数 subtitleText：副标题字符串。
    void setSubtitleText(const QString& subtitleText);

    // setAccentColor 作用：设置卡片边框与折线主色。
    // 参数 accentColor：主色值。
    void setAccentColor(const QColor& accentColor);

    // setSeriesColors 作用：设置缩略图主/次两条曲线的颜色。
    // 参数 primarySeriesColor：主序列颜色；无效时回退到 accentColor。
    // 参数 secondarySeriesColor：次序列颜色；无效时隐藏第二条曲线。
    void setSeriesColors(
        const QColor& primarySeriesColor,
        const QColor& secondarySeriesColor = QColor());

    // setSelectedState 作用：设置是否选中，影响背景高亮渲染。
    // 参数 selected：true=选中，false=未选中。
    void setSelectedState(bool selected);

    // appendSample 作用：追加一个百分比采样点到缩略折线。
    // 参数 usagePercent：范围 0~100 的利用率值。
    void appendSample(double usagePercent);

    // appendDualSample 作用：同时追加两条缩略线采样点。
    // 参数 primaryUsagePercent：主序列百分比采样值。
    // 参数 secondaryUsagePercent：次序列百分比采样值。
    void appendDualSample(double primaryUsagePercent, double secondaryUsagePercent);

    // clearSamples 作用：清空缩略折线历史数据。
    void clearSamples();

    // sizeHint 作用：给 QListWidgetItem 提供推荐尺寸。
    [[nodiscard]] QSize sizeHint() const override;

protected:
    // paintEvent 作用：绘制卡片背景、边框、文字和缩略折线。
    // 参数 paintEventPointer：Qt 绘制事件对象。
    void paintEvent(QPaintEvent* paintEventPointer) override;

private:
    QString m_titleText;      // m_titleText：卡片主标题文本。
    QString m_subtitleText;   // m_subtitleText：卡片副标题文本。
    QColor m_accentColor;     // m_accentColor：折线与边框主色。
    QColor m_primarySeriesColor; // m_primarySeriesColor：主缩略线颜色。
    QColor m_secondarySeriesColor; // m_secondarySeriesColor：次缩略线颜色。
    bool m_selected = false;  // m_selected：当前卡片是否选中。
    bool m_primarySeriesFollowsAccentColor = true; // m_primarySeriesFollowsAccentColor：主缩略线是否跟随边框主色。
    bool m_secondarySeriesVisible = false; // m_secondarySeriesVisible：是否绘制第二条缩略线。
    QVector<double> m_primarySamples; // m_primarySamples：主缩略线历史采样列表。
    QVector<double> m_secondarySamples; // m_secondarySamples：次缩略线历史采样列表。
    int m_maxSampleCount = 36; // m_maxSampleCount：折线最多保留的点数。
};
