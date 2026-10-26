#include "pages.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTreeView>
#include <QTreeWidget>

namespace gt::gui {

SystemPage::SystemPage(AppState* state, QWidget* parent)
    : Page(tr("System & features"), tr("System information and everything the selected GPU and API report. Use the "
                                       "search box to filter."),
           parent),
      m_state(state) {
    auto* row = new QHBoxLayout;
    auto* hostCard = new Card;
    static_cast<QVBoxLayout*>(hostCard->layout())->addWidget(makeSubtitle(tr("Computer")));
    m_host = new QLabel;
    m_host->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_host->setWordWrap(true);
    static_cast<QVBoxLayout*>(hostCard->layout())->addWidget(m_host);
    auto* gpuCard = new Card;
    static_cast<QVBoxLayout*>(gpuCard->layout())->addWidget(makeSubtitle(tr("Graphics")));
    m_gpu = new QLabel;
    m_gpu->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_gpu->setWordWrap(true);
    static_cast<QVBoxLayout*>(gpuCard->layout())->addWidget(m_gpu);
    static_cast<QVBoxLayout*>(hostCard->layout())->addStretch();
    static_cast<QVBoxLayout*>(gpuCard->layout())->addStretch();
    row->addWidget(hostCard, 1);
    row->addWidget(gpuCard, 1);
    m_content->addLayout(row);

    auto* capsCard = new Card;
    auto* cl = static_cast<QVBoxLayout*>(capsCard->layout());
    auto* head = new QHBoxLayout;
    head->addWidget(makeSubtitle(tr("Capability explorer")));
    head->addStretch();
    m_search = new QLineEdit;
    m_search->setPlaceholderText(tr("Search extensions, features, limits…"));
    m_search->setClearButtonEnabled(true);
    m_search->setAccessibleName(tr("Search capabilities"));
    m_search->setMinimumWidth(280);
    head->addWidget(m_search);
    cl->addLayout(head);
    m_model = new QStandardItemModel(this);
    m_model->setHorizontalHeaderLabels({tr("Name"), tr("Value")});
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setRecursiveFilteringEnabled(true);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setFilterKeyColumn(-1);
    m_view = new QTreeView;
    m_view->setModel(m_proxy);
    m_view->setUniformRowHeights(true);
    m_view->setAlternatingRowColors(true);
    m_view->setAccessibleName(tr("Capabilities"));
    m_view->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_view->header()->resizeSection(0, 420);
    m_view->setMinimumHeight(320);
    cl->addWidget(m_view);
    m_content->addWidget(capsCard);

    auto* supCard = new Card;
    auto* sl = static_cast<QVBoxLayout*>(supCard->layout());
    sl->addWidget(makeSubtitle(tr("Test support")));
    sl->addWidget(makeHint(tr("Every test that cannot run on this GPU/API, and why.")));
    m_support = new QTreeWidget;
    m_support->setColumnCount(2);
    m_support->setHeaderLabels({tr("Test"), tr("Status")});
    m_support->setRootIsDecorated(false);
    m_support->setAccessibleName(tr("Test support"));
    m_support->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_support->setMinimumHeight(160);
    sl->addWidget(m_support);
    m_content->addWidget(supCard);

    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& t) {
        m_proxy->setFilterFixedString(t);
        if (!t.isEmpty()) m_view->expandAll();
    });
    connect(state, &AppState::detailsChanged, this, &SystemPage::update);
    update();
}

