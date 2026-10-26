#include "widgets.h"

#include <QApplication>
#include <QFile>
#include <QHash>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>
#include <QLayout>
#include <QVBoxLayout>

namespace gt::gui {

QPixmap themedPixmap(const QString& name, int size, const QColor& color, qreal dpr) {
    static QHash<QString, QByteArray> sources;
    if (!sources.contains(name)) {
        QFile f(QStringLiteral(":/icons/%1.svg").arg(name));
        sources.insert(name, f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray());
    }
    QByteArray svg = sources.value(name);
    svg.replace("currentColor", color.name(QColor::HexRgb).toLatin1());
    QSvgRenderer r(svg);
    QPixmap pm(QSize(size, size) * dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    r.render(&p, QRectF(0, 0, size * dpr, size * dpr));
    p.end();
    pm.setDevicePixelRatio(dpr);
    return pm;
}

QIcon themedIcon(const QString& name, const QColor& color) {
    const QColor c = color.isValid() ? color : qApp->palette().color(QPalette::WindowText);
    QIcon icon;
    for (int s : {16, 20, 24, 32})
        for (qreal dpr : {1.0, 2.0}) icon.addPixmap(themedPixmap(name, s, c, dpr));
    const QColor disabled = qApp->palette().color(QPalette::Disabled, QPalette::WindowText);
    for (int s : {16, 24}) icon.addPixmap(themedPixmap(name, s, disabled, 1.0), QIcon::Disabled);
    return icon;
}

// --- Card -----------------------------------------------------------------------------------
Card::Card(QWidget* parent) : QFrame(parent) {
    setAttribute(Qt::WA_Hover);
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(16, 14, 16, 14);
    l->setSpacing(8);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
}

void Card::setSelected(bool s) {
    m_selected = s;
    update();
}

void Card::setClickable(bool c) {
    m_clickable = c;
    setFocusPolicy(c ? Qt::StrongFocus : Qt::NoFocus);
    setCursor(c ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void Card::paintEvent(QPaintEvent*) {
    auto& tm = ThemeManager::instance();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    QColor bg = tm.surface();
    if (m_clickable && m_hover && !m_selected) bg = tm.isDark() ? bg.lighter(112) : bg.darker(103);
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(r, 10, 10);
    QPen pen(m_selected ? tm.accent() : tm.border(), m_selected ? 2.0 : 1.0);
    if (hasFocus() && m_clickable) pen = QPen(tm.accent(), 2.0, Qt::DashLine);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, 10, 10);
}

void Card::mouseReleaseEvent(QMouseEvent* e) {
    if (m_clickable && rect().contains(e->position().toPoint())) Q_EMIT clicked();
    QFrame::mouseReleaseEvent(e);
}

void Card::keyPressEvent(QKeyEvent* e) {
    if (m_clickable && (e->key() == Qt::Key_Space || e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)) {
        Q_EMIT clicked();
        return;
    }
    QFrame::keyPressEvent(e);
}

void Card::enterEvent(QEnterEvent* e) {
    m_hover = true;
    update();
    QFrame::enterEvent(e);
}

void Card::leaveEvent(QEvent* e) {
    m_hover = false;
    update();
    QFrame::leaveEvent(e);
}

// --- Badge ----------------------------------------------------------------------------------
Badge::Badge(const QString& text, Kind kind, QWidget* parent) : QLabel(text, parent), m_kind(kind) {
    setAlignment(Qt::AlignCenter);
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    f.setWeight(QFont::DemiBold);
    setFont(f);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, qOverload<>(&QWidget::update));
}

void Badge::setKind(Kind k) {
    m_kind = k;
    update();
}

QSize Badge::sizeHint() const {
    const QFontMetrics fm(font());
    return {fm.horizontalAdvance(text()) + 18, fm.height() + 6};
}

void Badge::paintEvent(QPaintEvent*) {
    auto& tm = ThemeManager::instance();
    QColor base;
    switch (m_kind) {
    case Kind::Neutral: base = tm.textMuted(); break;
    case Kind::Accent: base = tm.accent(); break;
    case Kind::Good: base = tm.status(ThemeManager::Status::Good); break;
    case Kind::Warning: base = tm.status(ThemeManager::Status::Warning); break;
    case Kind::Critical: base = tm.status(ThemeManager::Status::Critical); break;
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor fill = base;
    fill.setAlphaF(tm.isDark() ? 0.22f : 0.14f);
    QColor outline = base;
    outline.setAlphaF(0.55f);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(outline, 1));
    p.setBrush(fill);
    p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
    // Text always uses text tokens (never the status colour) for contrast.
    p.setPen(tm.textPrimary());
    p.setFont(font());
    p.drawText(rect(), Qt::AlignCenter, text());
}

// --- StatTile -------------------------------------------------------------------------------
StatTile::StatTile(const QString& caption, QWidget* parent) : QFrame(parent) {
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(14, 10, 14, 10);
    l->setSpacing(2);
    m_caption = new QLabel(caption, this);
    m_value = new QLabel(QStringLiteral("—"), this);
    m_sub = new QLabel(this);
    QFont vf = m_value->font();
    vf.setPointSizeF(vf.pointSizeF() * 1.55);
    vf.setWeight(QFont::DemiBold);
    m_value->setFont(vf);
    QFont cf = m_caption->font();
    cf.setPointSizeF(cf.pointSizeF() * 0.9);
    m_caption->setFont(cf);
    m_sub->setFont(cf);
    m_sub->hide();
    l->addWidget(m_caption);
    l->addWidget(m_value);
    l->addWidget(m_sub);
    setAccessibleName(caption);
    auto recolor = [this] {
        QPalette pal = palette();
        pal.setColor(QPalette::WindowText, ThemeManager::instance().textSecondary());
        m_caption->setPalette(pal);
        m_sub->setPalette(pal);
        update();
    };
    recolor();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, recolor);
}

QString StatTile::value() const { return m_value->text(); }

void StatTile::setValue(const QString& value, const QString& sub) {
    m_value->setText(value);
    m_sub->setText(sub);
    m_sub->setVisible(!sub.isEmpty());
    setAccessibleDescription(sub.isEmpty() ? value : value + QStringLiteral(", ") + sub);
}

void StatTile::setStatus(int status) {
    m_status = status;
    update();
}

void StatTile::paintEvent(QPaintEvent*) {
    auto& tm = ThemeManager::instance();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(tm.border(), 1));
    p.setBrush(tm.surface());
    p.drawRoundedRect(r, 8, 8);
    if (m_status >= 0) {
        // Status stripe on the left edge; the value text itself stays in text colours.
        p.setPen(Qt::NoPen);
        p.setBrush(tm.status(static_cast<ThemeManager::Status>(m_status)));
        p.drawRoundedRect(QRectF(r.left() + 3, r.top() + 8, 3, r.height() - 16), 1.5, 1.5);
    }
}

