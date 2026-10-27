#include "pages.h"

#include "settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>

#include <fstream>

namespace gt::gui {

TestsPage::TestsPage(AppState* state, QWidget* parent)
    : Page(tr("Tests"), tr("Run a preset suite or build your own. Tests your GPU or API cannot run are greyed out with "
                           "the reason."),
           parent),
      m_state(state) {
    m_content->addWidget(makeSubtitle(tr("Suites")));
    m_suiteBox = new QWidget;
    auto* sg = new QGridLayout(m_suiteBox);
    sg->setContentsMargins(0, 0, 0, 0);
    sg->setSpacing(10);
    m_content->addWidget(m_suiteBox);

    // --- settings
    auto* settingsCard = new Card;
    auto* sl = static_cast<QVBoxLayout*>(settingsCard->layout());
    sl->addWidget(makeSubtitle(tr("Settings")));
    auto* form = new QGridLayout;
    form->setHorizontalSpacing(14);
    form->setVerticalSpacing(8);
    auto addRow = [&](int r, int c, const QString& label, QWidget* w) {
        auto* l = new QLabel(label);
        l->setBuddy(w);
        w->setAccessibleName(QString(label).remove(QLatin1Char('&')).remove(QLatin1Char(':')));
        form->addWidget(l, r, c * 2);
        form->addWidget(w, r, c * 2 + 1);
    };
    m_api = new QComboBox;
    m_resolution = new QComboBox;
    m_resolution->setEditable(true);
    for (const char* r : {"720p", "1080p", "1440p", "4k"}) m_resolution->addItem(QString::fromLatin1(r));
    m_resolution->setToolTip(tr("Fixed internal render resolution (720p, 1080p, 1440p, 4k or WxH), independent of "
                                "the window size."));
    m_resolution->setCurrentText(Settings::instance().resolution());
    m_quality = new QComboBox;
    m_quality->addItem(tr("Suite default"), QString());
    for (const char* q : {"low", "medium", "high", "ultra"}) m_quality->addItem(QString::fromLatin1(q), QString::fromLatin1(q));
    m_duration = new QComboBox;
    m_duration->addItem(tr("Suite default"), QString());
    for (const char* d : {"quick", "standard", "extreme"}) m_duration->addItem(QString::fromLatin1(d), QString::fromLatin1(d));
    m_upscale = new QComboBox;
    m_upscale->addItem(tr("Off (native resolution)"), QString());
    m_upscale->addItem(tr("FSR 1 ultra quality (1.3x)"), QStringLiteral("fsr1:ultra-quality"));
    m_upscale->addItem(tr("FSR 1 quality (1.5x)"), QStringLiteral("fsr1:quality"));
    m_upscale->addItem(tr("FSR 1 balanced (1.7x)"), QStringLiteral("fsr1:balanced"));
    m_upscale->addItem(tr("FSR 1 performance (2x)"), QStringLiteral("fsr1:performance"));
    m_upscale->addItem(tr("Bilinear quality (1.5x)"), QStringLiteral("bilinear:quality"));
    m_upscale->setToolTip(tr("Scenes render below the selected resolution and are upscaled to it (AMD FidelityFX "
                             "Super Resolution 1, or plain bilinear filtering for comparison)."));
    m_loopMode = new QComboBox;
    m_loopMode->addItem(tr("Single run"), QStringLiteral("single"));
    m_loopMode->addItem(tr("Repeat N times"), QStringLiteral("count"));
    m_loopMode->addItem(tr("Timed loop"), QStringLiteral("timed"));
    m_loopMode->addItem(tr("Until stopped"), QStringLiteral("infinite"));
    m_loopCount = new QSpinBox;
    m_loopCount->setRange(2, 10000);
    m_loopCount->setValue(5);
    m_loopTime = new QLineEdit(QStringLiteral("20m"));
    m_loopTime->setToolTip(tr("Duration such as 90s, 20m or 1h30m"));
    m_loopTime->setMaximumWidth(120);
    auto* loopRow = new QWidget;
    auto* lr = new QHBoxLayout(loopRow);
    lr->setContentsMargins(0, 0, 0, 0);
    lr->addWidget(m_loopMode);
    lr->addWidget(m_loopCount);
    lr->addWidget(m_loopTime);
    lr->addStretch();
    m_loopCount->setVisible(false);
    m_loopTime->setVisible(false);
    connect(m_loopMode, &QComboBox::currentIndexChanged, this, [this] {
        const QString m = m_loopMode->currentData().toString();
        m_loopCount->setVisible(m == QLatin1String("count"));
        m_loopTime->setVisible(m == QLatin1String("timed"));
    });
    addRow(0, 0, tr("&API:"), m_api);
    addRow(0, 1, tr("&Resolution:"), m_resolution);
    addRow(1, 0, tr("&Quality:"), m_quality);
    addRow(1, 1, tr("&Duration:"), m_duration);
    addRow(2, 0, tr("&Upscaling:"), m_upscale);
    auto* loopLabel = new QLabel(tr("&Loop:"));
    loopLabel->setBuddy(m_loopMode);
    m_loopMode->setAccessibleName(tr("Loop mode"));
    form->addWidget(loopLabel, 3, 0);
    form->addWidget(loopRow, 3, 1, 1, 3);
    form->setColumnStretch(1, 1);
    form->setColumnStretch(3, 1);
    sl->addLayout(form);
    auto* checks = new QHBoxLayout;
    m_offscreen = new QCheckBox(tr("Offscreen (no window)"));
    m_fullscreen = new QCheckBox(tr("Fullscreen"));
    m_vsync = new QCheckBox(tr("VSync"));
    m_hud = new QCheckBox(tr("HUD in benchmark window"));
    m_validation = new QCheckBox(tr("API validation (slow, for debugging)"));
    m_offscreen->setChecked(Settings::instance().offscreen());
    m_fullscreen->setChecked(Settings::instance().fullscreen());
    m_hud->setChecked(Settings::instance().hud());
    m_validation->setChecked(Settings::instance().validation());
    for (auto* c : {m_offscreen, m_fullscreen, m_vsync, m_hud, m_validation}) checks->addWidget(c);
    checks->addStretch();
    sl->addLayout(checks);
    m_content->addWidget(settingsCard);

    // --- custom builder
    auto* builder = new Card;
    auto* bl = static_cast<QVBoxLayout*>(builder->layout());
    auto* bh = new QHBoxLayout;
    bh->addWidget(makeSubtitle(tr("Custom selection")));
    bh->addStretch();
    auto* load = new QPushButton(themedIcon(QStringLiteral("folder-open")), tr("Load…"));
    auto* save = new QPushButton(themedIcon(QStringLiteral("save")), tr("Save as suite…"));
    bh->addWidget(load);
    bh->addWidget(save);
    bl->addLayout(bh);
    bl->addWidget(makeHint(tr("Ticking tests switches to a custom run. Untick everything to go back to the selected "
                              "suite.")));
    m_tree = new QTreeWidget;
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("Test"), tr("Status")});
    m_tree->setAccessibleName(tr("Tests"));
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->setMinimumHeight(240);
    m_tree->setUniformRowHeights(true);
    bl->addWidget(m_tree);
    m_content->addWidget(builder);

    // --- run bar
    auto* runRow = new QHBoxLayout;
    m_summary = new QLabel;
    m_summary->setWordWrap(true);
    auto* run = new QPushButton(themedIcon(QStringLiteral("play")), tr("Run"));
    run->setDefault(true);
    run->setMinimumHeight(36);
    run->setMinimumWidth(140);
    runRow->addWidget(m_summary, 1);
    runRow->addWidget(run);
    m_content->addLayout(runRow);

    connect(run, &QPushButton::clicked, this, [this] { Q_EMIT runRequested(request()); });
    connect(save, &QPushButton::clicked, this, &TestsPage::saveSuite);
    connect(load, &QPushButton::clicked, this, &TestsPage::loadSuite);
    connect(m_tree, &QTreeWidget::itemChanged, this, [this] { rebuildSuites(); });
    connect(state, &AppState::catalogChanged, this, [this] {
        rebuildSuites();
        rebuildTree();
    });
    connect(state, &AppState::devicesChanged, this, [this] {
        m_api->clear();
        if (const GpuEntry* g = m_state->selectedGpu())
            for (auto it = g->apiIndex.begin(); it != g->apiIndex.end(); ++it) m_api->addItem(apiDisplayName(it.key()), it.key());
        m_api->setCurrentIndex(std::max(0, m_api->findData(m_state->selectedApi())));
    });
    connect(state, &AppState::selectionChanged, this, [this] {
        m_api->setCurrentIndex(std::max(0, m_api->findData(m_state->selectedApi())));
    });
    connect(m_api, &QComboBox::activated, this, [this] {
        m_state->select(m_state->selectedGpuKey(), m_api->currentData().toString());
    });
    connect(state, &AppState::detailsChanged, this, &TestsPage::updateSupport);
    rebuildSuites();
    rebuildTree();
}

