#include "appstate.h"

#include "settings.h"

namespace gt::gui {

QString apiDisplayName(const QString& api) {
    if (api == QLatin1String("vulkan")) return QStringLiteral("Vulkan");
    if (api == QLatin1String("d3d12")) return QStringLiteral("Direct3D 12");
    if (api == QLatin1String("d3d11")) return QStringLiteral("Direct3D 11");
    if (api == QLatin1String("opengl")) return QStringLiteral("OpenGL");
    return api;
}

namespace {
QString qs(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? QString::fromStdString(it->get<std::string>()) : QString();
}
QStringList qlist(const nlohmann::json& j, const char* key) {
    QStringList out;
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return out;
    for (const auto& v : *it)
        if (v.is_string()) out << QString::fromStdString(v.get<std::string>());
    return out;
}
nlohmann::json parse(const QByteArray& data) {
    try {
        return nlohmann::json::parse(data.constData(), data.constData() + data.size());
    } catch (...) {
        return {};
    }
}
} // namespace

AppState::AppState(RunnerClient* client, QObject* parent) : QObject(parent), m_client(client) {
    m_gpuKey = Settings::instance().gpuKey();
    m_api = Settings::instance().api();
}

const TestEntry* AppState::test(const QString& id) const {
    for (const auto& t : m_tests)
        if (t.id == id) return &t;
    return nullptr;
}

const GpuEntry* AppState::selectedGpu() const {
    for (const auto& g : m_gpus)
        if (g.key == m_gpuKey) return &g;
    return m_gpus.isEmpty() ? nullptr : &m_gpus.front();
}

int AppState::selectedAdapterIndex() const {
    const GpuEntry* g = selectedGpu();
    return g ? g->apiIndex.value(m_api, -1) : -1;
}

void AppState::select(const QString& gpuKey, const QString& api) {
    const GpuEntry* g = nullptr;
    for (const auto& e : m_gpus)
        if (e.key == gpuKey) g = &e;
    if (!g) return;
    QString a = api;
    if (!g->apiIndex.contains(a)) a = g->apiIndex.contains(QStringLiteral("vulkan")) ? QStringLiteral("vulkan")
                                                                                     : g->apiIndex.firstKey();
    if (gpuKey == m_gpuKey && a == m_api) return;
    m_gpuKey = gpuKey;
    m_api = a;
    Settings::instance().setGpuKey(m_gpuKey);
    Settings::instance().setApi(m_api);
    Q_EMIT selectionChanged();
    loadDetails();
}

void AppState::setDevicesJson(const nlohmann::json& d) {
    m_gpus.clear();
    m_apis.clear();
    int bestRank = 99;
    QString bestKey;
    for (const auto& api : d.value("apis", nlohmann::json::array())) {
        const QString apiId = qs(api, "api");
        if (api.value("available", false)) m_apis << apiId;
        for (const auto& a : api.value("adapters", nlohmann::json::array())) {
            const QString pci = qs(a, "pci_address");
            const QString key = !pci.isEmpty() ? pci
                                               : QStringLiteral("%1:%2:%3").arg(qs(a, "vendor_id"), qs(a, "device_id"),
                                                                                 qs(a, "name"));
            GpuEntry* e = nullptr;
            for (auto& g : m_gpus)
                if (g.key == key) e = &g;
            if (!e) {
                m_gpus.push_back({});
                e = &m_gpus.back();
                e->key = key;
                e->name = qs(a, "name");
                e->vendor = qs(a, "vendor");
                e->type = qs(a, "type");
                e->software = a.value("software", false);
                e->pci = pci;
                e->vramBytes = a.value("dedicated_memory", 0.0);
            }
            e->apiIndex.insert(apiId, a.value("index", 0));
            e->driver.insert(apiId, qs(a, "driver_version"));
            e->apiVersion.insert(apiId, qs(a, "api_version"));
            const int rank = e->software ? 3 : e->type == QLatin1String("discrete") ? 0 : e->type == QLatin1String("integrated") ? 1 : 2;
            if (rank < bestRank) {
                bestRank = rank;
                bestKey = key;
            }
        }
    }
    bool found = false;
    for (const auto& g : m_gpus) found = found || g.key == m_gpuKey;
    if (!found) m_gpuKey = bestKey;
    if (const GpuEntry* g = selectedGpu(); g && !g->apiIndex.contains(m_api))
        m_api = g->apiIndex.contains(QStringLiteral("vulkan")) ? QStringLiteral("vulkan") : g->apiIndex.firstKey();
    Q_EMIT devicesChanged();
    Q_EMIT selectionChanged();
}

void AppState::setTestsJson(const nlohmann::json& j) {
    m_tests.clear();
    for (const auto& t : j) {
        TestEntry e;
        e.id = qs(t, "id");
        e.name = qs(t, "name");
        e.description = qs(t, "description");
        e.category = qs(t, "category");
        e.group = qs(t, "group");
        e.apis = qlist(t, "apis");
        e.suites = qlist(t, "suites");
        e.tags = qlist(t, "tags");
        e.scored = t.value("scored", false);
        m_tests.push_back(e);
    }
    Q_EMIT catalogChanged();
}

void AppState::setSuitesJson(const nlohmann::json& j) {
    m_suites.clear();
    for (const auto& s : j) {
        SuiteEntry e;
        e.id = qs(s, "id");
        e.name = qs(s, "name");
        e.description = qs(s, "description");
        e.duration = qs(s, "duration");
        e.quality = qs(s, "quality");
        e.tests = qlist(s, "tests");
        m_suites.push_back(e);
    }
    Q_EMIT catalogChanged();
}

void AppState::refresh() {
    if (!RunnerClient::runnerAvailable()) {
        m_error = tr("The benchmark runner was not found at %1. Set its location in Settings.")
                      .arg(RunnerClient::runnerPath());
        Q_EMIT error(m_error);
        return;
    }
    m_client->query({QStringLiteral("--list-devices"), QStringLiteral("--json")},
                    [this](bool ok, const QByteArray& out, const QString& err) {
                        if (!ok) {
                            m_error = tr("Could not list GPUs: %1").arg(err);
                            Q_EMIT error(m_error);
                            return;
                        }
                        setDevicesJson(parse(out));
                        m_loaded = true;
                        loadDetails();
                    });
    m_client->query({QStringLiteral("--list-tests"), QStringLiteral("--json")},
                    [this](bool ok, const QByteArray& out, const QString&) {
                        if (ok) setTestsJson(parse(out));
                    });
    m_client->query({QStringLiteral("--list-suites"), QStringLiteral("--json")},
                    [this](bool ok, const QByteArray& out, const QString&) {
                        if (ok) setSuitesJson(parse(out));
                    });
}

void AppState::loadDetails() {
    const int idx = selectedAdapterIndex();
    if (idx < 0 || m_api.isEmpty()) return;
    const QStringList sel{QStringLiteral("--api"), m_api, QStringLiteral("--gpu"), QString::number(idx)};
    const QString gpu = m_gpuKey, api = m_api;
    m_client->query(QStringList{QStringLiteral("--sysinfo")} + sel, [this, gpu, api](bool ok, const QByteArray& out, const QString&) {
        if (!ok || gpu != m_gpuKey || api != m_api) return;
        m_sysinfo = parse(out);
        Q_EMIT detailsChanged();
    });
    m_client->query(QStringList{QStringLiteral("--caps"), QStringLiteral("--json")} + sel,
                    [this, gpu, api](bool ok, const QByteArray& out, const QString&) {
                        if (!ok || gpu != m_gpuKey || api != m_api) return;
                        m_caps = parse(out);
                        Q_EMIT detailsChanged();
                    });
    m_client->query(QStringList{QStringLiteral("--check-support"), QStringLiteral("--json")} + sel,
                    [this, gpu, api](bool ok, const QByteArray& out, const QString&) {
                        if (!ok || gpu != m_gpuKey || api != m_api) return;
                        m_support = parse(out);
                        Q_EMIT detailsChanged();
                    });
}

} // namespace gt::gui
