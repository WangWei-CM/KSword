#pragma once

// KsPainterChart is a small, source-compatible subset of the chart API used by
// KSword. It is implemented entirely with QWidget + QPainter.

#include <QAbstractScrollArea>
#include <QBrush>
#include <QEasingCurve>
#include <QFont>
#include <QFrame>
#include <QHash>
#include <QList>
#include <QMargins>
#include <QObject>
#include <QPainter>
#include <QPair>
#include <QPen>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class QPaintEvent;
class QShowEvent;
class QHideEvent;
class QVariantAnimation;

class KsPainterChartObject : public QObject
{
public:
    explicit KsPainterChartObject(QObject* parent = nullptr);

    void setChangeHandler(std::function<void()> changeHandler);

protected:
    void notifyChanged();

private:
    std::function<void()> m_changeHandler;
};

class QAbstractAxis : public KsPainterChartObject
{
public:
    explicit QAbstractAxis(QObject* parent = nullptr);

    void setLabelsVisible(bool visible);
    bool labelsVisible() const;
    void setGridLineVisible(bool visible);
    bool isGridLineVisible() const;
    void setMinorGridLineVisible(bool visible);
    bool isMinorGridLineVisible() const;
    void setLineVisible(bool visible);
    bool isLineVisible() const;

    void setLabelsBrush(const QBrush& brush);
    QBrush labelsBrush() const;
    void setTitleBrush(const QBrush& brush);
    QBrush titleBrush() const;
    void setLinePenColor(const QColor& color);
    void setGridLineColor(const QColor& color);
    void setLinePen(const QPen& pen);
    QPen linePen() const;
    void setGridLinePen(const QPen& pen);
    QPen gridLinePen() const;

    void setTitleText(const QString& titleText);
    QString titleText() const;
    void setLabelFormat(const QString& labelFormat);
    QString labelFormat() const;

    Qt::Alignment alignment() const;

private:
    friend class QChart;
    void setAlignment(Qt::Alignment alignment);

    bool m_labelsVisible = true;
    bool m_gridLineVisible = true;
    bool m_minorGridLineVisible = false;
    bool m_lineVisible = true;
    QBrush m_labelsBrush;
    QBrush m_titleBrush;
    QPen m_linePen = QPen(QColor(128, 128, 128, 150), 1.0);
    QPen m_gridLinePen = QPen(QColor(128, 128, 128, 55), 1.0);
    QString m_titleText;
    QString m_labelFormat;
    Qt::Alignment m_alignment = Qt::AlignBottom;
};

class QValueAxis : public QAbstractAxis
{
public:
    explicit QValueAxis(QObject* parent = nullptr);

    void setRange(qreal minimum, qreal maximum);
    qreal min() const;
    qreal max() const;

private:
    qreal m_minimum = 0.0;
    qreal m_maximum = 1.0;
};

class QBarCategoryAxis : public QAbstractAxis
{
public:
    explicit QBarCategoryAxis(QObject* parent = nullptr);

    void append(const QString& category);
    void append(const QStringList& categories);
    QStringList categories() const;

private:
    QStringList m_categories;
};

class QAbstractSeries : public KsPainterChartObject
{
public:
    explicit QAbstractSeries(QObject* parent = nullptr);

    void setName(const QString& name);
    QString name() const;
    bool attachAxis(QAbstractAxis* axis);
    QList<QAbstractAxis*> attachedAxes() const;

private:
    QString m_name;
    QList<QAbstractAxis*> m_attachedAxes;
};

class QLineSeries : public QAbstractSeries
{
public:
    explicit QLineSeries(QObject* parent = nullptr);

    void append(qreal x, qreal y);
    void append(const QPointF& point);
    bool remove(int index);
    void replace(const QList<QPointF>& points);
    int count() const;
    QList<QPointF> points() const;

    void setColor(const QColor& color);
    QColor color() const;
    void setPen(const QPen& pen);
    QPen pen() const;

private:
    QList<QPointF> m_points;
    QPen m_pen = QPen(QColor(52, 152, 219), 1.6);
};

