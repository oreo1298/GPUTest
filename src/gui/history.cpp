#include "history.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace gt::gui {
namespace {
const char* kConn = "history";
QSqlDatabase db() { return QSqlDatabase::database(QLatin1String(kConn)); }
} // namespace

History& History::instance() {
    static History h;
    return h;
}

bool History::open(const QString& dbPath, QString* error) {
    QDir().mkpath(QFileInfo(dbPath).absolutePath());
    QSqlDatabase d = QSqlDatabase::contains(QLatin1String(kConn))
                         ? QSqlDatabase::database(QLatin1String(kConn))
                         : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QLatin1String(kConn));
    d.setDatabaseName(dbPath);
    if (!d.open()) {
        if (error) *error = d.lastError().text();
        return false;
    }
    QSqlQuery q(d);
    const char* schema[] = {
        "CREATE TABLE IF NOT EXISTS runs (id INTEGER PRIMARY KEY AUTOINCREMENT, path TEXT UNIQUE NOT NULL, "
        "mtime INTEGER, created TEXT, suite TEXT, api TEXT, gpu TEXT, gpu_vendor INTEGER, gpu_device INTEGER, "
        "driver TEXT, width INTEGER, height INTEGER, quality TEXT, duration TEXT, loop TEXT, status TEXT, "
        "overall REAL, comparable_key TEXT, tests INTEGER, duration_s REAL, app_version TEXT)",
        "CREATE TABLE IF NOT EXISTS test_results (run_id INTEGER REFERENCES runs(id) ON DELETE CASCADE, "
        "test_id TEXT, name TEXT, loop INTEGER, status TEXT, score REAL, avg_fps REAL, low1_fps REAL, p99_ms REAL, "
        "verdict TEXT)",
        "CREATE INDEX IF NOT EXISTS idx_test_results_run ON test_results(run_id)",
        "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)",
        "CREATE TABLE IF NOT EXISTS forgotten (path TEXT PRIMARY KEY)",
        "INSERT OR IGNORE INTO meta(key, value) VALUES('schema_version', '1')",
        "PRAGMA foreign_keys = ON",
    };
    for (const char* s : schema) {
        if (!q.exec(QLatin1String(s))) {
            if (error) *error = q.lastError().text();
            return false;
        }
    }
    m_open = true;
    m_path = dbPath;
    return true;
}

bool History::importFile(const QString& pathIn, QString* error) {
    if (!m_open) return false;
    const QString path = QFileInfo(pathIn).absoluteFilePath();
    QSqlQuery f(db());
    f.prepare(QStringLiteral("DELETE FROM forgotten WHERE path = ?"));
    f.addBindValue(path);
    f.exec();
    return importOne(path, error);
}

bool History::importOne(const QString& path, QString* error) {
    std::string err;
    auto run = results::readFile(path.toStdString(), &err);
    if (!run) {
        if (error) *error = QString::fromStdString(err);
        return false;
    }
    QSqlDatabase d = db();
    d.transaction();
    QSqlQuery q(d);
    q.prepare(QStringLiteral("DELETE FROM test_results WHERE run_id IN (SELECT id FROM runs WHERE path = ?)"));
    q.addBindValue(path);
    q.exec();
    q.prepare(QStringLiteral(
        "INSERT INTO runs (path, mtime, created, suite, api, gpu, gpu_vendor, gpu_device, driver, width, height, "
        "quality, duration, loop, status, overall, comparable_key, tests, duration_s, app_version) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(path) DO UPDATE SET mtime=excluded.mtime, created=excluded.created, suite=excluded.suite, "
        "api=excluded.api, gpu=excluded.gpu, gpu_vendor=excluded.gpu_vendor, gpu_device=excluded.gpu_device, "
        "driver=excluded.driver, width=excluded.width, height=excluded.height, quality=excluded.quality, "
        "duration=excluded.duration, loop=excluded.loop, status=excluded.status, overall=excluded.overall, "
        "comparable_key=excluded.comparable_key, tests=excluded.tests, duration_s=excluded.duration_s, "
        "app_version=excluded.app_version"));
    const auto& g = run->system.gpu;
    q.addBindValue(path);
    q.addBindValue(QFileInfo(path).lastModified().toSecsSinceEpoch());
    q.addBindValue(QString::fromStdString(run->createdUtc));
    q.addBindValue(QString::fromStdString(run->config.suite));
    q.addBindValue(QString::fromStdString(run->config.api));
    q.addBindValue(QString::fromStdString(g.name));
    q.addBindValue(static_cast<qint64>(g.vendorId));
    q.addBindValue(static_cast<qint64>(g.deviceId));
    q.addBindValue(QString::fromStdString(g.driverName + " " + g.driverVersion));
    q.addBindValue(run->config.width);
    q.addBindValue(run->config.height);
    q.addBindValue(QString::fromStdString(run->config.quality));
    q.addBindValue(QString::fromStdString(run->config.duration));
    q.addBindValue(QString::fromStdString(run->config.loop));
    q.addBindValue(QString::fromStdString(results::toString(run->status)));
    q.addBindValue(run->scores.overall);
    q.addBindValue(QString::fromStdString(run->scores.comparableKey));
    q.addBindValue(static_cast<qint64>(run->tests.size()));
    q.addBindValue(run->durationS);
    q.addBindValue(QString::fromStdString(run->appVersion));
    if (!q.exec()) {
        if (error) *error = q.lastError().text();
        d.rollback();
        return false;
    }
    QSqlQuery idq(d);
    idq.prepare(QStringLiteral("SELECT id FROM runs WHERE path = ?"));
    idq.addBindValue(path);
    idq.exec();
    const qint64 runId = idq.next() ? idq.value(0).toLongLong() : 0;
    QSqlQuery tq(d);
    tq.prepare(QStringLiteral("INSERT INTO test_results (run_id, test_id, name, loop, status, score, avg_fps, low1_fps, "
                              "p99_ms, verdict) VALUES (?,?,?,?,?,?,?,?,?,?)"));
    for (const auto& t : run->tests) {
        tq.addBindValue(runId);
        tq.addBindValue(QString::fromStdString(t.id));
        tq.addBindValue(QString::fromStdString(t.name));
        tq.addBindValue(t.loop);
        tq.addBindValue(QString::fromStdString(results::toString(t.status)));
        tq.addBindValue(t.scored ? QVariant(t.score) : QVariant());
        tq.addBindValue(t.frameStats ? QVariant(t.frameStats->avgFps) : QVariant());
        tq.addBindValue(t.frameStats ? QVariant(t.frameStats->low1Fps) : QVariant());
        tq.addBindValue(t.frameStats ? QVariant(t.frameStats->p99Ms) : QVariant());
        tq.addBindValue(QString::fromStdString(results::toString(t.verdict)));
        tq.exec();
    }
    d.commit();
    if (!m_batch) Q_EMIT changed();
    return true;
}