// --- NavList --------------------------------------------------------------------------------
NavList::NavList(QWidget* parent) : QListWidget(parent) {
    setItemDelegate(new NavDelegate(this));
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setIconSize(QSize(20, 20));
    setUniformItemSizes(true);
    setAccessibleName(tr("Navigation"));
    setSpacing(2);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, &NavList::refreshIcons);
}

void NavList::addPage(const QString& icon, const QString& text, const QString& shortcutHint) {
    auto* item = new QListWidgetItem(themedIcon(icon), text, this);
    item->setToolTip(QStringLiteral("%1 (%2)").arg(text, shortcutHint));
    item->setData(Qt::AccessibleTextRole, text);
    m_icons << icon;
}

void NavList::refreshIcons() {
    for (int i = 0; i < count() && i < m_icons.size(); ++i) item(i)->setIcon(themedIcon(m_icons[i]));
    viewport()->update();
}

void NavDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const {
    auto& tm = ThemeManager::instance();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(opt.rect).adjusted(6, 1, -6, -1);
    const bool selected = opt.state & QStyle::State_Selected;
    const bool hover = opt.state & QStyle::State_MouseOver;
    if (selected || hover) {
        QColor bg = tm.accent();
        bg.setAlphaF(selected ? (tm.isDark() ? 0.28f : 0.16f) : 0.08f);
        p->setPen(Qt::NoPen);
        p->setBrush(bg);
        p->drawRoundedRect(r, 7, 7);
    }
    if (selected) {
        p->setBrush(tm.accent());
        p->drawRoundedRect(QRectF(r.left() + 2, r.center().y() - 8, 3, 16), 1.5, 1.5);
    }
    if (opt.state & QStyle::State_HasFocus) {
        p->setPen(QPen(tm.accent(), 1.5, Qt::DotLine));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(r.adjusted(1, 1, -1, -1), 6, 6);
    }
    const QIcon icon = idx.data(Qt::DecorationRole).value<QIcon>();
    const int iconSize = 20;
    const QRect iconRect(static_cast<int>(r.left()) + 14, static_cast<int>(r.center().y()) - iconSize / 2, iconSize, iconSize);
    icon.paint(p, iconRect);
    QFont f = opt.font;
    if (selected) f.setWeight(QFont::DemiBold);
    p->setFont(f);
    p->setPen(tm.textPrimary());
    const QRect textRect = QRect(iconRect.right() + 12, static_cast<int>(r.top()), static_cast<int>(r.right()) - iconRect.right() - 16,
                                 static_cast<int>(r.height()));
    p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, idx.data(Qt::DisplayRole).toString());
    p->restore();
}

QSize NavDelegate::sizeHint(const QStyleOptionViewItem& opt, const QModelIndex&) const {
    return {200, std::max(38, QFontMetrics(opt.font).height() + 18)};
}

QLabel* makeTitle(const QString& text, int pointDelta, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() + pointDelta);
    f.setWeight(QFont::DemiBold);
    l->setFont(f);
    l->setAccessibleName(text);
    return l;
}

QLabel* makeSubtitle(const QString& text, QWidget* parent) { return makeTitle(text, 2, parent); }

QLabel* makeHint(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setTextFormat(Qt::PlainText);
    auto recolor = [l] {
        QPalette pal = l->palette();
        pal.setColor(QPalette::WindowText, ThemeManager::instance().textSecondary());
        l->setPalette(pal);
    };
    recolor();
    QObject::connect(&ThemeManager::instance(), &ThemeManager::themeChanged, l, recolor);
    return l;
}

void clearLayout(QLayout* layout) {
    while (QLayoutItem* it = layout->takeAt(0)) {
        if (QWidget* w = it->widget()) {
            w->hide();
            w->deleteLater();
        } else if (QLayout* l = it->layout()) {
            clearLayout(l);
        }
        delete it;
    }
}

QString formatBytes(double bytes) {
    if (bytes >= 1024.0 * 1024 * 1024) return QStringLiteral("%1 GiB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 1);
    if (bytes >= 1024.0 * 1024) return QStringLiteral("%1 MiB").arg(bytes / (1024.0 * 1024), 0, 'f', 0);
    return QStringLiteral("%1 KiB").arg(bytes / 1024.0, 0, 'f', 0);
}

} // namespace gt::gui
