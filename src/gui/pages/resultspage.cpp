#include "pages.h"

#include "gt/app_info.h"
#include "gt/duration.h"
#include "gt/report.h"
#include "history.h"
#include "settings.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace gt::gui {

namespace {

QTableWidget* makeTable(const QStringList& headers, const QString& accessibleName) {
    auto* t = new QTableWidget;
    t->setColumnCount(static_cast<int>(headers.size()));
    t->setHorizontalHeaderLabels(headers);
    t->verticalHeader()->hide();
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setAlternatingRowColors(true);
    t->setAccessibleName(accessibleName);
    return t;
}

QString formatDurationText(double s) { return QString::fromStdString(formatDuration(s)); }

/// Three significant digits, without exponent notation for everyday magnitudes.
QString formatMetricValue(double v) {
    const double a = std::abs(v);
    if (a >= 100) return QString::number(v, 'f', 0);
    if (a >= 10) return QString::number(v, 'f', 1);
    if (a >= 1) return QString::number(v, 'f', 2);
    return QString::number(v, 'g', 3);
}

} // namespace

ResultsPage::ResultsPage(AppState* state, QWidget* parent)
    : Page(tr("Results"), tr("Scores, per-test metrics, frame times and sensor data. Scores are only comparable between "
                             "runs with the same suite, API, resolution, quality and duration."),
           parent),
      m_state(state) {
    auto* bar = new QHBoxLayout;
    m_header = new QLabel(tr("No results loaded. Run a suite, or open a results file."));
    m_header->setWordWrap(true);
    m_header->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bar->addWidget(m_header, 1);
    auto* open = new QPushButton(themedIcon(QStringLiteral("folder-open")), tr("Open…"));
    open->setToolTip(tr("Open a results file (.json)"));
    bar->addWidget(open);
    m_compare = new QPushButton(themedIcon(QStringLiteral("git-compare")), tr("Compare…"));
    m_compare->setToolTip(tr("Compare this run with another run from the history"));
    bar->addWidget(m_compare);
    m_export = new QToolButton;
    m_export->setText(tr("Export"));
    m_export->setIcon(themedIcon(QStringLiteral("download")));
    m_export->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_export->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(m_export);
    menu->addAction(themedIcon(QStringLiteral("file-text")), tr("HTML report…"), this, &ResultsPage::exportHtml);
    menu->addAction(themedIcon(QStringLiteral("file-code")), tr("CSV…"), this, &ResultsPage::exportCsv);
    menu->addAction(themedIcon(QStringLiteral("save")), tr("Save a copy of the results (JSON)…"), this, &ResultsPage::saveCopy);
    menu->addSeparator();
    menu->addAction(themedIcon(QStringLiteral("copy")), tr("Copy summary to clipboard"), this, &ResultsPage::copySummary);
    m_export->setMenu(menu);
    bar->addWidget(m_export);
    m_content->addLayout(bar);
    m_note = makeHint(QString());
    m_note->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_note->setOpenExternalLinks(true);
    m_note->hide();
    m_content->addWidget(m_note);
    m_banner = new QLabel;
    m_banner->setWordWrap(true);
    m_banner->hide();
    m_content->addWidget(m_banner);

    m_scoreBox = new QWidget;
    auto* sl = new QHBoxLayout(m_scoreBox);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->setSpacing(10);
    m_content->addWidget(m_scoreBox);

    // Diagnosis / checks: one row per test that delivered a PASS / WARN / FAIL verdict.
    m_verdictCard = new Card;
    auto* vl = static_cast<QVBoxLayout*>(m_verdictCard->layout());
    m_verdictTitle = makeSubtitle(tr("Diagnosis"));
    vl->addWidget(m_verdictTitle);
    m_verdictGrid = new QGridLayout;
    m_verdictGrid->setHorizontalSpacing(12);
    m_verdictGrid->setVerticalSpacing(8);
    m_verdictGrid->setColumnStretch(2, 1);
    vl->addLayout(m_verdictGrid);
    m_verdictCard->hide();
    m_content->addWidget(m_verdictCard);

    m_tabs = new TabView;

    // Tests tab: table + selected-test details.
    auto* testsTab = new QWidget;
    auto* tl = new QVBoxLayout(testsTab);
    tl->setContentsMargins(0, 10, 0, 0);
    tl->setSpacing(10);
    m_table = makeTable({tr("Test"), tr("Status"), tr("Result"), tr("Score"), tr("Avg FPS"), tr("1% low"), tr("p99 ms"),
                         tr("Verdict")},
                        tr("Test results"));
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->setMinimumHeight(170);
    tl->addWidget(m_table);
    auto* detail = new Card;
    auto* dl = static_cast<QVBoxLayout*>(detail->layout());
    m_testDetails = new QLabel(tr("Select a test to see its details."));
    m_testDetails->setWordWrap(true);
    m_testDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dl->addWidget(m_testDetails);
    auto* charts = new QHBoxLayout;
    charts->setSpacing(10);
    m_frameChart = new LineChart;
    m_frameChart->setTitle(tr("Frame times"));
    m_frameChart->setAxisLabels(tr("frame"), tr("ms"));
    m_frameChart->setUnit(QStringLiteral("ms"));
    m_frameChart->addSeries(tr("Frame time"), 0);
    m_frameChart->addSeries(tr("GPU time"), 1);
    m_frameChart->setMinimumHeight(250);
    charts->addWidget(m_frameChart, 3);
    m_hist = new HistogramChart;
    m_hist->setTitle(tr("Frame time distribution"));
    m_hist->setUnit(QStringLiteral("ms"));
    m_hist->setMinimumHeight(250);
    charts->addWidget(m_hist, 2);
    dl->addLayout(charts);
    dl->addWidget(makeSubtitle(tr("Sensors during this test")));
    m_testTelemetry = new TelemetryView;
    m_testTelemetry->setShowTable(false);
    dl->addWidget(m_testTelemetry);
    tl->addWidget(detail);
    m_tabs->addTab(testsTab, themedIcon(QStringLiteral("list-checks")), tr("Tests"));

    // Telemetry tab: whole run.
    auto* telTab = new QWidget;
    auto* el = new QVBoxLayout(telTab);
    el->setContentsMargins(0, 10, 0, 0);
    m_runTelemetry = new TelemetryView;
    el->addWidget(m_runTelemetry);
    el->addStretch();
    m_tabs->addTab(telTab, themedIcon(QStringLiteral("thermometer")), tr("Sensors"));

    // Loops tab.
    m_loopsTab = new QWidget;
    auto* ll = new QVBoxLayout(m_loopsTab);
    ll->setContentsMargins(0, 10, 0, 0);
    ll->setSpacing(10);
    m_loopNote = makeHint(QString());
    ll->addWidget(m_loopNote);
    m_loopChart = new LineChart;
    m_loopChart->setTitle(tr("Score per loop"));
    m_loopChart->setAxisLabels(tr("loop"), tr("score"));
    m_loopChart->addSeries(tr("Score"), 0);
    m_loopChart->setYStartsAtZero(false);
    m_loopChart->setXInteger(true);
    m_loopChart->setMinimumHeight(240);
    ll->addWidget(m_loopChart);
    m_loopTable = makeTable({tr("Loop"), tr("Start"), tr("Duration"), tr("Score"), tr("vs. best"), tr("Complete")},
                            tr("Loop results"));
    m_loopTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_loopTable->setMinimumHeight(160);
    ll->addWidget(m_loopTable);
    m_tabs->addTab(m_loopsTab, themedIcon(QStringLiteral("refresh-cw")), tr("Loops"));

    // System tab.
    m_system = new QTreeWidget;
    m_system->setColumnCount(2);
    m_system->setHeaderLabels({tr("Property"), tr("Value")});
    m_system->setAlternatingRowColors(true);
    m_system->setRootIsDecorated(true);
    m_system->setMinimumHeight(420);
    m_system->setAccessibleName(tr("System report"));
    m_tabs->addTab(m_system, themedIcon(QStringLiteral("cpu")), tr("System report"));
    m_content->addWidget(m_tabs);
    m_content->addStretch();

    m_compare->setEnabled(false);
    m_export->setEnabled(false);
    m_tabs->hide(); // nothing but the hint and Open until a run is loaded

    connect(open, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Open results"), Settings::instance().resultsDir(),
                                                       tr("Results (*.json)"));
        if (f.isEmpty()) return;
        if (loadFile(f)) History::instance().importFile(f);
    });
    connect(m_compare, &QPushButton::clicked, this, [this] { Q_EMIT compareRequested(m_file); });
    connect(m_table, &QTableWidget::currentCellChanged, this, [this](int row, int, int, int) { showTest(row); });
}

