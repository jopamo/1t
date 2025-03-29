#ifndef MOUSEHANDLER_H
#define MOUSEHANDLER_H

#include <QMouseEvent>

class TerminalWidget;

class MouseHandler {
   public:
    explicit MouseHandler(TerminalWidget* terminal);

    void handleMousePressEvent(QMouseEvent* event);
    void handleMouseReleaseEvent(QMouseEvent* event);
    void handleMouseMoveEvent(QMouseEvent* event);

    void setMouseEnabled(bool enabled);

   private:
    TerminalWidget* m_terminal;
    bool m_mouseEnabled;
};

#endif