int History::scanFolder(const QString& dir) {
    if (!m_open) return 0;
    QHash<QString, qint64> known;
    QSqlQuery q(db());
    q.exec(QStringLiteral("SELECT path, mtime FROM runs"));
    while (q.next()) known.insert(q.value(0).toString(), q.value(1).toLongLong());
    QSet<QString> forgotten;
    q.exec(QStringLiteral("SELECT path FROM forgotten"));
    while (q.next()) forgotten.insert(q.value(0).toString());
    int n = 0;
    m_batch = true;
    QDirIterator it(dir, {QStringLiteral("*.json")}, QDir::Files);
    while (it.hasNext()) {
        const QString path = it.next();
        const qint64 mtime = QFileInfo(path).lastModified().toSecsSinceEpoch();
        if (known.value(path, -1) == mtime || forgotten.contains(path)) continue;
        if (importOne(path, nullptr)) ++n;
    }
    // Forget runs whose files were deleted.
    QSqlQuery del(db());
    for (auto i = known.begin(); i != known.end(); ++i) {
        if (!QFileInfo::exists(i.key())) {
            del.prepare(QStringLiteral("DELETE FROM runs WHERE path = ?"));
            del.addBindValue(i.key());
            del.exec();
            ++n;
        }
    }
    m_batch = false;
    if (n) Q_EMIT changed();
    return n;
}

QVector<HistoryEntry> History::entries(const QString& filter) const {
    QVector<HistoryEntry> out;
    if (!m_open) return out;
    QSqlQuery q(db());
    QString sql = QStringLiteral("SELECT id, path, created, suite, api, gpu, driver, width, height, quality, duration, "
                                 "loop, status, overall, comparable_key, tests, duration_s FROM runs");
    if (!filter.isEmpty()) sql += QStringLiteral(" WHERE gpu LIKE ? OR suite LIKE ? OR api LIKE ? OR created LIKE ?");
    sql += QStringLiteral(" ORDER BY created DESC");
    q.prepare(sql);
    if (!filter.isEmpty())
        for (int i = 0; i < 4; ++i) q.addBindValue(QStringLiteral("%%1%").arg(filter));
    q.exec();
    while (q.next()) {
        HistoryEntry e;
        e.id = q.value(0).toLongLong();
        e.path = q.value(1).toString();
        e.created = q.value(2).toString();
        e.suite = q.value(3).toString();
        e.api = q.value(4).toString();
        e.gpu = q.value(5).toString();
        e.driver = q.value(6).toString();
        e.width = q.value(7).toInt();
        e.height = q.value(8).toInt();
        e.quality = q.value(9).toString();
        e.duration = q.value(10).toString();
        e.loop = q.value(11).toString();
        e.status = q.value(12).toString();
        e.overall = q.value(13).toDouble();
        e.comparableKey = q.value(14).toString();
        e.tests = q.value(15).toInt();
        e.durationS = q.value(16).toDouble();
        out.push_back(e);
    }
    return out;
}

bool History::remove(qint64 id) {
    QSqlQuery q(db());
    q.prepare(QStringLiteral("INSERT OR IGNORE INTO forgotten(path) SELECT path FROM runs WHERE id = ?"));
    q.addBindValue(id);
    q.exec();
    q.prepare(QStringLiteral("DELETE FROM runs WHERE id = ?"));
    q.addBindValue(id);
    const bool ok = q.exec();
    if (ok) Q_EMIT changed();
    return ok;
}

} // namespace gt::gui
