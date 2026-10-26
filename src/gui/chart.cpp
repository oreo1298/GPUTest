#include "chart.h"

#include "theme.h"

#include <QAccessible>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace gt::gui {

LineChart::LineChart(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(140);
    m_emptyText = tr("No data yet");
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
}

void LineChart::setTitle(const QString& t) {
    m_title = t;
    setAccessibleName(t);
    update();
}

void LineChart::setAxisLabels(const QString& x, const QString& y) {
    m_xLabel = x;
    m_yLabel = y;
    update();
}

void LineChart::setFixedYRange(double lo, double hi) {
    m_fixedY = true;
    m_fixedYRange = {lo, hi};
    update();
}

int LineChart::addSeries(const QString& name, int colorSlot) {
    m_series.push_back({name, colorSlot, {}, true, false});
    update();
    return static_cast<int>(m_series.size()) - 1;
}

void LineChart::setPoints(int s, QVector<QPointF> pts) {
    if (s < 0 || s >= m_series.size()) return;
    m_series[s].points = std::move(pts);
    updateAccessibleSummary();
    update();
}

void LineChart::appendPoint(int s, QPointF p, int maxPoints) {
    if (s < 0 || s >= m_series.size()) return;
    auto& pts = m_series[s].points;
    pts.push_back(p);
    if (maxPoints > 0 && pts.size() > maxPoints) pts.remove(0, pts.size() - maxPoints);
    update();
}

void LineChart::clearData() {
    for (auto& s : m_series) s.points.clear();
    m_bands.clear();
    m_refs.clear();
    resetZoom();
}

void LineChart::setBands(QVector<Band> b) {
    m_bands = std::move(b);
    update();
}

void LineChart::setRefLines(QVector<RefLine> l) {
    m_refs = std::move(l);
    update();
}

void LineChart::setFollowLatest(double w) {
    m_follow = w;
    update();
}

void LineChart::resetZoom() {
    m_zoomed = false;
    update();
}

void LineChart::dataRange(Range& x, Range& y) const {
    double xlo = std::numeric_limits<double>::max(), xhi = std::numeric_limits<double>::lowest();
    double ylo = xlo, yhi = xhi;
    bool any = false;
    for (const auto& s : m_series) {
        if (!s.visible) continue;
        for (const QPointF& p : s.points) {
            if (!std::isfinite(p.y())) continue;
            xlo = std::min(xlo, p.x());
            xhi = std::max(xhi, p.x());
            any = true;
        }
    }
    if (!any) {
        x = {0, 1};
        y = {0, 1};
        return;
    }
    if (m_follow > 0 && xhi - xlo > m_follow) xlo = xhi - m_follow;
    x = {xlo, xhi > xlo ? xhi : xlo + 1};
    if (m_zoomed) x = m_view;
    for (const auto& s : m_series) {
        if (!s.visible) continue;
        for (const QPointF& p : s.points) {
            if (!std::isfinite(p.y()) || p.x() < x.lo || p.x() > x.hi) continue;
            ylo = std::min(ylo, p.y());
            yhi = std::max(yhi, p.y());
        }
    }
    for (const auto& r : m_refs) {
        ylo = std::min(ylo, r.y);
        yhi = std::max(yhi, r.y);
    }
    if (ylo > yhi) ylo = yhi = 0;
    if (m_yZero) ylo = std::min(0.0, ylo);
    if (yhi - ylo < 1e-9) yhi = ylo + (std::abs(ylo) > 0 ? std::abs(ylo) * 0.1 : 1.0);
    y = {ylo, yhi + (yhi - ylo) * 0.06};
    if (m_fixedY) y = m_fixedYRange;
}

QVector<double> LineChart::niceTicks(double lo, double hi, int target, double* stepOut) {
    QVector<double> ticks;
    double step = 1;
    // Ask for more ticks until at least two land inside the range.
    for (int attempt = 0, t = std::max(1, target); attempt < 4; ++attempt, t *= 2) {
        const double raw = (hi - lo) / t;
        const double mag = std::pow(10.0, std::floor(std::log10(raw)));
        for (double m : {1.0, 2.0, 2.5, 5.0, 10.0}) {
            step = m * mag;
            if (step >= raw) break;
        }
        ticks.clear();
        for (double v = std::ceil(lo / step) * step; v <= hi + step * 1e-6; v += step)
            ticks.push_back(std::abs(v) < step * 1e-9 ? 0.0 : v);
        if (ticks.size() >= 2) break;
    }
    if (stepOut) *stepOut = step;
    return ticks;
}

