#pragma once
// Custom QPainter line chart (Qt Charts / Qt Graphs are GPL-only). One y axis,
// thin 2 px lines, recessive grid, legend for >= 2 series, hover crosshair with a
// tooltip, wheel/drag zoom + pan, keyboard control (+/-/arrows/Home), shaded bands
// for events such as throttling, and horizontal reference lines.

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
    static QVector<double> niceTicks(double lo, double hi, int target, double* step);
    QString formatValue(double v) const;
    static QString formatTick(double v, double step);

    QString m_title, m_xLabel, m_yLabel, m_yUnit, m_emptyText;
    QVector<Series> m_series;
    QVector<Band> m_bands;
    QVector<RefLine> m_refs;
    bool m_yZero = true;
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

} // namespace gt::gui
