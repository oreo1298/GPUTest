#pragma once
// Central GUI state: GPUs (merged across APIs), the test catalogue, preset
// suites, the current selection, and per-GPU system info. Everything is
// obtained from the runner's information commands.

#include "runnerclient.h"

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <nlohmann/json.hpp>

namespace gt::gui {

struct GpuEntry {
    QString key;       ///< stable identity (PCI address, else vendor:device:name)
    QString name;
    QString vendor;
    QString type;      ///< discrete / integrated / cpu / ...
    bool software = false;
    QString pci;
    double vramBytes = 0;
    QMap<QString, int> apiIndex;      ///< api -> adapter index for --gpu
    QMap<QString, QString> driver;    ///< api -> driver version string
    QMap<QString, QString> apiVersion;///< api -> API version
};

struct TestEntry {
    QString id, name, description, category, group;
    QStringList apis, suites, tags;
    bool scored = false;
};

struct SuiteEntry {
    QString id, name, description, duration, quality;
    QStringList tests;
};

class AppState : public QObject {
    Q_OBJECT
public:
    explicit AppState(RunnerClient* client, QObject* parent = nullptr);

    void refresh();          ///< re-query devices, tests and suites
    bool loaded() const { return m_loaded; }
    QString loadError() const { return m_error; }

    const QVector<GpuEntry>& gpus() const { return m_gpus; }
    const QVector<TestEntry>& tests() const { return m_tests; }
    const QVector<SuiteEntry>& suites() const { return m_suites; }
    const TestEntry* test(const QString& id) const;
    QStringList apis() const { return m_apis; } ///< compiled & available APIs

    const GpuEntry* selectedGpu() const;
    QString selectedGpuKey() const { return m_gpuKey; }
    QString selectedApi() const { return m_api; }
    void select(const QString& gpuKey, const QString& api);
    int selectedAdapterIndex() const;

    /// Cached `--sysinfo` for the selected GPU/API (empty object until loaded).
    const nlohmann::json& sysinfo() const { return m_sysinfo; }
    const nlohmann::json& capabilities() const { return m_caps; }
    const nlohmann::json& support() const { return m_support; } ///< [{id, supported, reason}]
    void loadDetails(); ///< sysinfo + caps + support for the selection

    RunnerClient* client() const { return m_client; }

    /// For demos/screenshots: inject data without a runner.
    void setDevicesJson(const nlohmann::json& j);
    void setTestsJson(const nlohmann::json& j);
    void setSuitesJson(const nlohmann::json& j);

Q_SIGNALS:
    void devicesChanged();
    void catalogChanged();
    void selectionChanged();
    void detailsChanged();
    void error(const QString& message);

private:
    RunnerClient* m_client;
    bool m_loaded = false;
    QString m_error;
    QVector<GpuEntry> m_gpus;
    QVector<TestEntry> m_tests;
    QVector<SuiteEntry> m_suites;
    QStringList m_apis;
    QString m_gpuKey, m_api;
    nlohmann::json m_sysinfo = nlohmann::json::object();
    nlohmann::json m_caps = nlohmann::json::array();
    nlohmann::json m_support = nlohmann::json::array();
    int m_pending = 0;
};

QString apiDisplayName(const QString& api);

} // namespace gt::gui