QString LineChart::formatValue(double v) const {
    const double a = std::abs(v);
    QString s;
    if (a >= 100000) s = QString::number(v, 'f', 0);
    else if (a >= 100) s = QString::number(v, 'f', 0);
    else if (a >= 10) s = QString::number(v, 'f', 1);
    else s = QString::number(v, 'f', 2);
    return s;
}

QString LineChart::formatTick(double v, double step) {
    if (std::abs(v) >= 10000 && step >= 1000) return QString::number(v / 1000.0, 'f', step >= 10000 ? 0 : 1) + QStringLiteral("k");
    // Enough decimals to show the step exactly (2.5 -> 1, 0.25 -> 2).
    int decimals = 0;
    while (decimals < 4 && std::abs(step * std::pow(10.0, decimals) - std::round(step * std::pow(10.0, decimals))) > 1e-6)
        ++decimals;
    return QString::number(v, 'f', decimals);
}

QRectF LineChart::plotRect() const {
    const QFontMetrics fm(font());
    const double top = (m_title.isEmpty() ? 6 : fm.height() + 10) + (m_series.size() >= 2 ? fm.height() + 6 : 0);
    const double left = fm.horizontalAdvance(QStringLiteral("00000.0")) + 12;
    const double bottom = fm.height() * (m_xLabel.isEmpty() ? 1.6 : 2.8);
    return QRectF(left, top, std::max(10.0, width() - left - 12), std::max(10.0, height() - top - bottom));
}

