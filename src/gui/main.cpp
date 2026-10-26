#include "appstate.h"
#include "gt/app_info.h"
#include "gt/paths.h"
#include "history.h"
#include "mainwindow.h"
#include "runnerclient.h"
#include "settings.h"
#include "theme.h"
#include "widgets.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QIcon>
#include <QLayout>
#include <QScrollArea>
#include <QTableWidget>
#include <QTimer>

using namespace gt::gui;

namespace {

/// Saves every page in light and dark mode as PNG (used for docs and UI review).
void takeScreenshots(MainWindow* w, AppState* state, const QString& dir) {
    QDir().mkpath(dir);
    auto* themes = new QStringList{QStringLiteral("light"), QStringLiteral("dark")};
    auto step = std::make_shared<std::function<void(int)>>();
    *step = [=](int i) {
        const int total = static_cast<int>(themes->size()) * MainWindow::PageCount;
        if (i >= total) {
            delete themes;
            QCoreApplication::quit();
            return;
        }
        const QString theme = themes->at(i / MainWindow::PageCount);
        const int page = i % MainWindow::PageCount;
        if (page == 0) ThemeManager::instance().setMode(ThemeManager::modeFromString(theme));
        w->showPage(page);
        QTimer::singleShot(400, w, [=] {
            const QString base = QDir(dir).filePath(QStringLiteral("%1-%2-%3").arg(theme).arg(page).arg(w->pageKey(page)));
            w->grab().save(base + QStringLiteral(".png"));
            // Full-length captures of scrolling pages, one per tab, for reviewing what is below the fold.
            // The body is sized like QScrollArea sizes it: max(viewport, height for the viewport width).
            QWidget* pw = w->pageWidget(page);
            auto* sa = pw->findChild<QScrollArea*>();
            auto grabBody = [sa](const QString& file) {
                for (int k = 0; k < 6; ++k) QCoreApplication::processEvents();
                QWidget* body = sa->widget();
                body->layout()->activate();
                const int vw = sa->viewport()->width();
                const int need = body->hasHeightForWidth() ? body->heightForWidth(vw) : body->sizeHint().height();
                const int h = std::max({sa->viewport()->height(), need, body->minimumSizeHint().height()});
                if (h <= sa->viewport()->height() + 4) return;
                body->resize(vw, h);
                QCoreApplication::processEvents();
                body->grab().save(file);
            };
            if (sa && sa->widget()) {
                const auto tabs = pw->findChildren<TabView*>();
                if (tabs.isEmpty()) {
                    grabBody(base + QStringLiteral("-full.png"));
                } else {
                    TabView* tw = tabs.front();
                    const int current = tw->currentIndex();
                    for (int t = 0; t < tw->count(); ++t) {
                        if (!tw->isTabVisible(t)) continue;
                        tw->setCurrentIndex(t);
                        grabBody(base + QStringLiteral("-full-tab%1.png").arg(t));
                    }
                    tw->setCurrentIndex(current);
                }
            }
            (*step)(i + 1);
        });
    };
    // Show something on the Compare and History pages: the two newest runs.
    const auto runs = History::instance().entries();
    if (runs.size() >= 2) w->comparePage()->setRuns(runs[1].path, runs[0].path);
    if (auto* t = w->pageWidget(MainWindow::HistoryPage_)->findChild<QTableWidget*>(); t && t->rowCount() > 0) t->selectRow(0);
    // Give the runner queries time to fill the pages.
    auto start = [=] { QTimer::singleShot(600, w, [=] { (*step)(0); }); };
    int waited = 0;
    auto* poll = new QTimer(w);
    QObject::connect(poll, &QTimer::timeout, w, [=]() mutable {
        waited += 250;
        if ((state->loaded() && !state->sysinfo().empty() && !state->capabilities().empty()) || waited > 20000) {
            poll->stop();
            start();
        }
    });
    poll->start(250);
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QString::fromUtf8(gt::app::kId));
    QApplication::setApplicationDisplayName(QString::fromUtf8(gt::app::kDisplayName));
    QApplication::setApplicationVersion(QString::fromUtf8(gt::app::kVersion));
    QApplication::setDesktopFileName(QString::fromUtf8(gt::app::kRdnn));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/app/app.svg")));

    QCommandLineParser cli;
    cli.setApplicationDescription(QString::fromUtf8(gt::app::kDescription));
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption themeOpt(QStringLiteral("theme"), QStringLiteral("Override the theme: system, light or dark."),
                                QStringLiteral("mode"));
    QCommandLineOption resultsOpt(QStringLiteral("results"), QStringLiteral("Open a results file."), QStringLiteral("file"));
    QCommandLineOption replayOpt(QStringLiteral("replay"), QStringLiteral("Replay recorded runner events (JSON lines)."),
                                 QStringLiteral("file"));
    QCommandLineOption shotsOpt(QStringLiteral("screenshots"),
                                QStringLiteral("Save every page in light and dark mode to <dir> and exit."),
                                QStringLiteral("dir"));
    QCommandLineOption pageOpt(QStringLiteral("page"), QStringLiteral("Page to show at start (0-7)."), QStringLiteral("n"));
    cli.addOptions({themeOpt, resultsOpt, replayOpt, shotsOpt, pageOpt});
    cli.process(app);

    auto& theme = ThemeManager::instance();
    theme.init(app);
    theme.setMode(ThemeManager::modeFromString(cli.isSet(themeOpt) ? cli.value(themeOpt) : Settings::instance().themeMode()));

    // Local run history (an index over the results folder; the JSON files stay the source of truth).
    {
        QString err;
        const QString db = QString::fromStdString((gt::paths::dataDir() / "history.sqlite").string());
        if (History::instance().open(db, &err))
            History::instance().scanFolder(Settings::instance().resultsDir());
        else
            qWarning("history database unavailable (%s): %s", qPrintable(db), qPrintable(err));
    }

    RunnerClient client;
    AppState state(&client);
    MainWindow w(&state);
    w.show();
    state.refresh();

    if (cli.isSet(resultsOpt)) {
        w.resultsPage()->loadFile(cli.value(resultsOpt));
        w.showPage(MainWindow::Results);
    }
    if (cli.isSet(replayOpt)) client.replay(cli.value(replayOpt));
    if (cli.isSet(pageOpt)) w.showPage(cli.value(pageOpt).toInt());
    if (cli.isSet(shotsOpt)) takeScreenshots(&w, &state, cli.value(shotsOpt));
    return QApplication::exec();
}
