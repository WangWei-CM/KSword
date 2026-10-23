#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

class QPaintEvent;
class QHideEvent;
class QShowEvent;
class QVariantAnimation;

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
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void startLatestSampleAnimation(double previousSample);
    double animatedXRatio(int sampleIndex, int sampleCount) const;
    QString m_titleText;
    QString m_subtitleText;
    QColor m_accentColor;
    bool m_selected = false;
    QVector<double> m_samples;
    int m_maxSampleCount = 36;
    int m_previousSampleCount = 0;
    bool m_historyWindowShifted = false;
    QVariantAnimation* m_sampleAnimation = nullptr;
    double m_previousSample = 0.0;
    double m_animationProgress = 1.0;
};
