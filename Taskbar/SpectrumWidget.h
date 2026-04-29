#pragma once
#ifndef SPECTRUMWIDGET_H
#define SPECTRUMWIDGET_H

#include <QWidget>
#include <QVector>
#include <QElapsedTimer>

class SpectrumWidget : public QWidget
{
    Q_OBJECT

public:
    enum Direction {
        LeftToRight,    // 从左到右（正常）
        RightToLeft,    // 从右到左（镜像）
        CenterToLeft,   // 从中心向左
        CenterToRight   // 从中心向右
    };

    explicit SpectrumWidget(Direction direction = LeftToRight, QWidget* parent = nullptr);
    ~SpectrumWidget();

    void setSpectrumData(const QVector<float>& data);

    // 设置条形的最大高度比例（0.0-1.0）
    void setMaxHeightRatio(float ratio) { m_maxHeightRatio = qBound(0.1f, ratio, 1.0f); }

    // 设置绘制方向
    void setDirection(Direction direction) {
        m_direction = direction;
        update();
    }

protected: 
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void setupUI();

    // 频谱条相关
    static constexpr int NUM_BARS = 16;
    QVector<float> m_spectrumData;

    // 绘制方向
    Direction m_direction;

    // 样式配置
    int m_barWidth = 12;
    int m_barSpacing = 3;
    float m_maxHeightRatio = 0.8f; // 最大高度占可用区域的80%

    // 绘制区域计算
    int m_drawAreaWidth = 0;
    int m_drawAreaHeight = 0;
    int m_drawAreaX = 0;
    int m_drawAreaY = 0;

    // 颜色配置
    QColor m_barColor = QColor(67, 160, 255); // #43A0FF

    // 性能优化
    QElapsedTimer m_paintTimer;
    int m_minPaintInterval = 16; // 约60fps，避免过度绘制
};

#endif // SPECTRUMWIDGET_H