bool ResultsPage::loadFile(const QString& path) {
    std::string err;
    auto r = results::readFile(path.toStdString(), &err);
    if (!r) {
        m_header->setText(tr("Could not read %1: %2").arg(path.toHtmlEscaped(), QString::fromStdString(err).toHtmlEscaped()));
        return false;
    }
    showRun(*r, path);
    return true;
}

void ResultsPage::showRun(const results::RunResult& run, const QString& file) {
    m_run = run;
    m_file = file;
    m_loaded = true;
    m_export->setEnabled(true);
    m_tabs->show();
    m_compare->setEnabled(!file.isEmpty());
    m_note->hide();
    const auto& g = run.system.gpu;
    m_header->setText(tr("<b>%1</b> · %2 · %3 · %4×%5 %6 / %7<br>%8 — %9")
                          .arg(QString::fromStdString(g.name).toHtmlEscaped(),
                               apiDisplayName(QString::fromStdString(run.config.api)),
                               QString::fromStdString(run.config.suite).toHtmlEscaped())
                          .arg(run.config.width)
                          .arg(run.config.height)
                          .arg(QString::fromStdString(run.config.quality), QString::fromStdString(run.config.duration),
                               QString::fromStdString(run.createdUtc),
                               file.isEmpty() ? tr("not saved") : file.toHtmlEscaped()));
    const QString status = QString::fromStdString(results::toString(run.status));
    QStringList banner;
    if (run.status != results::RunStatus::Completed)
        banner << tr("⚠ Run status: %1. %2").arg(status, QString::fromStdString(run.statusMessage).toHtmlEscaped());
    if (!run.deviceLosses.empty())
        banner << (run.deviceLosses.size() == 1
                       ? tr("⚠ The GPU stopped responding once during this run (device lost / driver reset).")
                       : tr("⚠ The GPU stopped responding %1 times during this run (device lost / driver reset).")
                             .arg(run.deviceLosses.size())) +
                      QLatin1Char(' ') +
                      tr("The driver's fault report and the last sensor readings are in System report → Device losses.");
    if (run.validationErrors > 0)
        banner << (run.validationErrors == 1 ? tr("⚠ The API validation layer reported an error during this run.")
                                             : tr("⚠ The API validation layer reported %1 errors during this run.")
                                                   .arg(run.validationErrors));
    m_banner->setText(banner.join(QStringLiteral("<br>")));
    m_banner->setVisible(!banner.isEmpty());

    clearLayout(m_scoreBox->layout());
    auto* box = static_cast<QHBoxLayout*>(m_scoreBox->layout());
    int passed = 0, warned = 0, failed = 0;
    for (const auto& t : run.tests) {
        passed += t.verdict == results::Verdict::Pass;
        warned += t.verdict == results::Verdict::Warn;
        failed += t.verdict == results::Verdict::Fail;
    }
    if (run.scores.overall > 0 || passed + warned + failed == 0) {
        auto* overall = new StatTile(tr("Overall score"));
        overall->setValue(run.scores.overall > 0 ? QString::number(run.scores.overall, 'f', 0) : QStringLiteral("—"),
                          std::isfinite(run.scores.perWatt) ? tr("%1 points per watt").arg(run.scores.perWatt, 0, 'f', 1)
                                                            : QString());
        box->addWidget(overall);
    }
    // "Diagnosis" when diagnostics ran; otherwise the verdicts are the correctness checks of feature tests.
    const bool diagnostics = std::any_of(run.tests.begin(), run.tests.end(), [](const results::TestResult& t) {
        return t.category == "diagnostic" && t.verdict != results::Verdict::None;
    });
    const QString verdictTitle = diagnostics ? tr("Diagnosis") : tr("Checks");
    m_verdictTitle->setText(verdictTitle);
    if (passed + warned + failed > 0) {
        auto* diag = new StatTile(verdictTitle);
        diag->setValue(failed ? tr("FAIL") : warned ? tr("WARN") : tr("PASS"),
                       tr("%1 failed · %2 · %3 passed").arg(failed).arg(warned == 1 ? tr("1 warning") : tr("%1 warnings").arg(warned)).arg(passed));
        diag->setStatus(static_cast<int>(failed ? ThemeManager::Status::Critical
                                         : warned ? ThemeManager::Status::Warning
                                                  : ThemeManager::Status::Good));
        box->addWidget(diag);
    }
    for (const auto& [cat, score] : run.scores.subscores) {
        auto* t = new StatTile(tr("%1 score").arg(capitalized(QString::fromStdString(cat))));
        t->setValue(QString::number(score, 'f', 0));
        box->addWidget(t);
    }
    if (run.stability.valid) {
        auto* t = new StatTile(tr("Stability"));
        t->setValue(QStringLiteral("%1 %").arg(run.stability.ratio * 100.0, 0, 'f', 1),
                    run.stability.passed ? tr("pass (≥ %1 %)").arg(run.stability.threshold * 100.0, 0, 'f', 0)
                                         : tr("FAIL (< %1 %)").arg(run.stability.threshold * 100.0, 0, 'f', 0));
        t->setStatus(static_cast<int>(run.stability.passed ? ThemeManager::Status::Good : ThemeManager::Status::Critical));
        box->addWidget(t);
    }
    if (const auto it = run.telemetry.summary.find("temp_edge_c"); it != run.telemetry.summary.end() && it->second.count) {
        auto* t = new StatTile(tr("Peak GPU temperature"));
        t->setValue(formatTemp(it->second.max), tr("average %1").arg(formatTemp(it->second.avg)));
        box->addWidget(t);
    }
    if (const auto it = run.telemetry.summary.find("power_w"); it != run.telemetry.summary.end() && it->second.count) {
        auto* t = new StatTile(tr("Average board power"));
        t->setValue(QStringLiteral("%1 W").arg(it->second.avg, 0, 'f', 0), tr("peak %1 W").arg(it->second.max, 0, 'f', 0));
        box->addWidget(t);
    }
    box->addStretch();

    clearLayout(m_verdictGrid);
    int vrow = 0;
    for (const auto& t : run.tests) {
        if (t.verdict == results::Verdict::None) continue;
        const Badge::Kind kind = t.verdict == results::Verdict::Pass   ? Badge::Kind::Good
                                 : t.verdict == results::Verdict::Warn ? Badge::Kind::Warning
                                                                       : Badge::Kind::Critical;
        auto* badge = new Badge(QString::fromStdString(results::toString(t.verdict)).toUpper(), kind);
        auto* name = new QLabel(QStringLiteral("<b>%1</b>").arg(QString::fromStdString(t.name).toHtmlEscaped() +
                                                                (run.loops.size() > 1 ? tr(" (loop %1)").arg(t.loop + 1) : QString())));
        auto* text = new QLabel(QString::fromStdString(t.verdictMessage).toHtmlEscaped());
        text->setWordWrap(true);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_verdictGrid->addWidget(badge, vrow, 0, Qt::AlignTop);
        m_verdictGrid->addWidget(name, vrow, 1, Qt::AlignTop);
        m_verdictGrid->addWidget(text, vrow, 2);
        ++vrow;
    }
    m_verdictCard->setVisible(vrow > 0);

    m_table->setRowCount(static_cast<int>(run.tests.size()));
    for (int i = 0; i < static_cast<int>(run.tests.size()); ++i) {
        const auto& t = run.tests[static_cast<std::size_t>(i)];
        auto set = [&](int col, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            if (col >= 2 && col <= 6) item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_table->setItem(i, col, item);
        };
        QString name = QString::fromStdString(t.name);
        if (run.loops.size() > 1) name += tr(" (loop %1)").arg(t.loop + 1);
        set(0, name);
        QString st = QString::fromStdString(results::toString(t.status));
        if (!t.message.empty()) st += QStringLiteral(": ") + QString::fromStdString(t.message);
        set(1, st);
        m_table->item(i, 1)->setToolTip(st);
        const results::Metric* headline = t.headline();
        set(2, headline ? QStringLiteral("%1 %2").arg(formatMetricValue(headline->value), QString::fromStdString(headline->unit))
                        : QStringLiteral("—"));
        set(3, t.scored ? QString::number(t.score, 'f', 0) : QStringLiteral("—"));
        set(4, t.frameStats ? QString::number(t.frameStats->avgFps, 'f', 1) : QStringLiteral("—"));
        set(5, t.frameStats ? QString::number(t.frameStats->low1Fps, 'f', 1) : QStringLiteral("—"));
        set(6, t.frameStats ? QString::number(t.frameStats->p99Ms, 'f', 2) : QStringLiteral("—"));
        QString verdict;
        if (t.verdict != results::Verdict::None)
            verdict = QString::fromStdString(results::toString(t.verdict)).toUpper() + QStringLiteral(": ") +
                      QString::fromStdString(t.verdictMessage);
        set(7, verdict);
        m_table->item(i, 7)->setToolTip(verdict);
    }
    // Long status / verdict texts are elided (full text in the tooltip) instead of widening the table.
    m_table->resizeColumnsToContents();
    m_table->setColumnWidth(1, std::min(m_table->columnWidth(1), 220));
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(7, QHeaderView::Stretch);
    const int rows = std::min<int>(static_cast<int>(run.tests.size()), 10);
    m_table->setFixedHeight(m_table->horizontalHeader()->sizeHint().height() + 6 +
                            std::max(rows, 3) * m_table->verticalHeader()->defaultSectionSize());
    if (!run.tests.empty()) m_table->selectRow(0);
    showTest(run.tests.empty() ? -1 : 0);

    m_runTelemetry->setTelemetry(run.telemetry, NAN, NAN);
    showLoops();
    showSystem();
}

