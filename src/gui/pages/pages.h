#pragma once

#include "appstate.h"
#include "chart.h"
#include "history.h"
#include "telemetryview.h"
#include "widgets.h"

#include <QWidget>

#include <optional>

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
class QTabWidget;
class QTableWidget;
class QToolButton;
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
    QComboBox* m_upscale;
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
    LineChart* m_loopChart;
    QVector<QPointF> m_loopScores;
    QVector<LineChart::Band> m_throttleBands;
    std::uint32_t m_throttleMask = 0;
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
    QString file() const { return m_file; }
Q_SIGNALS:
    void compareRequested(const QString& file);

private:
    void showTest(int row);
    void showLoops();
    void showSystem();
    void exportHtml();
    void exportCsv();
    void saveCopy();
    void copySummary();
    void setNote(const QString& text);
    QString exportBaseName(const QString& ext) const;
    AppState* m_state;
    gt::results::RunResult m_run;
    QString m_file;
    bool m_loaded = false;
    QLabel* m_header;
    QLabel* m_banner;
    QLabel* m_note;
    QWidget* m_scoreBox;
    Card* m_verdictCard;
    QGridLayout* m_verdictGrid;
    QLabel* m_verdictTitle;
    QToolButton* m_export;
    QPushButton* m_compare;
    TabView* m_tabs;
    QTableWidget* m_table;
    QLabel* m_testDetails;
    LineChart* m_frameChart;
    HistogramChart* m_hist;
    TelemetryView* m_testTelemetry;
    TelemetryView* m_runTelemetry;
    QWidget* m_loopsTab;
    LineChart* m_loopChart;
    QLabel* m_loopNote;
    QTableWidget* m_loopTable;
    QTreeWidget* m_system;
};

class HistoryPage : public Page {
    Q_OBJECT
public:
    HistoryPage(AppState* state, QWidget* parent = nullptr);
    void refresh();
Q_SIGNALS:
    void openRun(const QString& file);
    void compareRuns(const QString& a, const QString& b);

private:
    QVector<int> selectedRows() const;
    void updateButtons();
    void updateTrend();
    void importFiles();
    void removeSelected(bool deleteFiles);
    AppState* m_state;
    QLineEdit* m_filter;
    QTableWidget* m_table;
    QPushButton* m_open;
    QPushButton* m_compare;
    QPushButton* m_remove;
    QPushButton* m_delete;
    QLabel* m_status;
    LineChart* m_trend;
    QLabel* m_trendNote;
    QVector<HistoryEntry> m_entries;
};

class ComparePage : public Page {
    Q_OBJECT
public:
    ComparePage(AppState* state, QWidget* parent = nullptr);
    void setRuns(const QString& a, const QString& b);
    void refreshChoices();

private:
    struct Row {
        QString id, name;
        const gt::results::TestResult* a = nullptr;
        const gt::results::TestResult* b = nullptr;
        double scoreA = 0, scoreB = 0;
        bool scoredA = false, scoredB = false;
    };
    void load();
    void rebuild();
    void showTest(int row);
    AppState* m_state;
    QComboBox* m_a;
    QComboBox* m_b;
    QPushButton* m_swap;
    QLabel* m_warning;
    QLabel* m_infoA;
    QLabel* m_infoB;
    QWidget* m_tiles;
    QTableWidget* m_table;
    QWidget* m_frameCard; ///< the frame-time charts (hidden for tests without frames)
    LineChart* m_frames;
    HistogramChart* m_hist;
    QTableWidget* m_telemetry;
    QLabel* m_empty;
    QWidget* m_body;
    std::optional<gt::results::RunResult> m_runA, m_runB;
    QString m_fileA, m_fileB;
    QVector<Row> m_rows;
    bool m_updating = false;
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
