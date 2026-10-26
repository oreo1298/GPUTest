#include "mainwindow.h"

#include "gt/app_info.h"
#include "history.h"
#include "settings.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QShortcut>
#include <QStackedWidget>

namespace gt::gui {

MainWindow::MainWindow(AppState* state, QWidget* parent) : QMainWindow(parent), m_state(state) {
    setWindowTitle(QString::fromUtf8(app::kDisplayName));
    auto* central = new QWidget;
    auto* row = new QHBoxLayout(central);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);

    auto* side = new QWidget;
    side->setObjectName(QStringLiteral("sidebar"));
    side->setAutoFillBackground(true);
    auto* sl = new QVBoxLayout(side);
    sl->setContentsMargins(8, 14, 8, 10);
    auto* brand = makeTitle(QString::fromUtf8(app::kDisplayName), 6);
    brand->setContentsMargins(14, 0, 0, 10);
    sl->addWidget(brand);
    m_nav = new NavList;
    m_nav->addPage(QStringLiteral("house"), tr("Home"), QStringLiteral("Ctrl+1"));
    m_nav->addPage(QStringLiteral("list-checks"), tr("Tests"), QStringLiteral("Ctrl+2"));
    m_nav->addPage(QStringLiteral("activity"), tr("Live run"), QStringLiteral("Ctrl+3"));
    m_nav->addPage(QStringLiteral("chart-column"), tr("Results"), QStringLiteral("Ctrl+4"));
    m_nav->addPage(QStringLiteral("clock"), tr("History"), QStringLiteral("Ctrl+5"));
    m_nav->addPage(QStringLiteral("git-compare"), tr("Compare"), QStringLiteral("Ctrl+6"));
    m_nav->addPage(QStringLiteral("cpu"), tr("System & features"), QStringLiteral("Ctrl+7"));
    m_nav->addPage(QStringLiteral("settings"), tr("Settings"), QStringLiteral("Ctrl+8"));
    m_nav->setFixedWidth(232);
    sl->addWidget(m_nav, 1);
    auto* version = makeHint(QStringLiteral("v%1").arg(QString::fromUtf8(app::kVersion)));
    version->setContentsMargins(14, 0, 0, 0);
    sl->addWidget(version);
    row->addWidget(side);
    auto recolorSide = [side, nav = m_nav] {
        QPalette p = side->palette();
        p.setColor(QPalette::Window, ThemeManager::instance().surfaceAlt());
        side->setPalette(p);
        QPalette np = nav->palette();
        np.setColor(QPalette::Base, ThemeManager::instance().surfaceAlt());
        nav->setPalette(np);
    };
    recolorSide();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, side, recolorSide);

    m_stack = new QStackedWidget;
    m_home = new HomePage(state);
    m_tests = new TestsPage(state);
    m_run = new RunPage(state);
    m_results = new ResultsPage(state);
    m_history = new HistoryPage(state);
    m_compare = new ComparePage(state);
    m_system = new SystemPage(state);
    m_settings = new SettingsPage;
    for (QWidget* p : std::initializer_list<QWidget*>{m_home, m_tests, m_run, m_results, m_history, m_compare, m_system,
                                                      m_settings})
        m_stack->addWidget(p);
    row->addWidget(m_stack, 1);
    setCentralWidget(central);

    connect(m_nav, &QListWidget::currentRowChanged, m_stack, &QStackedWidget::setCurrentIndex);
    m_nav->setCurrentRow(0);
    for (int i = 0; i < PageCount; ++i) {
        auto* sc = new QShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + i)), this);
        connect(sc, &QShortcut::activated, this, [this, i] { showPage(i); });
    }
    connect(m_home, &HomePage::runRecommended, this, [this] {
        m_tests->selectSuite(QStringLiteral("standard"));
        startRun(m_tests->request());
    });
    connect(m_home, &HomePage::showSystem, this, [this] { showPage(System); });
    connect(m_tests, &TestsPage::runRequested, this, &MainWindow::startRun);
    connect(m_run, &RunPage::viewResults, this, [this] { showPage(Results); });
    connect(m_results, &ResultsPage::compareRequested, this, &MainWindow::compareWithPrevious);
    connect(m_history, &HistoryPage::openRun, this, [this](const QString& f) {
        if (m_results->loadFile(f)) showPage(Results);
    });
    connect(m_history, &HistoryPage::compareRuns, this, [this](const QString& a, const QString& b) {
        m_compare->setRuns(a, b);
        showPage(Compare);
    });
    connect(state->client(), &RunnerClient::finished, this, &MainWindow::onRunFinished);
    resize(1280, 860);
}

