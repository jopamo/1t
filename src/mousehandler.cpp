#include "mousehandler.h"
#include "terminalwidget.h"
#include <QMouseEvent>
#include <unistd.h>

MouseHandler::MouseHandler(TerminalWidget* terminal) : m_terminal(terminal), m_mouseEnabled(false) {}

void MouseHandler::handleMousePressEvent(QMouseEvent* event) {
    QByteArray seq;
    seq.append("\x1B[M");
    seq.append(char(32 + static_cast<int>(event->button())));
    seq.append(char(32 + (event->pos().x() / m_terminal->charWidth())));
    seq.append(char(32 + (event->pos().y() / m_terminal->charHeight())));

    ::write(m_terminal->getPtyMaster(), seq.constData(), seq.size());
}

void MouseHandler::handleMouseReleaseEvent(QMouseEvent* event) {
    QByteArray seq;
    seq.append("\x1B[M");
    seq.append(char(32 + 3));
    seq.append(char(32 + (event->pos().x() / m_terminal->charWidth())));
    seq.append(char(32 + (event->pos().y() / m_terminal->charHeight())));

    ::write(m_terminal->getPtyMaster(), seq.constData(), seq.size());
}

void MouseHandler::handleMouseMoveEvent(QMouseEvent* event) {
    QByteArray seq;
    seq.append("\x1B[M");
    seq.append(char(32));
    seq.append(char(32 + (event->pos().x() / m_terminal->charWidth())));
    seq.append(char(32 + (event->pos().y() / m_terminal->charHeight())));

    ::write(m_terminal->getPtyMaster(), seq.constData(), seq.size());
}

void MouseHandler::setMouseEnabled(bool enabled) {
    m_mouseEnabled = enabled;
}
