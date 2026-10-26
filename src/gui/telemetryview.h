#pragma once
// Telemetry as small multiples: one chart per unit (temperatures, power, clocks,
// load, VRAM) so no chart needs a second y axis. Throttle periods are shaded on
// every chart; a compact table lists min / average / peak per channel.

#include "chart.h"
#include "gt/results.h"

#include <QWidget>

class QGridLayout;
class QLabel;
class QTableWidget;

namespace gt::gui {

class TelemetryView : public QWidget {
    Q_OBJECT
public:
    explicit TelemetryView(QWidget* parent = nullptr);
    /// Shows the samples of `telemetry` within [t0, t1] (seconds since run start;
    /// NaN = unbounded). `x0` is subtracted from the time axis.
    void setTelemetry(const results::Telemetry& telemetry, double t0, double t1, double x0 = 0.0);
    bool hasData() const { return m_hasData; }
    void setShowTable(bool show);

private:
    QLabel* m_note;
    QWidget* m_grid;
    QGridLayout* m_gridLayout;
    QTableWidget* m_table;
    bool m_hasData = false;
    bool m_showTable = true;
};

/// Human-readable throttle reasons ("power, thermal").
QString throttleText(std::uint32_t mask);
/// Status level used to shade a throttle period (ThemeManager::Status as int).
int throttleSeverity(std::uint32_t mask);

} // namespace gt::gui
