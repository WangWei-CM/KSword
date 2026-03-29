#pragma once

// ============================================================
// PerformanceNavCard.h
// 作用：
// 1) 提供“任务管理器风格”左侧性能导航卡片；
// 2) 卡片展示标题、副标题与缩略折线；
// 3) 支持选中高亮，供 HardwareDock 利用率页复用。
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

    // setSelectedState 作用：设置是否选中，影响背景高亮渲染。
    // 参数 selected：true=选中，false=未选中。
    void setSelectedState(bool selected);

    // appendSample 作用：追加一个百分比采样点到缩略折线。
    // 参数 usagePercent：范围 0~100 的利用率值。
    void appendSample(double usagePercent);

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
    bool m_selected = false;  // m_selected：当前卡片是否选中。
    QVector<double> m_samples; // m_samples：缩略折线历史采样列表。
    int m_maxSampleCount = 36; // m_maxSampleCount：折线最多保留的点数。
};
