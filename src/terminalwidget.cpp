#include "terminalwidget.h"
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QResizeEvent>
#include <QClipboard>
#include <QGuiApplication>
#include <pty.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <algorithm>
#include <array>

TerminalWidget::TerminalWidget(QWidget *parent)
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
      m_mouseEnabled(true),
      m_selecting(false),
      m_hasSelection(false)
{
    setFont(QFont("Monospace", 10));

    m_charWidth  = fontMetrics().horizontalAdvance('M');
    m_charHeight = fontMetrics().height();

    m_mainScreen      = std::make_unique<ScreenBuffer>(24, 80);
    m_alternateScreen = std::make_unique<ScreenBuffer>(24, 80);

    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

QSize TerminalWidget::sizeHint() const {
    int w = m_mainScreen->cols() * m_charWidth + verticalScrollBar()->sizeHint().width();
    int h = m_mainScreen->rows() * m_charHeight;
    return QSize(w, h);
}

void TerminalWidget::resizeEvent(QResizeEvent *event) {
    Q_UNUSED(event);
    int newCols = std::max(1, width() / m_charWidth);
    int newRows = std::max(1, height() / m_charHeight);
    setTerminalSize(newRows, newCols);
}

void TerminalWidget::paintEvent(QPaintEvent *event) {
    QPainter painter(viewport());
    painter.fillRect(event->rect(), Qt::black);

    int firstVisibleLine = verticalScrollBar()->value();
    int visibleRows      = height() / m_charHeight;

    for (int rowIndex = 0; rowIndex < visibleRows; ++rowIndex) {
        int lineIndex = firstVisibleLine + rowIndex;

        if (lineIndex >= int(m_scrollbackBuffer.size()) + currentBuffer().rows())
            break;

        const Cell* cells = getCellsAtAbsoluteLine(lineIndex);
        if (!cells) continue;

        for (int col = 0; col < currentBuffer().cols(); ++col) {
            drawCell(painter, rowIndex, col, cells[col]);

            if (isWithinLineSelection(lineIndex, col)) {
                painter.fillRect(col * m_charWidth, rowIndex * m_charHeight,
                                 m_charWidth, m_charHeight,
                                 QColor(128, 128, 255, 128));
            }
        }
    }

    if (m_showCursor) {
        drawCursor(painter, firstVisibleLine, visibleRows);
    }
}

void TerminalWidget::mousePressEvent(QMouseEvent *event) {
    handleIfMouseEnabled(event, [=]() {
        if (event->button() == Qt::LeftButton) {

            if (event->type() == QEvent::MouseButtonDblClick) {
                int row = (event->pos().y() / m_charHeight) + verticalScrollBar()->value();
                int col = (event->pos().x() / m_charWidth);
                clampLineCol(row, col);
                selectWordAtPosition(row, col);
            }
            else {

                m_selecting = true;
                m_hasSelection = false;

                int row = (event->pos().y() / m_charHeight) + verticalScrollBar()->value();
                int col = (event->pos().x() / m_charWidth);
                clampLineCol(row, col);

                m_selAnchorAbsLine = row;
                m_selAnchorCol     = col;
                m_selActiveAbsLine = row;
                m_selActiveCol     = col;
                viewport()->update();
            }
        }
    });
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *event) {
    handleIfMouseEnabled(event, [=]() {
        if (m_selecting && (event->buttons() & Qt::LeftButton)) {
            int row = (event->pos().y() / m_charHeight) + verticalScrollBar()->value();
            int col = (event->pos().x() / m_charWidth);
            clampLineCol(row, col);

            m_selActiveAbsLine = row;
            m_selActiveCol     = col;
            viewport()->update();
        }
    });
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *event) {
    handleIfMouseEnabled(event, [=]() {
        if (event->button() == Qt::LeftButton) {

            m_selecting   = false;
            m_hasSelection = true;
            viewport()->update();
        }
    });
}

void TerminalWidget::keyPressEvent(QKeyEvent *event) {

    if ((event->modifiers() & Qt::ControlModifier) &&
        (event->modifiers() & Qt::ShiftModifier))
    {
        if (event->key() == Qt::Key_C) {

            copyToClipboard();
            return;
        }
        else if (event->key() == Qt::Key_V) {

            pasteFromClipboard();
            return;
        }
    }

    QByteArray seq = keyEventToAnsiSequence(event);
    if (!seq.isEmpty()) {
        if (m_ptyMaster >= 0) {
            ::write(m_ptyMaster, seq.constData(), seq.size());
        }
        return;
    }

    QString text = event->text();
    if (!text.isEmpty()) {
        if (m_ptyMaster >= 0) {
            QByteArray ba = text.toUtf8();
            ::write(m_ptyMaster, ba.constData(), ba.size());
        }
    }
    else {

        handleSpecialKey(event->key());
    }
}

void TerminalWidget::setPtyInfo(int ptyMaster, pid_t shellPid)
{
    m_ptyMaster = ptyMaster;
    m_shellPid  = shellPid;
}

void TerminalWidget::setMouseEnabled(bool on)
{
    m_mouseEnabled = on;
}

void TerminalWidget::updateScreen()
{
    viewport()->update();
}

void TerminalWidget::useAlternateScreen(bool alt)
{
    if (m_inAlternateScreen == alt) return;
    if (alt) {

        m_alternateScreen->resize(m_mainScreen->rows(), m_mainScreen->cols());
        Cell blank;
        fillScreen(*m_alternateScreen, blank);
    }
    m_inAlternateScreen = alt;
    viewport()->update();
}

void TerminalWidget::setScrollingRegion(int top, int bottom)
{
    if (bottom < top) {
        m_scrollRegionTop    = 0;
        m_scrollRegionBottom = currentBuffer().rows() - 1;
    } else {
        m_scrollRegionTop    = std::clamp(top, 0, currentBuffer().rows() - 1);
        m_scrollRegionBottom = std::clamp(bottom, 0, currentBuffer().rows() - 1);
    }
}

void TerminalWidget::lineFeed()
{
    m_cursorRow++;
    if (m_cursorRow > m_scrollRegionBottom) {
        scrollUp(m_scrollRegionTop, m_scrollRegionBottom);
        m_cursorRow = m_scrollRegionBottom;
    }
    clampCursor();
}

void TerminalWidget::reverseLineFeed()
{
    if (m_cursorRow == m_scrollRegionTop) {
        scrollDown(m_scrollRegionTop, m_scrollRegionBottom);
    } else {
        m_cursorRow--;
        if (m_cursorRow < 0) m_cursorRow = 0;
    }
    clampCursor();
}

void TerminalWidget::putChar(QChar ch)
{
    if (ch == '\r') {
        m_cursorCol = 0;
    } else if (ch == '\n') {
        lineFeed();
    } else {

        Cell &c = currentBuffer().cell(m_cursorRow, m_cursorCol);
        c.ch    = ch;
        c.fg    = m_currentFg;
        c.bg    = m_currentBg;
        c.style = m_currentStyle;

        m_cursorCol++;
        if (m_cursorCol >= currentBuffer().cols()) {
            m_cursorCol = 0;
            lineFeed();
        }
    }
}

void TerminalWidget::setCursorPos(int row, int col, bool doClamp)
{
    if (doClamp) {
        row = std::clamp(row, 0, currentBuffer().rows() - 1);
        col = std::clamp(col, 0, currentBuffer().cols() - 1);
    }
    m_cursorRow = row;
    m_cursorCol = col;
}

void TerminalWidget::saveCursorPos()
{
    m_savedCursorRow = m_cursorRow;
    m_savedCursorCol = m_cursorCol;
}

void TerminalWidget::restoreCursorPos()
{
    m_cursorRow = m_savedCursorRow;
    m_cursorCol = m_savedCursorCol;
    clampCursor();
}

void TerminalWidget::eraseInLine(int mode)
{
    Cell blank = makeCellForCurrentAttr();
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

void TerminalWidget::eraseInDisplay(int mode)
{
    Cell blank = makeCellForCurrentAttr();

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

void TerminalWidget::setSGR(const std::vector<int> &params)
{
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

        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            m_currentFg = p - 30;
            break;

        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            m_currentBg = p - 40;
            break;

        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            m_currentFg = (p - 90) + 8;
            break;

        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            m_currentBg = (p - 100) + 8;
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

void TerminalWidget::scrollUp(int top, int bottom)
{

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
    blank.fg    = m_currentFg;
    blank.bg    = m_currentBg;
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

void TerminalWidget::scrollDown(int top, int bottom)
{

    for (int r = bottom; r > top; --r) {
        for (int c = 0; c < currentBuffer().cols(); ++c) {
            currentBuffer().cell(r, c) = currentBuffer().cell(r - 1, c);
        }
    }

    Cell blank;
    blank.fg    = m_currentFg;
    blank.bg    = m_currentBg;
    blank.style = 0;
    currentBuffer().fillRow(top, 0, currentBuffer().cols(), blank);
}

void TerminalWidget::clampCursor()
{
    m_cursorRow = std::clamp(m_cursorRow, 0, currentBuffer().rows() - 1);
    m_cursorCol = std::clamp(m_cursorCol, 0, currentBuffer().cols() - 1);
}

void TerminalWidget::clampLineCol(int &line, int &col)
{
    int maxAbsLine = int(m_scrollbackBuffer.size()) + currentBuffer().rows() - 1;
    line = std::clamp(line, 0, maxAbsLine);
    col  = std::clamp(col, 0, currentBuffer().cols() - 1);
}

ScreenBuffer &TerminalWidget::currentBuffer()
{
    return m_inAlternateScreen ? *m_alternateScreen : *m_mainScreen;
}

const ScreenBuffer &TerminalWidget::currentBuffer() const
{
    return m_inAlternateScreen ? *m_alternateScreen : *m_mainScreen;
}

void TerminalWidget::fillScreen(ScreenBuffer &buf, const Cell &blank)
{
    for (int r = 0; r < buf.rows(); ++r) {
        buf.fillRow(r, 0, buf.cols(), blank);
    }
}

void TerminalWidget::setTerminalSize(int rows, int cols)
{
    m_mainScreen->resize(rows, cols);
    m_alternateScreen->resize(rows, cols);

    m_scrollRegionTop    = 0;
    m_scrollRegionBottom = rows - 1;

    clampCursor();

    int maxScroll = static_cast<int>(m_scrollbackBuffer.size());
    verticalScrollBar()->setRange(0, maxScroll);
    verticalScrollBar()->setPageStep(rows);

    if (m_ptyMaster >= 0) {
        struct winsize ws;
        ws.ws_row = rows;
        ws.ws_col = cols;
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
        ioctl(m_ptyMaster, TIOCSWINSZ, &ws);
    }

    viewport()->update();
}

QColor TerminalWidget::ansiIndexToColor(int idx, bool bold)
{

    if (idx < 16) {
        static const QColor basicTable[16] = {
            QColor(Qt::black),
            QColor(Qt::red),
            QColor(Qt::green),
            QColor(Qt::yellow),
            QColor(Qt::blue),
            QColor(Qt::magenta),
            QColor(Qt::cyan),
            QColor(Qt::lightGray),

            QColor(Qt::darkGray),
            QColor(Qt::red).lighter(150),
            QColor(Qt::green).lighter(150),
            QColor(Qt::yellow).lighter(150),
            QColor(Qt::blue).lighter(150),
            QColor(Qt::magenta).lighter(150),
            QColor(Qt::cyan).lighter(150),
            QColor(Qt::white)
        };
        int safe = std::clamp(idx, 0, 15);
        QColor c = basicTable[safe];

        if (bold && idx < 8) {
            c = c.lighter(130);
        }
        return c;
    }
    else if (idx < 256) {

        int offset = idx - 16;
        if (offset < 216) {

            int r = offset / 36;
            int g = (offset % 36) / 6;
            int b = offset % 6;
            auto rgb = [&](int v){ return (v == 0) ? 0 : 55 + v * 40; };
            return QColor(rgb(r), rgb(g), rgb(b));
        }
        else {

            int level = idx - 232;
            int gray  = 8 + level * 10;
            return QColor(gray, gray, gray);
        }
    }

    return (idx < 0) ? QColor(Qt::black) : QColor(Qt::white);
}

void TerminalWidget::drawCell(QPainter& p, int canvasRow, int col, const Cell& cell)
{
    int x = col * m_charWidth;
    int y = canvasRow * m_charHeight;

    bool isBold      = (cell.style & (unsigned char)TextStyle::Bold);
    bool isUnderline = (cell.style & (unsigned char)TextStyle::Underline);
    bool isInverse   = (cell.style & (unsigned char)TextStyle::Inverse);

    QColor fg = ansiIndexToColor(cell.fg, isBold);
    QColor bg = ansiIndexToColor(cell.bg, false);

    if (isInverse) {
        std::swap(fg, bg);
    }

    p.fillRect(x, y, m_charWidth, m_charHeight, bg);

    p.setPen(fg);
    int baseline = y + fontMetrics().ascent();
    p.drawText(x, baseline, QString(cell.ch));

    if (isUnderline) {
        int underlineY = y + fontMetrics().underlinePos();
        p.drawLine(x, underlineY, x + m_charWidth, underlineY);
    }
}

void TerminalWidget::selectWordAtPosition(int row, int col)
{

    const Cell* cells = nullptr;
    if (row < int(m_scrollbackBuffer.size())) {
        cells = m_scrollbackBuffer[size_t(row)].data();
    } else {
        int screenRow = row - int(m_scrollbackBuffer.size());
        if (screenRow >= 0 && screenRow < currentBuffer().rows()) {
            cells = &currentBuffer().cell(screenRow, 0);
        }
    }
    if (!cells) return;

    int startCol = col;
    while (startCol > 0 && !cells[startCol - 1].ch.isSpace()) {
        startCol--;
    }

    int endCol = col;
    while (endCol < currentBuffer().cols() - 1 && !cells[endCol + 1].ch.isSpace()) {
        endCol++;
    }

    m_selAnchorAbsLine = row;
    m_selAnchorCol     = startCol;
    m_selActiveAbsLine = row;
    m_selActiveCol     = endCol;
    m_hasSelection     = true;

    viewport()->update();
}

void TerminalWidget::clearSelection()
{
    m_hasSelection = false;
    viewport()->update();
}

bool TerminalWidget::hasSelection() const
{
    if (!m_hasSelection) return false;

    bool sameLine = (m_selAnchorAbsLine == m_selActiveAbsLine);
    bool sameCol  = (m_selAnchorCol == m_selActiveCol);
    return !(sameLine && sameCol);
}

QString TerminalWidget::selectedText() const
{
    if (!hasSelection()) return QString();

    int startLine = std::min(m_selAnchorAbsLine, m_selActiveAbsLine);
    int endLine   = std::max(m_selAnchorAbsLine, m_selActiveAbsLine);

    QStringList lines;

    for (int absLine = startLine; absLine <= endLine; ++absLine) {
        const Cell *rowCells = getCellsAtAbsoluteLine(absLine);
        if (!rowCells) continue;

        int sc = (absLine == startLine)
           ? ((m_selAnchorAbsLine < m_selActiveAbsLine) ? m_selAnchorCol : m_selActiveCol)
           : 0;
        int ec = (absLine == endLine)
           ? ((m_selAnchorAbsLine > m_selActiveAbsLine) ? m_selAnchorCol : m_selActiveCol)
           : currentBuffer().cols() - 1;

        if (sc > ec) std::swap(sc, ec);
        sc = std::clamp(sc, 0, currentBuffer().cols() - 1);
        ec = std::clamp(ec, 0, currentBuffer().cols() - 1);

        QString lineText;
        for (int c = sc; c <= ec; ++c) {
            lineText.append(rowCells[c].ch);
        }
        lines << lineText;
    }
    return lines.join("\n");
}

bool TerminalWidget::isWithinLineSelection(int lineIndex, int col) const
{
    if (!m_hasSelection) return false;

    int startLine = std::min(m_selAnchorAbsLine, m_selActiveAbsLine);
    int endLine   = std::max(m_selAnchorAbsLine, m_selActiveAbsLine);
    if (lineIndex < startLine || lineIndex > endLine) return false;

    int lineStartCol = (lineIndex == startLine)
         ? ((m_selAnchorAbsLine < m_selActiveAbsLine) ? m_selAnchorCol : m_selActiveCol)
         : 0;
    int lineEndCol = (lineIndex == endLine)
         ? ((m_selAnchorAbsLine > m_selActiveAbsLine) ? m_selAnchorCol : m_selActiveCol)
         : currentBuffer().cols() - 1;
    if (lineStartCol > lineEndCol) std::swap(lineStartCol, lineEndCol);

    return (col >= lineStartCol && col <= lineEndCol);
}

void TerminalWidget::drawCursor(QPainter &p, int firstVisibleLine, int visibleRows)
{
    int cursorAbsRow = int(m_scrollbackBuffer.size()) + m_cursorRow;
    if (cursorAbsRow >= firstVisibleLine &&
        cursorAbsRow < firstVisibleLine + visibleRows)
    {
        int cy = (cursorAbsRow - firstVisibleLine) * m_charHeight;
        QRect r(m_cursorCol * m_charWidth, cy, m_charWidth, m_charHeight);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        p.drawRect(r);
    }
}

const Cell* TerminalWidget::getCellsAtAbsoluteLine(int absLine) const
{

    if (absLine < int(m_scrollbackBuffer.size())) {
        return m_scrollbackBuffer[size_t(absLine)].data();
    }

    int offset = absLine - int(m_scrollbackBuffer.size());
    if (offset >= 0 && offset < currentBuffer().rows()) {
        return &currentBuffer().cell(offset, 0);
    }
    return nullptr;
}

void TerminalWidget::handleSpecialKey(int key)
{
    if (m_ptyMaster < 0) return;

    switch (key) {
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

void TerminalWidget::copyToClipboard()
{
    if (!hasSelection()) return;
    QString sel = selectedText();
    QClipboard *cb = QGuiApplication::clipboard();
    cb->setText(sel, QClipboard::Clipboard);
}

void TerminalWidget::pasteFromClipboard()
{
    QClipboard *cb = QGuiApplication::clipboard();
    QString text   = cb->text(QClipboard::Clipboard);
    if (!text.isEmpty() && m_ptyMaster >= 0) {
        QByteArray ba = text.toUtf8();
        ::write(m_ptyMaster, ba.constData(), ba.size());
    }
}

Cell TerminalWidget::makeCellForCurrentAttr() const
{
    Cell blank;
    blank.fg    = m_currentFg;
    blank.bg    = m_currentBg;
    blank.style = m_currentStyle;
    return blank;
}

QByteArray TerminalWidget::keyEventToAnsiSequence(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Up:       return "\x1B[A";
    case Qt::Key_Down:     return "\x1B[B";
    case Qt::Key_Right:    return "\x1B[C";
    case Qt::Key_Left:     return "\x1B[D";
    case Qt::Key_Home:     return "\x1B[H";
    case Qt::Key_End:      return "\x1B[F";
    case Qt::Key_PageUp:   return "\x1B[5~";
    case Qt::Key_PageDown: return "\x1B[6~";
    case Qt::Key_Insert:   return "\x1B[2~";
    case Qt::Key_Delete:   return "\x1B[3~";
    default:
        return QByteArray();
    }
}

void TerminalWidget::handleIfMouseEnabled(QMouseEvent* event, std::function<void()> fn)
{
    if (!m_mouseEnabled) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    fn();
}
