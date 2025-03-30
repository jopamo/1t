#include "terminalwidget.h"
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QResizeEvent>
#include <QClipboard>
#include <QGuiApplication>
#include <QFont>
#include <QFontDatabase>

#include <pty.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <algorithm>

TerminalWidget::TerminalWidget(QWidget *parent)
    : QAbstractScrollArea(parent),
      m_scrollbackMax(1000),
      m_inAlternateScreen(false),
      m_showCursor(true),
      m_mouseEnabled(true),
      m_selecting(false),
      m_hasSelection(false),
      m_isClosed(false),
      m_cursorRow(0),
      m_cursorCol(0),
      m_savedCursorRow(0),
      m_savedCursorCol(0),
      m_currentFg(7),
      m_currentBg(0),
      m_currentStyle(0),
      m_scrollRegionTop(0),
      m_scrollRegionBottom(23),
      m_ptyMaster(-1),
      m_shellPid(-1),
      m_escape(false),
      m_bracket(false),
      m_textBlinkState(false)
{
    QFont font;
    font.setPointSize(10);
    font.setFamilies({
        "Source Code Pro",
        "Fira Code",
        "Noto Color Emoji",
        "Noto Sans Symbols2",
        "Noto Sans"
    });
    setFont(font);

    m_charWidth  = fontMetrics().horizontalAdvance(QChar('M'));
    m_charHeight = fontMetrics().height();

    // Default to 24x80
    m_mainScreen      = std::make_unique<ScreenBuffer>(24, 80);
    m_alternateScreen = std::make_unique<ScreenBuffer>(24, 80);

    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    // Setup blinking timer (for text with the Blink attribute)
    connect(&m_blinkTimer, &QTimer::timeout, this, &TerminalWidget::blinkEvent);
    m_blinkTimer.start(800); // 800ms blink

    // Optionally hide scrollbars:
    // setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}

TerminalWidget::~TerminalWidget()
{
    // cleanup if needed
}

QSize TerminalWidget::sizeHint() const
{
    int w = m_mainScreen->cols() * m_charWidth + verticalScrollBar()->sizeHint().width();
    int h = m_mainScreen->rows() * m_charHeight;
    return QSize(w, h);
}

void TerminalWidget::setPtyInfo(int ptyMaster, pid_t shellPid)
{
    m_ptyMaster = ptyMaster;
    m_shellPid  = shellPid;
}

/*
void TerminalWidget::setShellProcess(QProcess *proc)
{
    m_shellProcess = proc;
    connect(m_shellProcess, &QProcess::readyReadStandardOutput, this, [=](){
        QByteArray data = m_shellProcess->readAllStandardOutput();
        processIncomingData(data);
    });
    connect(m_shellProcess, &QProcess::readyReadStandardError, this, [=](){
        QByteArray data = m_shellProcess->readAllStandardError();
        processIncomingData(data);
    });
}
*/

void TerminalWidget::processIncomingData(const QByteArray &data)
{
    // Pass each byte to our minimal parse
    for (char c : data) {
        processOneChar(c);
    }
    viewport()->update();
}

void TerminalWidget::processOneChar(char c)
{
    // If we are not currently handling an escape, check if c==ESC
    if (!m_escape) {
        if (c == 0x1B) { // ESC
            m_escape = true;
            m_bracket = false;
            m_escParams.clear();
        }
        else if (c == '\r') {
            m_cursorCol = 0;
        }
        else if (c == '\n') {
            lineFeed();
        }
        else if (c == 0x08) { // backspace
            if (m_cursorCol > 0) {
                m_cursorCol--;
            }
        }
        else {
            // normal char
            if (c >= ' ') {
                putChar(QChar::fromLatin1(c));
            }
        }
    }
    else {
        // We are inside an ESC sequence
        if (!m_bracket) {
            // If it's '[' then it's a CSI
            if (c == '[') {
                m_bracket = true;
            } else {
                // some other ESC code we skip
                m_escape = false;
            }
        }
        else {
            // We have ESC [
            // Accumulate digits and semicolons
            if ((c >= '0' && c <= '9') || c == ';') {
                m_escParams.append(c);
            }
            else {
                // We got a command
                switch (c) {
                case 'J': // erase screen (ESC [ J or ESC [2J)
                {
                    if (m_escParams.isEmpty() || m_escParams=="0" || m_escParams=="2") {
                        doClearScreen();
                    }
                }
                break;
                case 'm': // SGR
                {
                    // parse the numbers in m_escParams separated by ';'
                    QStringList parts = m_escParams.split(';', Qt::SkipEmptyParts);
                    std::vector<int> nums;
                    for (auto &s : parts) {
                        bool ok;
                        int v = s.toInt(&ok);
                        if (ok) nums.push_back(v);
                    }
                    setSGR(nums);
                }
                break;
                // handle more commands if you want...
                default:
                    // skip
                    break;
                }
                // done
                m_escape = false;
            }
        }
    }
}

void TerminalWidget::doClearScreen()
{
    fillScreen(currentBuffer(), makeCellForCurrentAttr());
    setCursorPos(0, 0);
}

void TerminalWidget::blinkEvent()
{
    // Toggle blink state
    m_textBlinkState = !m_textBlinkState;
    viewport()->update();
}

void TerminalWidget::resizeEvent(QResizeEvent *event)
{
    Q_UNUSED(event);
    int newCols = std::max(1, viewport()->width() / m_charWidth);
    int newRows = std::max(1, viewport()->height() / m_charHeight);
    setTerminalSize(newRows, newCols);
}

void TerminalWidget::paintEvent(QPaintEvent *event)
{
    QPainter painter(viewport());
    painter.setFont(font());
    painter.setBackgroundMode(Qt::OpaqueMode);

    if (m_isClosed) {
        painter.drawText(QPointF(5, m_charHeight + 5), "Terminal is closed.");
        return;
    }

    int totalLines = int(m_scrollbackBuffer.size()) + currentBuffer().rows();
    int visibleLines = std::min(viewport()->height() / m_charHeight, totalLines);
    int firstLine = verticalScrollBar()->value();
    int lastLine  = std::min(firstLine + visibleLines, totalLines);

    for (int absLine = firstLine; absLine < lastLine; ++absLine) {
        int screenRow = absLine - firstLine;
        const Cell *rowCells = getCellsAtAbsoluteLine(absLine);
        if (!rowCells) continue;
        int numCols = currentBuffer().cols();
        for (int col = 0; col < numCols; ++col) {
            const Cell &cell = rowCells[col];
            drawCell(painter, screenRow, col, cell);
        }
    }

    if (m_showCursor) {
        int cursorAbsLine = int(m_scrollbackBuffer.size()) + m_cursorRow;
        if (cursorAbsLine >= firstLine && cursorAbsLine < lastLine) {
            drawCursor(painter, firstLine, visibleLines);
        }
    }

    QAbstractScrollArea::paintEvent(event);
}

void TerminalWidget::drawCell(QPainter &p, int screenRow, int col, const Cell &cell)
{
    int x = col * m_charWidth;
    int y = screenRow * m_charHeight;

    bool isBold      = (cell.style & (unsigned char)TextStyle::Bold);
    bool isUnderline = (cell.style & (unsigned char)TextStyle::Underline);
    bool isInverse   = (cell.style & (unsigned char)TextStyle::Inverse);
    bool isBlink     = (cell.style & (unsigned char)TextStyle::Blink);

    // If blinking text is set AND m_textBlinkState==true, skip or invert, etc.
    // For demonstration, let's skip painting the char while blink is active:
    bool skipChar = false;
    if (isBlink && m_textBlinkState) {
        skipChar = true;
    }

    QColor fg = ansiIndexToColor(cell.fg, isBold);
    QColor bg = ansiIndexToColor(cell.bg, false);
    if (isInverse) std::swap(fg, bg);

    p.fillRect(x, y, m_charWidth, m_charHeight, bg);

    if (!skipChar) {
        p.setPen(fg);
        int baseline = y + fontMetrics().ascent();
        p.drawText(x, baseline, QString(cell.ch));
        if (isUnderline) {
            int underlineY = y + fontMetrics().underlinePos();
            p.drawLine(x, underlineY, x + m_charWidth, underlineY);
        }
    }
}

void TerminalWidget::drawCursor(QPainter &painter, int firstLine, int visibleLines)
{
    int cursorAbsLine = int(m_scrollbackBuffer.size()) + m_cursorRow;
    int screenRow = cursorAbsLine - firstLine;
    int xCursor = m_cursorCol * m_charWidth;
    int yCursor = screenRow * m_charHeight;

    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    painter.setOpacity(0.4);
    painter.drawRect(xCursor, yCursor, m_charWidth, m_charHeight);

    // Optionally redraw the character
    if (m_cursorRow >= 0 && m_cursorRow < currentBuffer().rows() &&
        m_cursorCol >= 0 && m_cursorCol < currentBuffer().cols())
    {
        const Cell &cell = currentBuffer().cell(m_cursorRow, m_cursorCol);
        QColor textColor = Qt::black;
        painter.setPen(textColor);
        painter.setOpacity(1.0);
        int baseline = yCursor + fontMetrics().ascent();
        painter.drawText(xCursor, baseline, QString(cell.ch));
    }
    painter.restore();
}

void TerminalWidget::mousePressEvent(QMouseEvent *event)
{
    handleIfMouseEnabled(event, [=]() {
        if (event->button() == Qt::LeftButton) {
            if (event->type() == QEvent::MouseButtonDblClick) {
                int absLine = (event->pos().y() / m_charHeight) + verticalScrollBar()->value();
                int col     = (event->pos().x() / m_charWidth);
                clampLineCol(absLine, col);
                selectWordAtPosition(absLine, col);
            } else {
                m_selecting = true;
                m_hasSelection = false;

                int absLine = (event->pos().y() / m_charHeight) + verticalScrollBar()->value();
                int col     = (event->pos().x() / m_charWidth);
                clampLineCol(absLine, col);

                m_selAnchorAbsLine = absLine;
                m_selAnchorCol     = col;
                m_selActiveAbsLine = absLine;
                m_selActiveCol     = col;
                viewport()->update();
            }
        }
    });
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *event)
{
    handleIfMouseEnabled(event, [=]() {
        if (m_selecting && (event->buttons() & Qt::LeftButton)) {
            int absLine = (event->pos().y() / m_charHeight) + verticalScrollBar()->value();
            int col     = (event->pos().x() / m_charWidth);
            clampLineCol(absLine, col);

            m_selActiveAbsLine = absLine;
            m_selActiveCol     = col;
            viewport()->update();
        }
    });
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *event)
{
    handleIfMouseEnabled(event, [=]() {
        if (event->button() == Qt::LeftButton) {
            m_selecting   = false;
            m_hasSelection = true;
            viewport()->update();
        }
    });
}

void TerminalWidget::keyPressEvent(QKeyEvent *event)
{
    // Ctrl+Shift+C => copy, Ctrl+Shift+V => paste
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
        // if using QProcess:
        // if (m_shellProcess) m_shellProcess->write(seq);
        return;
    }

    QString text = event->text();
    if (!text.isEmpty()) {
        QByteArray ba = text.toUtf8();
        if (m_ptyMaster >= 0) {
            ::write(m_ptyMaster, ba.constData(), ba.size());
        }
        // if (m_shellProcess) m_shellProcess->write(ba);
    }
    else {
        handleSpecialKey(event->key());
    }
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
    }
    else if (ch == '\n') {
        lineFeed();
    }
    else {
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
        currentBuffer().fillRow(row, m_cursorCol, currentBuffer().cols(), blank);
        break;
    case 1:
        currentBuffer().fillRow(row, 0, m_cursorCol + 1, blank);
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
        setCursorPos(0,0);
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
        case 1: // bold
            m_currentStyle |= (unsigned char)TextStyle::Bold;
            break;
        case 4: // underline
            m_currentStyle |= (unsigned char)TextStyle::Underline;
            break;
        case 5: // blink
            m_currentStyle |= (unsigned char)TextStyle::Blink;
            break;
        case 7: // inverse
            m_currentStyle |= (unsigned char)TextStyle::Inverse;
            break;
        case 22: // disable bold
            m_currentStyle &= ~(unsigned char)TextStyle::Bold;
            break;
        case 24: // disable underline
            m_currentStyle &= ~(unsigned char)TextStyle::Underline;
            break;
        case 25: // disable blink
            m_currentStyle &= ~(unsigned char)TextStyle::Blink;
            break;
        case 27: // disable inverse
            m_currentStyle &= ~(unsigned char)TextStyle::Inverse;
            break;
        case 39:
            m_currentFg = 7;
            break;
        case 49:
            m_currentBg = 0;
            break;
        // Basic 3/4-bit
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            m_currentFg = p - 30;
            break;
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            m_currentBg = p - 40;
            break;
        // Bright
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            m_currentFg = (p - 90) + 8;
            break;
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            m_currentBg = (p - 100) + 8;
            break;
        // 256-colors
        case 38:
            if (i+1<params.size() && params[i]==5) {
                i++;
                if (i<params.size()) {
                    m_currentFg = params[i++];
                }
            }
            break;
        case 48:
            if (i+1<params.size() && params[i]==5) {
                i++;
                if (i<params.size()) {
                    m_currentBg = params[i++];
                }
            }
            break;
        default:
            // unhandled
            break;
        }
    }
}

