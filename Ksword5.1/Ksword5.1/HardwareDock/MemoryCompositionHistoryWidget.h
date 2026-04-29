#pragma once

#include <QWidget>

#include <vector>

// MemoryCompositionHistoryWidget 作用：
// - 绘制内存占用历史折线；
// - 在折线下方按采样时刻的内存构成比例进行分层填充。
class MemoryCompositionHistoryWidget final : public QWidget
{
public:
    // CompositionSample 作用：保存单次内存采样的百分比构成。
    // 调用方式：appendSample 接收该结构并触发重绘。
    struct CompositionSample
    {
        double usedPercent = 0.0;          // usedPercent：总物理内存占用百分比。
        double activePercent = 0.0;        // activePercent：扣除缓存/池后的近似活跃占用百分比。
        double cachedPercent = 0.0;        // cachedPercent：系统缓存占物理内存百分比。
        double pagedPoolPercent = 0.0;     // pagedPoolPercent：分页池占物理内存百分比。
        double nonPagedPoolPercent = 0.0;  // nonPagedPoolPercent：非分页池占物理内存百分比。
    };

    // 构造函数作用：初始化自绘控件最小高度与绘制属性。
    explicit MemoryCompositionHistoryWidget(QWidget* parent = nullptr);

    // setHistoryLength 作用：设置历史保留点数，超过后自动丢弃最旧采样。
    void setHistoryLength(int historyLength);

    // appendSample 作用：追加一个时刻的内存构成采样并刷新图表。
    void appendSample(const CompositionSample& sample);

    // clearSamples 作用：清空历史采样，供未来重置采样窗口使用。
    void clearSamples();

protected:
    // paintEvent 作用：绘制网格、构成填充、占用折线与图例。
    void paintEvent(QPaintEvent* paintEventPointer) override;

private:
    // boundedPercent 作用：把百分比限制到 0~100，避免异常 API 数据撑破图表。
    static double boundedPercent(double percentValue);

    // sampleX 作用：把历史采样索引映射到绘图区 X 坐标。
    double sampleX(int sampleIndex, const QRectF& plotRect) const;

    // percentY 作用：把百分比映射到绘图区 Y 坐标。
    static double percentY(double percentValue, const QRectF& plotRect);

    // drawStackedComposition 作用：绘制折线下方的内存构成分层填充。
    void drawStackedComposition(QPainter& painter, const QRectF& plotRect) const;

    // drawUsageLine 作用：在构成填充上方绘制总占用折线。
    void drawUsageLine(QPainter& painter, const QRectF& plotRect) const;

    // drawLegend 作用：绘制颜色图例，说明填充区域代表的内存构成。
    void drawLegend(QPainter& painter, const QRectF& plotRect) const;

    int m_historyLength = 60; // m_historyLength：最多保留的历史采样点数量。
    std::vector<CompositionSample> m_sampleList; // m_sampleList：按时间顺序保存的内存构成历史。
};