void ResultsPage::showTest(int row) {
    if (row < 0 || row >= static_cast<int>(m_run.tests.size())) {
        m_frameChart->clearData();
        m_hist->clear();
        m_testTelemetry->setTelemetry({}, NAN, NAN);
        return;
    }
    const auto& t = m_run.tests[static_cast<std::size_t>(row)];
    QStringList lines;
    lines << QStringLiteral("<b>%1</b> — %2").arg(QString::fromStdString(t.name).toHtmlEscaped(),
                                                  QString::fromStdString(t.id));
    if (t.frameStats) {
        const auto& f = *t.frameStats;
        lines << tr("Average %1 fps · min %2 · max %3 · 1% low %4 · 0.1% low %5")
                     .arg(f.avgFps, 0, 'f', 1)
                     .arg(f.minFps, 0, 'f', 1)
                     .arg(f.maxFps, 0, 'f', 1)
                     .arg(f.low1Fps, 0, 'f', 1)
                     .arg(f.low01Fps, 0, 'f', 1);
        lines << tr("Frame time p50 %1 · p90 %2 · p95 %3 · p99 %4 · p99.9 %5 ms · σ %6 ms · %7 stutters")
                     .arg(f.p50Ms, 0, 'f', 2)
                     .arg(f.p90Ms, 0, 'f', 2)
                     .arg(f.p95Ms, 0, 'f', 2)
                     .arg(f.p99Ms, 0, 'f', 2)
                     .arg(f.p999Ms, 0, 'f', 2)
                     .arg(f.stddevMs, 0, 'f', 2)
                     .arg(f.stutterCount);
    }
    QStringList misc;
    if (std::isfinite(t.gpuBusyPercent)) misc << tr("GPU busy %1 %").arg(t.gpuBusyPercent, 0, 'f', 1);
    if (t.frames > 0) misc << tr("%1 frames in %2 s").arg(t.frames).arg(t.durationS, 0, 'f', 1);
    else misc << tr("took %1").arg(formatDurationText(t.durationS));
    lines << misc.join(QStringLiteral(" · "));
    // Metrics as a two-pair-per-row table: feature tests report up to a dozen.
    if (!t.metrics.empty()) {
        QString table = QStringLiteral("<table cellspacing=\"0\" cellpadding=\"2\">");
        for (std::size_t i = 0; i < t.metrics.size(); ++i) {
            const auto& m = t.metrics[i];
            if (i % 2 == 0) table += QStringLiteral("<tr>");
            table += QStringLiteral("<td>%1</td><td align=\"right\" style=\"padding-left:12px\"><b>%2</b> %3</td>")
                         .arg(QString::fromStdString(m.label).toHtmlEscaped(), formatMetricValue(m.value),
                              QString::fromStdString(m.unit).toHtmlEscaped());
            table += i % 2 == 0 ? QStringLiteral("<td style=\"padding-left:32px\"></td>") : QStringLiteral("</tr>");
        }
        if (t.metrics.size() % 2) table += QStringLiteral("</tr>");
        lines << table + QStringLiteral("</table>");
    }
    if (t.timeLimited) lines << tr("⚠ Hit the time limit — partial result.");
    if (!t.message.empty()) lines << QString::fromStdString(t.message).toHtmlEscaped();
    m_testDetails->setText(lines.join(QStringLiteral("<br>")));

    QVector<QPointF> ft, gt;
    for (std::size_t i = 0; i < t.frameTimesMs.size(); ++i) ft.push_back(QPointF(static_cast<double>(i), t.frameTimesMs[i]));
    for (std::size_t i = 0; i < t.gpuTimesMs.size(); ++i) gt.push_back(QPointF(static_cast<double>(i), t.gpuTimesMs[i]));
    const bool frames = !t.frameTimesMs.empty();
    m_frameChart->setVisible(frames);
    m_hist->setVisible(frames);
    m_frameChart->resetZoom();
    m_frameChart->setPoints(0, ft);
    m_frameChart->setPoints(1, gt);
    QVector<LineChart::RefLine> refs;
    if (t.frameStats) refs.push_back({t.frameStats->p99Ms, tr("p99 %1 ms").arg(t.frameStats->p99Ms, 0, 'f', 2)});
    m_frameChart->setRefLines(refs);

    m_hist->clear();
    if (!t.frameTimesMs.empty()) {
        const std::vector<double> v(t.frameTimesMs.begin(), t.frameTimesMs.end());
        m_hist->addSeries(tr("Frame time"), 0, frameTimeHistogram(v, 40));
        if (t.frameStats)
            m_hist->setMarkers({{t.frameStats->p50Ms, tr("median")}, {t.frameStats->p99Ms, tr("p99")}});
    }
    m_testTelemetry->setTelemetry(m_run.telemetry, t.startS, t.startS + t.durationS);
}

