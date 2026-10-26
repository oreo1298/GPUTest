#include "settings.h"

#include "gt/paths.h"

#include <QDir>
#include <cmath>

namespace gt::gui {

Settings::Settings()
    : m_s(QString::fromStdString((paths::configDir() / "settings.ini").string()), QSettings::IniFormat) {}

Settings& Settings::instance() {
    static Settings s;
    return s;
}

QVariant Settings::value(const char* key, const QVariant& def) const { return m_s.value(QLatin1String(key), def); }

void Settings::set(const char* key, const QVariant& v) {
    m_s.setValue(QLatin1String(key), v);
    m_s.sync();
}

QString Settings::filePath() const { return m_s.fileName(); }

QString Settings::resultsDir() const {
    const QString v = value("paths/results").toString();
    if (!v.isEmpty()) return v;
    return QString::fromStdString(paths::resultsDir().string());
}

QString formatTemp(double c) {
    if (!std::isfinite(c)) return QStringLiteral("—");
    if (Settings::instance().fahrenheit()) return QStringLiteral("%1 °F").arg(c * 9.0 / 5.0 + 32.0, 0, 'f', 0);
    return QStringLiteral("%1 °C").arg(c, 0, 'f', 0);
}

} // namespace gt::gui
