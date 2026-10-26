#pragma once
// Talks to the benchmark runner process. The GUI never touches the GPU itself:
// it launches the runner, reads its JSON-lines event stream and assembles the
// results as they arrive, so a runner crash or GPU reset never takes the GUI down
// and every finished test is kept.

#include "gt/results.h"

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <nlohmann/json.hpp>

namespace gt::gui {

struct RunRequest {
    QString suite;          ///< preset id or custom suite file (ignored when tests is non-empty)
    QStringList tests;      ///< explicit test ids
    QString api;            ///< "vulkan", ...
    int gpu = -1;
    QString resolution = QStringLiteral("1080p");
    QString quality;        ///< empty = suite default
    QString duration;       ///< empty = suite default
    QString loop = QStringLiteral("1");
    bool offscreen = false;
    bool fullscreen = false;
    bool vsync = false;
    bool hud = true;
    bool validation = false;
    bool safety = true;
    double maxTemp = 90, maxHotspot = 105, maxMemTemp = 100, maxPower = 0;
    double stabilityThreshold = 0.97;
    QString out;            ///< results file path
    QStringList extraArgs;
    QStringList toArguments() const;
};

class RunnerClient : public QObject {
    Q_OBJECT
public:
    explicit RunnerClient(QObject* parent = nullptr);
    ~RunnerClient() override;

    /// Path of the runner executable: settings override, else next to the GUI.
    static QString runnerPath();
    static bool runnerAvailable();

    /// Runs an information command (e.g. {"--list-devices","--json"}) asynchronously.
    using QueryCallback = std::function<void(bool ok, const QByteArray& stdoutData, const QString& error)>;
    void query(const QStringList& args, QueryCallback cb, int timeoutMs = 60000);

    bool start(const RunRequest& req);
    void stop();
    void kill();
    bool isRunning() const;
    const results::RunResult& run() const { return m_run; }
    QString resultsFile() const { return m_resultsFile; }
    QStringList logLines() const { return m_log; }

    /// Feeds pre-recorded events (JSON lines) instead of a live runner (demo / screenshots).
    void replay(const QString& jsonlPath);

Q_SIGNALS:
    void started();
    void runStarted(const nlohmann::json& event);
    void loopStarted(int loop);
    void testStarted(const QString& id, const QString& name, int index, int count, int loop);
    void progress(const QString& test, double fraction, const QString& status);
    void frames(const QVector<double>& frameTimesMs, double runTimeS);
    void telemetry(const gt::results::TelemetrySample& sample);
    void testFinished(const gt::results::TestResult& result);
    void loopFinished(int loop, double score);
    void logLine(const QString& level, const QString& text);
    void safetyAbort(const QString& message);
    void deviceLost(const QString& message);
    /// status: completed / stopped / safety_abort / device_lost / crashed / ...
    void finished(const QString& status, const QString& message, const QString& resultsFile);

private:
    void onStdout();
    void onStderr();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void handleEvent(const nlohmann::json& e);

    QProcess* m_proc = nullptr;
    QByteArray m_buffer;
    results::RunResult m_run;
    QString m_resultsFile;
    QString m_finalStatus;
    QString m_finalMessage;
    QStringList m_log;
    bool m_stopRequested = false;
};

} // namespace gt::gui