void ResultsPage::showLoops() {
    const auto& loops = m_run.loops;
    const int idx = m_tabs->indexOf(m_loopsTab);
    m_tabs->setTabVisible(idx, loops.size() > 1);
    QVector<QPointF> pts;
    double best = 0;
    for (const auto& l : loops) best = std::max(best, l.score);
    m_loopTable->setRowCount(static_cast<int>(loops.size()));
    for (int i = 0; i < static_cast<int>(loops.size()); ++i) {
        const auto& l = loops[static_cast<std::size_t>(i)];
        if (l.complete && l.score > 0) pts.push_back(QPointF(l.index + 1, l.score));
        const QStringList cells{QString::number(l.index + 1), formatDurationText(l.startS), formatDurationText(l.durationS),
                                l.score > 0 ? QString::number(l.score, 'f', 0) : QStringLiteral("—"),
                                best > 0 && l.score > 0 ? QStringLiteral("%1 %").arg(100.0 * l.score / best, 0, 'f', 1)
                                                        : QStringLiteral("—"),
                                l.complete ? tr("yes") : tr("no (stopped)")};
        for (int c = 0; c < cells.size(); ++c) {
            auto* it = new QTableWidgetItem(cells[c]);
            if (c >= 1 && c <= 4) it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_loopTable->setItem(i, c, it);
        }
    }
    m_loopChart->setPoints(0, pts);
    QVector<LineChart::RefLine> refs;
    if (best > 0 && m_run.stability.valid)
        refs.push_back({best * m_run.stability.threshold,
                        tr("stability threshold (%1 % of best)").arg(m_run.stability.threshold * 100.0, 0, 'f', 0)});
    m_loopChart->setRefLines(refs);
    QString note;
    if (m_run.stability.valid) {
        note = tr("Worst loop is %1 % of the best (threshold %2 %): %3.")
                   .arg(m_run.stability.ratio * 100.0, 0, 'f', 1)
                   .arg(m_run.stability.threshold * 100.0, 0, 'f', 0)
                   .arg(m_run.stability.passed ? tr("stable") : tr("NOT stable"));
        if (std::isfinite(m_run.stability.trendPercentPerHour))
            note += QStringLiteral(" ") + tr("Score trend %1 % per hour.").arg(m_run.stability.trendPercentPerHour, 0, 'f', 1);
        else
            note += QStringLiteral(" ") + tr("A trend needs at least three loops over a minute or more.");
    } else {
        note = tr("Stability needs at least two complete loops.");
    }
    m_loopNote->setText(note);
}

