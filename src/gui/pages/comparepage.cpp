#include "pages.h"

#include "history.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTableWidget>

#include <cmath>

namespace gt::gui {

namespace {

constexpr double kNoiseBandPercent = 2.0; ///< differences below this are shown as "about the same"

QString formatPercentChange(double pct) {
    if (std::abs(pct) < 0.05) return QStringLiteral("0.0 %");
    return QStringLiteral("%1%2 %").arg(pct > 0 ? QStringLiteral("+") : QStringLiteral("−")).arg(std::abs(pct), 0, 'f', 1);
}

QIcon deltaIcon(int direction, bool better) {
    // A small triangle in the status colour; the sign in the text carries the same
    // information, so the colour is never the only cue.
    const qreal dpr = 2.0;
    QPixmap pm(QSize(12, 12) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    if (direction == 0) return QIcon(pm);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(ThemeManager::instance().status(better ? ThemeManager::Status::Good : ThemeManager::Status::Critical));
    QPainterPath path;
    if (direction > 0) {
        path.moveTo(6, 2);
        path.lineTo(11, 10);
        path.lineTo(1, 10);
    } else {
        path.moveTo(1, 2);
        path.lineTo(11, 2);
        path.lineTo(6, 10);
    }
    path.closeSubpath();
    p.drawPath(path);
    return QIcon(pm);
}

/// Relative change of b against a, as a table cell ("+5.2 %"), with an arrow icon
/// coloured by whether the change is an improvement.
QTableWidgetItem* deltaItem(double a, double b, bool higherIsBetter) {
    auto* it = new QTableWidgetItem;
    it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    if (!(std::isfinite(a) && std::isfinite(b)) || a <= 0) {
        it->setText(QStringLiteral("—"));
        return it;
    }
    const double pct = (b - a) / a * 100.0;
    const bool noise = std::abs(pct) < kNoiseBandPercent;
    it->setText(formatPercentChange(pct));
    const bool better = higherIsBetter ? pct > 0 : pct < 0;
    it->setIcon(deltaIcon(noise ? 0 : (pct > 0 ? 1 : -1), better));
    it->setToolTip(noise ? QCoreApplication::translate("ComparePage", "Within ±%1 %: about the same").arg(kNoiseBandPercent)
                         : (better ? QCoreApplication::translate("ComparePage", "B is better")
                                   : QCoreApplication::translate("ComparePage", "B is worse")));
    return it;
}

QTableWidgetItem* numItem(double v, int decimals) {
    auto* it = new QTableWidgetItem(std::isfinite(v) ? QString::number(v, 'f', decimals) : QStringLiteral("—"));
    it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return it;
}

QString runLabel(const HistoryEntry& e) {
    QDateTime dt = QDateTime::fromString(e.created, Qt::ISODate);
    const QString when = dt.isValid() ? dt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : e.created;
    return QStringLiteral("%1 · %2 · %3 · %4")
        .arg(when, e.suite, e.overall > 0 ? QString::number(e.overall, 'f', 0) : QStringLiteral("—"), e.gpu);
}

QWidget* swatch(int slot) {
    auto* l = new QLabel;
    l->setFixedSize(12, 12);
    auto paint = [l, slot] {
        QPixmap pm(QSize(12, 12) * 2);
        pm.setDevicePixelRatio(2);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(ThemeManager::instance().series(slot));
        p.drawRoundedRect(QRectF(0, 0, 12, 12), 3, 3);
        l->setPixmap(pm);
    };
    paint();
    QObject::connect(&ThemeManager::instance(), &ThemeManager::themeChanged, l, paint);
    return l;
}

} // namespace

ComparePage::ComparePage(AppState* state, QWidget* parent)
    : Page(tr("Compare"), tr("Put two runs side by side. B is compared against A; differences under ±2 % are within "
                             "normal run-to-run variation."),
           parent),
      m_state(state) {
    auto* pick = new QHBoxLayout;
    pick->addWidget(swatch(0));
    pick->addWidget(new QLabel(tr("A")));
    m_a = new QComboBox;
    m_a->setAccessibleName(tr("Run A (baseline)"));
    m_a->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_a->setMinimumContentsLength(24);
    pick->addWidget(m_a, 1);
    m_swap = new QPushButton(themedIcon(QStringLiteral("refresh-cw")), tr("Swap"));
    m_swap->setToolTip(tr("Swap A and B"));
    pick->addWidget(m_swap);
    pick->addWidget(swatch(1));
    pick->addWidget(new QLabel(tr("B")));
    m_b = new QComboBox;
    m_b->setAccessibleName(tr("Run B (compared)"));
    m_b->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_b->setMinimumContentsLength(24);
    pick->addWidget(m_b, 1);
    m_content->addLayout(pick);

    m_warning = new QLabel;
    m_warning->setWordWrap(true);
    m_warning->hide();
    m_content->addWidget(m_warning);
    m_empty = makeHint(tr("Pick two runs above, or select two runs on the History page and press Compare."));
    m_content->addWidget(m_empty);

    m_body = new QWidget;
    auto* bl = new QVBoxLayout(m_body);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(12);
    auto* infos = new QHBoxLayout;
    infos->setSpacing(10);
    for (int i = 0; i < 2; ++i) {
        auto* card = new Card;
        auto* cl = static_cast<QVBoxLayout*>(card->layout());
        auto* head = new QHBoxLayout;
        head->addWidget(swatch(i));
        head->addWidget(makeSubtitle(i == 0 ? tr("A — baseline") : tr("B — compared")), 1);
        cl->addLayout(head);
        auto* info = new QLabel;
        info->setWordWrap(true);
        info->setTextInteractionFlags(Qt::TextSelectableByMouse);
        cl->addWidget(info);
        (i == 0 ? m_infoA : m_infoB) = info;
        infos->addWidget(card, 1);
    }
    bl->addLayout(infos);
    m_tiles = new QWidget;
    auto* tl = new QHBoxLayout(m_tiles);
    tl->setContentsMargins(0, 0, 0, 0);
    tl->setSpacing(10);
    bl->addWidget(m_tiles);

    m_table = new QTableWidget;
    m_table->setColumnCount(10);
    m_table->setHorizontalHeaderLabels({tr("Test"), tr("Score A"), tr("Score B"), tr("Δ score"), tr("Result A"), tr("Result B"),
                                        tr("Δ result"), tr("p99 ms A"), tr("p99 ms B"), tr("Δ p99")});
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setAccessibleName(tr("Per-test comparison"));
    bl->addWidget(m_table);

    auto* detail = new Card;
    m_frameCard = detail;
    auto* dl = static_cast<QVBoxLayout*>(detail->layout());
    auto* charts = new QHBoxLayout;
    charts->setSpacing(10);
    m_frames = new LineChart;
    m_frames->setTitle(tr("Frame times of the selected test"));
    m_frames->setAxisLabels(tr("frame"), tr("ms"));
    m_frames->setUnit(QStringLiteral("ms"));
    m_frames->addSeries(tr("A"), 0);
    m_frames->addSeries(tr("B"), 1);
    m_frames->setMinimumHeight(250);
    charts->addWidget(m_frames, 3);
    m_hist = new HistogramChart;
    m_hist->setTitle(tr("Frame time distribution"));
    m_hist->setUnit(QStringLiteral("ms"));
    m_hist->setMinimumHeight(250);
    charts->addWidget(m_hist, 2);
    dl->addLayout(charts);
    bl->addWidget(detail);

    bl->addWidget(makeSubtitle(tr("Sensors (whole run)")));
    m_telemetry = new QTableWidget;
    m_telemetry->setColumnCount(7);
    m_telemetry->setHorizontalHeaderLabels(
        {tr("Sensor"), tr("Average A"), tr("Average B"), tr("Δ average"), tr("Peak A"), tr("Peak B"), tr("Unit")});
    m_telemetry->verticalHeader()->hide();
    m_telemetry->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_telemetry->setSelectionMode(QAbstractItemView::NoSelection);
    m_telemetry->setFocusPolicy(Qt::NoFocus);
    m_telemetry->setAlternatingRowColors(true);
    m_telemetry->setAccessibleName(tr("Sensor comparison"));
    bl->addWidget(m_telemetry);
    bl->addStretch(); // spare height goes below the tables, not into the headings
    m_content->addWidget(m_body);
    m_content->addStretch();
    m_body->hide();

    auto changed = [this] {
        if (m_updating) return;
        load();
    };
    connect(m_a, qOverload<int>(&QComboBox::currentIndexChanged), this, changed);
    connect(m_b, qOverload<int>(&QComboBox::currentIndexChanged), this, changed);
    connect(m_swap, &QPushButton::clicked, this, [this] { setRuns(m_fileB, m_fileA); });
    connect(m_table, &QTableWidget::currentCellChanged, this, [this](int row, int, int, int) { showTest(row); });
    connect(&History::instance(), &History::changed, this, &ComparePage::refreshChoices);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
        if (m_runA && m_runB) rebuild();
    });
    refreshChoices();
}

