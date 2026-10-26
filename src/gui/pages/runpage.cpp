#include "pages.h"

#include "settings.h"

#include "gt/duration.h"

#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>

#include <cmath>

namespace gt::gui {

RunPage::RunPage(AppState* state, QWidget* parent)
    : Page(tr("Live run"), tr("Progress, live frame times and telemetry of the current run."), parent), m_state(state) {
    auto* top = new Card;
    auto* tl = static_cast<QVBoxLayout*>(top->layout());
    auto* head = new QHBoxLayout;
    auto* textCol = new QVBoxLayout;
    m_headline = makeSubtitle(tr("Nothing is running"));
    m_subline = makeHint(tr("Start a suite from the Home or Tests page."));
    textCol->addWidget(m_headline);
    textCol->addWidget(m_subline);
    head->addLayout(textCol, 1);
    m_timeLabel = new QLabel;
    m_timeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    head->addWidget(m_timeLabel);
    m_stop = new QPushButton(themedIcon(QStringLiteral("square")), tr("Stop"));
    m_stop->setEnabled(false);
    m_stop->setMinimumHeight(34);
    m_stop->setShortcut(QKeySequence(Qt::Key_Escape));
    m_stop->setToolTip(tr("Stop the run (Esc). Finished tests are kept."));
    head->addWidget(m_stop);
    tl->addLayout(head);
    m_overall = new QProgressBar;
    m_overall->setAccessibleName(tr("Overall progress"));
    m_overall->setFormat(tr("Overall %p%"));
    m_overall->setRange(0, 1000);
    m_overall->setValue(0);
    m_testProgress = new QProgressBar;
    m_testProgress->setAccessibleName(tr("Current test progress"));
    m_testProgress->setFormat(tr("Test %p%"));
    m_testProgress->setRange(0, 1000);
    m_testProgress->setValue(0);
    tl->addWidget(m_overall);
    tl->addWidget(m_testProgress);
    auto* bannerRow = new QHBoxLayout;
    m_banner = new QLabel;
    m_banner->setWordWrap(true);
    m_banner->hide();
    m_results = new QPushButton(themedIcon(QStringLiteral("chart-column")), tr("View results"));
    m_results->hide();
    bannerRow->addWidget(m_banner, 1);
    bannerRow->addWidget(m_results);
    tl->addLayout(bannerRow);
    m_content->addWidget(top);

    // Telemetry tiles
    auto* tiles = new QGridLayout;
    tiles->setSpacing(10);
    const QString captions[8] = {tr("GPU clock"), tr("Memory clock"), tr("GPU temperature"), tr("Hotspot"),
                                 tr("Board power"), tr("Fan"),         tr("VRAM used"),       tr("GPU load")};
    for (int i = 0; i < 8; ++i) {
        m_tiles[i] = new StatTile(captions[i]);
        tiles->addWidget(m_tiles[i], i / 4, i % 4);
    }
    m_content->addLayout(tiles);

    auto* charts = new QGridLayout;
    charts->setSpacing(10);
    m_frameChart = new LineChart;
    m_frameChart->setTitle(tr("Frame time"));
    m_frameChart->setAxisLabels(tr("frame"), tr("ms"));
    m_frameChart->setUnit(QStringLiteral("ms"));
    m_frameSeries = m_frameChart->addSeries(tr("Frame time"), 0);
    m_frameChart->setFollowLatest(1500);
    m_fpsChart = new LineChart;
    m_fpsChart->setTitle(tr("Frames per second"));
    m_fpsChart->setAxisLabels(tr("seconds"), tr("fps"));
    m_fpsChart->setUnit(QStringLiteral("fps"));
    m_fpsSeries = m_fpsChart->addSeries(tr("FPS"), 0);
    m_tempChart = new LineChart;
    m_tempChart->setTitle(tr("Temperatures"));
    m_tempChart->setAxisLabels(tr("seconds"), QStringLiteral("°C"));
    m_tempChart->setUnit(QStringLiteral("°C"));
    m_tempSeries[0] = m_tempChart->addSeries(tr("GPU"), 0);
    m_tempSeries[1] = m_tempChart->addSeries(tr("Hotspot"), 1);
    m_tempSeries[2] = m_tempChart->addSeries(tr("Memory"), 2);
    m_tempChart->setEmptyText(tr("No temperature sensors reported by this GPU / driver"));
    charts->addWidget(m_frameChart, 0, 0);
    charts->addWidget(m_fpsChart, 0, 1);
    charts->addWidget(m_tempChart, 1, 0, 1, 2);
    m_content->addLayout(charts);

    auto* lists = new QHBoxLayout;
    auto* doneCol = new QVBoxLayout;
    doneCol->addWidget(makeSubtitle(tr("Finished tests")));
    m_done = new QPlainTextEdit;
    m_done->setReadOnly(true);
    m_done->setAccessibleName(tr("Finished tests"));
    m_done->setMaximumHeight(160);
    doneCol->addWidget(m_done);
    auto* logCol = new QVBoxLayout;
    logCol->addWidget(makeSubtitle(tr("Log")));
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setAccessibleName(tr("Runner log"));
    m_log->setMaximumHeight(160);
    m_log->setMaximumBlockCount(2000);
    QFont mono(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSizeF(m_log->font().pointSizeF() * 0.9);
    m_log->setFont(mono);
    logCol->addWidget(m_log);
    lists->addLayout(doneCol, 1);
    lists->addLayout(logCol, 1);
    m_content->addLayout(lists);

    RunnerClient* c = state->client();
    connect(m_stop, &QPushButton::clicked, c, [this, c] {
        m_stop->setEnabled(false);
        m_subline->setText(tr("Stopping… (the current test finishes its frame and the partial results are saved)"));
        c->stop();
    });
    connect(m_results, &QPushButton::clicked, this, [this] { Q_EMIT viewResults(m_resultsFile); });
    connect(c, &RunnerClient::started, this, [this] {
        m_running = true;
        m_startMs = QDateTime::currentMSecsSinceEpoch();
        m_frameChart->clearData();
        m_fpsChart->clearData();
        m_tempChart->clearData();
        m_frameIndex = 0;
        m_done->clear();
        m_log->clear();
        m_banner->hide();
        m_results->hide();
        m_stop->setEnabled(true);
        m_headline->setText(tr("Starting…"));
        m_subline->setText(tr("Creating the device and opening the benchmark window."));
        for (auto* t : m_tiles) t->setValue(QStringLiteral("—"));
    });
    connect(c, &RunnerClient::runStarted, this, &RunPage::onRunStarted);
    connect(c, &RunnerClient::testStarted, this, &RunPage::onTestStarted);
    connect(c, &RunnerClient::progress, this, [this](const QString&, double f, const QString&) {
        m_testFraction = f;
        m_testProgress->setValue(static_cast<int>(f * 1000));
        if (m_loopSeconds <= 0 && m_overall->maximum() > 0)
            m_overall->setValue(static_cast<int>((m_testIndex + f) / m_testCount * 1000));
    });
    connect(c, &RunnerClient::frames, this, &RunPage::onFrames);
    connect(c, &RunnerClient::telemetry, this, &RunPage::onTelemetry);
    connect(c, &RunnerClient::testFinished, this, &RunPage::onTestFinished);
    connect(c, &RunnerClient::loopStarted, this, [this](int loop) { m_loop = loop; });
    connect(c, &RunnerClient::logLine, this, [this](const QString& level, const QString& text) {
        m_log->appendPlainText(level == QLatin1String("stderr") ? text : QStringLiteral("[%1] %2").arg(level, text));
    });
    connect(c, &RunnerClient::safetyAbort, this, [this](const QString& msg) {
        m_banner->setText(QStringLiteral("⚠ ") + msg);
        m_banner->show();
    });
    connect(c, &RunnerClient::finished, this, &RunPage::onFinished);
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &RunPage::tick);
    timer->start(500);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
        m_stop->setIcon(themedIcon(QStringLiteral("square")));
        m_results->setIcon(themedIcon(QStringLiteral("chart-column")));
    });
}

