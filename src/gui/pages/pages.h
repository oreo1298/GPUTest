#pragma once

#include "appstate.h"
#include "chart.h"
#include "widgets.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QTreeView;
class QTreeWidget;
class QVBoxLayout;
class QStandardItemModel;
class QSortFilterProxyModel;

namespace gt::gui {

/// Base for pages: a title, an optional subtitle and a scrollable content column.
class Page : public QWidget {
    Q_OBJECT
public:
    Page(const QString& title, const QString& subtitle, QWidget* parent = nullptr, bool scroll = true);
    QVBoxLayout* content() { return m_content; }

protected:
    QVBoxLayout* m_content;
};

class HomePage : public Page {
    Q_OBJECT
public:
    HomePage(AppState* state, QWidget* parent = nullptr);
Q_SIGNALS:
    void runRecommended();
    void showSystem();

private:
    void rebuildGpus();
    void updateDetails();
    AppState* m_state;
    QWidget* m_gpuBox;
    QGridLayout* m_gpuGrid;
    QComboBox* m_api;
    QWidget* m_badgeBox;
    QLabel* m_detailLine;
    QLabel* m_status;
    QPushButton* m_runButton;
};

class TestsPage : public Page {
    Q_OBJECT
public:
    TestsPage(AppState* state, QWidget* parent = nullptr);
    RunRequest request() const;
    void selectSuite(const QString& id);
Q_SIGNALS:
    void runRequested(const gt::gui::RunRequest& request);

private:
    void rebuildSuites();
    void rebuildTree();
    void updateSupport();
    void saveSuite();
    void loadSuite();
    AppState* m_state;
    QWidget* m_suiteBox;
    QVector<Card*> m_suiteCards;
    QString m_suite = QStringLiteral("standard");
    QTreeWidget* m_tree;
    QComboBox* m_api;
    QComboBox* m_resolution;
    QComboBox* m_quality;
    QComboBox* m_duration;
    QComboBox* m_loopMode;
    QSpinBox* m_loopCount;
    QLineEdit* m_loopTime;
    QCheckBox* m_offscreen;
    QCheckBox* m_fullscreen;
    QCheckBox* m_vsync;
    QCheckBox* m_hud;
    QCheckBox* m_validation;
    QLabel* m_summary;
    QString m_customFile;
};

class RunPage : public Page {
    Q_OBJECT
public:
    RunPage(AppState* state, QWidget* parent = nullptr);
Q_SIGNALS:
    void viewResults(const QString& resultsFile);

private:
    void onRunStarted(const nlohmann::json& e);
    void onTestStarted(const QString& id, const QString& name, int index, int count, int loop);
    void onFrames(const QVector<double>& ms, double t);
    void onTelemetry(const gt::results::TelemetrySample& s);
    void onTestFinished(const gt::results::TestResult& r);
    void onFinished(const QString& status, const QString& message, const QString& file);
    void tick();
    AppState* m_state;
    QLabel* m_headline;
    QLabel* m_subline;
    QLabel* m_timeLabel;
    QProgressBar* m_overall;
    QProgressBar* m_testProgress;
    QPushButton* m_stop;
    QPushButton* m_results;
    QLabel* m_banner;
    LineChart* m_frameChart;
    LineChart* m_fpsChart;
    LineChart* m_tempChart;
    StatTile* m_tiles[8];
    QPlainTextEdit* m_log;
    QPlainTextEdit* m_done;
    int m_frameSeries, m_fpsSeries, m_tempSeries[3];
    double m_frameIndex = 0;
    double m_runStart = 0;
    qint64 m_startMs = 0;
    int m_testIndex = 0, m_testCount = 1, m_loop = 0;
    QString m_loopText;
    double m_loopSeconds = 0;
    double m_testFraction = 0;
    QString m_resultsFile;
    bool m_running = false;
};

class ResultsPage : public Page {
    Q_OBJECT
public:
    ResultsPage(AppState* state, QWidget* parent = nullptr);
    void showRun(const gt::results::RunResult& run, const QString& file);
    bool loadFile(const QString& path);
    const gt::results::RunResult& run() const { return m_run; }

private:
    void showTest(int row);
    AppState* m_state;
    gt::results::RunResult m_run;
    QString m_file;
    QLabel* m_header;
    QLabel* m_banner;
    QWidget* m_scoreBox;
    QTableWidget* m_table;
    LineChart* m_frameChart;
    QLabel* m_testDetails;
};

class SystemPage : public Page {
    Q_OBJECT
public:
    SystemPage(AppState* state, QWidget* parent = nullptr);

private:
    void update();
    AppState* m_state;
    QLabel* m_host;
    QLabel* m_gpu;
    QLineEdit* m_search;
    QTreeView* m_view;
    QStandardItemModel* m_model;
    QSortFilterProxyModel* m_proxy;
    QTreeWidget* m_support;
};

class SettingsPage : public Page {
    Q_OBJECT
public:
    SettingsPage(QWidget* parent = nullptr);
};

} // namespace gt::gui