void ComparePage::refreshChoices() {
    m_updating = true;
    const QString a = m_a->currentData().toString(), b = m_b->currentData().toString();
    m_a->clear();
    m_b->clear();
    m_a->addItem(tr("Choose a run…"), QString());
    m_b->addItem(tr("Choose a run…"), QString());
    for (const auto& e : History::instance().entries()) {
        m_a->addItem(runLabel(e), e.path);
        m_b->addItem(runLabel(e), e.path);
        m_a->setItemData(m_a->count() - 1, e.path, Qt::ToolTipRole);
        m_b->setItemData(m_b->count() - 1, e.path, Qt::ToolTipRole);
    }
    auto select = [](QComboBox* c, const QString& path) {
        if (path.isEmpty()) return;
        int i = c->findData(path);
        if (i < 0) {
            c->addItem(QFileInfo(path).fileName(), path);
            i = c->count() - 1;
        }
        c->setCurrentIndex(i);
    };
    select(m_a, a);
    select(m_b, b);
    m_updating = false;
}

void ComparePage::setRuns(const QString& a, const QString& b) {
    m_updating = true;
    for (auto [combo, path] : {std::pair{m_a, a}, std::pair{m_b, b}}) {
        if (path.isEmpty()) {
            combo->setCurrentIndex(0);
            continue;
        }
        int i = combo->findData(path);
        if (i < 0) {
            combo->addItem(QFileInfo(path).fileName(), path);
            i = combo->count() - 1;
        }
        combo->setCurrentIndex(i);
    }
    m_updating = false;
    load();
}