void LineChart::paintEvent(QPaintEvent*) {
    auto& tm = ThemeManager::instance();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QFontMetrics fm(font());
    p.fillRect(rect(), tm.surface());

    // Title + legend
    double y0 = 4;
    if (!m_title.isEmpty()) {
        QFont tf = font();
        tf.setWeight(QFont::DemiBold);
        p.setFont(tf);
        p.setPen(tm.textPrimary());
        p.drawText(QRectF(8, y0, width() - 16, fm.height() + 4), Qt::AlignLeft | Qt::AlignVCenter, m_title);
        p.setFont(font());
        y0 += fm.height() + 6;
    }
    m_legendRects.clear();
    if (m_series.size() >= 2) {
        double x = 8;
        for (const auto& s : m_series) {
            const double w = 14 + 6 + fm.horizontalAdvance(s.name) + 14;
            const QRectF lr(x, y0, w, fm.height());
            m_legendRects.push_back(lr);
            QColor c = tm.series(s.colorSlot);
            if (!s.visible) c.setAlphaF(0.25f);
            p.setPen(QPen(c, 3, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(x, lr.center().y()), QPointF(x + 14, lr.center().y()));
            p.setPen(s.visible ? tm.textSecondary() : tm.textMuted());
            p.drawText(QRectF(x + 20, y0, w - 20, fm.height()), Qt::AlignVCenter | Qt::AlignLeft, s.name);
            x += w;
        }
    }

    const QRectF pr = plotRect();
    Range xr, yr;
    dataRange(xr, yr);
    bool empty = true;
    for (const auto& s : m_series) empty = empty && s.points.isEmpty();
    auto mapX = [&](double x) { return pr.left() + (x - xr.lo) / (xr.hi - xr.lo) * pr.width(); };
    auto mapY = [&](double y) { return pr.bottom() - (y - yr.lo) / (yr.hi - yr.lo) * pr.height(); };

    if (empty) {
        p.setPen(QPen(tm.axisLine(), 1));
        p.drawLine(QPointF(pr.left(), pr.bottom()), QPointF(pr.right(), pr.bottom()));
        p.setPen(tm.textMuted());
        p.drawText(pr, Qt::AlignCenter | Qt::TextWordWrap, m_emptyText);
        return;
    }

    // Grid + y ticks
    double ystep = 1;
    const auto yt = niceTicks(yr.lo, yr.hi, std::max(2, static_cast<int>(pr.height() / 42)), &ystep);
    p.setPen(QPen(tm.gridLine(), 1));
    for (double v : yt) {
        const double y = mapY(v);
        if (y < pr.top() - 0.5 || y > pr.bottom() + 0.5) continue;
        p.drawLine(QPointF(pr.left(), y), QPointF(pr.right(), y));
    }
    p.setPen(tm.textMuted());
    for (double v : yt) {
        const double y = mapY(v);
        if (y < pr.top() - 0.5 || y > pr.bottom() + 0.5) continue;
        p.drawText(QRectF(0, y - fm.height() / 2.0, pr.left() - 6, fm.height()), Qt::AlignRight | Qt::AlignVCenter,
                   formatTick(v, ystep));
    }
    // x ticks
    double xstep = 1;
    auto xt = niceTicks(xr.lo, xr.hi, std::max(2, static_cast<int>(pr.width() / 90)), &xstep);
    if (m_xInteger && xstep < 1) {
        xt.clear();
        xstep = 1;
        for (double v = std::ceil(xr.lo); v <= xr.hi + 1e-9; v += 1) xt.push_back(v);
    }
    for (double v : xt) {
        const double x = mapX(v);
        if (x < pr.left() - 0.5 || x > pr.right() + 0.5) continue;
        p.drawText(QRectF(x - 40, pr.bottom() + 4, 80, fm.height()), Qt::AlignHCenter | Qt::AlignTop, formatTick(v, xstep));
    }
    if (!m_xLabel.isEmpty())
        p.drawText(QRectF(pr.left(), pr.bottom() + fm.height() + 6, pr.width(), fm.height()), Qt::AlignHCenter,
                   m_xLabel);
    if (!m_yLabel.isEmpty()) {
        p.save();
        p.translate(10, pr.center().y());
        p.rotate(-90);
        p.restore();
    }
    // Baseline
    p.setPen(QPen(tm.axisLine(), 1));
    p.drawLine(QPointF(pr.left(), pr.bottom()), QPointF(pr.right(), pr.bottom()));

    p.save();
    p.setClipRect(pr.adjusted(-1, -2, 1, 2));
    // Event bands (e.g. throttling) behind the data.
    for (const auto& b : m_bands) {
        QColor c = tm.status(static_cast<ThemeManager::Status>(b.status));
        c.setAlphaF(tm.isDark() ? 0.20f : 0.14f);
        const double x0 = std::max(pr.left(), mapX(b.x0)), x1 = std::min(pr.right(), mapX(b.x1));
        if (x1 > x0) p.fillRect(QRectF(x0, pr.top(), std::max(1.5, x1 - x0), pr.height()), c);
    }
    // Reference lines
    for (const auto& r : m_refs) {
        const double y = mapY(r.y);
        p.setPen(QPen(tm.textMuted(), 1, Qt::DashLine));
        p.drawLine(QPointF(pr.left(), y), QPointF(pr.right(), y));
        p.setPen(tm.textSecondary());
        p.drawText(QRectF(pr.left() + 4, y - fm.height() - 1, pr.width() - 8, fm.height()), Qt::AlignRight | Qt::AlignBottom,
                   r.label);
    }
    // Series (decimate to ~2 points per pixel column: min + max).
    for (const auto& s : m_series) {
        if (!s.visible || s.points.isEmpty()) continue;
        QPainterPath path;
        bool started = false;
        const int columns = std::max(1, static_cast<int>(pr.width()));
        const auto& pts = s.points;
        if (pts.size() > columns * 3) {
            int col = -1;
            double mn = 0, mx = 0, firstX = 0;
            auto flush = [&] {
                if (col < 0) return;
                const double x = mapX(firstX);
                if (!started) {
                    path.moveTo(x, mapY(mn));
                    started = true;
                } else {
                    path.lineTo(x, mapY(mn));
                }
                path.lineTo(x, mapY(mx));
            };
            for (const QPointF& pt : pts) {
                if (pt.x() < xr.lo || pt.x() > xr.hi || !std::isfinite(pt.y())) continue;
                const int c = static_cast<int>((pt.x() - xr.lo) / (xr.hi - xr.lo) * columns);
                if (c != col) {
                    flush();
                    col = c;
                    mn = mx = pt.y();
                    firstX = pt.x();
                } else {
                    mn = std::min(mn, pt.y());
                    mx = std::max(mx, pt.y());
                }
            }
            flush();
        } else {
            double prevY = 0;
            for (const QPointF& pt : pts) {
                if (!std::isfinite(pt.y())) {
                    started = false;
                    continue;
                }
                const QPointF m(mapX(pt.x()), mapY(pt.y()));
                if (!started) {
                    path.moveTo(m);
                    started = true;
                } else {
                    if (s.step) path.lineTo(m.x(), prevY);
                    path.lineTo(m);
                }
                prevY = m.y();
            }
        }
        p.setPen(QPen(tm.series(s.colorSlot), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        // Few points far apart: mark each one (8 px dot with a 2 px surface ring).
        if (pts.size() <= 40 && pr.width() / static_cast<double>(std::max<qsizetype>(1, pts.size())) >= 12) {
            p.setPen(QPen(tm.surface(), 2));
            p.setBrush(tm.series(s.colorSlot));
            for (const QPointF& pt : pts)
                if (std::isfinite(pt.y()) && pt.x() >= xr.lo && pt.x() <= xr.hi) p.drawEllipse(QPointF(mapX(pt.x()), mapY(pt.y())), 4, 4);
        }
    }
    p.restore();

    // Hover crosshair + tooltip
    if (pr.contains(m_hover)) {
        const double hx = xr.lo + (m_hover.x() - pr.left()) / pr.width() * (xr.hi - xr.lo);
        p.setPen(QPen(tm.textMuted(), 1));
        p.drawLine(QPointF(m_hover.x(), pr.top()), QPointF(m_hover.x(), pr.bottom()));
        QStringList lines;
        lines << (m_xLabel.isEmpty() ? QString() : m_xLabel + QStringLiteral(": ")) +
                     (m_xInteger ? QString::number(std::llround(hx)) : formatValue(hx));
        QVector<QColor> colors{QColor()};
        for (const auto& s : m_series) {
            if (!s.visible || s.points.isEmpty()) continue;
            auto it = std::lower_bound(s.points.begin(), s.points.end(), hx,
                                       [](const QPointF& a, double v) { return a.x() < v; });
            if (it == s.points.end()) it = s.points.end() - 1;
            if (it != s.points.begin() && std::abs((it - 1)->x() - hx) < std::abs(it->x() - hx)) --it;
            if (!std::isfinite(it->y())) continue;
            const QPointF m(mapX(it->x()), mapY(it->y()));
            p.setPen(QPen(tm.surface(), 2));
            p.setBrush(tm.series(s.colorSlot));
            p.drawEllipse(m, 4.5, 4.5);
            lines << QStringLiteral("%1: %2 %3").arg(s.name, formatValue(it->y()), m_yUnit);
            colors << tm.series(s.colorSlot);
        }
        for (const auto& b : m_bands)
            if (hx >= b.x0 && hx <= b.x1 && !b.label.isEmpty()) {
                lines << QStringLiteral("⚠ ") + b.label;
                colors << QColor();
            }
        int w = 0;
        for (const auto& l : lines) w = std::max(w, fm.horizontalAdvance(l));
        const double bw = w + 30, bh = static_cast<double>(lines.size()) * (fm.height() + 2) + 10;
        double bx = m_hover.x() + 14;
        if (bx + bw > width() - 4) bx = m_hover.x() - 14 - bw;
        const double by = std::clamp(m_hover.y() - bh / 2, 2.0, height() - bh - 2);
        p.setPen(QPen(tm.border(), 1));
        p.setBrush(palette().color(QPalette::ToolTipBase));
        p.drawRoundedRect(QRectF(bx, by, bw, bh), 6, 6);
        for (int i = 0; i < lines.size(); ++i) {
            const double ly = by + 5 + i * (fm.height() + 2);
            if (colors[i].isValid()) {
                p.setPen(Qt::NoPen);
                p.setBrush(colors[i]);
                p.drawEllipse(QPointF(bx + 12, ly + fm.height() / 2.0), 4, 4);
            }
            p.setPen(i == 0 ? tm.textSecondary() : tm.textPrimary());
            p.drawText(QRectF(bx + 22, ly, w + 4, fm.height()), Qt::AlignLeft | Qt::AlignVCenter, lines[i]);
        }
    }
    if (hasFocus()) {
        p.setPen(QPen(tm.accent(), 1.5, Qt::DotLine));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 4, 4);
    }
}

void LineChart::mouseMoveEvent(QMouseEvent* e) {
    m_hover = e->position();
    if (m_dragging) {
        const QRectF pr = plotRect();
        const double dx = (e->position().x() - m_dragStart.x()) / pr.width() * (m_dragView.hi - m_dragView.lo);
        m_view = {m_dragView.lo - dx, m_dragView.hi - dx};
        m_zoomed = true;
    }
    update();
}

void LineChart::mousePressEvent(QMouseEvent* e) {
    for (int i = 0; i < m_legendRects.size() && i < m_series.size(); ++i) {
        if (m_legendRects[i].contains(e->position())) {
            m_series[i].visible = !m_series[i].visible;
            update();
            return;
        }
    }
    if (e->button() == Qt::LeftButton && plotRect().contains(e->position())) {
        Range xr, yr;
        dataRange(xr, yr);
        m_dragging = true;
        m_dragStart = e->position();
        m_dragView = xr;
        setCursor(Qt::ClosedHandCursor);
    }
}

void LineChart::mouseReleaseEvent(QMouseEvent*) {
    m_dragging = false;
    unsetCursor();
}

void LineChart::mouseDoubleClickEvent(QMouseEvent*) { resetZoom(); }

void LineChart::zoomX(double factor, double anchor) {
    Range xr, yr;
    dataRange(xr, yr);
    const double a = xr.lo + anchor * (xr.hi - xr.lo);
    m_view = {a - (a - xr.lo) * factor, a + (xr.hi - a) * factor};
    if (m_view.hi - m_view.lo < 1e-6) return;
    m_zoomed = true;
    update();
}

void LineChart::panX(double fraction) {
    Range xr, yr;
    dataRange(xr, yr);
    const double d = (xr.hi - xr.lo) * fraction;
    m_view = {xr.lo + d, xr.hi + d};
    m_zoomed = true;
    update();
}

void LineChart::wheelEvent(QWheelEvent* e) {
    const QRectF pr = plotRect();
    const double anchor = std::clamp((e->position().x() - pr.left()) / pr.width(), 0.0, 1.0);
    zoomX(e->angleDelta().y() > 0 ? 0.8 : 1.25, anchor);
    e->accept();
}

void LineChart::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
    case Qt::Key_Plus:
    case Qt::Key_Equal: zoomX(0.8, 0.5); break;
    case Qt::Key_Minus: zoomX(1.25, 0.5); break;
    case Qt::Key_Left: panX(-0.1); break;
    case Qt::Key_Right: panX(0.1); break;
    case Qt::Key_Home:
    case Qt::Key_0: resetZoom(); break;
    default: QWidget::keyPressEvent(e); return;
    }
}