class QAreaSeries : public QAbstractSeries
{
public:
    explicit QAreaSeries(
        QLineSeries* upperSeries,
        QLineSeries* lowerSeries = nullptr,
        QObject* parent = nullptr);

    QLineSeries* upperSeries() const;
    QLineSeries* lowerSeries() const;

    void setColor(const QColor& color);
    QColor color() const;
    void setBorderColor(const QColor& color);
    QColor borderColor() const;
    void setPen(const QPen& pen);
    QPen pen() const;
    void setBrush(const QBrush& brush);
    QBrush brush() const;

private:
    QLineSeries* m_upperSeries = nullptr;
    QLineSeries* m_lowerSeries = nullptr;
    QBrush m_brush = QBrush(QColor(52, 152, 219, 48));
    QPen m_pen = QPen(QColor(52, 152, 219), 1.6);
};

class QBarSet : public KsPainterChartObject
{
public:
    explicit QBarSet(const QString& label, QObject* parent = nullptr);

    QBarSet& operator<<(qreal value);
    void append(qreal value);
    void replace(int index, qreal value);
    int count() const;
    QVector<qreal> values() const;
    QString label() const;

    void setColor(const QColor& color);
    QColor color() const;
    void setBorderColor(const QColor& color);
    QColor borderColor() const;
    void setBrush(const QBrush& brush);
    QBrush brush() const;
    void setLabelBrush(const QBrush& brush);
    QBrush labelBrush() const;

private:
    QString m_label;
    QVector<qreal> m_values;
    QBrush m_brush = QBrush(QColor(52, 152, 219));
    QColor m_borderColor = Qt::transparent;
    QBrush m_labelBrush;
};

class QBarSeries : public QAbstractSeries
{
public:
    explicit QBarSeries(QObject* parent = nullptr);

    bool append(QBarSet* set);
    bool append(const QList<QBarSet*>& sets);
    QList<QBarSet*> barSets() const;

private:
    void attachSetHandler(QBarSet* set);

    QList<QBarSet*> m_sets;
};

class QLegend : public KsPainterChartObject
{
public:
    explicit QLegend(QObject* parent = nullptr);

    void hide();
    void setVisible(bool visible);
    bool isVisible() const;
    void setAlignment(Qt::Alignment alignment);
    Qt::Alignment alignment() const;
    void setLabelColor(const QColor& color);
    QColor labelColor() const;
    void setLabelBrush(const QBrush& brush);
    QBrush labelBrush() const;
    void setFont(const QFont& font);
    QFont font() const;

private:
    bool m_visible = true;
    Qt::Alignment m_alignment = Qt::AlignTop;
    QBrush m_labelBrush;
    QFont m_font;
};

class QChart : public KsPainterChartObject
{
public:
    enum AnimationOption
    {
        NoAnimation = 0x0,
        GridAxisAnimations = 0x1,
        SeriesAnimations = 0x2,
        AllAnimations = GridAxisAnimations | SeriesAnimations
    };

    explicit QChart(QObject* parent = nullptr);

    void addSeries(QAbstractSeries* series);
    QList<QAbstractSeries*> series() const;
    void addAxis(QAbstractAxis* axis, Qt::Alignment alignment);
    QList<QAbstractAxis*> axes() const;
    QLegend* legend() const;

    void setTitle(const QString& title);
    QString title() const;
    void setTitleBrush(const QBrush& brush);
    QBrush titleBrush() const;
    void setTitleFont(const QFont& font);
    QFont titleFont() const;

    void setBackgroundVisible(bool visible);
    bool isBackgroundVisible() const;
    void setBackgroundRoundness(qreal roundness);
    qreal backgroundRoundness() const;
    void setBackgroundBrush(const QBrush& brush);
    QBrush backgroundBrush() const;
    void setMargins(const QMargins& margins);
    QMargins margins() const;