void ComparePage::load() {
    m_fileA = m_a->currentData().toString();
    m_fileB = m_b->currentData().toString();
    m_runA.reset();
    m_runB.reset();
    QStringList errors;
    auto read = [&](const QString& f, std::optional<results::RunResult>& out) {
        if (f.isEmpty()) return;
        std::string err;
        out = results::readFile(f.toStdString(), &err);
        if (!out) errors << tr("Could not read %1: %2").arg(f, QString::fromStdString(err));
    };
    read(m_fileA, m_runA);
    read(m_fileB, m_runB);
    if (!errors.isEmpty()) {
        m_warning->setText(QStringLiteral("⚠ ") + errors.join(QStringLiteral("<br>")).toHtmlEscaped());
        m_warning->show();
    }
    if (!m_runA || !m_runB) {
        if (errors.isEmpty()) m_warning->hide();
        m_body->hide();
        m_empty->show();
        return;
    }
    m_empty->hide();
    rebuild();
    m_body->show();
}

void ComparePage::rebuild() {
    const auto& A = *m_runA;
    const auto& B = *m_runB;

    // Comparability: which of suite / API / resolution / quality / duration / upscaling differ (native runs
    // have no upscaling part in their key).
    QStringList ka = QString::fromStdString(A.scores.comparableKey).split(QLatin1Char('|'));
    QStringList kb = QString::fromStdString(B.scores.comparableKey).split(QLatin1Char('|'));
    if (ka.size() == 5) ka << QStringLiteral("off");
    if (kb.size() == 5) kb << QStringLiteral("off");
    const QStringList names{tr("suite"), tr("API"), tr("resolution"), tr("quality"), tr("duration"), tr("upscaling")};
    QStringList diffs;
    for (int i = 0; i < names.size(); ++i) {
        const QString va = i < ka.size() ? ka[i] : QString(), vb = i < kb.size() ? kb[i] : QString();
        if (va != vb) diffs << tr("%1 (%2 vs %3)").arg(names[i], va, vb);
    }
    QStringList warn;
    if (!diffs.isEmpty())
        warn << tr("⚠ Different settings: %1. Scores are not directly comparable; per-test numbers still are where the "
                   "same test ran.")
                    .arg(diffs.join(QStringLiteral(", ")));
    if (m_fileA == m_fileB) warn << tr("A and B are the same file.");
    if (A.status != results::RunStatus::Completed || B.status != results::RunStatus::Completed)
        warn << tr("⚠ At least one run did not complete (A: %1, B: %2).")
                    .arg(QString::fromStdString(results::toString(A.status)), QString::fromStdString(results::toString(B.status)));
    m_warning->setText(warn.join(QStringLiteral("<br>")));
    m_warning->setVisible(!warn.isEmpty());

    auto info = [](const results::RunResult& r, const QString& file) {
        const auto& g = r.system.gpu;
        return QStringLiteral("<b>%1</b><br>%2 %3 · %4 %5<br>%6 · %7×%8 · %9 / %10<br>%11 · %12")
            .arg(QString::fromStdString(g.name).toHtmlEscaped(), QString::fromStdString(g.driverName).toHtmlEscaped(),
                 QString::fromStdString(g.driverVersion).toHtmlEscaped(), apiDisplayName(QString::fromStdString(r.config.api)),
                 QString::fromStdString(g.apiVersion), QString::fromStdString(r.config.suite).toHtmlEscaped())
            .arg(r.config.width)
            .arg(r.config.height)
            .arg(QString::fromStdString(r.config.quality), QString::fromStdString(r.config.duration),
                 QString::fromStdString(r.createdUtc), QFileInfo(file).fileName().toHtmlEscaped());
    };
    m_infoA->setText(info(A, m_fileA));
    m_infoB->setText(info(B, m_fileB));

    clearLayout(m_tiles->layout());
    auto* tl = static_cast<QHBoxLayout*>(m_tiles->layout());
    auto addTile = [&](const QString& caption, double a, double b, int decimals, bool higherIsBetter, const QString& unit) {
        if (!(std::isfinite(a) && std::isfinite(b)) || a <= 0 || b <= 0) return;
        auto* t = new StatTile(caption);
        const double pct = (b - a) / a * 100.0;
        t->setValue(formatPercentChange(pct),
                    tr("A %1 → B %2 %3").arg(a, 0, 'f', decimals).arg(b, 0, 'f', decimals).arg(unit));
        if (std::abs(pct) >= kNoiseBandPercent)
            t->setStatus(static_cast<int>((higherIsBetter ? pct > 0 : pct < 0) ? ThemeManager::Status::Good
                                                                                : ThemeManager::Status::Critical));
        tl->addWidget(t);
    };
    addTile(tr("Overall score"), A.scores.overall, B.scores.overall, 0, true, QString());
    for (const auto& [cat, sa] : A.scores.subscores)
        if (auto it = B.scores.subscores.find(cat); it != B.scores.subscores.end())
            addTile(tr("%1 score").arg(capitalized(QString::fromStdString(cat))), sa, it->second, 0, true, QString());
    auto summary = [](const results::RunResult& r, const char* key, bool peak) {
        auto it = r.telemetry.summary.find(key);
        return it != r.telemetry.summary.end() && it->second.count ? (peak ? it->second.max : it->second.avg) : NAN;
    };
    addTile(tr("Average board power"), summary(A, "power_w", false), summary(B, "power_w", false), 0, false,
            QStringLiteral("W"));
    addTile(tr("Points per watt"), A.scores.perWatt, B.scores.perWatt, 1, true, QString());
    m_tiles->setVisible(tl->count() > 0); // runs without scores (unscored tests only) get no empty row
    tl->addStretch();

    // Tests matched by id; scores averaged over loops, frame data from the first loop.
    m_rows.clear();
    auto addRuns = [this](const results::RunResult& r, bool isA) {
        for (const auto& t : r.tests) {
            const QString id = QString::fromStdString(t.id);
            auto it = std::find_if(m_rows.begin(), m_rows.end(), [&](const Row& x) { return x.id == id; });
            if (it == m_rows.end()) {
                m_rows.push_back({id, QString::fromStdString(t.name)});
                it = m_rows.end() - 1;
            }
            const results::TestResult*& slot = isA ? it->a : it->b;
            if (!slot) slot = &t;
        }
    };
    addRuns(A, true);
    addRuns(B, false);
    auto meanScore = [](const results::RunResult& r, const QString& id, bool* scored) {
        double sum = 0;
        int n = 0;
        for (const auto& t : r.tests)
            if (t.scored && QString::fromStdString(t.id) == id) {
                sum += t.score;
                ++n;
            }
        *scored = n > 0;
        return n ? sum / n : NAN;
    };
    m_table->setRowCount(static_cast<int>(m_rows.size()));
    for (int i = 0; i < m_rows.size(); ++i) {
        auto& row = m_rows[i];
        row.scoreA = meanScore(A, row.id, &row.scoredA);
        row.scoreB = meanScore(B, row.id, &row.scoredB);
        auto fs = [](const results::TestResult* t, auto f) {
            return t && t->frameStats ? static_cast<double>(f(*t->frameStats)) : NAN;
        };
        // Headline metric: FPS for scenes, the test's primary metric (TFLOPS, GB/s, ...) otherwise.
        auto headline = [](const results::TestResult* t) -> const results::Metric* {
            if (!t) return nullptr;
            if (const auto* m = t->headline()) return m;
            return t->metric("avg_fps");
        };
        const results::Metric* ha = headline(row.a);
        const results::Metric* hb = headline(row.b);
        const double fpsA = ha ? ha->value : NAN;
        const double fpsB = hb ? hb->value : NAN;
        const QString unit = QString::fromStdString((ha ? ha : hb) ? (ha ? ha : hb)->unit : std::string());
        const bool higherBetter = (ha ? ha : hb) ? (ha ? ha : hb)->higherIsBetter : true;
        const double p99A = fs(row.a, [](const FrameStats& f) { return f.p99Ms; });
        const double p99B = fs(row.b, [](const FrameStats& f) { return f.p99Ms; });
        QString name = row.name;
        if (!row.a) name += tr(" (only in B)");
        if (!row.b) name += tr(" (only in A)");
        m_table->setItem(i, 0, new QTableWidgetItem(name));
        m_table->setItem(i, 1, numItem(row.scoreA, 0));
        m_table->setItem(i, 2, numItem(row.scoreB, 0));
        m_table->setItem(i, 3, deltaItem(row.scoreA, row.scoreB, true));
        auto valueItem = [&](double v) {
            auto* it = numItem(v, std::abs(v) >= 100 ? 0 : std::abs(v) >= 10 ? 1 : 2);
            if (std::isfinite(v) && !unit.isEmpty()) it->setText(it->text() + QLatin1Char(' ') + unit);
            return it;
        };
        m_table->setItem(i, 4, valueItem(fpsA));
        m_table->setItem(i, 5, valueItem(fpsB));
        m_table->setItem(i, 6, deltaItem(fpsA, fpsB, higherBetter));
        m_table->setItem(i, 7, numItem(p99A, 2));
        m_table->setItem(i, 8, numItem(p99B, 2));
        m_table->setItem(i, 9, deltaItem(p99A, p99B, false));
    }
    m_table->resizeColumnsToContents();
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->setFixedHeight(m_table->horizontalHeader()->sizeHint().height() + 6 +
                            std::clamp(static_cast<int>(m_rows.size()), 1, 10) * m_table->verticalHeader()->defaultSectionSize());
    if (!m_rows.isEmpty()) m_table->selectRow(0);
    showTest(m_rows.isEmpty() ? -1 : 0);

    // Sensors: average + peak per channel present in either run.
    const auto& channels = results::telemetryChannels();
    struct TRow {
        QString label, unit;
        double avgA, avgB, peakA, peakB;
        bool higherIsBetter;
    };
    QVector<TRow> trows;
    for (std::size_t c = 0; c < channels.size(); ++c) {
        const char* key = channels[c].c_str();
        if (channels[c] == "power_limit_w" || channels[c] == "vram_total_mb") continue;
        const double aa = summary(A, key, false), ab = summary(B, key, false);
        if (!std::isfinite(aa) && !std::isfinite(ab)) continue;
        const auto& ci = results::telemetryChannelInfo()[c];
        const bool clocks = channels[c].find("clock") != std::string::npos;
        trows.push_back({QString::fromStdString(ci.label), QString::fromStdString(ci.unit), aa, ab, summary(A, key, true),
                         summary(B, key, true), clocks});
    }
    m_telemetry->setRowCount(static_cast<int>(trows.size()));
    for (int i = 0; i < trows.size(); ++i) {
        const auto& r = trows[i];
        const int d = r.unit == QLatin1String("MB") || r.avgA >= 100 ? 0 : 1;
        m_telemetry->setItem(i, 0, new QTableWidgetItem(r.label));
        m_telemetry->setItem(i, 1, numItem(r.avgA, d));
        m_telemetry->setItem(i, 2, numItem(r.avgB, d));
        auto* delta = deltaItem(r.avgA, r.avgB, r.higherIsBetter);
        if (!r.higherIsBetter && r.unit == QLatin1String("%")) delta->setIcon(QIcon()); // load: neither good nor bad
        m_telemetry->setItem(i, 3, delta);
        m_telemetry->setItem(i, 4, numItem(r.peakA, d));
        m_telemetry->setItem(i, 5, numItem(r.peakB, d));
        m_telemetry->setItem(i, 6, new QTableWidgetItem(r.unit));
    }
    m_telemetry->resizeColumnsToContents();
    m_telemetry->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_telemetry->setFixedHeight(m_telemetry->horizontalHeader()->sizeHint().height() + 6 +
                                std::max(1, static_cast<int>(trows.size())) * m_telemetry->verticalHeader()->defaultSectionSize());
    m_telemetry->setVisible(!trows.isEmpty());
}