void LineChart::leaveEvent(QEvent*) {
    m_hover = {-1, -1};
    update();
}

void LineChart::updateAccessibleSummary() {
    QStringList parts;
    for (const auto& s : m_series) {
        if (s.points.isEmpty()) continue;
        double mn = std::numeric_limits<double>::max(), mx = std::numeric_limits<double>::lowest(), sum = 0;
        int n = 0;
        for (const auto& pt : s.points) {
            if (!std::isfinite(pt.y())) continue;
            mn = std::min(mn, pt.y());
            mx = std::max(mx, pt.y());
            sum += pt.y();
            ++n;
        }
        if (n)
            parts << tr("%1: min %2, average %3, max %4 %5")
                         .arg(s.name, formatValue(mn), formatValue(sum / n), formatValue(mx), m_yUnit);
    }
    setAccessibleDescription(parts.join(QStringLiteral("; ")));
}

// --- HistogramChart -----------------------------------------------------------------------

HistogramChart::HistogramChart(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(140);
    m_emptyText = tr("No data");
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
}

void HistogramChart::setTitle(const QString& t) {
    m_title = t;
    setAccessibleName(t);
    update();
}

void HistogramChart::setUnit(const QString& u) {
    m_unit = u;
    update();
}

void HistogramChart::clear() {
    m_series.clear();
    m_markers.clear();
    m_hoverBin = -1;
    updateAccessibleSummary();
    update();
}