void TerminalWidget::scrollUp(int top, int bottom)
{
    int cols = currentBuffer().cols();
    std::vector<Cell> scrolledLine(cols);
    for (int c = 0; c < cols; ++c) {
        scrolledLine[size_t(c)] = currentBuffer().cell(top, c);
    }
    for (int r = top; r < bottom; ++r) {
        for (int c = 0; c < cols; ++c) {
            currentBuffer().cell(r, c) = currentBuffer().cell(r + 1, c);
        }
    }
    Cell blank = makeCellForCurrentAttr();
    currentBuffer().fillRow(bottom, 0, cols, blank);

    if (m_scrollbackBuffer.size() >= size_t(m_scrollbackMax)) {
        m_scrollbackBuffer.pop_front();
    }
    m_scrollbackBuffer.push_back(std::move(scrolledLine));

    int maxScroll = int(m_scrollbackBuffer.size());
    verticalScrollBar()->setRange(0, maxScroll);
    verticalScrollBar()->setPageStep(currentBuffer().rows());
    verticalScrollBar()->setValue(maxScroll);
}

void TerminalWidget::scrollDown(int top, int bottom)
{
    int cols = currentBuffer().cols();
    for (int r = bottom; r > top; --r) {
        for (int c = 0; c < cols; ++c) {
            currentBuffer().cell(r, c) = currentBuffer().cell(r - 1, c);
        }
    }
    Cell blank = makeCellForCurrentAttr();
    currentBuffer().fillRow(top, 0, cols, blank);
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

    int maxScroll = int(m_scrollbackBuffer.size());
    verticalScrollBar()->setRange(0, maxScroll);
    verticalScrollBar()->setPageStep(rows);

    // Inform the pty
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

    if (idx < 0) return QColor(Qt::black);
    if (idx < 16) {
        QColor c = basicTable[idx];
        if (bold && idx < 8) {
            c = c.lighter(130);
        }
        return c;
    }
    if (idx < 256) {
        int offset = idx - 16;
        if (offset < 216) {
            int r = offset / 36;
            int g = (offset % 36) / 6;
            int b = offset % 6;
            auto rgbVal = [&](int v){ return (v==0) ? 0 : 55 + v*40; };
            return QColor(rgbVal(r), rgbVal(g), rgbVal(b));
        }
        else {
            int level = idx - 232;
            int gray  = 8 + level*10;
            return QColor(gray, gray, gray);
        }
    }
    return QColor(Qt::white);
}

