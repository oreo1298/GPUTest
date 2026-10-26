#pragma once
// Persistent user settings (INI file in the per-user config directory).

#include <QSettings>
#include <QString>
#include <QVariant>

namespace gt::gui {

class Settings {
public:
    static Settings& instance();

    QString themeMode() const { return value("ui/theme", QStringLiteral("system")).toString(); }
    void setThemeMode(const QString& v) { set("ui/theme", v); }
    bool fahrenheit() const { return value("ui/fahrenheit", false).toBool(); }
    void setFahrenheit(bool v) { set("ui/fahrenheit", v); }

    QString runnerPath() const { return value("paths/runner").toString(); }
    void setRunnerPath(const QString& v) { set("paths/runner", v); }
    QString resultsDir() const;
    void setResultsDir(const QString& v) { set("paths/results", v); }

    QString api() const { return value("run/api").toString(); }
    void setApi(const QString& v) { set("run/api", v); }
    QString gpuKey() const { return value("run/gpu").toString(); }
    void setGpuKey(const QString& v) { set("run/gpu", v); }
    QString resolution() const { return value("run/resolution", QStringLiteral("1080p")).toString(); }
    void setResolution(const QString& v) { set("run/resolution", v); }
    bool offscreen() const { return value("run/offscreen", false).toBool(); }
    void setOffscreen(bool v) { set("run/offscreen", v); }
    bool fullscreen() const { return value("run/fullscreen", false).toBool(); }
    void setFullscreen(bool v) { set("run/fullscreen", v); }
    bool hud() const { return value("run/hud", true).toBool(); }
    void setHud(bool v) { set("run/hud", v); }
    bool validation() const { return value("run/validation", false).toBool(); }
    void setValidation(bool v) { set("run/validation", v); }

    bool safetyEnabled() const { return value("safety/enabled", true).toBool(); }
    void setSafetyEnabled(bool v) { set("safety/enabled", v); }
    double maxTemp() const { return value("safety/max_temp", 90.0).toDouble(); }
    void setMaxTemp(double v) { set("safety/max_temp", v); }
    double maxHotspot() const { return value("safety/max_hotspot", 105.0).toDouble(); }
    void setMaxHotspot(double v) { set("safety/max_hotspot", v); }
    double maxMemTemp() const { return value("safety/max_mem_temp", 100.0).toDouble(); }
    void setMaxMemTemp(double v) { set("safety/max_mem_temp", v); }
    double maxPower() const { return value("safety/max_power", 0.0).toDouble(); }
    void setMaxPower(double v) { set("safety/max_power", v); }
    double stabilityThreshold() const { return value("safety/stability_threshold", 0.97).toDouble(); }
    bool recoverDeviceLost() const { return value("safety/recover_device_lost", false).toBool(); }
    void setRecoverDeviceLost(bool v) { set("safety/recover_device_lost", v); }
    void setStabilityThreshold(double v) { set("safety/stability_threshold", v); }

    QVariant value(const char* key, const QVariant& def = {}) const;
    void set(const char* key, const QVariant& v);
    QString filePath() const;

private:
    Settings();
    QSettings m_s;
};

/// Formats a temperature in the user's unit.
QString formatTemp(double celsius);

} // namespace gt::gui