void TestsPage::selectSuite(const QString& id) {
    m_suite = id;
    m_customFile.clear();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* cat = m_tree->topLevelItem(i);
        for (int j = 0; j < cat->childCount(); ++j) cat->child(j)->setCheckState(0, Qt::Unchecked);
    }
    rebuildSuites();
}

void TestsPage::rebuildSuites() {
    auto* grid = static_cast<QGridLayout*>(m_suiteBox->layout());
    clearLayout(grid);
    m_suiteCards.clear();
    QStringList custom;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* cat = m_tree->topLevelItem(i);
        for (int j = 0; j < cat->childCount(); ++j)
            if (cat->child(j)->checkState(0) == Qt::Checked) custom << cat->child(j)->data(0, Qt::UserRole).toString();
    }
    int i = 0;
    for (const auto& s : m_state->suites()) {
        if (s.id == QLatin1String("smoke")) continue;
        auto* card = new Card;
        card->setClickable(true);
        card->setSelected(custom.isEmpty() && m_customFile.isEmpty() && s.id == m_suite);
        card->setAccessibleName(tr("Suite %1").arg(s.name));
        card->setAccessibleDescription(s.description);
        auto* l = static_cast<QVBoxLayout*>(card->layout());
        auto* head = new QHBoxLayout;
        head->addWidget(makeSubtitle(s.name));
        head->addStretch();
        head->addWidget(new Badge(s.tests.size() == 1 ? tr("1 test") : tr("%1 tests").arg(s.tests.size()),
                                  s.tests.isEmpty() ? Badge::Kind::Neutral : Badge::Kind::Accent));
        l->addLayout(head);
        l->addWidget(makeHint(s.description));
        l->addStretch(); // top-aligned when a neighbour in the row has a longer description
        const QString id = s.id;
        connect(card, &Card::clicked, this, [this, id] { selectSuite(id); });
        grid->addWidget(card, i / 3, i % 3);
        m_suiteCards << card;
        ++i;
    }
    for (int c = 0; c < 3; ++c) grid->setColumnStretch(c, 1);
    if (!custom.isEmpty()) m_summary->setText(tr("Custom run: %1 selected.").arg(custom.size() == 1 ? tr("1 test") : tr("%1 tests").arg(custom.size())));
    else if (!m_customFile.isEmpty()) m_summary->setText(tr("Custom suite file: %1").arg(m_customFile));
    else {
        QString name = m_suite;
        for (const auto& s : m_state->suites())
            if (s.id == m_suite) name = s.name;
        m_summary->setText(tr("Suite: %1").arg(name));
    }
}

