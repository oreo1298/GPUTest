#pragma once

#include "appstate.h"
#include "pages/pages.h"
#include "runnerclient.h"

#include <QMainWindow>

class QStackedWidget;

namespace gt::gui {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    enum PageId { Home = 0, Tests, Run, Results, System, SettingsPage_, PageCount };
    MainWindow(AppState* state, QWidget* parent = nullptr);
    void showPage(int index);
    int currentPage() const;
    QString pageKey(int index) const;
    ResultsPage* resultsPage() { return m_results; }
    RunPage* runPage() { return m_run; }
    void startRun(const RunRequest& req);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void onRunFinished(const QString& status, const QString& message, const QString& file);
    AppState* m_state;
    NavList* m_nav;
    QStackedWidget* m_stack;
    HomePage* m_home;
    TestsPage* m_tests;
    RunPage* m_run;
    ResultsPage* m_results;
    SystemPage* m_system;
    SettingsPage* m_settings;
};

} // namespace gt::gui
