#include "pages.h"

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

namespace gt::gui {
namespace {

class FlowRow : public QWidget {
public:
    explicit FlowRow(QWidget* parent = nullptr) : QWidget(parent) {}
};

QString typeLabel(const QString& t) {
    if (t == QLatin1String("discrete")) return QObject::tr("Discrete GPU");
    if (t == QLatin1String("integrated")) return QObject::tr("Integrated GPU");
    if (t == QLatin1String("cpu")) return QObject::tr("Software rasterizer (CPU)");
    if (t == QLatin1String("virtual")) return QObject::tr("Virtual GPU");
    return t;
}

} // namespace

HomePage::HomePage(AppState* state, QWidget* parent)
    : Page(tr("Home"), tr("Pick the GPU to test. Systems with an integrated and a dedicated GPU list both."), parent),
      m_state(state) {
    m_status = makeHint(tr("Looking for GPUs…"));
    m_content->addWidget(m_status);

    m_gpuBox = new QWidget;
    m_gpuGrid = new QGridLayout(m_gpuBox);
    m_gpuGrid->setContentsMargins(0, 0, 0, 0);
    m_gpuGrid->setSpacing(12);
    m_content->addWidget(m_gpuBox);

    auto* detail = new Card;
    auto* dl = static_cast<QVBoxLayout*>(detail->layout());
    auto* row = new QHBoxLayout;
    row->addWidget(makeSubtitle(tr("Selected GPU")));
    row->addStretch();
    auto* apiLabel = new QLabel(tr("Graphics API:"));
    m_api = new QComboBox;
    m_api->setAccessibleName(tr("Graphics API"));
    apiLabel->setBuddy(m_api);
    row->addWidget(apiLabel);
    row->addWidget(m_api);
    dl->addLayout(row);
    m_detailLine = new QLabel;
    m_detailLine->setWordWrap(true);
    m_detailLine->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dl->addWidget(m_detailLine);
    m_badgeBox = new QWidget;
    auto* bl = new QHBoxLayout(m_badgeBox);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(6);
    dl->addWidget(m_badgeBox);
    auto* actions = new QHBoxLayout;
    m_runButton = new QPushButton(themedIcon(QStringLiteral("play")), tr("Run recommended suite"));
    m_runButton->setDefault(true);
    m_runButton->setMinimumHeight(36);
    m_runButton->setToolTip(tr("Runs the Standard suite at 1080p / High with your safety limits."));
    auto* sys = new QPushButton(themedIcon(QStringLiteral("cpu")), tr("System && features…"));
    actions->addWidget(m_runButton);
    actions->addWidget(sys);
    actions->addStretch();
    dl->addLayout(actions);
    m_content->addWidget(detail);
    m_content->addStretch();

    connect(m_runButton, &QPushButton::clicked, this, &HomePage::runRecommended);
    connect(sys, &QPushButton::clicked, this, &HomePage::showSystem);
    connect(m_api, &QComboBox::activated, this, [this] {
        m_state->select(m_state->selectedGpuKey(), m_api->currentData().toString());
    });
    connect(state, &AppState::devicesChanged, this, &HomePage::rebuildGpus);
    connect(state, &AppState::selectionChanged, this, &HomePage::rebuildGpus);
    connect(state, &AppState::detailsChanged, this, &HomePage::updateDetails);
    connect(state, &AppState::error, this, [this](const QString& msg) { m_status->setText(msg); });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this, sys] {
        m_runButton->setIcon(themedIcon(QStringLiteral("play")));
        sys->setIcon(themedIcon(QStringLiteral("cpu")));
    });
    rebuildGpus();
}