void TerminalWidget::selectWordAtPosition(int absLine, int col)
{
    const Cell* cells = getCellsAtAbsoluteLine(absLine);
    if (!cells) return;

    int startCol = col;
    while (startCol > 0 && !cells[startCol - 1].ch.isSpace()) {
        startCol--;
    }
    int endCol = col;
    int maxC = currentBuffer().cols()-1;
    while (endCol < maxC && !cells[endCol+1].ch.isSpace()) {
        endCol++;
    }

    m_selAnchorAbsLine = absLine;
    m_selAnchorCol     = startCol;
    m_selActiveAbsLine = absLine;
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

        int sc = (absLine==startLine)
                 ? ((m_selAnchorAbsLine< m_selActiveAbsLine)?m_selAnchorCol:m_selActiveCol)
                 : 0;
        int ec = (absLine==endLine)
                 ? ((m_selAnchorAbsLine> m_selActiveAbsLine)?m_selAnchorCol:m_selActiveCol)
                 : (currentBuffer().cols()-1);

        if (sc>ec) std::swap(sc,ec);
        sc = std::clamp(sc, 0, currentBuffer().cols()-1);
        ec = std::clamp(ec, 0, currentBuffer().cols()-1);

        QString lineText;
        for (int c = sc; c<=ec; ++c) {
            lineText.append(rowCells[c].ch);
        }
        lines << lineText;
    }
    return lines.join("\n");
}