void TestsPage::rebuildTree() {
    QSignalBlocker block(m_tree);
    m_tree->clear();
    QMap<QString, QTreeWidgetItem*> cats;
    const QMap<QString, QString> catNames{{QStringLiteral("scene"), tr("Scenes")},
                                          {QStringLiteral("synthetic"), tr("Synthetic")},
                                          {QStringLiteral("feature"), tr("Feature tests")},
                                          {QStringLiteral("diagnostic"), tr("Diagnostics")}};
    for (const auto& t : m_state->tests()) {
        if (t.tags.contains(QStringLiteral("debug"))) continue; // development-only tests
        QTreeWidgetItem*& cat = cats[t.category];
        if (!cat) {
            cat = new QTreeWidgetItem(m_tree, {catNames.value(t.category, t.category)});
            cat->setFlags(Qt::ItemIsEnabled);
            QFont f = cat->font(0);
            f.setWeight(QFont::DemiBold);
            cat->setFont(0, f);
        }
        auto* item = new QTreeWidgetItem(cat, {t.name, QString()});
        item->setData(0, Qt::UserRole, t.id);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
        item->setCheckState(0, Qt::Unchecked);
        item->setToolTip(0, t.description);
    }
    m_tree->expandAll();
    updateSupport();
}

void TestsPage::updateSupport() {
    QMap<QString, QPair<bool, QString>> support;
    for (const auto& e : m_state->support())
        support.insert(QString::fromStdString(e.value("id", std::string())),
                       {e.value("supported", true), QString::fromStdString(e.value("reason", std::string()))});
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* cat = m_tree->topLevelItem(i);
        for (int j = 0; j < cat->childCount(); ++j) {
            QTreeWidgetItem* it = cat->child(j);
            const QString id = it->data(0, Qt::UserRole).toString();
            if (!support.contains(id)) {
                it->setText(1, QString());
                continue;
            }
            const auto [ok, reason] = support.value(id);
            it->setText(1, ok ? tr("Supported") : tr("Not supported: %1").arg(reason));
            it->setToolTip(1, it->text(1));
            it->setDisabled(!ok);
        }
    }
}

