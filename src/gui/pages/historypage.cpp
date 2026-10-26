#include "pages.h"

#include "history.h"
#include "settings.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QUrl>

namespace gt::gui {

namespace {

QString prettyDate(const QString& utc) {
    QDateTime dt = QDateTime::fromString(utc, Qt::ISODate);
    if (!dt.isValid()) return utc;
    return dt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

} // namespace

HistoryPage::HistoryPage(AppState* state, QWidget* parent)
    : Page(tr("History"), tr("Every run saved in the results folder is indexed here. Select one run to open it or two "
                             "runs to compare them."),
           parent),
      m_state(state) {
    auto* bar = new QHBoxLayout;
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("Filter by GPU, suite, API or date…"));
    m_filter->setClearButtonEnabled(true);
    m_filter->addAction(themedIcon(QStringLiteral("search")), QLineEdit::LeadingPosition);
    m_filter->setAccessibleName(tr("Filter runs"));
    bar->addWidget(m_filter, 1);
    auto* import = new QPushButton(themedIcon(QStringLiteral("plus")), tr("Import…"));
    import->setToolTip(tr("Add results files from another folder to the history"));
    bar->addWidget(import);
    auto* rescan = new QPushButton(themedIcon(QStringLiteral("refresh-cw")), tr("Rescan"));
    rescan->setToolTip(tr("Look for new or deleted files in the results folder"));
    bar->addWidget(rescan);
    auto* folder = new QPushButton(themedIcon(QStringLiteral("folder-open")), tr("Results folder"));
    bar->addWidget(folder);
    m_content->addLayout(bar);

    m_table = new QTableWidget;
    m_table->setColumnCount(8);
    m_table->setHorizontalHeaderLabels(
        {tr("Date"), tr("Suite"), tr("GPU"), tr("API"), tr("Resolution"), tr("Quality / duration"), tr("Score"), tr("Status")});
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(false);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->setMinimumHeight(260);
    m_table->setAccessibleName(tr("Saved runs"));
    m_content->addWidget(m_table, 1);

    auto* actions = new QHBoxLayout;
    m_status = makeHint(QString());
    actions->addWidget(m_status, 1);
    m_open = new QPushButton(themedIcon(QStringLiteral("chart-column")), tr("Open"));
    m_compare = new QPushButton(themedIcon(QStringLiteral("git-compare")), tr("Compare"));
    m_remove = new QPushButton(themedIcon(QStringLiteral("x")), tr("Forget"));
    m_remove->setToolTip(tr("Remove from the history but keep the results file"));
    m_delete = new QPushButton(themedIcon(QStringLiteral("trash")), tr("Delete files"));
    m_delete->setToolTip(tr("Delete the results files from disk"));
    for (auto* b : {m_open, m_compare, m_remove, m_delete}) actions->addWidget(b);
    m_content->addLayout(actions);

    auto* trendCard = new Card;
    auto* tl = static_cast<QVBoxLayout*>(trendCard->layout());
    m_trend = new LineChart;
    m_trend->setTitle(tr("Score history"));
    m_trend->setAxisLabels(tr("run"), tr("score"));
    m_trend->addSeries(tr("Overall score"), 0);
    m_trend->setYStartsAtZero(false);
    m_trend->setXInteger(true);
    m_trend->setEmptyText(tr("Select a run to see the scores of all comparable runs over time."));
    m_trend->setMinimumHeight(220);
    tl->addWidget(m_trend);
    m_trendNote = makeHint(QString());
    tl->addWidget(m_trendNote);
    m_content->addWidget(trendCard);

    connect(m_filter, &QLineEdit::textChanged, this, &HistoryPage::refresh);
    connect(import, &QPushButton::clicked, this, &HistoryPage::importFiles);
    connect(rescan, &QPushButton::clicked, this, [] { History::instance().scanFolder(Settings::instance().resultsDir()); });
    connect(folder, &QPushButton::clicked, this,
            [] { QDesktopServices::openUrl(QUrl::fromLocalFile(Settings::instance().resultsDir())); });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        updateButtons();
        updateTrend();
    });
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (row >= 0 && row < m_entries.size()) Q_EMIT openRun(m_entries[row].path);
    });
    connect(m_open, &QPushButton::clicked, this, [this] {
        const auto rows = selectedRows();
        if (!rows.isEmpty()) Q_EMIT openRun(m_entries[rows.front()].path);
    });
    connect(m_compare, &QPushButton::clicked, this, [this] {
        const auto rows = selectedRows();
        // Older run is the baseline (A), newer the candidate (B).
        if (rows.size() == 2) Q_EMIT compareRuns(m_entries[rows[1]].path, m_entries[rows[0]].path);
    });
    connect(m_remove, &QPushButton::clicked, this, [this] { removeSelected(false); });
    connect(m_delete, &QPushButton::clicked, this, [this] { removeSelected(true); });
    connect(&History::instance(), &History::changed, this, &HistoryPage::refresh);
    refresh();
}