const Cell* TerminalWidget::getCellsAtAbsoluteLine(int absLine) const
{
    if (absLine < int(m_scrollbackBuffer.size())) {
        return m_scrollbackBuffer[size_t(absLine)].data();
    }
    int offset = absLine - int(m_scrollbackBuffer.size());
    if (offset>=0 && offset<currentBuffer().rows()) {
        return &currentBuffer().cell(offset,0);
    }
    return nullptr;
}

void TerminalWidget::handleSpecialKey(int key)
{
    if (m_ptyMaster<0) return;
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        ::write(m_ptyMaster, "\r",1);
        break;
    case Qt::Key_Backspace:
        ::write(m_ptyMaster, "\x7F",1);
        break;
    case Qt::Key_Tab:
        ::write(m_ptyMaster, "\t",1);
        break;
    default:
        break;
    }
}

void TerminalWidget::copyToClipboard()
{
    if (!hasSelection()) return;
    QClipboard *cb = QGuiApplication::clipboard();
    cb->setText(selectedText(), QClipboard::Clipboard);
}

void TerminalWidget::pasteFromClipboard()
{
    QClipboard *cb = QGuiApplication::clipboard();
    QString text = cb->text(QClipboard::Clipboard);
    if (!text.isEmpty() && m_ptyMaster>=0) {
        QByteArray ba = text.toUtf8();
        ::write(m_ptyMaster, ba.constData(), ba.size());
    }
}

Cell TerminalWidget::makeCellForCurrentAttr() const
{
    Cell blank;
    blank.ch = QChar(' ');
    blank.fg = m_currentFg;
    blank.bg = m_currentBg;
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

void TerminalWidget::handleIfMouseEnabled(QMouseEvent *event, std::function<void()> fn)
{
    if (!m_mouseEnabled) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    fn();
}
