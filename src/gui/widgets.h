#pragma once
// Small themed building blocks. They paint with ThemeManager colours so they
// follow light/dark/accent changes live, and they keep native widgets intact
// (no global style sheet, so the Windows 11 style stays native).

#include "theme.h"

#include <QFrame>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QStyledItemDelegate>

namespace gt::gui {

/// Monochrome Lucide icon tinted with `color` (default: current text colour).
QIcon themedIcon(const QString& name, const QColor& color = {});
QPixmap themedPixmap(const QString& name, int logicalSize, const QColor& color, qreal dpr);

/// Rounded surface panel.
class Card : public QFrame {
    Q_OBJECT
public:
    explicit Card(QWidget* parent = nullptr);
    void setSelected(bool s);
    bool isSelected() const { return m_selected; }
    void setClickable(bool c);
Q_SIGNALS:
    void clicked();

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void enterEvent(QEnterEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    bool m_selected = false;
    bool m_clickable = false;
    bool m_hover = false;
};

/// Pill-shaped label for feature support / status.
class Badge : public QLabel {
    Q_OBJECT
public:
    enum class Kind { Neutral, Accent, Good, Warning, Critical };
    explicit Badge(const QString& text, Kind kind = Kind::Neutral, QWidget* parent = nullptr);
    void setKind(Kind k);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    Kind m_kind;
};

/// Headline number with a caption: "GPU clock / 2,520 MHz".
class StatTile : public QFrame {
    Q_OBJECT
public:
    explicit StatTile(const QString& caption, QWidget* parent = nullptr);
    void setValue(const QString& value, const QString& sub = {});
    void setStatus(int status); ///< -1 none, else ThemeManager::Status
    QString value() const;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QLabel* m_caption;
    QLabel* m_value;
    QLabel* m_sub;
    int m_status = -1;
};

/// Sidebar navigation list (keyboard navigable, screen-reader friendly).
class NavList : public QListWidget {
    Q_OBJECT
public:
    explicit NavList(QWidget* parent = nullptr);
    void addPage(const QString& icon, const QString& text, const QString& shortcutHint);
    void refreshIcons();

private:
    QStringList m_icons;
};

class NavDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
};

/// Bold section title used at the top of pages and cards.
QLabel* makeTitle(const QString& text, int pointDelta = 6, QWidget* parent = nullptr);
QLabel* makeSubtitle(const QString& text, QWidget* parent = nullptr);
/// A muted, word-wrapped explanatory label.
QLabel* makeHint(const QString& text, QWidget* parent = nullptr);

QString formatBytes(double bytes);

/// Removes and (deferred-)deletes every item of a layout. Deferred so a widget can
/// trigger a rebuild from its own signal handler.
void clearLayout(QLayout* layout);

} // namespace gt::gui