void RunPage::onRunStarted(const nlohmann::json& e) {
    m_testCount = std::max<int>(1, static_cast<int>(e.value("tests", nlohmann::json::array()).size()));
    m_loopText = QString::fromStdString(e.value("loop", std::string("single")));
    m_loopSeconds = e.value("loop_seconds", nlohmann::json()).is_number() ? e["loop_seconds"].get<double>() : 0.0;
    const auto& gpu = e.value("gpu", nlohmann::json::object());
    m_subline->setText(tr("%1 · %2 on %3 · %4x%5 %6 · loop: %7")
                           .arg(QString::fromStdString(e.value("suite_name", std::string())),
                                apiDisplayName(QString::fromStdString(e.value("api", std::string()))),
                                QString::fromStdString(gpu.value("name", std::string())))
                           .arg(e.value("resolution", nlohmann::json::array({0, 0}))[0].get<int>())
                           .arg(e.value("resolution", nlohmann::json::array({0, 0}))[1].get<int>())
                           .arg(QString::fromStdString(e.value("quality", std::string())), m_loopText));
    if (m_loopText == QLatin1String("infinite")) m_overall->setRange(0, 0);
    else m_overall->setRange(0, 1000);
}

void RunPage::onTestStarted(const QString&, const QString& name, int index, int count, int loop) {
    m_testIndex = index;
    m_testCount = std::max(1, count);
    m_loop = loop;
    m_testFraction = 0;
    m_testProgress->setValue(0);
    m_headline->setText(tr("%1  (%2 of %3%4)")
                            .arg(name)
                            .arg(index + 1)
                            .arg(count)
                            .arg(m_loopText != QLatin1String("single") ? tr(", loop %1").arg(loop + 1) : QString()));
    m_headline->setAccessibleName(m_headline->text());
}