void ResultsPage::showSystem() {
    m_system->clear();
    auto group = [this](const QString& name) {
        auto* g = new QTreeWidgetItem(m_system, {name});
        QFont f = g->font(0);
        f.setWeight(QFont::DemiBold);
        g->setFont(0, f);
        g->setFirstColumnSpanned(true);
        return g;
    };
    auto add = [](QTreeWidgetItem* g, const QString& k, const QString& v) {
        if (v.trimmed().isEmpty()) return;
        auto* it = new QTreeWidgetItem(g, {k, v});
        it->setToolTip(1, v);
    };
    auto s = [](const std::string& v) { return QString::fromStdString(v); };
    const auto& gpu = m_run.system.gpu;
    const auto& host = m_run.system.host;
    auto* g = group(tr("Graphics card"));
    add(g, tr("Name"), s(gpu.name));
    add(g, tr("Vendor"), s(gpu.vendor));
    add(g, tr("PCI IDs"), QStringLiteral("%1:%2 (subsystem %3:%4, rev %5)")
                              .arg(gpu.vendorId, 4, 16, QLatin1Char('0'))
                              .arg(gpu.deviceId, 4, 16, QLatin1Char('0'))
                              .arg(gpu.subsysVendorId, 4, 16, QLatin1Char('0'))
                              .arg(gpu.subsysId, 4, 16, QLatin1Char('0'))
                              .arg(gpu.revision, 2, 16, QLatin1Char('0')));
    add(g, tr("Type"), s(gpu.type));
    add(g, tr("Driver"), (s(gpu.driverName) + QLatin1Char(' ') + s(gpu.driverVersion)).trimmed());
    add(g, tr("Driver info"), s(gpu.driverInfo));
    add(g, tr("API"), (apiDisplayName(s(gpu.api)) + QLatin1Char(' ') + s(gpu.apiVersion)).trimmed());
    add(g, tr("Dedicated memory"), gpu.vramBytes ? formatBytes(static_cast<double>(gpu.vramBytes)) : QString());
    add(g, tr("Shared memory"), gpu.sharedBytes ? formatBytes(static_cast<double>(gpu.sharedBytes)) : QString());
    add(g, tr("PCIe link"), gpu.pcieCurrent.empty() ? QString()
                                                    : tr("%1 (max %2)").arg(s(gpu.pcieCurrent), s(gpu.pcieMax)));
    add(g, tr("Resizable BAR"), gpu.vramBytes ? (gpu.resizableBar ? tr("enabled") : tr("disabled / unknown")) : QString());
    add(g, tr("VBIOS"), s(gpu.vbios));
    add(g, tr("PCI address"), s(gpu.pciAddress));
    add(g, tr("Extensions"), m_run.system.extensions.empty() ? QString() : QString::number(m_run.system.extensions.size()));
    auto* h = group(tr("Computer"));
    add(h, tr("CPU"), host.cpuCores ? tr("%1 (%2 cores, %3 threads)").arg(s(host.cpu)).arg(host.cpuCores).arg(host.cpuThreads)
                                    : s(host.cpu));
    add(h, tr("Memory"), host.ramBytes ? formatBytes(static_cast<double>(host.ramBytes)) : QString());
    add(h, tr("Operating system"), s(host.os));
    add(h, tr("Kernel"), s(host.kernel));
    add(h, tr("Display server"), s(host.displayServer));
    add(h, tr("Desktop"), s(host.desktop));
    auto* r = group(tr("Run"));
    add(r, tr("Created (UTC)"), s(m_run.createdUtc));
    add(r, tr("Application"), QStringLiteral("%1 %2 (%3)").arg(s(m_run.appName), s(m_run.appVersion), s(m_run.appRevision)));
    add(r, tr("Suite"), s(m_run.config.suite));
    add(r, tr("Resolution"), QStringLiteral("%1 × %2%3").arg(m_run.config.width).arg(m_run.config.height)
                                 .arg(m_run.config.offscreen ? tr(" (offscreen)") : (m_run.config.fullscreen ? tr(" (fullscreen)") : QString())));
    add(r, tr("Quality / duration"), QStringLiteral("%1 / %2").arg(s(m_run.config.quality), s(m_run.config.duration)));
    add(r, tr("Upscaling"), m_run.config.upscale == "off" ? tr("off (native)") : s(m_run.config.upscale));
    add(r, tr("Looping"), s(m_run.config.loop));
    add(r, tr("Seed"), QString::number(m_run.config.seed));
    add(r, tr("Telemetry"), s(m_run.telemetry.provider));
    add(r, tr("Validation"), m_run.config.validation
                                 ? tr("%1 errors, %2 warnings").arg(m_run.validationErrors).arg(m_run.validationWarnings)
                                 : tr("off"));
    add(r, tr("Comparable key"), s(m_run.scores.comparableKey));
    add(r, tr("Total time"), formatDurationText(m_run.durationS));
    add(r, tr("Command line"), s(m_run.commandLine));
    add(r, tr("File"), m_file);
    for (std::size_t i = 0; i < m_run.deviceLosses.size(); ++i) {
        const auto& d = m_run.deviceLosses[i];
        auto* g2 = group(m_run.deviceLosses.size() > 1 ? tr("Device loss %1").arg(i + 1) : tr("Device loss"));
        add(g2, tr("During"), tr("%1 (loop %2) at %3").arg(s(d.test)).arg(d.loop + 1).arg(formatDurationText(d.t)));
        add(g2, tr("Error"), s(d.message));
        add(g2, tr("Outcome"), d.recovered ? tr("device recreated, run continued") : tr("run stopped"));
        add(g2, tr("Driver fault report"),
            !d.faultReportSupported ? tr("not available (the driver offers no fault reporting, e.g. VK_EXT_device_fault)")
            : d.faultDescription.empty() ? tr("(empty)")
                                         : s(d.faultDescription));
        for (const auto& line : d.faultDetails) add(g2, tr("Fault detail"), s(line));
        if (d.lastSample) {
            const auto& t = *d.lastSample;
            QStringList parts;
            if (std::isfinite(t.tempEdgeC)) parts << tr("GPU %1").arg(formatTemp(t.tempEdgeC));
            if (std::isfinite(t.tempHotspotC)) parts << tr("hotspot %1").arg(formatTemp(t.tempHotspotC));
            if (std::isfinite(t.powerW)) parts << tr("%1 W").arg(t.powerW, 0, 'f', 0);
            if (std::isfinite(t.gpuClockMhz)) parts << tr("%1 MHz").arg(t.gpuClockMhz, 0, 'f', 0);
            if (std::isfinite(t.vramUsedMB)) parts << tr("VRAM %1").arg(formatBytes(t.vramUsedMB * 1048576.0));
            if (t.throttle) parts << tr("throttled: %1").arg(throttleText(t.throttle));
            add(g2, tr("Sensors just before"), parts.isEmpty() ? tr("no sensor data") : parts.join(QStringLiteral(" · ")));
        }
    }
    if (!m_run.warnings.empty()) {
        auto* w = group(tr("Warnings"));
        for (const auto& x : m_run.warnings) add(w, QStringLiteral("⚠"), s(x));
    }
    m_system->expandAll();
    m_system->resizeColumnToContents(0);
}