    void setPlotAreaBackgroundVisible(bool visible);
    bool isPlotAreaBackgroundVisible() const;
    void setPlotAreaBackgroundBrush(const QBrush& brush);
    QBrush plotAreaBackgroundBrush() const;
    void setPlotAreaBackgroundPen(const QPen& pen);
    QPen plotAreaBackgroundPen() const;

    void setAnimationOptions(AnimationOption options);
    AnimationOption animationOptions() const;
    void setAnimationDuration(int durationMs);
    int animationDuration() const;
    void setAnimationEasingCurve(const QEasingCurve& easingCurve);
    QEasingCurve animationEasingCurve() const;

    void update();

private:
    void attachSeriesHandlers(QAbstractSeries* series);

    QList<QAbstractSeries*> m_series;
    QList<QAbstractAxis*> m_axes;
    QLegend* m_legend = nullptr;
    QString m_title;
    QBrush m_titleBrush;
    QFont m_titleFont;
    bool m_backgroundVisible = true;
    qreal m_backgroundRoundness = 0.0;
    QBrush m_backgroundBrush;
    QMargins m_margins;
    bool m_plotAreaBackgroundVisible = false;
    QBrush m_plotAreaBackgroundBrush;
    QPen m_plotAreaBackgroundPen = QPen(Qt::NoPen);
    AnimationOption m_animationOptions = NoAnimation;
    int m_animationDurationMs = 250;
    QEasingCurve m_animationEasingCurve = QEasingCurve(QEasingCurve::OutCubic);
};

class QChartView : public QFrame
{
public:
    explicit QChartView(QChart* chart, QWidget* parent = nullptr);
    ~QChartView() override;

    QChart* chart() const;
    // After a host restores hidden series/axes, present one consistent current
    // model without replaying the hidden interval or an earlier queued update.
    // Hidden views retain only dirty state and defer the snapshot until Show.
    void presentLatestWithoutAnimation();
    QWidget* viewport();
    const QWidget* viewport() const;

    void setRenderHint(QPainter::RenderHint hint, bool enabled = true);
    void setHorizontalScrollBarPolicy(Qt::ScrollBarPolicy policy);
    void setVerticalScrollBarPolicy(Qt::ScrollBarPolicy policy);
    void setSizeAdjustPolicy(QAbstractScrollArea::SizeAdjustPolicy policy);
    void setBackgroundBrush(const QBrush& brush);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    using LinePointMap = QHash<const QLineSeries*, QList<QPointF>>;
    using BarValueMap = QHash<const QBarSet*, QVector<qreal>>;
    using AxisRangeMap = QHash<const QValueAxis*, QPair<qreal, qreal>>;

    bool isPresentationVisible() const;
    void scheduleModelUpdate();
    void applyPendingModelUpdate();
    void startModelAnimation();
    void syncSnapshotsToCurrent();
    void captureCurrentFrameAsDisplayed();
    LinePointMap currentLinePoints() const;
    BarValueMap currentBarValues() const;
    AxisRangeMap currentAxisRanges() const;
    QList<QPointF> renderedPoints(const QLineSeries* series) const;
    QVector<qreal> renderedBarValues(const QBarSet* set) const;
    QPair<qreal, qreal> renderedAxisRange(const QValueAxis* axis) const;

    QChart* m_chart = nullptr;
    QVariantAnimation* m_animation = nullptr;
    bool m_updateScheduled = false;
    bool m_modelDirty = true;
    bool m_presentationExposed = false;
    bool m_antialiasingEnabled = true;
    qreal m_animationProgress = 1.0;
    QBrush m_viewBackgroundBrush = QBrush(Qt::NoBrush);

    LinePointMap m_displayedLinePoints;
    LinePointMap m_fromLinePoints;
    LinePointMap m_toLinePoints;
    BarValueMap m_displayedBarValues;
    BarValueMap m_fromBarValues;
    BarValueMap m_toBarValues;
    AxisRangeMap m_displayedAxisRanges;
    AxisRangeMap m_fromAxisRanges;
    AxisRangeMap m_toAxisRanges;
};