void HomePage::rebuildGpus() {
    clearLayout(m_gpuGrid);
    const auto& gpus = m_state->gpus();
    if (gpus.isEmpty()) {
        if (m_state->loaded()) m_status->setText(tr("No GPUs were found. Check that a graphics driver is installed."));
        m_runButton->setEnabled(false);
        return;
    }
    m_status->setText(gpus.size() == 1 ? tr("1 GPU found.") : tr("%1 GPUs found.").arg(gpus.size()));
    int i = 0;
    for (const auto& g : gpus) {
        auto* card = new Card;
        card->setClickable(true);
        card->setSelected(g.key == m_state->selectedGpuKey());
        card->setAccessibleName(tr("GPU: %1").arg(g.name));
        auto* l = static_cast<QVBoxLayout*>(card->layout());
        auto* head = new QHBoxLayout;
        auto* icon = new QLabel;
        icon->setPixmap(themedPixmap(QStringLiteral("cpu"), 28, ThemeManager::instance().accent(), devicePixelRatioF()));
        head->addWidget(icon);
        auto* name = makeSubtitle(g.name);
        name->setWordWrap(true);
        head->addWidget(name, 1);
        l->addLayout(head);
        QString line = typeLabel(g.type);
        if (g.vramBytes > 0 && g.type == QLatin1String("discrete")) line += QStringLiteral(" · %1").arg(formatBytes(g.vramBytes));
        if (!g.pci.isEmpty()) line += QStringLiteral(" · PCI %1").arg(g.pci);
        l->addWidget(makeHint(line));
        auto* apis = new QHBoxLayout;
        apis->setSpacing(6);
        for (auto it = g.apiIndex.begin(); it != g.apiIndex.end(); ++it) {
            auto* b = new Badge(QStringLiteral("%1 %2").arg(apiDisplayName(it.key()), g.apiVersion.value(it.key())),
                                it.key() == m_state->selectedApi() && g.key == m_state->selectedGpuKey()
                                    ? Badge::Kind::Accent
                                    : Badge::Kind::Neutral);
            apis->addWidget(b);
        }
        apis->addStretch();
        l->addLayout(apis);
        const QString drv = g.driver.value(m_state->selectedApi(), g.driver.isEmpty() ? QString() : g.driver.first());
        if (!drv.isEmpty()) l->addWidget(makeHint(tr("Driver %1").arg(drv)));
        if (g.software)
            l->addWidget(makeHint(tr("This is a CPU-based software renderer: fine for checking that things work, "
                                     "meaningless for performance.")));
        const QString key = g.key;
        connect(card, &Card::clicked, this, [this, key] { m_state->select(key, m_state->selectedApi()); });
        m_gpuGrid->addWidget(card, i / 2, i % 2);
        ++i;
    }
    m_gpuGrid->setColumnStretch(0, 1);
    m_gpuGrid->setColumnStretch(1, 1);

    m_api->clear();
    if (const GpuEntry* g = m_state->selectedGpu()) {
        for (auto it = g->apiIndex.begin(); it != g->apiIndex.end(); ++it) m_api->addItem(apiDisplayName(it.key()), it.key());
        m_api->setCurrentIndex(std::max(0, m_api->findData(m_state->selectedApi())));
    }
    m_runButton->setEnabled(true);
    updateDetails();
}

void HomePage::updateDetails() {
    const GpuEntry* g = m_state->selectedGpu();
    const auto& si = m_state->sysinfo();
    QStringList parts;
    if (g) parts << QStringLiteral("<b>%1</b>").arg(g->name.toHtmlEscaped());
    if (si.contains("gpu")) {
        const auto& gpu = si["gpu"];
        auto s = [&](const char* k) { return QString::fromStdString(gpu.value(k, std::string())); };
        if (!s("driver_name").isEmpty()) parts << tr("Driver %1 %2").arg(s("driver_name"), s("driver_version"));
        if (!s("vbios").isEmpty()) parts << tr("VBIOS %1").arg(s("vbios"));
        if (!s("pcie_current").isEmpty())
            parts << tr("PCIe %1 (max %2)").arg(s("pcie_current"), s("pcie_max").isEmpty() ? QStringLiteral("?") : s("pcie_max"));
        parts << tr("Telemetry: %1").arg(QString::fromStdString(si.value("telemetry_provider", std::string("none"))));
    }
    m_detailLine->setText(parts.join(QStringLiteral(" · ")));

    clearLayout(m_badgeBox->layout());
    if (!si.contains("features")) return;
    const auto& f = si["features"];
    struct B {
        const char* key;
        QString label;
    };
    const B badges[] = {{"ray_tracing_pipeline", tr("Ray tracing")}, {"ray_query", tr("Ray query")},
                        {"mesh_shader", tr("Mesh shaders")},        {"variable_rate_shading", tr("VRS")},
                        {"shader_float16", tr("FP16")},             {"shader_float64", tr("FP64")},
                        {"shader_int8", tr("INT8")},                {"atomic_int64", tr("64-bit atomics")},
                        {"async_compute_queue", tr("Async compute")}, {"resizable_bar", tr("Resizable BAR")}};
    for (const auto& b : badges) {
        const bool on = f.value(b.key, false);
        auto* badge = new Badge((on ? QStringLiteral("✓ ") : QStringLiteral("✗ ")) + b.label,
                                on ? Badge::Kind::Good : Badge::Kind::Neutral);
        badge->setAccessibleName(b.label + (on ? tr(": supported") : tr(": not supported")));
        badge->setToolTip(badge->accessibleName());
        m_badgeBox->layout()->addWidget(badge);
    }
    static_cast<QHBoxLayout*>(m_badgeBox->layout())->addStretch();
}

} // namespace gt::gui
