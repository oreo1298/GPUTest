#include "theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QProxyStyle>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleOption>
#include <QStyleHints>
#include <QTextStream>
#include <QTimer>

#ifdef GT_HAVE_QTDBUS
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVariant>
#endif

namespace gt::gui {
namespace {

double luminance(const QColor& c) {
    auto lin = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
}

QColor mix(const QColor& a, const QColor& b, double t) {
    return QColor::fromRgbF(static_cast<float>(a.redF() * (1 - t) + b.redF() * t),
                            static_cast<float>(a.greenF() * (1 - t) + b.greenF() * t),
                            static_cast<float>(a.blueF() * (1 - t) + b.blueF() * t));
}

QColor readableOn(const QColor& bg) { return luminance(bg) > 0.4 ? QColor(0x0b, 0x0b, 0x0b) : QColor(Qt::white); }

/// Fusion outlines check boxes and radio buttons with a darker shade of the window colour: invisible on dark
/// backgrounds and too faint on white list views. This passes the indicator a window colour chosen so that
/// Fusion's outline comes out as a mid grey with at least 3:1 contrast against the background in both modes.
class ContrastStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    void drawPrimitive(PrimitiveElement pe, const QStyleOption* opt, QPainter* p, const QWidget* w) const override {
        const auto* button = qstyleoption_cast<const QStyleOptionButton*>(opt);
        if (button && (pe == PE_IndicatorCheckBox || pe == PE_IndicatorRadioButton)) {
            QStyleOptionButton o(*button);
            const QColor outline = mix(o.palette.color(QPalette::Text), o.palette.color(QPalette::Base), 0.55);
            o.palette.setColor(QPalette::Window, outline.lighter(140)); // Fusion: outline = window.darker(140)
            QProxyStyle::drawPrimitive(pe, &o, p, w);
            return;
        }
        QProxyStyle::drawPrimitive(pe, opt, p, w);
    }
};

// Reference categorical palette (light / dark steps), validated for CVD separation.
const char* kSeriesLight[] = {"#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"};
const char* kSeriesDark[] = {"#3987e5", "#d95926", "#199e70", "#c98500", "#d55181", "#008300", "#9085e9", "#e66767"};

} // namespace

ThemeManager& ThemeManager::instance() {
    static ThemeManager* tm = new ThemeManager();
    return *tm;
}

ThemeManager::Mode ThemeManager::modeFromString(const QString& s) {
    if (s.compare(QLatin1String("light"), Qt::CaseInsensitive) == 0) return Mode::Light;
    if (s.compare(QLatin1String("dark"), Qt::CaseInsensitive) == 0) return Mode::Dark;
    return Mode::System;
}

QString ThemeManager::modeToString(Mode m) {
    switch (m) {
    case Mode::Light: return QStringLiteral("light");
    case Mode::Dark: return QStringLiteral("dark");
    case Mode::System: break;
    }
    return QStringLiteral("system");
}

void ThemeManager::init(QApplication& app) {
    m_app = &app;
#ifdef Q_OS_WIN
    // Native Windows 11 look (also used on Windows 10); falls back to Fusion if unavailable.
    if (QStyle* s = QStyleFactory::create(QStringLiteral("windows11"))) {
        app.setStyle(s);
        m_nativeStyle = true;
    } else {
        app.setStyle(new ContrastStyle(QStyleFactory::create(QStringLiteral("Fusion"))));
    }
#else
    app.setStyle(new ContrastStyle(QStyleFactory::create(QStringLiteral("Fusion"))));
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &ThemeManager::refresh);
#endif

#ifdef GT_HAVE_QTDBUS
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        bus.connect(QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
                    QStringLiteral("org.freedesktop.portal.Settings"), QStringLiteral("SettingChanged"), this,
                    SLOT(onPortalSettingChanged(QString, QString, QDBusVariant)));
        bus.connect(QString(), QStringLiteral("/KGlobalSettings"), QStringLiteral("org.kde.KGlobalSettings"),
                    QStringLiteral("notifyChange"), this, SLOT(onKdeSettingsChanged()));
    }
#endif
#ifndef Q_OS_WIN
    const QString kdeglobals =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/kdeglobals");
    m_watcher = new QFileSystemWatcher(this);
    if (QFileInfo::exists(kdeglobals)) m_watcher->addPath(kdeglobals);
    m_watcher->addPath(QFileInfo(kdeglobals).absolutePath());
    // KDE replaces the file atomically, which drops the file watch: re-arm on directory changes too.
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, &ThemeManager::onKdeSettingsChanged);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this, kdeglobals](const QString&) {
        if (QFileInfo::exists(kdeglobals) && !m_watcher->files().contains(kdeglobals)) m_watcher->addPath(kdeglobals);
        onKdeSettingsChanged();
    });