int HistogramChart::addSeries(const QString& name, int colorSlot, const gt::Histogram& h) {
    if (!m_series.isEmpty()) {
        const auto& f = m_series.front().hist;
        if (h.counts.size() != f.counts.size() || std::abs(h.lo - f.lo) > 1e-9 || std::abs(h.binWidth - f.binWidth) > 1e-9)
            return -1;
    }
    m_series.push_back({name, colorSlot, h});
    updateAccessibleSummary();
    update();
    return static_cast<int>(m_series.size()) - 1;
}

void HistogramChart::setMarkers(QVector<Marker> m) {
    m_markers = std::move(m);
    update();
}

double HistogramChart::share(const Series& s, int bin) const {
    if (s.hist.total == 0 || bin < 0 || bin >= static_cast<int>(s.hist.counts.size())) return 0.0;
    return 100.0 * static_cast<double>(s.hist.counts[static_cast<std::size_t>(bin)]) / static_cast<double>(s.hist.total);
}

QRectF HistogramChart::plotRect() const {
    const QFontMetrics fm(font());
    const double top = (m_title.isEmpty() ? 6 : fm.height() + 10) + (m_series.size() >= 2 ? fm.height() + 6 : 0) +
                       (m_markers.isEmpty() ? 0 : fm.height() + 2);
    const double left = fm.horizontalAdvance(QStringLiteral("100 %")) + 12;
    const double bottom = fm.height() * 2.8;
    return QRectF(left, top, std::max(10.0, width() - left - 12), std::max(10.0, height() - top - bottom));
}