void SystemPage::update() {
    const auto& si = m_state->sysinfo();
    auto s = [](const nlohmann::json& j, const char* k) {
        auto it = j.find(k);
        if (it == j.end()) return QString();
        if (it->is_string()) return QString::fromStdString(it->get<std::string>());
        if (it->is_number_integer()) return QString::number(it->get<long long>());
        if (it->is_boolean()) return it->get<bool>() ? QObject::tr("yes") : QObject::tr("no");
        return QString::fromStdString(it->dump());
    };
    auto table = [](const QList<QPair<QString, QString>>& rows) {
        QString html = QStringLiteral("<table cellspacing='4'>");
        for (const auto& [k, v] : rows) {
            if (v.isEmpty()) continue;
            html += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>").arg(k.toHtmlEscaped(), v.toHtmlEscaped());
        }
        return html + QStringLiteral("</table>");
    };
    if (si.contains("host")) {
        const auto& h = si["host"];
        const double ram = h.value("ram_bytes", 0.0);
        m_host->setText(table({{tr("CPU"), s(h, "cpu")},
                               {tr("Cores / threads"), QStringLiteral("%1 / %2").arg(s(h, "cpu_cores"), s(h, "cpu_threads"))},
                               {tr("Memory"), ram > 0 ? formatBytes(ram) : QString()},
                               {tr("OS"), s(h, "os")},
                               {tr("Kernel"), s(h, "kernel")},
                               {tr("Display server"), s(h, "display_server")},
                               {tr("Desktop"), s(h, "desktop")}}));
    }
    if (si.contains("gpu")) {
        const auto& g = si["gpu"];
        const double vram = g.value("vram_bytes", 0.0);
        m_gpu->setText(table({{tr("GPU"), s(g, "name")},
                              {tr("Vendor"), QStringLiteral("%1 (%2)").arg(s(g, "vendor"), s(g, "vendor_id"))},
                              {tr("Device / subsystem"), QStringLiteral("%1 / %2:%3").arg(s(g, "device_id"), s(g, "subsys_vendor_id"), s(g, "subsys_id"))},
                              {tr("Driver"), QStringLiteral("%1 %2").arg(s(g, "driver_name"), s(g, "driver_version"))},
                              {tr("Driver info"), s(g, "driver_info")},
                              {tr("VBIOS"), s(g, "vbios")},
                              {tr("API"), QStringLiteral("%1 %2").arg(apiDisplayName(s(g, "api")), s(g, "api_version"))},
                              {tr("VRAM"), vram > 0 ? formatBytes(vram) : QString()},
                              {tr("PCI address"), s(g, "pci_address")},
                              {tr("PCIe link"), s(g, "pcie_current").isEmpty() ? QString()
                                                                                : tr("%1 (max %2)").arg(s(g, "pcie_current"), s(g, "pcie_max"))},
                              {tr("Resizable BAR"), s(g, "resizable_bar")},
                              {tr("Telemetry"), s(si, "telemetry_provider")}}));
    }

    m_model->removeRows(0, m_model->rowCount());
    QMap<QString, QStandardItem*> groups;
    const QColor muted = ThemeManager::instance().textMuted();
    for (const auto& c : m_state->capabilities()) {
        const QString group = QString::fromStdString(c.value("group", std::string()));
        QStandardItem*& g = groups[group];
        if (!g) {
            g = new QStandardItem(group);
            QFont f = g->font();
            f.setWeight(QFont::DemiBold);
            g->setFont(f);
            m_model->appendRow({g, new QStandardItem()});
        }
        auto* name = new QStandardItem(QString::fromStdString(c.value("name", std::string())));
        auto* value = new QStandardItem(QString::fromStdString(c.value("value", std::string())));
        if (!c.value("supported", true)) {
            name->setForeground(muted);
            value->setForeground(muted);
        }
        g->appendRow({name, value});
    }
    for (auto* g : groups) g->setText(QStringLiteral("%1 (%2)").arg(g->text()).arg(g->rowCount()));

    m_support->clear();
    for (const auto& e : m_state->support()) {
        const bool ok = e.value("supported", true);
        const QString id = QString::fromStdString(e.value("id", std::string()));
        const TestEntry* t = m_state->test(id);
        auto* item = new QTreeWidgetItem(m_support, {t ? t->name : id,
                                                     ok ? tr("Supported")
                                                        : tr("Not supported — %1").arg(QString::fromStdString(e.value("reason", std::string())))});
        item->setToolTip(1, item->text(1));
    }
}

} // namespace gt::gui
