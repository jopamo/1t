#include "terminalwidget.h"
#include "mousehandler.h"

#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QScrollBar>

#include <pty.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <termios.h>
#include <fcntl.h>

#include <algorithm>
#include <array>
#include <memory>

TerminalWidget::TerminalWidget(QWidget* parent)
    : QAbstractScrollArea(parent),
      m_inAlternateScreen(false),
      m_scrollbackMax(1000),
      m_showCursor(true),
      m_cursorRow(0),
      m_cursorCol(0),
      m_savedCursorRow(0),
      m_savedCursorCol(0),
      m_currentFg(7),
      m_currentBg(0),
      m_currentStyle(0),
      m_scrollRegionTop(0),
      m_scrollRegionBottom(23),
      m_mouseHandler(std::make_unique<MouseHandler>(this)) {
    setFont(QFont("Monospace", 10, QFont::Normal));
    m_charWidth = fontMetrics().horizontalAdvance('M');
    m_charHeight = fontMetrics().height();

    m_mainScreen = std::make_unique<ScreenBuffer>(24, 80);
    m_alternateScreen = std::make_unique<ScreenBuffer>(24, 80);
    m_scrollRegionTop = 0;
    m_scrollRegionBottom = 24 - 1;

    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

QSize TerminalWidget::sizeHint() const {
    int w = m_mainScreen->cols() * m_charWidth + verticalScrollBar()->sizeHint().width();
    int h = m_mainScreen->rows() * m_charHeight;
    return QSize(w, h);
}

void TerminalWidget::useAlternateScreen(bool alt) {
    if (m_inAlternateScreen == alt)
        return;

    if (alt) {
        m_alternateScreen->resize(m_mainScreen->rows(), m_mainScreen->cols());
        Cell blank;
        fillScreen(*m_alternateScreen, blank);
    }
    m_inAlternateScreen = alt;
    viewport()->update();
}

void TerminalWidget::setScrollingRegion(int top, int bottom) {
    if (bottom < top) {
        m_scrollRegionTop = 0;
        m_scrollRegionBottom = currentBuffer().rows() - 1;
    }
    else {
        m_scrollRegionTop = std::clamp(top, 0, currentBuffer().rows() - 1);
        m_scrollRegionBottom = std::clamp(bottom, 0, currentBuffer().rows() - 1);
    }
}

void TerminalWidget::lineFeed() {
    m_cursorRow++;
    if (m_cursorRow > m_scrollRegionBottom) {
        scrollUp(m_scrollRegionTop, m_scrollRegionBottom);
        m_cursorRow = m_scrollRegionBottom;
    }
    clampCursor();
}

void TerminalWidget::reverseLineFeed() {
    if (m_cursorRow == m_scrollRegionTop) {
        scrollDown(m_scrollRegionTop, m_scrollRegionBottom);
    }
    else {
        m_cursorRow--;
        if (m_cursorRow < 0)
            m_cursorRow = 0;
    }
    clampCursor();
}

void TerminalWidget::putChar(QChar ch) {
    if (ch == '\r') {
        m_cursorCol = 0;
    }
    else if (ch == '\n') {
        lineFeed();
    }
    else {
        auto& c = currentBuffer().cell(m_cursorRow, m_cursorCol);
        c.ch = ch;
        c.fg = m_currentFg;
        c.bg = m_currentBg;
        c.style = m_currentStyle;
        m_cursorCol++;
        if (m_cursorCol >= currentBuffer().cols()) {
            m_cursorCol = 0;
            lineFeed();
        }
    }
}

void TerminalWidget::setCursorPos(int row, int col, bool clamp) {
    if (clamp) {
        row = std::clamp(row, 0, currentBuffer().rows() - 1);
        col = std::clamp(col, 0, currentBuffer().cols() - 1);
    }
    m_cursorRow = row;
    m_cursorCol = col;
}

void TerminalWidget::saveCursorPos() {
    m_savedCursorRow = m_cursorRow;
    m_savedCursorCol = m_cursorCol;
}

void TerminalWidget::restoreCursorPos() {
    m_cursorRow = m_savedCursorRow;
    m_cursorCol = m_savedCursorCol;
    clampCursor();
}

void TerminalWidget::eraseInLine(int mode) {
    Cell blank;
    blank.fg = m_currentFg;
    blank.bg = m_currentBg;
    blank.style = m_currentStyle;

    int row = m_cursorRow;
    switch (mode) {
        case 0:
            if (m_cursorCol < currentBuffer().cols()) {
                currentBuffer().fillRow(row, m_cursorCol, currentBuffer().cols(), blank);
            }
            break;
        case 1:
            if (m_cursorCol >= 0) {
                currentBuffer().fillRow(row, 0, m_cursorCol + 1, blank);
            }
            break;
        case 2:
        default:
            currentBuffer().fillRow(row, 0, currentBuffer().cols(), blank);
            break;
    }
}

void TerminalWidget::eraseInDisplay(int mode) {
    Cell blank;
    blank.fg = m_currentFg;
    blank.bg = m_currentBg;
    blank.style = m_currentStyle;

    if (mode == 2) {
        fillScreen(currentBuffer(), blank);
    }
    else if (mode == 0) {
        eraseInLine(0);

        for (int r = m_cursorRow + 1; r < currentBuffer().rows(); ++r) {
            currentBuffer().fillRow(r, 0, currentBuffer().cols(), blank);
        }
    }
    else if (mode == 1) {
        eraseInLine(1);
        for (int r = 0; r < m_cursorRow; ++r) {
            currentBuffer().fillRow(r, 0, currentBuffer().cols(), blank);
        }
    }
}

void TerminalWidget::setSGR(const std::vector<int>& params) {
    if (params.empty()) {
        m_currentFg = 7;
        m_currentBg = 0;
        m_currentStyle = 0;
        return;
    }

    size_t i = 0;
    while (i < params.size()) {
        int p = params[i++];
        switch (p) {
            case 0:
                m_currentFg = 7;
                m_currentBg = 0;
                m_currentStyle = 0;
                break;
            case 1:
                m_currentStyle |= (unsigned char)TextStyle::Bold;
                break;
            case 4:
                m_currentStyle |= (unsigned char)TextStyle::Underline;
                break;
            case 7:
                m_currentStyle |= (unsigned char)TextStyle::Inverse;
                break;
            case 22:
                m_currentStyle &= ~(unsigned char)TextStyle::Bold;
                break;
            case 24:
                m_currentStyle &= ~(unsigned char)TextStyle::Underline;
                break;
            case 27:
                m_currentStyle &= ~(unsigned char)TextStyle::Inverse;
                break;
            case 39:
                m_currentFg = 7;
                break;
            case 49:
                m_currentBg = 0;
                break;

            case 30:
            case 31:
            case 32:
            case 33:
            case 34:
            case 35:
            case 36:
            case 37:
                m_currentFg = p - 30;
                break;

            case 40:
            case 41:
            case 42:
            case 43:
            case 44:
            case 45:
            case 46:
            case 47:
                m_currentBg = p - 40;
                break;

            case 38:
                if (i + 1 < params.size() && params[i] == 5) {
                    i++;
                    if (i < params.size()) {
                        m_currentFg = params[i++];
                    }
                }
                break;

            case 48:
                if (i + 1 < params.size() && params[i] == 5) {
                    i++;
                    if (i < params.size()) {
                        m_currentBg = params[i++];
                    }
                }
                break;
            default:

                break;
        }
    }
}

void TerminalWidget::scrollUp(int top, int bottom) {
    std::vector<Cell> scrolledLine(currentBuffer().cols());
    for (int c = 0; c < currentBuffer().cols(); ++c) {
        scrolledLine[size_t(c)] = currentBuffer().cell(top, c);
    }

    for (int r = top; r < bottom; ++r) {
        for (int c = 0; c < currentBuffer().cols(); ++c) {
            currentBuffer().cell(r, c) = currentBuffer().cell(r + 1, c);
        }
    }

    Cell blank;
    blank.fg = m_currentFg;
    blank.bg = m_currentBg;
    blank.style = 0;
    currentBuffer().fillRow(bottom, 0, currentBuffer().cols(), blank);

    if (m_scrollbackBuffer.size() >= size_t(m_scrollbackMax)) {
        m_scrollbackBuffer.pop_front();
    }
    m_scrollbackBuffer.push_back(std::move(scrolledLine));

    int maxScroll = static_cast<int>(m_scrollbackBuffer.size());
    verticalScrollBar()->setRange(0, maxScroll);
    verticalScrollBar()->setPageStep(currentBuffer().rows());
    verticalScrollBar()->setValue(maxScroll);
}

void TerminalWidget::scrollDown(int top, int bottom) {
    for (int r = bottom; r > top; --r) {
        for (int c = 0; c < currentBuffer().cols(); ++c) {
            currentBuffer().cell(r, c) = currentBuffer().cell(r - 1, c);
        }
    }

    Cell blank;
    blank.fg = m_currentFg;
    blank.bg = m_currentBg;
    blank.style = 0;
    currentBuffer().fillRow(top, 0, currentBuffer().cols(), blank);
}

void TerminalWidget::clampCursor() {
    m_cursorRow = std::clamp(m_cursorRow, 0, currentBuffer().rows() - 1);
    m_cursorCol = std::clamp(m_cursorCol, 0, currentBuffer().cols() - 1);
}

void TerminalWidget::setTerminalSize(int rows, int cols) {
    if (rows < 1 || cols < 1)
        return;

    m_mainScreen->resize(rows, cols);
    m_alternateScreen->resize(rows, cols);

    clampCursor();
    m_scrollRegionTop = 0;
    m_scrollRegionBottom = rows - 1;

    if (m_ptyMaster >= 0) {
        struct winsize ws;
        ws.ws_row = rows;
        ws.ws_col = cols;
        ws.ws_xpixel = cols * m_charWidth;
        ws.ws_ypixel = rows * m_charHeight;
        ioctl(m_ptyMaster, TIOCSWINSZ, &ws);
        if (m_shellPid > 0) {
            kill(m_shellPid, SIGWINCH);
        }
    }
    viewport()->update();
}

void TerminalWidget::setPtyInfo(int ptyMaster, pid_t shellPid) {
    m_ptyMaster = ptyMaster;
    m_shellPid = shellPid;
}

void TerminalWidget::fillScreen(ScreenBuffer& buf, const Cell& blank) {
    for (int r = 0; r < buf.rows(); ++r) {
        buf.fillRow(r, 0, buf.cols(), blank);
    }
}

void TerminalWidget::paintEvent(QPaintEvent* event) {
    QPainter painter(viewport());
    painter.fillRect(event->rect(), Qt::black);

    int firstVisibleLine = verticalScrollBar()->value();
    int visibleRows = height() / m_charHeight;

    for (int row = 0; row < visibleRows; ++row) {
        int lineIndex = firstVisibleLine + row;

        if (lineIndex >= int(m_scrollbackBuffer.size()) + currentBuffer().rows()) {
            break;
        }

        const Cell* cells = nullptr;
        if (lineIndex < int(m_scrollbackBuffer.size())) {
            cells = m_scrollbackBuffer[size_t(lineIndex)].data();
        }
        else {
            int screenRow = lineIndex - int(m_scrollbackBuffer.size());
            if (screenRow >= 0 && screenRow < currentBuffer().rows()) {
                cells = &currentBuffer().cell(screenRow, 0);
            }
        }

        if (!cells) {
            continue;
        }

        for (int col = 0; col < currentBuffer().cols(); ++col) {
            drawCell(painter, row, col, cells[col]);
        }
    }

    if (m_showCursor) {
        int cursorAbsoluteRow = int(m_scrollbackBuffer.size()) + m_cursorRow;

        if (cursorAbsoluteRow >= firstVisibleLine && cursorAbsoluteRow < firstVisibleLine + visibleRows) {
            int cursorY = (cursorAbsoluteRow - firstVisibleLine) * m_charHeight;
            QRect r(m_cursorCol * m_charWidth, cursorY, m_charWidth, m_charHeight);
            painter.setPen(Qt::NoPen);
            painter.setBrush(Qt::white);
            painter.drawRect(r);
        }
    }
}

void TerminalWidget::resizeEvent(QResizeEvent* event) {
    Q_UNUSED(event);
    int newCols = std::max(1, width() / m_charWidth);
    int newRows = std::max(1, height() / m_charHeight);
    setTerminalSize(newRows, newCols);
}

void TerminalWidget::keyPressEvent(QKeyEvent* event) {
    if (m_ptyMaster < 0) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    QByteArray seq = keyEventToAnsiSequence(event);
    if (!seq.isEmpty()) {
        ::write(m_ptyMaster, seq.constData(), seq.size());
        return;
    }

    QString text = event->text();
    if (!text.isEmpty()) {
        QByteArray ba = text.toUtf8();
        ::write(m_ptyMaster, ba.constData(), ba.size());
    }
    else {
        switch (event->key()) {
            case Qt::Key_Enter:
            case Qt::Key_Return:
                ::write(m_ptyMaster, "\r", 1);
                break;
            case Qt::Key_Backspace:

                ::write(m_ptyMaster, "\x7F", 1);
                break;
            case Qt::Key_Tab:
                ::write(m_ptyMaster, "\t", 1);
                break;
            default:
                break;
        }
    }
}

void TerminalWidget::updateScreen() {
    viewport()->update();
}

ScreenBuffer& TerminalWidget::currentBuffer() {
    return m_inAlternateScreen ? *m_alternateScreen : *m_mainScreen;
}
const ScreenBuffer& TerminalWidget::currentBuffer() const {
    return m_inAlternateScreen ? *m_alternateScreen : *m_mainScreen;
}

void TerminalWidget::drawCell(QPainter& p, int canvasRow, int col, const Cell& cell) {
    int x = col * m_charWidth;
    int y = canvasRow * m_charHeight;

    bool inverse = (cell.style & (unsigned char)TextStyle::Inverse) != 0;
    bool bold = (cell.style & (unsigned char)TextStyle::Bold) != 0;
    bool underline = (cell.style & (unsigned char)TextStyle::Underline) != 0;

    int fgIndex = inverse ? cell.bg : cell.fg;
    int bgIndex = inverse ? cell.fg : cell.bg;

    QColor fgColor = ansiIndexToColor(fgIndex, bold);
    QColor bgColor = ansiIndexToColor(bgIndex, false);

    p.fillRect(x, y, m_charWidth, m_charHeight, bgColor);

    p.setPen(fgColor);
    p.drawText(x, y + m_charHeight - fontMetrics().descent(), QString(cell.ch));

    if (underline) {
        p.drawLine(x, y + m_charHeight - 1, x + m_charWidth, y + m_charHeight - 1);
    }
}

QColor TerminalWidget::ansiIndexToColor(int idx, bool bold) {
    static const std::array<QColor, 16> basicTable = {
        QColor(0, 0, 0),       QColor(128, 0, 0),   QColor(0, 128, 0),   QColor(128, 128, 0),
        QColor(0, 0, 128),     QColor(128, 0, 128), QColor(0, 128, 128), QColor(192, 192, 192),
        QColor(128, 128, 128), QColor(255, 0, 0),   QColor(0, 255, 0),   QColor(255, 255, 0),
        QColor(0, 0, 255),     QColor(255, 0, 255), QColor(0, 255, 255), QColor(255, 255, 255)};

    if (idx < 0)
        idx = 0;
    if (idx > 255)
        idx = 255;

    if (idx < 16) {
        if (bold && idx < 8) {
            idx += 8;
        }
        return basicTable[size_t(idx)];
    }
    else if (idx < 232) {
        int n = idx - 16;
        int r = (n / 36) % 6;
        int g = (n / 6) % 6;
        int b = (n % 6);
        return QColor(r * 51, g * 51, b * 51);
    }
    else {
        int level = (idx - 232) * 10 + 8;
        return QColor(level, level, level);
    }
}

QByteArray TerminalWidget::keyEventToAnsiSequence(QKeyEvent* event) {
    switch (event->key()) {
        case Qt::Key_Up:
            return "\x1B[A";
        case Qt::Key_Down:
            return "\x1B[B";
        case Qt::Key_Right:
            return "\x1B[C";
        case Qt::Key_Left:
            return "\x1B[D";
        case Qt::Key_Home:
            return "\x1B[H";
        case Qt::Key_End:
            return "\x1B[F";
        case Qt::Key_PageUp:
            return "\x1B[5~";
        case Qt::Key_PageDown:
            return "\x1B[6~";
        case Qt::Key_Insert:
            return "\x1B[2~";
        case Qt::Key_Delete:
            return "\x1B[3~";
        default:
            return QByteArray();
    }
}

void TerminalWidget::mousePressEvent(QMouseEvent* event) {
    m_mouseHandler->handleMousePressEvent(event);
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent* event) {
    m_mouseHandler->handleMouseReleaseEvent(event);
}

void TerminalWidget::mouseMoveEvent(QMouseEvent* event) {
    m_mouseHandler->handleMouseMoveEvent(event);
}

void TerminalWidget::setMouseEnabled(bool on) {
    m_mouseHandler->setMouseEnabled(on);
}
