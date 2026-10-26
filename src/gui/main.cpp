#include "appstate.h"
#include "gt/app_info.h"
#include "mainwindow.h"
#include "runnerclient.h"
#include "settings.h"
#include "theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QIcon>
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
            const QString file = QDir(dir).filePath(QStringLiteral("%1-%2-%3.png").arg(theme).arg(page).arg(w->pageKey(page)));
            w->grab().save(file);
            (*step)(i + 1);
        });
    };
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
    QCommandLineOption pageOpt(QStringLiteral("page"), QStringLiteral("Page to show at start (0-5)."), QStringLiteral("n"));
    cli.addOptions({themeOpt, resultsOpt, replayOpt, shotsOpt, pageOpt});
    cli.process(app);

    auto& theme = ThemeManager::instance();
    theme.init(app);
    theme.setMode(ThemeManager::modeFromString(cli.isSet(themeOpt) ? cli.value(themeOpt) : Settings::instance().themeMode()));

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