RunRequest TestsPage::request() const {
    RunRequest r;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* cat = m_tree->topLevelItem(i);
        for (int j = 0; j < cat->childCount(); ++j)
            if (cat->child(j)->checkState(0) == Qt::Checked) r.tests << cat->child(j)->data(0, Qt::UserRole).toString();
    }
    r.suite = m_customFile.isEmpty() ? m_suite : m_customFile;
    r.api = m_api->currentData().toString();
    r.gpu = m_state->selectedAdapterIndex();
    r.resolution = m_resolution->currentText().trimmed();
    r.quality = m_quality->currentData().toString();
    r.duration = m_duration->currentData().toString();
    if (const QString up = m_upscale->currentData().toString(); !up.isEmpty()) {
        r.upscale = up.section(QLatin1Char(':'), 0, 0);
        r.upscaleMode = up.section(QLatin1Char(':'), 1, 1);
    }
    const QString mode = m_loopMode->currentData().toString();
    r.loop = mode == QLatin1String("count")    ? QString::number(m_loopCount->value())
             : mode == QLatin1String("timed")  ? m_loopTime->text().trimmed()
             : mode == QLatin1String("infinite") ? QStringLiteral("inf")
                                                : QStringLiteral("1");
    r.offscreen = m_offscreen->isChecked();
    r.fullscreen = m_fullscreen->isChecked();
    r.vsync = m_vsync->isChecked();
    r.hud = m_hud->isChecked();
    r.validation = m_validation->isChecked();
    auto& s = Settings::instance();
    s.setResolution(r.resolution);
    s.setOffscreen(r.offscreen);
    s.setFullscreen(r.fullscreen);
    s.setHud(r.hud);
    s.setValidation(r.validation);
    r.safety = s.safetyEnabled();
    r.maxTemp = s.maxTemp();
    r.maxHotspot = s.maxHotspot();
    r.maxMemTemp = s.maxMemTemp();
    r.maxPower = s.maxPower();
    r.stabilityThreshold = s.stabilityThreshold();
    r.recoverDeviceLost = s.recoverDeviceLost();
    return r;
}

void TestsPage::saveSuite() {
    const RunRequest r = request();
    QStringList tests = r.tests;
    if (tests.isEmpty())
        for (const auto& s : m_state->suites())
            if (s.id == m_suite) tests = s.tests;
    if (tests.isEmpty()) {
        QMessageBox::information(this, tr("Save suite"), tr("Select at least one test first."));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("Save custom suite"), Settings::instance().resultsDir(),
                                                      tr("Suite files (*.json)"));
    if (path.isEmpty()) return;
    nlohmann::json j{{"id", QFileInfo(path).baseName().toStdString()},
                     {"name", QFileInfo(path).baseName().toStdString()},
                     {"description", "Custom suite"},
                     {"tests", nlohmann::json::array()}};
    for (const auto& t : tests) j["tests"].push_back(t.toStdString());
    if (!r.duration.isEmpty()) j["duration"] = r.duration.toStdString();
    if (!r.quality.isEmpty()) j["quality"] = r.quality.toStdString();
    if (r.loop != QLatin1String("1")) j["loop"] = r.loop.toStdString();
    j["resolution"] = r.resolution.toStdString();
    std::ofstream f(path.toStdString());
    f << j.dump(2) << "\n";
}

void TestsPage::loadSuite() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Load custom suite"), Settings::instance().resultsDir(),
                                                      tr("Suite files (*.json)"));
    if (path.isEmpty()) return;
    m_customFile = path;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* cat = m_tree->topLevelItem(i);
        for (int j = 0; j < cat->childCount(); ++j) cat->child(j)->setCheckState(0, Qt::Unchecked);
    }
    rebuildSuites();
}

} // namespace gt::gui