#endif
    detectSystem();
    apply();
}

void ThemeManager::setMode(Mode m) {
    if (m == m_mode) return;
    m_mode = m;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    if (m_nativeStyle) {
        QGuiApplication::styleHints()->setColorScheme(m == Mode::Dark    ? Qt::ColorScheme::Dark
                                                      : m == Mode::Light ? Qt::ColorScheme::Light
                                                                         : Qt::ColorScheme::Unknown);
    }
#endif
    apply();
}

void ThemeManager::refresh() {
    detectSystem();
    apply();
}

void ThemeManager::onKdeSettingsChanged() {
    // Coalesce bursts of change notifications.
    QTimer::singleShot(150, this, &ThemeManager::refresh);
}

void ThemeManager::onPortalSettingChanged(const QString& ns, const QString& key, const QDBusVariant& value) {
#ifdef GT_HAVE_QTDBUS
    if (ns != QLatin1String("org.freedesktop.appearance")) return;
    if (key == QLatin1String("color-scheme")) {
        m_portalScheme = static_cast<int>(value.variant().toUInt());
    } else if (key == QLatin1String("accent-color")) {
        const QDBusArgument arg = value.variant().value<QDBusArgument>();
        double r = -1, g = -1, b = -1;
        arg.beginStructure();
        arg >> r >> g >> b;
        arg.endStructure();
        if (r >= 0 && r <= 1 && g >= 0 && g <= 1 && b >= 0 && b <= 1)
            m_systemAccent = QColor::fromRgbF(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b));
    }
    QTimer::singleShot(50, this, &ThemeManager::refresh);
#else
    (void)ns;
    (void)key;
    (void)value;
#endif
}

void ThemeManager::readPortal() {
#ifdef GT_HAVE_QTDBUS
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return;
    auto readOne = [&](const QString& key) -> QVariant {
        QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.portal.Settings"), QStringLiteral("ReadOne"));
        msg << QStringLiteral("org.freedesktop.appearance") << key;
        QDBusMessage reply = bus.call(msg, QDBus::Block, 400);
        if (reply.type() != QDBusMessage::ReplyMessage) {
            // Old portals only have Read, which wraps the value in one more variant.
            msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.portal.Desktop"),
                                                 QStringLiteral("/org/freedesktop/portal/desktop"),
                                                 QStringLiteral("org.freedesktop.portal.Settings"), QStringLiteral("Read"));
            msg << QStringLiteral("org.freedesktop.appearance") << key;
            reply = bus.call(msg, QDBus::Block, 400);
            if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) return {};
            QVariant v = reply.arguments().first().value<QDBusVariant>().variant();
            if (v.canConvert<QDBusVariant>()) v = v.value<QDBusVariant>().variant();
            return v;
        }
        if (reply.arguments().isEmpty()) return {};
        return reply.arguments().first().value<QDBusVariant>().variant();
    };
    const QVariant scheme = readOne(QStringLiteral("color-scheme"));
    if (scheme.isValid()) m_portalScheme = static_cast<int>(scheme.toUInt());
    const QVariant accent = readOne(QStringLiteral("accent-color"));
    if (accent.isValid() && accent.canConvert<QDBusArgument>()) {
        const QDBusArgument arg = accent.value<QDBusArgument>();
        double r = -1, g = -1, b = -1;
        arg.beginStructure();
        arg >> r >> g >> b;
        arg.endStructure();
        if (r >= 0 && r <= 1 && g >= 0 && g <= 1 && b >= 0 && b <= 1)
            m_systemAccent = QColor::fromRgbF(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b));
    }
#endif
}