void MainWindow::showPage(int index) {
    m_nav->setCurrentRow(index);
    m_stack->widget(index)->setFocus();
}

int MainWindow::currentPage() const { return m_stack->currentIndex(); }

QWidget* MainWindow::pageWidget(int index) const { return m_stack->widget(index); }

QString MainWindow::pageKey(int index) const {
    static const char* keys[] = {"home", "tests", "run", "results", "history", "compare", "system", "settings"};
    return index >= 0 && index < PageCount ? QString::fromLatin1(keys[index]) : QString();
}

void MainWindow::startRun(const RunRequest& reqIn) {
    RunnerClient* c = m_state->client();
    if (c->isRunning()) {
        QMessageBox::information(this, windowTitle(), tr("A run is already in progress."));
        return;
    }
    if (!RunnerClient::runnerAvailable()) {
        QMessageBox::warning(this, windowTitle(),
                             tr("The benchmark runner was not found at %1.\nSet its location in Settings.")
                                 .arg(RunnerClient::runnerPath()));
        return;
    }
    RunRequest req = reqIn;
    if (req.out.isEmpty()) {
        QDir dir(Settings::instance().resultsDir());
        dir.mkpath(QStringLiteral("."));
        const QString name = req.tests.isEmpty() ? QFileInfo(req.suite).baseName() : QStringLiteral("custom");
        req.out = dir.filePath(QStringLiteral("%1_%2.json").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")), name));
    }
    c->start(req);
    showPage(Run);
}

void MainWindow::compareWithPrevious(const QString& file) {
    const QString path = QFileInfo(file).absoluteFilePath();
    QString key, created;
    const auto entries = History::instance().entries();
    for (const auto& e : entries)
        if (e.path == path) {
            key = e.comparableKey;
            created = e.created;
        }
    QString baseline;
    for (const auto& e : entries) // newest first
        if (e.path != path && e.comparableKey == key && !key.isEmpty() && e.created <= created) {
            baseline = e.path;
            break;
        }
    m_compare->setRuns(baseline.isEmpty() ? path : baseline, baseline.isEmpty() ? QString() : path);
    showPage(Compare);
}

void MainWindow::onRunFinished(const QString& status, const QString& message, const QString& file) {
    if (!file.isEmpty() && QFileInfo::exists(file)) History::instance().importFile(file);
    // Prefer the file the runner wrote (complete, atomic); fall back to what the stream delivered.
    if (!file.isEmpty() && QFileInfo::exists(file) && m_results->loadFile(file)) {
        if (status == QLatin1String("crashed")) {
            auto run = m_results->run();
            run.status = results::RunStatus::Crashed;
            run.statusMessage = message.toStdString();
            m_results->showRun(run, file);
        }
        return;
    }
    m_results->showRun(m_state->client()->run(), file);
}

void MainWindow::closeEvent(QCloseEvent* e) {
    RunnerClient* c = m_state->client();
    if (c->isRunning()) {
        const auto r = QMessageBox::question(this, windowTitle(), tr("A benchmark is running. Stop it and quit?"));
        if (r != QMessageBox::Yes) {
            e->ignore();
            return;
        }
        c->stop();
    }
    e->accept();
}

} // namespace gt::gui