QVector<int> HistoryPage::selectedRows() const {
    QVector<int> rows;
    for (const auto& r : m_table->selectionModel()->selectedRows()) rows.push_back(r.row());
    std::sort(rows.begin(), rows.end());
    return rows;
}

void HistoryPage::refresh() {
    QStringList selected;
    for (int r : selectedRows())
        if (r < m_entries.size()) selected << m_entries[r].path;
    m_entries = History::instance().entries(m_filter->text().trimmed());
    m_table->setRowCount(static_cast<int>(m_entries.size()));
    for (int i = 0; i < m_entries.size(); ++i) {
        const auto& e = m_entries[i];
        const QStringList cells{prettyDate(e.created),
                                e.suite,
                                e.gpu,
                                apiDisplayName(e.api),
                                QStringLiteral("%1×%2").arg(e.width).arg(e.height),
                                QStringLiteral("%1 / %2").arg(e.quality, e.duration),
                                e.overall > 0 ? QString::number(e.overall, 'f', 0) : QStringLiteral("—"),
                                e.status};
        for (int c = 0; c < cells.size(); ++c) {
            auto* it = new QTableWidgetItem(cells[c]);
            if (c == 6) it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            it->setToolTip(c == 2 ? QStringLiteral("%1\n%2").arg(e.gpu, e.driver) : e.path);
            m_table->setItem(i, c, it);
        }
    }
    m_table->resizeColumnsToContents();
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->clearSelection();
    for (int i = 0; i < m_entries.size(); ++i)
        if (selected.contains(m_entries[i].path)) m_table->selectionModel()->select(
                m_table->model()->index(i, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    if (!History::instance().isOpen())
        m_status->setText(tr("⚠ The history database could not be opened; runs can still be opened from files."));
    else if (m_entries.isEmpty())
        m_status->setText(m_filter->text().isEmpty() ? tr("No saved runs yet. Results of every run appear here.")
                                                     : tr("No runs match the filter."));
    updateButtons();
    updateTrend();
}

void HistoryPage::updateButtons() {
    const auto rows = selectedRows();
    m_open->setEnabled(rows.size() == 1);
    m_compare->setEnabled(rows.size() == 2);
    m_remove->setEnabled(!rows.isEmpty());
    m_delete->setEnabled(!rows.isEmpty());
    if (!m_entries.isEmpty()) {
        if (rows.isEmpty())
            m_status->setText((m_entries.size() == 1 ? tr("1 run.") : tr("%1 runs.").arg(m_entries.size())) +
                              QLatin1Char(' ') + tr("Select two runs to compare them."));
        else
            m_status->setText(tr("%1 selected.").arg(rows.size()));
    }
}

void HistoryPage::updateTrend() {
    const auto rows = selectedRows();
    if (rows.isEmpty()) {
        m_trend->clearData();
        m_trendNote->setText(QString());
        return;
    }
    const QString key = m_entries[rows.front()].comparableKey;
    // All comparable runs (ignoring the text filter), oldest first.
    auto all = History::instance().entries();
    std::reverse(all.begin(), all.end());
    QVector<QPointF> pts;
    QStringList gpus;
    for (const auto& e : all) {
        if (e.comparableKey != key || e.overall <= 0) continue;
        pts.push_back(QPointF(static_cast<double>(pts.size() + 1), e.overall));
        if (!gpus.contains(e.gpu)) gpus << e.gpu;
    }
    m_trend->resetZoom();
    m_trend->setPoints(0, pts);
    m_trendNote->setText(pts.size() < 2 ? tr("Only one run with these settings (%1) so far.").arg(key)
                                        : tr("%1 runs with the same settings (%2) on %3.")
                                              .arg(pts.size())
                                              .arg(key, gpus.join(QStringLiteral(", "))));
}

void HistoryPage::importFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Import results"), QString(), tr("Results (*.json)"));
    int ok = 0;
    QStringList failed;
    for (const QString& f : files) {
        QString err;
        if (History::instance().importFile(f, &err)) ++ok;
        else failed << QStringLiteral("%1: %2").arg(f, err);
    }
    if (!failed.isEmpty())
        QMessageBox::warning(this, tr("Import"), tr("Imported: %1. These files could not be read:\n%2")
                                                     .arg(ok)
                                                     .arg(failed.join(QLatin1Char('\n'))));
}

void HistoryPage::removeSelected(bool deleteFiles) {
    const auto rows = selectedRows();
    if (rows.isEmpty()) return;
    if (deleteFiles) {
        const auto r = QMessageBox::question(
            this, tr("Delete files"),
            rows.size() == 1 ? tr("Delete this results file from disk? This cannot be undone.")
                             : tr("Delete %1 results files from disk? This cannot be undone.").arg(rows.size()));
        if (r != QMessageBox::Yes) return;
    }
    QVector<HistoryEntry> victims;
    for (int r : rows) victims.push_back(m_entries[r]);
    for (const auto& e : victims) {
        if (deleteFiles) QFile::remove(e.path);
        History::instance().remove(e.id);
    }
}

} // namespace gt::gui
