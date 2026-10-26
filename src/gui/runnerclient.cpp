#include "runnerclient.h"

#include "gt/app_info.h"
#include "settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

namespace gt::gui {

QStringList RunRequest::toArguments() const {
    QStringList a;
    if (!tests.isEmpty()) a << QStringLiteral("--test") << tests.join(QLatin1Char(','));
    else if (!suite.isEmpty()) a << QStringLiteral("--suite") << suite;
    if (!api.isEmpty()) a << QStringLiteral("--api") << api;
    if (gpu >= 0) a << QStringLiteral("--gpu") << QString::number(gpu);
    if (!resolution.isEmpty()) a << QStringLiteral("--resolution") << resolution;
    if (!quality.isEmpty()) a << QStringLiteral("--quality") << quality;
    if (!duration.isEmpty()) a << QStringLiteral("--duration") << duration;
    if (!loop.isEmpty()) a << QStringLiteral("--loop") << loop;
    if (offscreen) a << QStringLiteral("--offscreen");
    if (fullscreen) a << QStringLiteral("--fullscreen");
    if (vsync) a << QStringLiteral("--vsync");
    if (!hud) a << QStringLiteral("--no-hud");
    if (validation) a << QStringLiteral("--validation");
    if (!safety) {
        a << QStringLiteral("--no-safety");
    } else {
        a << QStringLiteral("--max-temp") << QString::number(maxTemp) << QStringLiteral("--max-hotspot")
          << QString::number(maxHotspot) << QStringLiteral("--max-mem-temp") << QString::number(maxMemTemp);
        if (maxPower > 0) a << QStringLiteral("--max-power") << QString::number(maxPower);
    }
    a << QStringLiteral("--stability-threshold") << QString::number(stabilityThreshold);
    if (!out.isEmpty()) a << QStringLiteral("--out") << out;
    a << QStringLiteral("--progress") << QStringLiteral("jsonl") << QStringLiteral("--control-stdin");
    a << extraArgs;
    return a;
}

RunnerClient::RunnerClient(QObject* parent) : QObject(parent) {}

RunnerClient::~RunnerClient() {
    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        m_proc->write("stop\n");
        m_proc->closeWriteChannel();
        if (!m_proc->waitForFinished(5000)) m_proc->kill();
    }
}

QString RunnerClient::runnerPath() {
    const QString overridePath = Settings::instance().runnerPath();
    if (!overridePath.isEmpty() && QFileInfo::exists(overridePath)) return overridePath;
    QString name = QString::fromUtf8(app::kRunnerName);
#ifdef Q_OS_WIN
    name += QStringLiteral(".exe");
#endif
    return QDir(QCoreApplication::applicationDirPath()).filePath(name);
}

bool RunnerClient::runnerAvailable() { return QFileInfo(runnerPath()).isExecutable(); }

void RunnerClient::query(const QStringList& args, QueryCallback cb, int timeoutMs) {
    auto* p = new QProcess(this);
    p->setProgram(runnerPath());
    p->setArguments(args);
    auto* timer = new QTimer(p);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, p, [p] { p->kill(); });
    connect(p, &QProcess::finished, this, [p, cb](int code, QProcess::ExitStatus st) {
        const QByteArray out = p->readAllStandardOutput();
        const QString err = QString::fromUtf8(p->readAllStandardError());
        if (st != QProcess::NormalExit || code != 0)
            cb(false, out, err.isEmpty() ? QStringLiteral("runner exited with code %1").arg(code) : err);
        else cb(true, out, err);
        p->deleteLater();
    });
    connect(p, &QProcess::errorOccurred, this, [p, cb](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            cb(false, {}, QStringLiteral("cannot start the runner: %1").arg(p->errorString()));
            p->deleteLater();
        }
    });
    timer->start(timeoutMs);
    p->start();
}

bool RunnerClient::isRunning() const { return m_proc && m_proc->state() != QProcess::NotRunning; }

bool RunnerClient::start(const RunRequest& req) {
    if (isRunning()) return false;
    m_run = results::RunResult{};
    m_buffer.clear();
    m_log.clear();
    m_resultsFile = req.out;
    m_finalStatus.clear();
    m_finalMessage.clear();
    m_stopRequested = false;
    delete m_proc;
    m_proc = new QProcess(this);
    m_proc->setProgram(runnerPath());
    m_proc->setArguments(req.toArguments());
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &RunnerClient::onStdout);
    connect(m_proc, &QProcess::readyReadStandardError, this, &RunnerClient::onStderr);
    connect(m_proc, &QProcess::finished, this, &RunnerClient::onFinished);
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            Q_EMIT logLine(QStringLiteral("error"), QStringLiteral("Cannot start the runner: %1").arg(m_proc->errorString()));
            Q_EMIT finished(QStringLiteral("crashed"), m_proc->errorString(), m_resultsFile);
        }
    });
    m_proc->start();
    Q_EMIT started();
    return true;
}

void RunnerClient::stop() {
    if (!isRunning()) return;
    m_stopRequested = true;
    m_proc->write("stop\n");
    // If the runner is stuck in the driver it cannot react; kill it after a grace period.
    QTimer::singleShot(15000, this, [this] {
        if (isRunning()) {
            Q_EMIT logLine(QStringLiteral("warn"), tr("The runner did not stop in time and was terminated."));
            m_proc->kill();
        }
    });
}