QString ResultsPage::exportBaseName(const QString& ext) const {
    const QFileInfo fi(m_file);
    const QString dir = m_file.isEmpty() ? Settings::instance().resultsDir() : fi.absolutePath();
    const QString base = m_file.isEmpty() ? QStringLiteral("results") : fi.completeBaseName();
    return QDir(dir).filePath(base + ext);
}

void ResultsPage::setNote(const QString& text) {
    m_note->setText(text);
    m_note->show();
}

void ResultsPage::exportHtml() {
    const QString f = QFileDialog::getSaveFileName(this, tr("Export HTML report"), exportBaseName(QStringLiteral(".html")),
                                                   tr("HTML report (*.html)"));
    if (f.isEmpty()) return;
    std::string err;
    if (!report::writeText(f.toStdString(), report::toHtml(m_run), &err)) {
        setNote(tr("⚠ Export failed: %1").arg(QString::fromStdString(err).toHtmlEscaped()));
        return;
    }
    setNote(tr("Saved the report to <a href=\"%1\">%2</a>.")
                .arg(QUrl::fromLocalFile(f).toString(), f.toHtmlEscaped()));
}

void ResultsPage::exportCsv() {
    const QString f = QFileDialog::getSaveFileName(this, tr("Export CSV"), exportBaseName(QStringLiteral(".csv")),
                                                   tr("CSV (*.csv)"));
    if (f.isEmpty()) return;
    std::string err;
    if (!report::writeText(f.toStdString(), report::toCsv(m_run), &err)) {
        setNote(tr("⚠ Export failed: %1").arg(QString::fromStdString(err).toHtmlEscaped()));
        return;
    }
    setNote(tr("Saved CSV to %1.").arg(f.toHtmlEscaped()));
}

