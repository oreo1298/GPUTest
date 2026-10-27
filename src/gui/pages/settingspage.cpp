#include "pages.h"

#include "gt/app_info.h"
#include "settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

namespace gt::gui {

SettingsPage::SettingsPage(QWidget* parent)
    : Page(tr("Settings"), tr("Changes are saved immediately."), parent) {
    auto& s = Settings::instance();

    auto* look = new Card;
    auto* ll = static_cast<QVBoxLayout*>(look->layout());
    ll->addWidget(makeSubtitle(tr("Appearance")));
    auto* lf = new QFormLayout;
    lf->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    auto* theme = new QComboBox;
    theme->addItem(tr("Follow the system"), QStringLiteral("system"));
    theme->addItem(tr("Light"), QStringLiteral("light"));
    theme->addItem(tr("Dark"), QStringLiteral("dark"));
    theme->setCurrentIndex(std::max(0, theme->findData(s.themeMode())));
    theme->setAccessibleName(tr("Theme"));
    lf->addRow(tr("&Theme:"), theme);
    auto* source = makeHint(QString());
    source->setWordWrap(false); // short; wrapping squeezed it into the combo box's width
    auto updateSource = [source] {
        source->setText(tr("System colour scheme: %1").arg(ThemeManager::instance().source()));
    };
    updateSource();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, source, updateSource);
    lf->addRow(QString(), source);
    auto* units = new QComboBox;
    units->addItem(tr("Celsius (°C)"), false);
    units->addItem(tr("Fahrenheit (°F)"), true);
    units->setCurrentIndex(s.fahrenheit() ? 1 : 0);
    units->setAccessibleName(tr("Temperature unit"));
    lf->addRow(tr("Temperature &unit:"), units);
    ll->addLayout(lf);
    m_content->addWidget(look);
    connect(theme, &QComboBox::currentIndexChanged, this, [theme] {
        const QString m = theme->currentData().toString();
        Settings::instance().setThemeMode(m);
        ThemeManager::instance().setMode(ThemeManager::modeFromString(m));
    });
    connect(units, &QComboBox::currentIndexChanged, this,
            [units] { Settings::instance().setFahrenheit(units->currentData().toBool()); });

    auto* safety = new Card;
    auto* sl = static_cast<QVBoxLayout*>(safety->layout());
    sl->addWidget(makeSubtitle(tr("Safety limits")));
    sl->addWidget(makeHint(tr("A run is stopped with a clear message when a limit is exceeded for 2 seconds. Temperatures "
                              "are in °C. These defaults sit below every vendor's own shutdown limits.")));
    auto* sf = new QFormLayout;
    sf->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    auto* enabled = new QCheckBox(tr("Enable safety cutoffs"));
    enabled->setChecked(s.safetyEnabled());
    sf->addRow(enabled);
    auto spin = [](double v, double lo, double hi, const QString& suffix) {
        auto* b = new QDoubleSpinBox;
        b->setRange(lo, hi);
        b->setDecimals(0);
        b->setValue(v);
        b->setSuffix(suffix);
        b->setMinimumWidth(160);
        return b;
    };
    auto* temp = spin(s.maxTemp(), 40, 120, QStringLiteral(" °C"));
    auto* hot = spin(s.maxHotspot(), 40, 125, QStringLiteral(" °C"));
    auto* mem = spin(s.maxMemTemp(), 40, 125, QStringLiteral(" °C"));
    auto* power = spin(s.maxPower(), 0, 2000, QStringLiteral(" W"));
    power->setSpecialValueText(tr("GPU's own limit"));
    auto* stab = new QDoubleSpinBox;
    stab->setRange(50, 100);
    stab->setDecimals(1);
    stab->setSuffix(QStringLiteral(" %"));
    stab->setValue(s.stabilityThreshold() * 100.0);
    sf->addRow(tr("Max GPU &temperature:"), temp);
    sf->addRow(tr("Max &hotspot temperature:"), hot);
    sf->addRow(tr("Max &memory temperature:"), mem);
    sf->addRow(tr("Max board &power:"), power);
    sf->addRow(tr("Loop &stability pass threshold:"), stab);
    auto* lost = new QComboBox;
    lost->addItem(tr("Stop the run"), false);
    lost->addItem(tr("Recover and continue"), true);
    lost->setCurrentIndex(s.recoverDeviceLost() ? 1 : 0);
    lost->setToolTip(tr("What to do when the GPU stops responding and the driver resets it (device lost / TDR). "
                        "Either way the loss is recorded with the driver's fault report and the sensor readings."));
    sf->addRow(tr("If the GPU &resets:"), lost);
    for (QWidget* w : {static_cast<QWidget*>(temp), static_cast<QWidget*>(hot), static_cast<QWidget*>(mem),
                       static_cast<QWidget*>(power), static_cast<QWidget*>(stab), static_cast<QWidget*>(lost)})
        w->setAccessibleName(static_cast<QLabel*>(sf->labelForField(w))->text().remove(QLatin1Char('&')).remove(QLatin1Char(':')));
    sl->addLayout(sf);
    m_content->addWidget(safety);
    connect(enabled, &QCheckBox::toggled, this, [](bool v) { Settings::instance().setSafetyEnabled(v); });
    connect(temp, &QDoubleSpinBox::valueChanged, this, [](double v) { Settings::instance().setMaxTemp(v); });
    connect(hot, &QDoubleSpinBox::valueChanged, this, [](double v) { Settings::instance().setMaxHotspot(v); });
    connect(mem, &QDoubleSpinBox::valueChanged, this, [](double v) { Settings::instance().setMaxMemTemp(v); });
    connect(power, &QDoubleSpinBox::valueChanged, this, [](double v) { Settings::instance().setMaxPower(v); });
    connect(stab, &QDoubleSpinBox::valueChanged, this, [](double v) { Settings::instance().setStabilityThreshold(v / 100.0); });
    connect(lost, &QComboBox::currentIndexChanged, this,
            [lost] { Settings::instance().setRecoverDeviceLost(lost->currentData().toBool()); });

