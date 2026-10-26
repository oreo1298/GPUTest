#include "pages.h"

#include <QScrollArea>
#include <QVBoxLayout>

namespace gt::gui {

Page::Page(const QString& title, const QString& subtitle, QWidget* parent, bool scroll) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* body = new QWidget;
    m_content = new QVBoxLayout(body);
    m_content->setContentsMargins(28, 22, 28, 22);
    m_content->setSpacing(14);
    m_content->addWidget(makeTitle(title, 8));
    if (!subtitle.isEmpty()) m_content->addWidget(makeHint(subtitle));
    if (scroll) {
        auto* sa = new QScrollArea;
        sa->setWidgetResizable(true);
        sa->setFrameShape(QFrame::NoFrame);
        sa->setWidget(body);
        outer->addWidget(sa);
    } else {
        outer->addWidget(body);
    }
    setAccessibleName(title);
}

} // namespace gt::gui
