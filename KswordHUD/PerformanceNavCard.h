#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

class QPaintEvent;

class PerformanceNavCard final : public QWidget
{
public:
    explicit PerformanceNavCard(QWidget* parent = nullptr);

    void setTitleText(const QString& titleText);
    void setSubtitleText(const QString& subtitleText);
    void setAccentColor(const QColor& accentColor);
    void setSelectedState(bool selected);
    void appendSample(double usagePercent);
    void clearSamples();

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* paintEventPointer) override;

private:
    QString m_titleText;
    QString m_subtitleText;
    QColor m_accentColor;
    bool m_selected = false;
    QVector<double> m_samples;
    int m_maxSampleCount = 36;
};