void HistogramChart::paintEvent(QPaintEvent*) {
    auto& tm = ThemeManager::instance();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QFontMetrics fm(font());
    p.fillRect(rect(), tm.surface());

    double y0 = 4;
    if (!m_title.isEmpty()) {
        QFont tf = font();
        tf.setWeight(QFont::DemiBold);
        p.setFont(tf);
        p.setPen(tm.textPrimary());
        p.drawText(QRectF(8, y0, width() - 16, fm.height() + 4), Qt::AlignLeft | Qt::AlignVCenter, m_title);
        p.setFont(font());
        y0 += fm.height() + 6;
    }
    if (m_series.size() >= 2) {
        double x = 8;
        for (const auto& s : m_series) {
            const double w = 12 + 6 + fm.horizontalAdvance(s.name) + 14;
            p.setPen(Qt::NoPen);
            p.setBrush(tm.series(s.colorSlot));
            p.drawRoundedRect(QRectF(x, y0 + fm.height() / 2.0 - 5, 12, 10), 2, 2);
            p.setPen(tm.textSecondary());
            p.drawText(QRectF(x + 18, y0, w - 18, fm.height()), Qt::AlignVCenter | Qt::AlignLeft, s.name);
            x += w;
        }
    }

    const QRectF pr = plotRect();
    const int bins = binCount();
    if (bins == 0) {
        p.setPen(QPen(tm.axisLine(), 1));
        p.drawLine(QPointF(pr.left(), pr.bottom()), QPointF(pr.right(), pr.bottom()));
        p.setPen(tm.textMuted());
        p.drawText(pr, Qt::AlignCenter | Qt::TextWordWrap, m_emptyText);
        return;
    }
    const auto& h0 = m_series.front().hist;
    const double xlo = h0.lo, xhi = h0.binStart(static_cast<std::size_t>(bins));
    double ymax = 0;
    for (const auto& s : m_series)
        for (int b = 0; b < bins; ++b) ymax = std::max(ymax, share(s, b));
    ymax = std::max(ymax * 1.08, 1.0);
    auto mapX = [&](double x) { return pr.left() + (x - xlo) / (xhi - xlo) * pr.width(); };
    auto mapY = [&](double y) { return pr.bottom() - y / ymax * pr.height(); };

    double ystep = 1;
    const auto yt = LineChart::niceTicks(0, ymax, std::max(2, static_cast<int>(pr.height() / 42)), &ystep);
    p.setPen(QPen(tm.gridLine(), 1));
    for (double v : yt)
        if (v > 0 && v <= ymax) p.drawLine(QPointF(pr.left(), mapY(v)), QPointF(pr.right(), mapY(v)));
    p.setPen(tm.textMuted());
    for (double v : yt)
        if (v <= ymax)
            p.drawText(QRectF(0, mapY(v) - fm.height() / 2.0, pr.left() - 6, fm.height()), Qt::AlignRight | Qt::AlignVCenter,
                       LineChart::formatTick(v, ystep) + QStringLiteral(" %"));
    double xstep = 1;
    const auto xt = LineChart::niceTicks(xlo, xhi, std::max(2, static_cast<int>(pr.width() / 80)), &xstep);
    for (double v : xt) {
        const double x = mapX(v);
        if (x < pr.left() - 0.5 || x > pr.right() + 0.5) continue;
        p.drawText(QRectF(x - 40, pr.bottom() + 4, 80, fm.height()), Qt::AlignHCenter | Qt::AlignTop,
                   LineChart::formatTick(v, xstep));
    }
    p.drawText(QRectF(pr.left(), pr.bottom() + fm.height() + 6, pr.width(), fm.height()), Qt::AlignHCenter,
               m_unit.isEmpty() ? tr("value") : m_unit);

    // Hovered bin background.
    const double binW = pr.width() / bins;
    if (m_hoverBin >= 0 && m_hoverBin < bins) {
        QColor c = tm.textMuted();
        c.setAlphaF(0.12f);
        p.fillRect(QRectF(pr.left() + binW * m_hoverBin, pr.top(), binW, pr.height()), c);
    }
    // Bars: grouped per bin, 2 px surface gap between neighbours, rounded data end.
    const int n = static_cast<int>(m_series.size());
    const double gap = binW >= 6 ? 2.0 : 0.0;
    const double barW = std::max(0.5, (binW - gap * n) / n);
    for (int si = 0; si < n; ++si) {
        const auto& s = m_series[si];
        p.setPen(Qt::NoPen);
        p.setBrush(tm.series(s.colorSlot));
        for (int b = 0; b < bins; ++b) {
            const double v = share(s, b);
            if (v <= 0) continue;
            const double x = pr.left() + binW * b + gap / 2 + si * (barW + gap);
            const double top = mapY(v);
            const double hgt = std::max(1.0, pr.bottom() - top);
            const double r = std::min({4.0, barW / 2, hgt});
            QPainterPath path;
            path.moveTo(x, pr.bottom());
            path.lineTo(x, pr.bottom() - hgt + r);
            path.quadTo(x, pr.bottom() - hgt, x + r, pr.bottom() - hgt);
            path.lineTo(x + barW - r, pr.bottom() - hgt);
            path.quadTo(x + barW, pr.bottom() - hgt, x + barW, pr.bottom() - hgt + r);
            path.lineTo(x + barW, pr.bottom());
            path.closeSubpath();
            p.drawPath(path);
        }
    }
    p.setPen(QPen(tm.axisLine(), 1));
    p.drawLine(QPointF(pr.left(), pr.bottom()), QPointF(pr.right(), pr.bottom()));

    // Markers (e.g. median / p99) as dashed verticals labelled above the plot.
    double lastLabelRight = -1e9;
    for (const auto& m : m_markers) {
        if (m.x < xlo || m.x > xhi) continue;
        const double x = mapX(m.x);
        p.setPen(QPen(tm.textMuted(), 1, Qt::DashLine));
        p.drawLine(QPointF(x, pr.top()), QPointF(x, pr.bottom()));
        const double w = fm.horizontalAdvance(m.label) + 4;
        double lx = std::clamp(x - w / 2, 0.0, width() - w);
        if (lx < lastLabelRight + 4) lx = lastLabelRight + 4;
        p.setPen(tm.textSecondary());
        p.drawText(QRectF(lx, pr.top() - fm.height() - 2, w, fm.height()), Qt::AlignCenter, m.label);
        lastLabelRight = lx + w;
    }
    // Frames outside the axis are never hidden silently.
    QStringList outside;
    for (const auto& s : m_series) {
        if (s.hist.over == 0) continue;
        const QString pct = QString::number(100.0 * static_cast<double>(s.hist.over) / static_cast<double>(std::max<std::size_t>(1, s.hist.total)), 'f', 1);
        outside << (n > 1 ? tr("%1: %2 % beyond axis").arg(s.name, pct) : tr("%1 % of frames beyond axis").arg(pct));
    }
    if (!outside.isEmpty()) {
        p.setPen(tm.textMuted());
        p.drawText(QRectF(pr.left(), pr.top() + 2, pr.width() - 4, fm.height()), Qt::AlignRight | Qt::AlignTop,
                   outside.join(QStringLiteral(" · ")) + QStringLiteral(" →"));
    }

    // Tooltip
    if (m_hoverBin >= 0 && m_hoverBin < bins) {
        QStringList lines;
        QVector<QColor> colors{QColor()};
        lines << QStringLiteral("%1 – %2 %3")
                     .arg(LineChart::formatTick(h0.binStart(static_cast<std::size_t>(m_hoverBin)), h0.binWidth / 10),
                          LineChart::formatTick(h0.binStart(static_cast<std::size_t>(m_hoverBin) + 1), h0.binWidth / 10), m_unit);
        for (const auto& s : m_series) {
            lines << tr("%1%2 frames (%3 %)")
                         .arg(n > 1 ? s.name + QStringLiteral(": ") : QString())
                         .arg(s.hist.counts[static_cast<std::size_t>(m_hoverBin)])
                         .arg(share(s, m_hoverBin), 0, 'f', 1);
            colors << tm.series(s.colorSlot);
        }
        int w = 0;
        for (const auto& l : lines) w = std::max(w, fm.horizontalAdvance(l));
        const double bw = w + 30, bh = static_cast<double>(lines.size()) * (fm.height() + 2) + 10;
        const double cx = pr.left() + binW * (m_hoverBin + 0.5);
        double bx = cx + 14;
        if (bx + bw > width() - 4) bx = cx - 14 - bw;
        const double by = pr.top() + 4;
        p.setPen(QPen(tm.border(), 1));
        p.setBrush(palette().color(QPalette::ToolTipBase));
        p.drawRoundedRect(QRectF(bx, by, bw, bh), 6, 6);
        for (int i = 0; i < lines.size(); ++i) {
            const double ly = by + 5 + i * (fm.height() + 2);
            if (colors[i].isValid()) {
                p.setPen(Qt::NoPen);
                p.setBrush(colors[i]);
                p.drawEllipse(QPointF(bx + 12, ly + fm.height() / 2.0), 4, 4);
            }
            p.setPen(i == 0 ? tm.textSecondary() : tm.textPrimary());
            p.drawText(QRectF(bx + 22, ly, w + 4, fm.height()), Qt::AlignLeft | Qt::AlignVCenter, lines[i]);
        }
    }
    if (hasFocus()) {
        p.setPen(QPen(tm.accent(), 1.5, Qt::DotLine));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 4, 4);
    }
}

