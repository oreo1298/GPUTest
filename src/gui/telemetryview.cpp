#include "telemetryview.h"

#include "theme.h"
#include "widgets.h"

#include <QCoreApplication>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>
#include <map>

namespace gt::gui {

QString throttleText(std::uint32_t mask) {
    static const char* labels[] = {
        QT_TRANSLATE_NOOP("Throttle", "power limit"),   QT_TRANSLATE_NOOP("Throttle", "thermal"),
        QT_TRANSLATE_NOOP("Throttle", "memory thermal"), QT_TRANSLATE_NOOP("Throttle", "current limit"),
        QT_TRANSLATE_NOOP("Throttle", "voltage limit"),  QT_TRANSLATE_NOOP("Throttle", "hardware slowdown"),
        QT_TRANSLATE_NOOP("Throttle", "driver power cap"), QT_TRANSLATE_NOOP("Throttle", "application clocks"),
        QT_TRANSLATE_NOOP("Throttle", "idle"),            QT_TRANSLATE_NOOP("Throttle", "other")};
    QStringList out;
    for (int i = 0; i < 10; ++i)
        if (mask & (1u << i)) out << QCoreApplication::translate("Throttle", labels[i]);
    return out.join(QStringLiteral(", "));
}

int throttleSeverity(std::uint32_t mask) {
    using namespace results::throttle;
    if (mask & kHwSlowdown) return static_cast<int>(ThemeManager::Status::Critical);
    if (mask & (kThermal | kMemThermal | kCurrent | kVoltage)) return static_cast<int>(ThemeManager::Status::Serious);
    return static_cast<int>(ThemeManager::Status::Warning);
}

namespace {

using Getter = float (*)(const results::TelemetrySample&);

struct ChannelDef {
    const char* label;
    Getter get;
    double scale;
};

struct ChartDef {
    const char* title;
    const char* unit;
    std::vector<ChannelDef> channels;
    bool zeroBased;
};

const std::vector<ChartDef>& chartDefs() {
    static const std::vector<ChartDef> defs{
        {QT_TRANSLATE_NOOP("TelemetryView", "Temperature"), "°C",
         {{QT_TRANSLATE_NOOP("TelemetryView", "GPU"), [](const results::TelemetrySample& s) { return s.tempEdgeC; }, 1.0},
          {QT_TRANSLATE_NOOP("TelemetryView", "Hotspot"), [](const results::TelemetrySample& s) { return s.tempHotspotC; }, 1.0},
          {QT_TRANSLATE_NOOP("TelemetryView", "Memory"), [](const results::TelemetrySample& s) { return s.tempMemC; }, 1.0}},
         false},
        {QT_TRANSLATE_NOOP("TelemetryView", "Board power"), "W",
         {{QT_TRANSLATE_NOOP("TelemetryView", "Power"), [](const results::TelemetrySample& s) { return s.powerW; }, 1.0}},
         true},
        {QT_TRANSLATE_NOOP("TelemetryView", "Clocks"), "MHz",
         {{QT_TRANSLATE_NOOP("TelemetryView", "GPU"), [](const results::TelemetrySample& s) { return s.gpuClockMhz; }, 1.0},
          {QT_TRANSLATE_NOOP("TelemetryView", "Memory"), [](const results::TelemetrySample& s) { return s.memClockMhz; }, 1.0}},
         true},
        {QT_TRANSLATE_NOOP("TelemetryView", "Load"), "%",
         {{QT_TRANSLATE_NOOP("TelemetryView", "GPU"), [](const results::TelemetrySample& s) { return s.gpuUtilPercent; }, 1.0},
          {QT_TRANSLATE_NOOP("TelemetryView", "Memory controller"),
           [](const results::TelemetrySample& s) { return s.memUtilPercent; }, 1.0},
          {QT_TRANSLATE_NOOP("TelemetryView", "Fan"), [](const results::TelemetrySample& s) { return s.fanPercent; }, 1.0}},
         true},
        {QT_TRANSLATE_NOOP("TelemetryView", "Fan speed"), "rpm",
         {{QT_TRANSLATE_NOOP("TelemetryView", "Fan"), [](const results::TelemetrySample& s) { return s.fanRpm; }, 1.0}},
         true},
        {QT_TRANSLATE_NOOP("TelemetryView", "VRAM used"), "GiB",
         {{QT_TRANSLATE_NOOP("TelemetryView", "Used"), [](const results::TelemetrySample& s) { return s.vramUsedMB; },
           1.0 / 1024.0}},
         true},
    };
    return defs;
}

bool inWindow(double t, double t0, double t1) {
    return (!std::isfinite(t0) || t >= t0) && (!std::isfinite(t1) || t <= t1);
}

} // namespace

TelemetryView::TelemetryView(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(10);
    m_note = makeHint(QString());
    v->addWidget(m_note);
    m_grid = new QWidget;
    m_gridLayout = new QGridLayout(m_grid);
    m_gridLayout->setContentsMargins(0, 0, 0, 0);
    m_gridLayout->setSpacing(10);
    v->addWidget(m_grid);
    m_table = new QTableWidget;
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({tr("Sensor"), tr("Min"), tr("Average"), tr("Peak"), tr("Unit")});
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->setAlternatingRowColors(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->setAccessibleName(tr("Telemetry summary"));
    v->addWidget(m_table);
}

void TelemetryView::setShowTable(bool show) {
    m_showTable = show;
    m_table->setVisible(show && m_hasData);
}

void TelemetryView::setTelemetry(const results::Telemetry& tel, double t0, double t1, double x0) {
    clearLayout(m_gridLayout);
    m_hasData = false;

    // Throttle periods inside the window.
    QVector<LineChart::Band> bands;
    std::map<std::uint32_t, double> throttleSeconds;
    double throttled = 0.0;
    for (const auto& e : tel.throttleEvents) {
        const double a = std::isfinite(t0) ? std::max(e.start, t0) : e.start;
        const double b = std::isfinite(t1) ? std::min(e.end, t1) : e.end;
        if (b < a) continue;
        bands.push_back({a - x0, std::max(b, a + tel.intervalS) - x0, tr("throttled: %1").arg(throttleText(e.reasons)),
                         throttleSeverity(e.reasons)});
        throttled += std::max(b - a, tel.intervalS);
        for (std::uint32_t bit = 1; bit && bit <= e.reasons; bit <<= 1)
            if (e.reasons & bit) throttleSeconds[bit] += std::max(b - a, tel.intervalS);
    }

    double first = NAN, last = NAN;
    std::size_t samples = 0;
    for (const auto& s : tel.samples) {
        if (!inWindow(s.t, t0, t1)) continue;
        if (!std::isfinite(first)) first = s.t;
        last = s.t;
        ++samples;
    }

    int slot = 0;
    const int columns = 2;
    struct Row {
        QString label;
        double mn, avg, mx;
        QString unit;
    };
    QVector<Row> rows;
    for (const auto& def : chartDefs()) {
        QVector<QVector<QPointF>> pts(static_cast<int>(def.channels.size()));
        bool any = false;
        for (std::size_t c = 0; c < def.channels.size(); ++c) {
            double mn = 0, mx = 0, sum = 0;
            int n = 0;
            for (const auto& s : tel.samples) {
                if (!inWindow(s.t, t0, t1)) continue;
                const float raw = def.channels[c].get(s);
                const double v = std::isfinite(raw) ? raw * def.channels[c].scale : NAN;
                pts[static_cast<int>(c)].push_back(QPointF(s.t - x0, v));
                if (!std::isfinite(v)) continue;
                if (n == 0) mn = mx = v;
                mn = std::min(mn, v);
                mx = std::max(mx, v);
                sum += v;
                ++n;
            }
            if (n == 0) {
                pts[static_cast<int>(c)].clear();
                continue;
            }
            any = true;
            const QString label = def.channels.size() == 1
                                      ? tr(def.title)
                                      : QStringLiteral("%1 — %2").arg(tr(def.title), tr(def.channels[c].label));
            rows.push_back({label, mn, sum / n, mx, QString::fromUtf8(def.unit)});
        }
        if (!any) continue;
        auto* chart = new LineChart;
        chart->setTitle(QStringLiteral("%1 (%2)").arg(tr(def.title), QString::fromUtf8(def.unit)));
        chart->setAxisLabels(tr("seconds"), QString::fromUtf8(def.unit));
        chart->setUnit(QString::fromUtf8(def.unit));
        chart->setYStartsAtZero(def.zeroBased);
        if (QString::fromUtf8(def.unit) == QLatin1String("%")) chart->setFixedYRange(0, 105);
        for (std::size_t c = 0; c < def.channels.size(); ++c) {
            if (pts[static_cast<int>(c)].isEmpty()) continue;
            // Colour follows the channel's fixed position in its chart, never which channels exist.
            const int s = chart->addSeries(tr(def.channels[c].label), static_cast<int>(c));
            chart->setPoints(s, pts[static_cast<int>(c)]);
        }
        chart->setBands(bands);
        if (def.unit == std::string("W")) {
            double limit = 0;
            int n = 0;
            for (const auto& s : tel.samples)
                if (inWindow(s.t, t0, t1) && std::isfinite(s.powerLimitW)) {
                    limit += s.powerLimitW;
                    ++n;
                }
            if (n) chart->setRefLines({{limit / n, tr("limit %1 W").arg(limit / n, 0, 'f', 0)}});
        }
        chart->setMinimumHeight(200);
        m_gridLayout->addWidget(chart, slot / columns, slot % columns);
        ++slot;
        m_hasData = true;
    }
    for (int c = 0; c < columns; ++c) m_gridLayout->setColumnStretch(c, 1);

    if (!m_hasData) {
        m_note->setText(tel.provider.empty() || tel.provider == "none"
                            ? tr("No telemetry was recorded: no sensor provider is available for this GPU and driver.")
                            : tr("No telemetry samples in this time range (provider: %1).")
                                  .arg(QString::fromStdString(tel.provider)));
    } else {
        QString note = tr("%1 samples from %2 every %3 s.")
                           .arg(samples)
                           .arg(QString::fromStdString(tel.provider))
                           .arg(tel.intervalS, 0, 'g', 3);
        const double span = last - first;
        if (throttled > 0 && span > 0) {
            QStringList parts;
            for (const auto& [bit, secs] : throttleSeconds)
                if (bit != results::throttle::kIdle) parts << tr("%1 %2 s").arg(throttleText(bit)).arg(secs, 0, 'f', 1);
            note += QStringLiteral(" ") + tr("⚠ Throttled %1 % of the time (%2); shaded on the charts.")
                                               .arg(std::min(100.0, 100.0 * throttled / span), 0, 'f', 0)
                                               .arg(parts.join(QStringLiteral(", ")));
        } else {
            note += QStringLiteral(" ") + tr("No throttling was reported.");
        }
        m_note->setText(note);
    }

    m_table->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        const int decimals = r.unit == QLatin1String("GiB") ? 2 : (r.mx >= 100 ? 0 : 1);
        auto set = [&](int col, const QString& text, bool right) {
            auto* it = new QTableWidgetItem(text);
            if (right) it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_table->setItem(i, col, it);
        };
        set(0, r.label, false);
        set(1, QString::number(r.mn, 'f', decimals), true);
        set(2, QString::number(r.avg, 'f', decimals), true);
        set(3, QString::number(r.mx, 'f', decimals), true);
        set(4, r.unit, false);
    }
    m_table->resizeColumnsToContents();
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    const int h = m_table->horizontalHeader()->height() + 4 +
                  static_cast<int>(rows.size()) * m_table->verticalHeader()->defaultSectionSize();
    m_table->setFixedHeight(std::max(h, 40));
    m_table->setVisible(m_showTable && m_hasData);
    m_grid->setVisible(m_hasData);
}

} // namespace gt::gui