bool ThemeManager::readKdeGlobals() {
    const QString path =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/kdeglobals");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    QString group;
    QHash<QString, QString> values;
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            group = line.mid(1, line.size() - 2);
            continue;
        }
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq > 0) values.insert(group + QLatin1Char('/') + line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
    }
    auto color = [&](const char* key) -> QColor {
        const QString v = values.value(QString::fromLatin1(key));
        const QStringList parts = v.split(QLatin1Char(','));
        if (parts.size() < 3) return {};
        return QColor(parts[0].toInt(), parts[1].toInt(), parts[2].toInt());
    };
    m_kdeWindow = color("Colors:Window/BackgroundNormal");
    m_kdeWindowText = color("Colors:Window/ForegroundNormal");
    m_kdeView = color("Colors:View/BackgroundNormal");
    m_kdeViewText = color("Colors:View/ForegroundNormal");
    m_kdeViewAlt = color("Colors:View/BackgroundAlternate");
    m_kdeButton = color("Colors:Button/BackgroundNormal");
    m_kdeButtonText = color("Colors:Button/ForegroundNormal");
    m_kdeSelection = color("Colors:Selection/BackgroundNormal");
    m_kdeSelectionText = color("Colors:Selection/ForegroundNormal");
    m_kdeTooltip = color("Colors:Tooltip/BackgroundNormal");
    m_kdeTooltipText = color("Colors:Tooltip/ForegroundNormal");
    m_kdeInactiveText = color("Colors:View/ForegroundInactive");
    m_kdeLink = color("Colors:View/ForegroundLink");
    const QColor accent = color("General/AccentColor");
    if (!m_kdeWindow.isValid() || !m_kdeWindowText.isValid()) return false;
    if (accent.isValid()) m_systemAccent = accent;
    else if (m_kdeSelection.isValid()) m_systemAccent = m_kdeSelection;
    return true;
}

void ThemeManager::detectSystem() {
    m_systemAccent = QColor();
    m_haveKde = false;
    m_source = QStringLiteral("default");
#ifdef Q_OS_WIN
    m_source = QStringLiteral("Windows");
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    m_systemDark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
    QSettings personalize(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
                          QSettings::NativeFormat);
    m_systemDark = personalize.value(QStringLiteral("AppsUseLightTheme"), 1).toInt() == 0;
#endif
    QSettings dwm(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\DWM"), QSettings::NativeFormat);
    bool ok = false;
    const quint32 abgr = dwm.value(QStringLiteral("AccentColor")).toUInt(&ok);
    if (ok) m_systemAccent = QColor(abgr & 0xff, (abgr >> 8) & 0xff, (abgr >> 16) & 0xff);
#else
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    readPortal();
    if (desktop.contains(QLatin1String("KDE"), Qt::CaseInsensitive) && readKdeGlobals()) {
        m_haveKde = true;
        m_systemDark = luminance(m_kdeWindow) < 0.25;
        m_source = QStringLiteral("KDE Plasma colour scheme");
    } else if (m_portalScheme >= 0) {
        m_systemDark = m_portalScheme == 1;
        m_source = QStringLiteral("xdg-desktop-portal");
    } else {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        m_systemDark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
        m_source = QStringLiteral("Qt platform theme");
#else
        m_systemDark = false;
#endif
    }
#endif
}

void ThemeManager::apply() {
    m_dark = m_mode == Mode::System ? m_systemDark : m_mode == Mode::Dark;
    m_accent = m_systemAccent.isValid() ? m_systemAccent : QColor(m_dark ? kSeriesDark[0] : kSeriesLight[0]);
    if (m_app && !m_nativeStyle) m_app->setPalette(buildPalette());
    Q_EMIT themeChanged();
}

QPalette ThemeManager::buildPalette() const {
    // Use the KDE scheme verbatim when it matches the requested mode; otherwise our own palette.
    const bool useKde = m_haveKde && m_mode == Mode::System;
    QColor window, windowText, base, altBase, text, button, buttonText, highlight, highlightText, tooltip,
        tooltipText, placeholder, link;
    if (useKde) {
        window = m_kdeWindow;
        windowText = m_kdeWindowText;
        base = m_kdeView.isValid() ? m_kdeView : window;
        altBase = m_kdeViewAlt.isValid() ? m_kdeViewAlt : mix(base, windowText, 0.03);
        text = m_kdeViewText.isValid() ? m_kdeViewText : windowText;
        button = m_kdeButton.isValid() ? m_kdeButton : window;
        buttonText = m_kdeButtonText.isValid() ? m_kdeButtonText : windowText;
        highlight = m_accent;
        highlightText = m_kdeSelectionText.isValid() ? m_kdeSelectionText : readableOn(highlight);
        tooltip = m_kdeTooltip.isValid() ? m_kdeTooltip : base;
        tooltipText = m_kdeTooltipText.isValid() ? m_kdeTooltipText : text;
        placeholder = m_kdeInactiveText.isValid() ? m_kdeInactiveText : mix(text, base, 0.45);
        link = m_kdeLink.isValid() ? m_kdeLink : m_accent;
    } else if (m_dark) {
        window = QColor(0x1d, 0x1d, 0x1c);
        windowText = QColor(0xf0, 0xef, 0xec);
        base = QColor(0x16, 0x16, 0x15);
        altBase = QColor(0x22, 0x22, 0x21);
        text = windowText;
        button = QColor(0x2b, 0x2b, 0x29);
        buttonText = windowText;
        highlight = m_accent;
        highlightText = readableOn(highlight);
        tooltip = QColor(0x2b, 0x2b, 0x29);
        tooltipText = windowText;
        placeholder = QColor(0x89, 0x87, 0x81);
        link = m_accent.lighter(125);
    } else {
        window = QColor(0xf4, 0xf4, 0xf2);
        windowText = QColor(0x0b, 0x0b, 0x0b);
        base = QColor(0xff, 0xff, 0xff);
        altBase = QColor(0xf7, 0xf7, 0xf5);
        text = windowText;
        button = QColor(0xfb, 0xfb, 0xfa);
        buttonText = windowText;
        highlight = m_accent;
        highlightText = readableOn(highlight);
        tooltip = QColor(0xff, 0xff, 0xff);
        tooltipText = windowText;
        placeholder = QColor(0x89, 0x87, 0x81);
        link = m_accent.darker(110);
    }
    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, windowText);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, altBase);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, buttonText);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, highlightText);
    p.setColor(QPalette::ToolTipBase, tooltip);
    p.setColor(QPalette::ToolTipText, tooltipText);
    p.setColor(QPalette::PlaceholderText, placeholder);
    p.setColor(QPalette::Link, link);
    p.setColor(QPalette::LinkVisited, link);
    const QColor light = mix(button, Qt::white, m_dark ? 0.08 : 0.6);
    const QColor dark = mix(button, Qt::black, m_dark ? 0.45 : 0.25);
    p.setColor(QPalette::Light, light);
    p.setColor(QPalette::Midlight, mix(button, light, 0.5));
    p.setColor(QPalette::Mid, mix(button, dark, 0.5));
    p.setColor(QPalette::Dark, dark);
    p.setColor(QPalette::Shadow, QColor(0, 0, 0, m_dark ? 160 : 80));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    p.setColor(QPalette::Accent, m_accent);
