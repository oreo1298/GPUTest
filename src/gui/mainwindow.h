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
    enum PageId { Home = 0, Tests, Run, Results, HistoryPage_, Compare, System, SettingsPage_, PageCount };
    MainWindow(AppState* state, QWidget* parent = nullptr);
    void showPage(int index);
    int currentPage() const;
    QString pageKey(int index) const;
    ResultsPage* resultsPage() { return m_results; }
    RunPage* runPage() { return m_run; }
    ComparePage* comparePage() { return m_compare; }
    QWidget* pageWidget(int index) const;
    /// Opens the Compare page with `file` as B and the newest earlier comparable run as A.
    void compareWithPrevious(const QString& file);
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
    HistoryPage* m_history;
    ComparePage* m_compare;
    SystemPage* m_system;
    SettingsPage* m_settings;
};

} // namespace gt::gui