void RunPage::onFrames(const QVector<double>& ms, double t) {
    double sum = 0;
    for (double v : ms) {
        m_frameChart->appendPoint(m_frameSeries, QPointF(m_frameIndex++, v), 4000);
        sum += v;
    }
    if (!ms.isEmpty() && sum > 0) {
        m_fpsChart->appendPoint(m_fpsSeries, QPointF(t, 1000.0 * ms.size() / sum), 20000);
    }
}

void RunPage::onTelemetry(const results::TelemetrySample& s) {
    auto num = [](float v, const char* unit, int prec = 0) {
        return std::isfinite(v) ? QStringLiteral("%1 %2").arg(v, 0, 'f', prec).arg(QLatin1String(unit)) : QStringLiteral("—");
    };
    m_tiles[0]->setValue(num(s.gpuClockMhz, "MHz"));
    m_tiles[1]->setValue(num(s.memClockMhz, "MHz"));
    m_tiles[2]->setValue(std::isfinite(s.tempEdgeC) ? formatTemp(s.tempEdgeC) : QStringLiteral("—"));
    m_tiles[3]->setValue(std::isfinite(s.tempHotspotC) ? formatTemp(s.tempHotspotC) : QStringLiteral("—"));
    m_tiles[4]->setValue(num(s.powerW, "W"),
                         std::isfinite(s.powerLimitW) ? tr("limit %1 W").arg(s.powerLimitW, 0, 'f', 0) : QString());
    m_tiles[5]->setValue(std::isfinite(s.fanPercent) ? num(s.fanPercent, "%") : num(s.fanRpm, "rpm"));
    m_tiles[6]->setValue(std::isfinite(s.vramUsedMB) ? formatBytes(double(s.vramUsedMB) * 1048576.0) : QStringLiteral("—"),
                         std::isfinite(s.vramTotalMB) ? tr("of %1").arg(formatBytes(double(s.vramTotalMB) * 1048576.0)) : QString());
    m_tiles[7]->setValue(num(s.gpuUtilPercent, "%"));
    const auto& lim = Settings::instance();
    auto status = [](double v, double limit) { return !std::isfinite(v) ? -1 : v > limit ? 3 : v > limit - 8 ? 1 : 0; };
    m_tiles[2]->setStatus(status(s.tempEdgeC, lim.maxTemp()));
    m_tiles[3]->setStatus(status(s.tempHotspotC, lim.maxHotspot()));
    if (s.throttle & ~results::throttle::kIdle) {
        QStringList names;
        for (const auto& n : results::throttle::names(s.throttle & ~results::throttle::kIdle)) names << QString::fromStdString(n);
        m_tiles[0]->setValue(m_tiles[0]->value(), tr("throttling: %1").arg(names.join(QStringLiteral(", "))));
        m_tiles[0]->setStatus(1);
    } else {
        m_tiles[0]->setStatus(-1);
    }
    const float temps[3] = {s.tempEdgeC, s.tempHotspotC, s.tempMemC};
    for (int i = 0; i < 3; ++i)
        if (std::isfinite(temps[i])) m_tempChart->appendPoint(m_tempSeries[i], QPointF(s.t, temps[i]), 20000);
}

void RunPage::onTestFinished(const results::TestResult& r) {
    m_done->appendPlainText(QString::fromStdString(results::summarize(r)));
}

void RunPage::onFinished(const QString& status, const QString& message, const QString& file) {
    m_running = false;
    m_resultsFile = file;
    m_stop->setEnabled(false);
    m_testProgress->setValue(m_testProgress->maximum());
    m_overall->setRange(0, 1000);
    QString text;
    if (status == QLatin1String("completed")) {
        text = tr("✓ Run completed.");
        m_overall->setValue(1000);
    } else if (status == QLatin1String("stopped")) {
        text = tr("Run stopped. Finished tests were saved.");
    } else if (status == QLatin1String("safety_abort")) {
        text = tr("⚠ Stopped by a safety cutoff: %1").arg(message);
    } else if (status == QLatin1String("device_lost")) {
        text = tr("✗ The GPU stopped responding (device lost / driver reset). Partial results were saved. %1").arg(message);
    } else if (status == QLatin1String("crashed")) {
        text = tr("✗ %1").arg(message);
    } else {
        text = tr("Run ended: %1 %2").arg(status, message);
    }
    m_headline->setText(tr("Finished"));
    m_banner->setText(text);
    m_banner->show();
    m_results->show();
    m_results->setFocus();
}

void RunPage::tick() {
    if (!m_running) return;
    const double el = (QDateTime::currentMSecsSinceEpoch() - m_startMs) / 1000.0;
    QString t = tr("Elapsed %1").arg(QString::fromStdString(formatDuration(el)));
    double frac = 0;
    if (m_loopSeconds > 0) {
        frac = std::min(1.0, el / m_loopSeconds);
        t += tr("\nRemaining %1").arg(QString::fromStdString(formatDuration(std::max(0.0, m_loopSeconds - el))));
    } else {
        frac = (m_testIndex + m_testFraction) / m_testCount;
    }
    if (m_overall->maximum() > 0) m_overall->setValue(static_cast<int>(frac * 1000));
    m_timeLabel->setText(t);
}

} // namespace gt::gui
