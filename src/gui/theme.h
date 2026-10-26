#pragma once
// Follows the OS light/dark mode and accent colour and updates live:
//  - Windows: Qt's windows11 style + QStyleHints colour scheme / accent (Qt >= 6.5/6.6).
//  - KDE Plasma: the full colour scheme from kdeglobals (watched for changes).
//  - GNOME and others: xdg-desktop-portal org.freedesktop.appearance color-scheme / accent-color.
// A user override (Light / Dark) wins over the system.

#include <QColor>
#include <QList>
#include <QObject>
#include <QPalette>
#include <QString>

class QApplication;
class QFileSystemWatcher;
class QDBusVariant;

namespace gt::gui {

class ThemeManager : public QObject {
    Q_OBJECT
public:
    enum class Mode { System, Light, Dark };
    enum class Status { Good, Warning, Serious, Critical };

    static ThemeManager& instance();
    void init(QApplication& app);
    void setMode(Mode m);
    Mode mode() const { return m_mode; }
    bool isDark() const { return m_dark; }
    QColor accent() const { return m_accent; }
    /// Where the current colours came from (shown in Settings).
    QString source() const { return m_source; }

    // Chart / semantic colours (validated reference palette, see docs/DECISIONS.md).
    QColor series(int index) const;
    QColor status(Status s) const;
    QColor surface() const;      ///< card / chart surface
    QColor surfaceAlt() const;   ///< subtle alternate surface (sidebar, headers)
    QColor border() const;       ///< hairline border
    QColor gridLine() const;
    QColor axisLine() const;
    QColor textPrimary() const;
    QColor textSecondary() const;
    QColor textMuted() const;

    static Mode modeFromString(const QString& s);
    static QString modeToString(Mode m);

Q_SIGNALS:
    void themeChanged();

private Q_SLOTS:
    void onPortalSettingChanged(const QString& ns, const QString& key, const QDBusVariant& value);
    void onKdeSettingsChanged();
    void refresh();

private:
    ThemeManager() = default;
    void detectSystem();
    void apply();
    QPalette buildPalette() const;
    bool readKdeGlobals();
    void readPortal();

    QApplication* m_app = nullptr;
    Mode m_mode = Mode::System;
    bool m_systemDark = false;
    bool m_dark = false;
    QColor m_accent{0x2a, 0x78, 0xd6};
    QColor m_systemAccent;
    QString m_source = QStringLiteral("default");
    bool m_nativeStyle = false; // Windows: let the windows11 style own the palette
    // KDE colour scheme (when available)
    bool m_haveKde = false;
    QColor m_kdeWindow, m_kdeWindowText, m_kdeView, m_kdeViewText, m_kdeViewAlt, m_kdeButton, m_kdeButtonText,
        m_kdeSelection, m_kdeSelectionText, m_kdeTooltip, m_kdeTooltipText, m_kdeInactiveText, m_kdeLink;
    QFileSystemWatcher* m_watcher = nullptr;
    int m_portalScheme = -1; ///< 0 no preference, 1 dark, 2 light, -1 unknown
};

} // namespace gt::gui