#endif
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        p.setColor(QPalette::Disabled, role, mix(p.color(role), window, 0.55));
    }
    p.setColor(QPalette::Disabled, QPalette::Highlight, mix(highlight, window, 0.5));
    return p;
}

QColor ThemeManager::series(int index) const {
    const int i = ((index % 8) + 8) % 8;
    return QColor(QString::fromLatin1(m_dark ? kSeriesDark[i] : kSeriesLight[i]));
}

QColor ThemeManager::status(Status s) const {
    switch (s) {
    case Status::Good: return QColor(0x0c, 0xa3, 0x0c);
    case Status::Warning: return QColor(0xfa, 0xb2, 0x19);
    case Status::Serious: return QColor(0xec, 0x83, 0x5a);
    case Status::Critical: return QColor(0xd0, 0x3b, 0x3b);
    }
    return {};
}

QColor ThemeManager::surface() const {
    const QPalette p = m_app ? m_app->palette() : QPalette();
    return m_dark ? mix(p.color(QPalette::Window), Qt::white, 0.035) : p.color(QPalette::Base);
}
QColor ThemeManager::surfaceAlt() const {
    const QPalette p = m_app ? m_app->palette() : QPalette();
    return m_dark ? mix(p.color(QPalette::Window), Qt::black, 0.18) : mix(p.color(QPalette::Window), Qt::black, 0.025);
}
QColor ThemeManager::border() const { return m_dark ? QColor(255, 255, 255, 26) : QColor(11, 11, 11, 26); }
QColor ThemeManager::gridLine() const { return m_dark ? QColor(0x2c, 0x2c, 0x2a) : QColor(0xe1, 0xe0, 0xd9); }
QColor ThemeManager::axisLine() const { return m_dark ? QColor(0x38, 0x38, 0x35) : QColor(0xc3, 0xc2, 0xb7); }
QColor ThemeManager::textPrimary() const {
    return m_app ? m_app->palette().color(QPalette::WindowText) : QColor(m_dark ? Qt::white : Qt::black);
}
QColor ThemeManager::textSecondary() const { return m_dark ? QColor(0xc3, 0xc2, 0xb7) : QColor(0x52, 0x51, 0x4e); }
QColor ThemeManager::textMuted() const { return QColor(0x89, 0x87, 0x81); }

} // namespace gt::gui
