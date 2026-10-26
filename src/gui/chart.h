#pragma once
// Custom QPainter line chart (Qt Charts / Qt Graphs are GPL-only). One y axis,
// thin 2 px lines, recessive grid, legend for >= 2 series, hover crosshair with a
// tooltip, wheel/drag zoom + pan, keyboard control (+/-/arrows/Home), shaded bands
// for events such as throttling, and horizontal reference lines.

#include "gt/stats.h"

#include <QPointF>
#include <QString>
#include <QVector>
#include <QWidget>

namespace gt::gui {

class LineChart : public QWidget {
    Q_OBJECT
public:
    struct Series {
        QString name;
        int colorSlot = 0;      ///< categorical slot (fixed per entity, never by rank)
        QVector<QPointF> points;
        bool visible = true;
        bool step = false;
    };
    struct Band {
        double x0, x1;
        QString label;
        int status = 1; ///< ThemeManager::Status
    };
    struct RefLine {
        double y;
        QString label;
    };

    explicit LineChart(QWidget* parent = nullptr);

    void setTitle(const QString& t);
    void setAxisLabels(const QString& x, const QString& y);
    void setUnit(const QString& yUnit) { m_yUnit = yUnit; }
    void setYStartsAtZero(bool z) { m_yZero = z; }
    void setFixedYRange(double lo, double hi);
    void setXInteger(bool on) { m_xInteger = on; update(); } ///< x values are counts (loops, runs)
    int addSeries(const QString& name, int colorSlot);
    void setPoints(int series, QVector<QPointF> pts);
    void appendPoint(int series, QPointF p, int maxPoints = 0);
    void clearData();
    void setBands(QVector<Band> bands);
    void setRefLines(QVector<RefLine> lines);
    void setFollowLatest(double windowWidth); ///< live mode: show the last `windowWidth` x units
    void resetZoom();
    int seriesCount() const { return static_cast<int>(m_series.size()); }
    void setEmptyText(const QString& t) { m_emptyText = t; update(); }
    QSize sizeHint() const override { return {520, 240}; }
    QSize minimumSizeHint() const override { return {240, 140}; }

    /// Tick positions at a 1/2/2.5/5 x 10^n step covering [lo, hi].
    static QVector<double> niceTicks(double lo, double hi, int target, double* step);
    static QString formatTick(double v, double step);

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void leaveEvent(QEvent*) override;

private:
    struct Range {
        double lo = 0, hi = 1;
    };
    void dataRange(Range& x, Range& y) const;
    QRectF plotRect() const;
    void zoomX(double factor, double anchor);
    void panX(double fraction);
    void updateAccessibleSummary();
    QString formatValue(double v) const;

    QString m_title, m_xLabel, m_yLabel, m_yUnit, m_emptyText;
    QVector<Series> m_series;
    QVector<Band> m_bands;
    QVector<RefLine> m_refs;
    bool m_yZero = true;
    bool m_xInteger = false;
    bool m_fixedY = false;
    Range m_fixedYRange;
    double m_follow = 0.0;
    bool m_zoomed = false;
    Range m_view; ///< x view range when zoomed
    QPointF m_hover{-1, -1};
    bool m_dragging = false;
    QPointF m_dragStart;
    Range m_dragView;
    QVector<QRectF> m_legendRects;
};

/// Histogram as bars anchored to the baseline (4 px rounded data end, 2 px surface
/// gap between bars). Several series share the bin layout and are drawn grouped
/// per bin; the y axis is the share of frames so runs of different length compare.
/// Hover highlights a bin and shows its range, count and share per series.
class HistogramChart : public QWidget {
    Q_OBJECT
public:
    struct Marker {
        double x;
        QString label;
    };
    explicit HistogramChart(QWidget* parent = nullptr);
    void setTitle(const QString& t);
    void setUnit(const QString& xUnit);
    void clear();
    /// All series must use the same lo / binWidth / bin count as the first one.
    int addSeries(const QString& name, int colorSlot, const gt::Histogram& h);
    void setMarkers(QVector<Marker> m);
    void setEmptyText(const QString& t) { m_emptyText = t; update(); }
    QSize sizeHint() const override { return {420, 240}; }
    QSize minimumSizeHint() const override { return {220, 140}; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent*) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    struct Series {
        QString name;
        int colorSlot;
        gt::Histogram hist;
    };
    QRectF plotRect() const;
    int binCount() const { return m_series.isEmpty() ? 0 : static_cast<int>(m_series.front().hist.counts.size()); }
    double share(const Series& s, int bin) const;
    void updateAccessibleSummary();
    QString m_title, m_unit, m_emptyText;
    QVector<Series> m_series;
    QVector<Marker> m_markers;
    int m_hoverBin = -1;
};

} // namespace gt::gui