void RunnerClient::kill() {
    if (isRunning()) m_proc->kill();
}

void RunnerClient::onStdout() {
    m_buffer += m_proc->readAllStandardOutput();
    int nl;
    while ((nl = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(nl).trimmed();
        m_buffer.remove(0, nl + 1);
        if (line.isEmpty()) continue;
        try {
            handleEvent(nlohmann::json::parse(line.constData(), line.constData() + line.size()));
        } catch (const std::exception&) {
            m_log << QString::fromUtf8(line);
        }
    }
}

void RunnerClient::onStderr() {
    const QStringList lines = QString::fromUtf8(m_proc->readAllStandardError()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& l : lines) {
        m_log << l;
        if (m_log.size() > 5000) m_log.removeFirst();
        Q_EMIT logLine(QStringLiteral("stderr"), l);
    }
}

void RunnerClient::handleEvent(const nlohmann::json& e) {
    const std::string type = e.value("type", "");
    if (type == "hello") {
        m_resultsFile = QString::fromStdString(e.value("results_file", m_resultsFile.toStdString()));
    } else if (type == "run_start") {
        m_run.config.suite = e.value("suite", "");
        m_run.config.api = e.value("api", "");
        if (e.contains("gpu")) m_run.system.gpu.name = e["gpu"].value("name", "");
        Q_EMIT runStarted(e);
    } else if (type == "loop_start") {
        results::LoopResult l;
        l.index = e.value("loop", 0);
        l.startS = e.value("t", 0.0);
        m_run.loops.push_back(l);
        Q_EMIT loopStarted(l.index);
    } else if (type == "test_start") {
        Q_EMIT testStarted(QString::fromStdString(e.value("test", "")), QString::fromStdString(e.value("name", "")),
                           e.value("index", 0), e.value("count", 0), e.value("loop", 0));
    } else if (type == "progress") {
        Q_EMIT progress(QString::fromStdString(e.value("test", "")), e.value("fraction", 0.0),
                        QString::fromStdString(e.value("status", "")));
    } else if (type == "frames") {
        QVector<double> v;
        for (const auto& x : e.value("frame_ms", nlohmann::json::array()))
            if (x.is_number()) v.push_back(x.get<double>());
        Q_EMIT frames(v, e.value("t", 0.0));
    } else if (type == "telemetry") {
        const results::TelemetrySample s = results::sampleFromJson(e);
        m_run.telemetry.samples.push_back(s);
        Q_EMIT telemetry(s);
    } else if (type == "test_end") {
        const results::TestResult t = results::testFromJson(e.value("result", nlohmann::json::object()));
        m_run.tests.push_back(t);
        Q_EMIT testFinished(t);
    } else if (type == "loop_end") {
        const int loop = e.value("loop", 0);
        const double score = e.value("score", 0.0);
        for (auto& l : m_run.loops)
            if (l.index == loop) l.score = score;
        Q_EMIT loopFinished(loop, score);
    } else if (type == "log") {
        const QString text = QString::fromStdString(e.value("message", ""));
        m_log << text;
        Q_EMIT logLine(QString::fromStdString(e.value("level", "info")), text);
    } else if (type == "safety_abort") {
        Q_EMIT safetyAbort(QString::fromStdString(e.value("message", "")));
    } else if (type == "device_lost") {
        Q_EMIT deviceLost(QString::fromStdString(e.value("message", "")));
    } else if (type == "error") {
        Q_EMIT logLine(QStringLiteral("error"), QString::fromStdString(e.value("message", "")));
        m_finalMessage = QString::fromStdString(e.value("message", ""));
    } else if (type == "run_end") {
        m_finalStatus = QString::fromStdString(e.value("status", "completed"));
        m_finalMessage = QString::fromStdString(e.value("message", m_finalMessage.toStdString()));
        m_resultsFile = QString::fromStdString(e.value("results_file", m_resultsFile.toStdString()));
    }
}

void RunnerClient::onFinished(int exitCode, QProcess::ExitStatus status) {
    onStdout();
    QString st = m_finalStatus;
    QString msg = m_finalMessage;
    if (st.isEmpty()) {
        // No run_end event: the runner died (driver crash, GPU reset, killed).
        st = QStringLiteral("crashed");
        msg = status == QProcess::CrashExit
                  ? tr("The runner crashed (this usually means the graphics driver crashed or the GPU was reset). "
                       "Tests that finished before the crash were kept.")
                  : tr("The runner exited unexpectedly (code %1). Tests that finished were kept.").arg(exitCode);
        if (m_stopRequested) {
            st = QStringLiteral("stopped");
            msg = tr("Stopped.");
        }
        m_run.status = results::RunStatus::Crashed;
        m_run.statusMessage = msg.toStdString();
    }
    Q_EMIT finished(st, msg, m_resultsFile);
}

void RunnerClient::replay(const QString& jsonlPath) {
    QFile f(jsonlPath);
    if (!f.open(QIODevice::ReadOnly)) return;
    m_run = results::RunResult{};
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (line.isEmpty()) continue;
        try {
            handleEvent(nlohmann::json::parse(line.constData(), line.constData() + line.size()));
        } catch (...) {
        }
    }
}

} // namespace gt::gui
