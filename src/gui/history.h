#pragma once
// Local run history in SQLite (QtSql QSQLITE driver). Result files stay the
// source of truth; the database indexes them for the History & Compare page.
// New files in the results folder are imported automatically.

#include "gt/results.h"

#include <QObject>
#include <QString>
#include <QVector>

namespace gt::gui {

struct HistoryEntry {
    qint64 id = 0;
    QString path;
    QString created;
    QString suite, api, gpu, driver, quality, duration, loop, status, comparableKey;
    int width = 0, height = 0;
    double overall = 0.0;
    int tests = 0;
    double durationS = 0.0;
};

class History : public QObject {
    Q_OBJECT
public:
    static History& instance();
    bool open(const QString& dbPath, QString* error = nullptr);
    bool isOpen() const { return m_open; }
    /// Imports (or refreshes) one results file. Returns false if it is not a results file.
    /// An explicit import also un-forgets a file removed earlier.
    bool importFile(const QString& path, QString* error = nullptr);
    /// Imports every *.json in `dir` that is new or changed since the last import
    /// (except forgotten files) and drops runs whose files no longer exist.
    int scanFolder(const QString& dir);
    QVector<HistoryEntry> entries(const QString& filter = {}) const;
    /// Removes a run from the history; scans will not re-add its file.
    bool remove(qint64 id);
    QString dbPath() const { return m_path; }

Q_SIGNALS:
    void changed();

private:
    History() = default;
    bool importOne(const QString& path, QString* error);
    bool m_open = false;
    bool m_batch = false;
    QString m_path;
};

} // namespace gt::gui