void ComparePage::showTest(int row) {
    m_frames->clearData();
    m_hist->clear();
    if (row < 0 || row >= m_rows.size()) return;
    const auto& r = m_rows[row];
    auto pts = [](const results::TestResult* t) {
        QVector<QPointF> out;
        if (!t) return out;
        for (std::size_t i = 0; i < t->frameTimesMs.size(); ++i) out.push_back(QPointF(static_cast<double>(i), t->frameTimesMs[i]));
        return out;
    };
    m_frames->setTitle(tr("Frame times — %1").arg(r.name));
    m_frames->setPoints(0, pts(r.a));
    m_frames->setPoints(1, pts(r.b));
    std::vector<double> va, vb, all;
    if (r.a) va.assign(r.a->frameTimesMs.begin(), r.a->frameTimesMs.end());
    if (r.b) vb.assign(r.b->frameTimesMs.begin(), r.b->frameTimesMs.end());
    all = va;
    all.insert(all.end(), vb.begin(), vb.end());
    m_frameCard->setVisible(!all.empty()); // as on the Results page: no charts for tests without frames
    if (all.empty()) return;
    const auto layout = frameTimeHistogram(all, 36);
    const auto bins = layout.counts.size();
    if (!va.empty()) m_hist->addSeries(tr("A"), 0, histogram(va, layout.lo, layout.binWidth, bins));
    if (!vb.empty()) m_hist->addSeries(tr("B"), 1, histogram(vb, layout.lo, layout.binWidth, bins));
}

} // namespace gt::gui