    auto* pathsCard = new Card;
    auto* pl = static_cast<QVBoxLayout*>(pathsCard->layout());
    pl->addWidget(makeSubtitle(tr("Locations")));
    auto* pf = new QFormLayout;
    auto pathRow = [this](const QString& value, const QString& placeholder, bool directory, std::function<void(const QString&)> save) {
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        auto* edit = new QLineEdit(value);
        edit->setPlaceholderText(placeholder);
        auto* browse = new QPushButton(tr("Browse…"));
        h->addWidget(edit, 1);
        h->addWidget(browse);
        connect(edit, &QLineEdit::editingFinished, w, [edit, save] { save(edit->text().trimmed()); });
        connect(browse, &QPushButton::clicked, w, [this, edit, directory, save] {
            const QString p = directory ? QFileDialog::getExistingDirectory(this, tr("Choose folder"), edit->text())
                                        : QFileDialog::getOpenFileName(this, tr("Choose the runner executable"), edit->text());
            if (!p.isEmpty()) {
                edit->setText(p);
                save(p);
            }
        });
        edit->setAccessibleName(placeholder);
        return w;
    };
    pf->addRow(tr("Results folder:"), pathRow(s.resultsDir(), tr("Results folder"), true,
                                              [](const QString& v) { Settings::instance().setResultsDir(v); }));
    pf->addRow(tr("Runner executable:"), pathRow(s.runnerPath(), tr("Automatic (next to this program)"), false,
                                                 [](const QString& v) { Settings::instance().setRunnerPath(v); }));
    pl->addLayout(pf);
    pl->addWidget(makeHint(tr("Settings file: %1").arg(s.filePath())));
    m_content->addWidget(pathsCard);

    auto* about = new Card;
    auto* al = static_cast<QVBoxLayout*>(about->layout());
    al->addWidget(makeSubtitle(tr("About")));
    auto* text = new QLabel(tr("<b>%1</b> %2 (%3)<br>%4<br>Free and open source under the MIT license. "
                               "<a href=\"%5\">%5</a>")
                                .arg(QString::fromUtf8(app::kDisplayName), QString::fromUtf8(app::kVersion),
                                     QString::fromUtf8(app::kGitRevision), QString::fromUtf8(app::kDescription),
                                     QString::fromUtf8(app::kHomepage)));
    text->setOpenExternalLinks(true);
    text->setWordWrap(true);
    text->setTextInteractionFlags(Qt::TextBrowserInteraction);
    al->addWidget(text);
    al->addWidget(makeHint(tr("No telemetry, no accounts, no network access: results stay on this computer.")));
    m_content->addWidget(about);
    m_content->addStretch();
}

} // namespace gt::gui
