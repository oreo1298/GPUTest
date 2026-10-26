#include "pages.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>

#include <cmath>

namespace gt::gui {

ResultsPage::ResultsPage(AppState* state, QWidget* parent)
    : Page(tr("Results"), tr("Scores, per-test metrics and frame times. Scores are only comparable between runs with the "
                             "same suite, API, resolution, quality and duration."),
           parent),
      m_state(state) {
    auto* bar = new QHBoxLayout;
    m_header = new QLabel(tr("No results loaded."));
    m_header->setWordWrap(true);
    m_header->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bar->addWidget(m_header, 1);
    auto* open = new QPushButton(themedIcon(QStringLiteral("folder-open")), tr("Open results…"));
    bar->addWidget(open);
    m_content->addLayout(bar);
    m_banner = new QLabel;
    m_banner->setWordWrap(true);
    m_banner->hide();
    m_content->addWidget(m_banner);

    m_scoreBox = new QWidget;
    auto* sl = new QHBoxLayout(m_scoreBox);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->setSpacing(10);
    m_content->addWidget(m_scoreBox);

    m_table = new QTableWidget;
    m_table->setColumnCount(7);
    m_table->setHorizontalHeaderLabels({tr("Test"), tr("Status"), tr("Score"), tr("Avg FPS"), tr("1% low"), tr("p99 ms"),
                                        tr("Verdict")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setAccessibleName(tr("Test results"));
    m_table->setMinimumHeight(180);
    m_content->addWidget(m_table);

    auto* detail = new Card;
    auto* dl = static_cast<QVBoxLayout*>(detail->layout());
    m_testDetails = new QLabel(tr("Select a test to see its details."));
    m_testDetails->setWordWrap(true);
    m_testDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dl->addWidget(m_testDetails);
    m_frameChart = new LineChart;
    m_frameChart->setTitle(tr("Frame times"));
    m_frameChart->setAxisLabels(tr("frame"), tr("ms"));
    m_frameChart->setUnit(QStringLiteral("ms"));
    m_frameChart->addSeries(tr("Frame time"), 0);
    m_frameChart->addSeries(tr("GPU time"), 2);
    m_frameChart->setMinimumHeight(240);
    dl->addWidget(m_frameChart);
    m_content->addWidget(detail);
    m_content->addStretch();

    connect(open, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Open results"), QString(), tr("Results (*.json)"));
        if (!f.isEmpty()) loadFile(f);
    });
    connect(m_table, &QTableWidget::currentCellChanged, this, [this](int row, int, int, int) { showTest(row); });
}

bool ResultsPage::loadFile(const QString& path) {
    std::string err;
    auto r = results::readFile(path.toStdString(), &err);
    if (!r) {
        m_header->setText(tr("Could not read %1: %2").arg(path, QString::fromStdString(err)));
        return false;
    }
    showRun(*r, path);
    return true;
}

void ResultsPage::showRun(const results::RunResult& run, const QString& file) {
    m_run = run;
    m_file = file;
    const auto& g = run.system.gpu;
    m_header->setText(tr("<b>%1</b> · %2 · %3 · %4×%5 %6 / %7<br>%8 — %9")
                          .arg(QString::fromStdString(g.name).toHtmlEscaped(),
                               apiDisplayName(QString::fromStdString(run.config.api)),
                               QString::fromStdString(run.config.suite))
                          .arg(run.config.width)
                          .arg(run.config.height)
                          .arg(QString::fromStdString(run.config.quality), QString::fromStdString(run.config.duration),
                               QString::fromStdString(run.createdUtc), file.toHtmlEscaped()));
    const QString status = QString::fromStdString(results::toString(run.status));
    if (run.status != results::RunStatus::Completed) {
        m_banner->setText(tr("⚠ Run status: %1. %2").arg(status, QString::fromStdString(run.statusMessage)));
        m_banner->show();
    } else {
        m_banner->hide();
    }

    clearLayout(m_scoreBox->layout());
    auto* box = static_cast<QHBoxLayout*>(m_scoreBox->layout());
    auto* overall = new StatTile(tr("Overall score"));
    overall->setValue(run.scores.overall > 0 ? QString::number(run.scores.overall, 'f', 0) : QStringLiteral("—"),
                      std::isfinite(run.scores.perWatt) ? tr("%1 points per watt").arg(run.scores.perWatt, 0, 'f', 1)
                                                        : QString());
    box->addWidget(overall);
    for (const auto& [cat, score] : run.scores.subscores) {
        auto* t = new StatTile(tr("%1 score").arg(QString::fromStdString(cat)));
        t->setValue(QString::number(score, 'f', 0));
        box->addWidget(t);
    }
    if (run.stability.valid) {
        auto* t = new StatTile(tr("Stability"));
        t->setValue(QStringLiteral("%1 %").arg(run.stability.ratio * 100.0, 0, 'f', 1),
                    run.stability.passed ? tr("pass (≥ %1 %)").arg(run.stability.threshold * 100.0, 0, 'f', 0)
                                         : tr("FAIL (< %1 %)").arg(run.stability.threshold * 100.0, 0, 'f', 0));
        t->setStatus(run.stability.passed ? 0 : 3);
        box->addWidget(t);
    }
    box->addStretch();

    m_table->setRowCount(static_cast<int>(run.tests.size()));
    for (int i = 0; i < static_cast<int>(run.tests.size()); ++i) {
        const auto& t = run.tests[static_cast<std::size_t>(i)];
        auto set = [&](int col, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            if (col >= 2 && col <= 5) item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_table->setItem(i, col, item);
        };
        QString name = QString::fromStdString(t.name);
        if (run.loops.size() > 1) name += tr(" (loop %1)").arg(t.loop + 1);
        set(0, name);
        QString st = QString::fromStdString(results::toString(t.status));
        if (!t.message.empty()) st += QStringLiteral(": ") + QString::fromStdString(t.message);
        set(1, st);
        m_table->item(i, 1)->setToolTip(st);
        set(2, t.scored ? QString::number(t.score, 'f', 0) : QStringLiteral("—"));
        set(3, t.frameStats ? QString::number(t.frameStats->avgFps, 'f', 1) : QStringLiteral("—"));
        set(4, t.frameStats ? QString::number(t.frameStats->low1Fps, 'f', 1) : QStringLiteral("—"));
        set(5, t.frameStats ? QString::number(t.frameStats->p99Ms, 'f', 2) : QStringLiteral("—"));
        QString verdict;
        if (t.verdict != results::Verdict::None)
            verdict = QString::fromStdString(results::toString(t.verdict)).toUpper() + QStringLiteral(": ") +
                      QString::fromStdString(t.verdictMessage);
        set(6, verdict);
        m_table->item(i, 6)->setToolTip(verdict);
    }
    m_table->resizeColumnsToContents();
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    if (!run.tests.empty()) m_table->selectRow(0);
    showTest(run.tests.empty() ? -1 : 0);
}

void ResultsPage::showTest(int row) {
    if (row < 0 || row >= static_cast<int>(m_run.tests.size())) {
        m_frameChart->clearData();
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
    if (std::isfinite(t.gpuBusyPercent)) lines << tr("GPU busy %1 %").arg(t.gpuBusyPercent, 0, 'f', 1);
    QStringList metrics;
    for (const auto& m : t.metrics)
        metrics << QStringLiteral("%1: %2 %3").arg(QString::fromStdString(m.label)).arg(m.value, 0, 'g', 5).arg(QString::fromStdString(m.unit));
    if (!metrics.isEmpty()) lines << metrics.join(QStringLiteral(" · ")).toHtmlEscaped();
    if (t.timeLimited) lines << tr("⚠ Hit the time limit — partial result.");
    m_testDetails->setText(lines.join(QStringLiteral("<br>")));
    QVector<QPointF> ft, gt;
    for (std::size_t i = 0; i < t.frameTimesMs.size(); ++i) ft.push_back(QPointF(static_cast<double>(i), t.frameTimesMs[i]));
    for (std::size_t i = 0; i < t.gpuTimesMs.size(); ++i) gt.push_back(QPointF(static_cast<double>(i), t.gpuTimesMs[i]));
    m_frameChart->resetZoom();
    m_frameChart->setPoints(0, ft);
    m_frameChart->setPoints(1, gt);
    QVector<LineChart::RefLine> refs;
    if (t.frameStats) refs.push_back({t.frameStats->p99Ms, tr("p99 %1 ms").arg(t.frameStats->p99Ms, 0, 'f', 2)});
    m_frameChart->setRefLines(refs);
}

} // namespace gt::gui