void HistogramChart::mouseMoveEvent(QMouseEvent* e) {
    const QRectF pr = plotRect();
    const int bins = binCount();
    int bin = -1;
    if (bins > 0 && e->position().x() >= pr.left() && e->position().x() < pr.right() && e->position().y() >= pr.top() - 4 &&
        e->position().y() <= pr.bottom() + 4)
        bin = std::clamp(static_cast<int>((e->position().x() - pr.left()) / pr.width() * bins), 0, bins - 1);
    if (bin != m_hoverBin) {
        m_hoverBin = bin;
        update();
    }
}

void HistogramChart::leaveEvent(QEvent*) {
    m_hoverBin = -1;
    update();
}

void HistogramChart::keyPressEvent(QKeyEvent* e) {
    const int bins = binCount();
    switch (e->key()) {
    case Qt::Key_Left: m_hoverBin = bins ? std::max(0, (m_hoverBin < 0 ? bins : m_hoverBin) - 1) : -1; break;
    case Qt::Key_Right: m_hoverBin = bins ? std::min(bins - 1, m_hoverBin + 1) : -1; break;
    case Qt::Key_Escape: m_hoverBin = -1; break;
    default: QWidget::keyPressEvent(e); return;
    }
    update();
}

void HistogramChart::updateAccessibleSummary() {
    QStringList parts;
    for (const auto& s : m_series) {
        int mode = 0;
        for (int b = 1; b < binCount(); ++b)
            if (s.hist.counts[static_cast<std::size_t>(b)] > s.hist.counts[static_cast<std::size_t>(mode)]) mode = b;
        if (s.hist.total)
            parts << tr("%1: %2 frames, most common %3 to %4 %5")
                         .arg(s.name)
                         .arg(s.hist.total)
                         .arg(s.hist.binStart(static_cast<std::size_t>(mode)))
                         .arg(s.hist.binStart(static_cast<std::size_t>(mode) + 1))
                         .arg(m_unit);
    }
    setAccessibleDescription(parts.join(QStringLiteral("; ")));
}

} // namespace gt::gui