void ResultsPage::saveCopy() {
    const QString f = QFileDialog::getSaveFileName(this, tr("Save results"), exportBaseName(QStringLiteral("-copy.json")),
                                                   tr("Results (*.json)"));
    if (f.isEmpty()) return;
    std::string err;
    if (!results::writeFile(m_run, f.toStdString(), &err)) {
        setNote(tr("⚠ Save failed: %1").arg(QString::fromStdString(err).toHtmlEscaped()));
        return;
    }
    setNote(tr("Saved a copy to %1.").arg(f.toHtmlEscaped()));
}

void ResultsPage::copySummary() {
    QStringList out;
    const auto& g = m_run.system.gpu;
    out << QStringLiteral("%1 %2 — %3").arg(QString::fromUtf8(app::kDisplayName), QString::fromStdString(m_run.appVersion),
                                           QString::fromStdString(m_run.config.suite));
    out << QStringLiteral("GPU: %1 (%2 %3), %4 %5").arg(QString::fromStdString(g.name), QString::fromStdString(g.driverName),
                                                        QString::fromStdString(g.driverVersion),
                                                        apiDisplayName(QString::fromStdString(m_run.config.api)),
                                                        QString::fromStdString(g.apiVersion));
    out << QStringLiteral("CPU: %1 · OS: %2").arg(QString::fromStdString(m_run.system.host.cpu),
                                                   QString::fromStdString(m_run.system.host.os));
    out << QStringLiteral("Settings: %1x%2, %3, %4").arg(m_run.config.width).arg(m_run.config.height)
               .arg(QString::fromStdString(m_run.config.quality), QString::fromStdString(m_run.config.duration));
    if (m_run.scores.overall > 0) out << QStringLiteral("Overall score: %1").arg(m_run.scores.overall, 0, 'f', 0);
    for (const auto& t : m_run.tests) out << QStringLiteral("  ") + QString::fromStdString(results::summarize(t));
    if (m_run.status != results::RunStatus::Completed)
        out << QStringLiteral("Status: %1").arg(QString::fromStdString(results::toString(m_run.status)));
    QApplication::clipboard()->setText(out.join(QLatin1Char('\n')));
    setNote(tr("Copied a text summary to the clipboard."));
}

} // namespace gt::gui
