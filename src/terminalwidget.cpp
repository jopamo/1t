#include "terminalwidget.h"
#include "debug.h"

#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QResizeEvent>
#include <QClipboard>
#include <QGuiApplication>
#include <QFont>
#include <QFontDatabase>
#include <QDebug>
#include <QApplication>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <memory>
#include <pty.h>
#include <unistd.h>
#include <sys/ioctl.h>

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
      m_mouseEnabled(true),
      m_selecting(false),
      m_hasSelection(false),
      m_ptyMaster(-1),
      m_shellPid(-1) {
    QFont mainFont(QStringLiteral("Source Code Pro"), 10);
    mainFont.setStyleHint(QFont::Monospace, QFont::PreferDefault);
    QFont::insertSubstitution(QStringLiteral("Source Code Pro"), QStringLiteral("Noto Color Emoji"));
    setFont(mainFont);

    m_charWidth = fontMetrics().horizontalAdvance(QChar('M'));
    m_charHeight = fontMetrics().height();

    m_mainScreen = std::make_unique<ScreenBuffer>(24, 80);
    m_alternateScreen = std::make_unique<ScreenBuffer>(24, 80);

    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

#ifdef ENABLE_DEBUG
    DBG() << "TerminalWidget created with rows=24 cols=80"
          << "charWidth=" << m_charWidth << "charHeight=" << m_charHeight;
#endif
}

TerminalWidget::~TerminalWidget() {
#ifdef ENABLE_DEBUG
    DBG() << "TerminalWidget destroyed";
#endif
}

QSize TerminalWidget::sizeHint() const {
    int w = m_mainScreen->cols() * m_charWidth + verticalScrollBar()->sizeHint().width();
    int h = m_mainScreen->rows() * m_charHeight;
#ifdef ENABLE_DEBUG
    DBG() << "sizeHint w=" << w << "h=" << h;
#endif
    return QSize(w, h);
}

void TerminalWidget::resizeEvent(QResizeEvent* event) {
    int newCols = std::max(1, width() / m_charWidth);
    int newRows = std::max(1, height() / m_charHeight);
    if (newCols != m_mainScreen->cols() || newRows != m_mainScreen->rows()) {
#ifdef ENABLE_DEBUG
        DBG() << "resizeEvent newRows=" << newRows << "newCols=" << newCols;
#endif
        setTerminalSize(newRows, newCols);
    }
    QAbstractScrollArea::resizeEvent(event);
}

void TerminalWidget::paintEvent(QPaintEvent* ev) {
    QPainter p(viewport());
    const QRegion clip = ev->region();
    p.setClipRegion(clip);
    p.fillRect(clip.boundingRect(), Qt::black);

    const int firstVisible = verticalScrollBar()->value();
    const int rowsOnScreen = height() / m_charHeight;
    const int cols = currentBuffer().cols();
    const int totalLines = int(m_scrollbackBuffer.size()) + currentBuffer().rows();

    if (totalLines == 0 || rowsOnScreen == 0 || cols == 0)
        return;

    const int lastVisible = std::min(firstVisible + rowsOnScreen, totalLines);

    for (int absLine = firstVisible; absLine < lastVisible; ++absLine) {
        const int canvasRow = absLine - firstVisible;
        const int y = canvasRow * m_charHeight;

        if (!clip.intersects(QRect(0, y, viewport()->width(), m_charHeight)))
            continue;

        const Cell* cells = getCellsAtAbsoluteLine(absLine);
        if (!cells)
            continue;

        for (int col = 0, x = 0; col < cols; ++col, x += m_charWidth) {
            drawCell(p, canvasRow, col, cells[col]);
        }

        if (m_hasSelection && absLine >= std::min(m_selAnchorAbsLine, m_selActiveAbsLine) &&
            absLine <= std::max(m_selAnchorAbsLine, m_selActiveAbsLine)) {
            int selStart = 0, selEnd = cols - 1;
            if (absLine == m_selAnchorAbsLine) {
                selStart = (m_selAnchorAbsLine < m_selActiveAbsLine) ? m_selAnchorCol : m_selActiveCol;
            }
            if (absLine == m_selActiveAbsLine) {
                selEnd = (m_selAnchorAbsLine > m_selActiveAbsLine) ? m_selAnchorCol : m_selActiveCol;
            }
            if (selStart > selEnd)
                std::swap(selStart, selEnd);

            p.fillRect(selStart * m_charWidth, y, (selEnd - selStart + 1) * m_charWidth, m_charHeight,
                       QColor(128, 128, 255, 128));
        }
    }

    if (m_showCursor)
        drawCursor(p, firstVisible, rowsOnScreen);
}

void TerminalWidget::keyPressEvent(QKeyEvent* event) {
#ifdef ENABLE_DEBUG
    DBG() << "keyPressEvent key=" << event->key() << "modifiers=" << event->modifiers();
#endif
    auto mods = event->modifiers();
    bool isPageUp = (event->key() == Qt::Key_PageUp);
    bool isPageDown = (event->key() == Qt::Key_PageDown);

    if (isPageUp || isPageDown) {
        if (mods & Qt::ShiftModifier) {
            int linesPerPage = viewport()->height() / m_charHeight;
            int dir = isPageUp ? -1 : +1;
            verticalScrollBar()->setValue(verticalScrollBar()->value() + dir * linesPerPage);
        }
        else {
            QByteArray seq = isPageUp ? "\x1B[5~" : "\x1B[6~";
            safeWriteToPty(seq);
        }
        return;
    }

    if ((mods & Qt::ControlModifier) && (mods & Qt::ShiftModifier)) {
        if (event->key() == Qt::Key_C) {
            copyToClipboard();
            return;
        }
        if (event->key() == Qt::Key_V) {
            pasteFromClipboard();
            return;
        }
    }

    if (QByteArray seq = keyEventToAnsiSequence(event); !seq.isEmpty()) {
        safeWriteToPty(seq);
        return;
    }

    if (QString txt = event->text(); !txt.isEmpty()) {
        safeWriteToPty(txt.toUtf8());
        return;
    }

    handleSpecialKey(event->key());
}

void TerminalWidget::setPtyInfo(int ptyMaster, pid_t shellPid) {
#ifdef ENABLE_DEBUG
    DBG() << "setPtyInfo ptyMaster=" << ptyMaster << "shellPid=" << shellPid;
#endif
    m_ptyMaster = ptyMaster;
    m_shellPid = shellPid;
}

void TerminalWidget::setMouseEnabled(bool on) {
#ifdef ENABLE_DEBUG
    DBG() << "setMouseEnabled" << on;
#endif
    m_mouseEnabled = on;
}

void TerminalWidget::updateScreen() {
#ifdef ENABLE_DEBUG
    DBG() << "updateScreen";
#endif
    viewport()->update();
}

void TerminalWidget::useAlternateScreen(bool alt) {
#ifdef ENABLE_DEBUG
    DBG() << "useAlternateScreen alt=" << alt;
#endif
    if (m_inAlternateScreen == alt)
        return;

    if (alt) {
        m_alternateScreen->resize(m_mainScreen->rows(), m_mainScreen->cols());
        fillScreen(*m_alternateScreen, makeCellForCurrentAttr());
    }
    m_inAlternateScreen = alt;
    viewport()->update();
}

void TerminalWidget::setScrollingRegion(int top, int bottom) {
#ifdef ENABLE_DEBUG
    DBG() << "setScrollingRegion top=" << top << "bottom=" << bottom;
#endif
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
#ifdef ENABLE_DEBUG
    DBG() << "lineFeed at row=" << m_cursorRow;
#endif

    m_cursorRow++;

    if (m_cursorRow > m_scrollRegionBottom) {
        scrollUp(m_scrollRegionTop, m_scrollRegionBottom);

        m_cursorRow = m_scrollRegionBottom;
    }

    clampCursor();

#ifdef ENABLE_DEBUG
    DBG() << "Updated cursor position: row=" << m_cursorRow;
#endif
}

void TerminalWidget::reverseLineFeed() {
#ifdef ENABLE_DEBUG
    DBG() << "reverseLineFeed at row=" << m_cursorRow;
#endif

    if (m_cursorRow == m_scrollRegionTop) {
        scrollDown(m_scrollRegionTop, m_scrollRegionBottom);
    }
    else {
        m_cursorRow = std::max(m_cursorRow - 1, 0);
    }

    clampCursor();

#ifdef ENABLE_DEBUG
    DBG() << "Updated cursor position: row=" << m_cursorRow;
#endif
}

void TerminalWidget::putChar(QChar ch) {
#ifdef ENABLE_DEBUG
    DBG() << "putChar: " << ch;
#endif

    if (ch == u'\r') {
#ifdef ENABLE_DEBUG
        DBG() << "Carriage return encountered. Resetting column to 0.";
#endif
        m_cursorCol = 0;
        clampCursor();
        return;
    }

    if (ch == u'\n') {
#ifdef ENABLE_DEBUG
        DBG() << "Newline encountered. Moving cursor to the next line.";
#endif
        lineFeed();
        m_cursorCol = 0;
        clampCursor();
        return;
    }

    if (!ch.isPrint() || ch == u'\x7F' || ch.isHighSurrogate() || ch.isLowSurrogate()) {
#ifdef ENABLE_DEBUG
        DBG() << "Non-printable character skipped: " << ch;
#endif
        return;
    }

    if (m_cursorCol >= currentBuffer().cols()) {
#ifdef ENABLE_DEBUG
        DBG() << "Column limit reached. Wrapping text to the next line.";
#endif
        lineFeed();
        m_cursorCol = 0;
    }

    Cell& cell = currentBuffer().cell(m_cursorRow, m_cursorCol);
    cell.ch = ch;

#ifdef ENABLE_DEBUG
    DBG() << "Cell updated at row=" << m_cursorRow << " col=" << m_cursorCol << " with char=" << ch;
#endif
    cell.fg = m_currentFg;
    cell.bg = m_currentBg;
    cell.style = m_currentStyle;

    invalidateCell(m_cursorRow, m_cursorCol);

    ++m_cursorCol;
}

inline void TerminalWidget::invalidateCell(int row, int col) {
    if (row < 0 || col < 0)
        return;

    int absLine = int(m_scrollbackBuffer.size()) + row;
    int firstVisibleLine = verticalScrollBar()->value();
    int canvasRow = absLine - firstVisibleLine;

    if (canvasRow < 0)
        return;

    int y = canvasRow * m_charHeight;
    int x = col * m_charWidth;
    viewport()->update(x, y, m_charWidth, m_charHeight);
}

void TerminalWidget::setCursorPos(int r, int c, bool doClamp) {
#ifdef ENABLE_DEBUG
    DBG() << "setCursorPos r=" << r << ", c=" << c << ", clamp=" << doClamp;
#endif

    if (doClamp) {
        r = std::clamp(r, 0, currentBuffer().rows() - 1);
        c = std::clamp(c, 0, currentBuffer().cols() - 1);
    }

    if (r == m_cursorRow && c == m_cursorCol)
        return;

    invalidateCell(m_cursorRow, m_cursorCol);
    m_prevCursorRow = m_cursorRow;
    m_prevCursorCol = m_cursorCol;
    m_cursorRow = r;
    m_cursorCol = c;
    invalidateCell(m_cursorRow, m_cursorCol);
}

void TerminalWidget::saveCursorPos() {
#ifdef ENABLE_DEBUG
    DBG() << "saveCursorPos row=" << m_cursorRow << ", col=" << m_cursorCol;
#endif
    m_savedCursorRow = m_cursorRow;
    m_savedCursorCol = m_cursorCol;
}

void TerminalWidget::restoreCursorPos() {
#ifdef ENABLE_DEBUG
    DBG() << "restoreCursorPos to row=" << m_savedCursorRow << ", col=" << m_savedCursorCol;
#endif
    m_cursorRow = m_savedCursorRow;
    m_cursorCol = m_savedCursorCol;
    clampCursor();
}

void TerminalWidget::clampCursor() {
#ifdef ENABLE_DEBUG
    DBG() << "Clamping cursor: row=" << m_cursorRow << ", col=" << m_cursorCol;
#endif

    m_cursorRow = std::clamp(m_cursorRow, 0, currentBuffer().rows() - 1);
    m_cursorCol = std::clamp(m_cursorCol, 0, currentBuffer().cols() - 1);

#ifdef ENABLE_DEBUG
    DBG() << "Clamped cursor: row=" << m_cursorRow << ", col=" << m_cursorCol;
#endif
}

void TerminalWidget::clampLineCol(int& line, int& col) {
    int maxAbsLine = int(m_scrollbackBuffer.size()) + currentBuffer().rows() - 1;
    line = std::clamp(line, 0, maxAbsLine);
    col = std::clamp(col, 0, currentBuffer().cols() - 1);
}

void TerminalWidget::deleteChars(int n) {
#ifdef ENABLE_DEBUG
    DBG() << "deleteChars n=" << n << "row=" << m_cursorRow << "col=" << m_cursorCol;
#endif

    int row = m_cursorRow;
    if (row < 0 || row >= currentBuffer().rows() || n < 1)
        return;

    for (int count = 0; count < n; ++count) {
        for (int c = m_cursorCol; c < currentBuffer().cols() - 1; ++c) {
            currentBuffer().cell(row, c) = currentBuffer().cell(row, c + 1);
        }
        currentBuffer().cell(row, currentBuffer().cols() - 1) = makeCellForCurrentAttr();
    }

    viewport()->update();
}

void TerminalWidget::eraseChars(int n) {
#ifdef ENABLE_DEBUG
    DBG() << "eraseChars n=" << n << "row=" << m_cursorRow << "col=" << m_cursorCol;
#endif

    int row = m_cursorRow;
    if (row < 0 || row >= currentBuffer().rows() || n < 1)
        return;

    for (int i = 0; i < n; ++i) {
        int c = m_cursorCol + i;
        if (c >= currentBuffer().cols())
            break;
        currentBuffer().cell(row, c) = makeCellForCurrentAttr();
    }

    viewport()->update();
}

void TerminalWidget::insertChars(int n) {
#ifdef ENABLE_DEBUG
    DBG() << "insertChars n=" << n << "row=" << m_cursorRow << "col=" << m_cursorCol;
#endif

    int row = m_cursorRow;
    if (row < 0 || row >= currentBuffer().rows() || n < 1)
        return;

    for (int count = 0; count < n; ++count) {
        for (int c = currentBuffer().cols() - 1; c > m_cursorCol; --c) {
            currentBuffer().cell(row, c) = currentBuffer().cell(row, c - 1);
        }
        currentBuffer().cell(row, m_cursorCol) = makeCellForCurrentAttr();
    }

    viewport()->update();
}

void TerminalWidget::drawCursor(QPainter& p, int firstVisibleLine, int visibleRows) {
    int cursorAbs = int(m_scrollbackBuffer.size()) + m_cursorRow;
    if (cursorAbs < firstVisibleLine || cursorAbs >= firstVisibleLine + visibleRows)
        return;

    int canvasRow = cursorAbs - firstVisibleLine;
    int y = canvasRow * m_charHeight;
    int x = m_cursorCol * m_charWidth;
    QRect cellRect(x, y, m_charWidth, m_charHeight);

    const Cell& cell = currentBuffer().cell(m_cursorRow, m_cursorCol);
    QColor fg = ansiIndexToColor(cell.bg, false);
    QColor bg = ansiIndexToColor(cell.fg, false);

    p.fillRect(cellRect, bg);
    if (cell.ch.isPrint()) {
        p.setPen(fg);
        int baseline = y + fontMetrics().ascent();
        p.drawText(x, baseline, QString(cell.ch));
    }
}

void TerminalWidget::eraseInLine(int mode) {
#ifdef ENABLE_DEBUG
    DBG() << "eraseInLine mode=" << mode << "cursorRow=" << m_cursorRow;
#endif

    int row = m_cursorRow;
    if (row < 0 || row >= currentBuffer().rows())
        return;

    int startCol, endCol;
    switch (mode) {
        case 0:
            startCol = m_cursorCol;
            endCol = currentBuffer().cols();
            break;
        case 1:
            startCol = 0;
            endCol = m_cursorCol + 1;
            break;
        case 2:
            startCol = 0;
            endCol = currentBuffer().cols();
            break;
        default:
            startCol = 0;
            endCol = currentBuffer().cols();
            break;
    }

    Cell blank = makeCellForCurrentAttr();
    for (int c = startCol; c < endCol; ++c) {
        Cell& cell = currentBuffer().cell(row, c);
        cell.ch = QChar(' ');
        cell.fg = blank.fg;
        cell.bg = blank.bg;
        cell.style = blank.style;
    }

    viewport()->update();
}

void TerminalWidget::eraseInDisplay(int mode) {
#ifdef ENABLE_DEBUG
    DBG() << "eraseInDisplay mode=" << mode << "cursorRow=" << m_cursorRow;
#endif

    Cell blank = makeCellForCurrentAttr();

    if (mode == 2) {
        fillScreen(currentBuffer(), blank);
        viewport()->update();
        return;
    }

    if (mode == 0) {
        eraseInLine(0);
        for (int r = m_cursorRow + 1; r < currentBuffer().rows(); ++r) {
            currentBuffer().fillRow(r, 0, currentBuffer().cols(), blank);
        }
        viewport()->update();
    }
    else if (mode == 1) {
        eraseInLine(1);
        for (int r = 0; r < m_cursorRow; ++r) {
            currentBuffer().fillRow(r, 0, currentBuffer().cols(), blank);
        }
        viewport()->update();
    }
}

inline bool TerminalWidget::isViewPinnedBottom() const noexcept {
    const QScrollBar* sb = verticalScrollBar();
    return sb->value() >= sb->maximum();
}

inline void TerminalWidget::maybeAdjustScrollBar(int deltaLines) {
    QScrollBar* sb = verticalScrollBar();
    const bool pinned = isViewPinnedBottom();
    sb->setRange(0, int(m_scrollbackBuffer.size()));
    sb->setPageStep(currentBuffer().rows());

    if (pinned) {
        sb->setValue(sb->maximum());
    }
    else {
        sb->setValue(sb->value() + deltaLines);
    }
}

void TerminalWidget::scrollUp(int top, int bottom) {
#ifdef ENABLE_DEBUG
    DBG() << "scrollUp top=" << top << "bottom=" << bottom;
#endif

    const int cols = currentBuffer().cols();
    const int regionHeight = bottom - top + 1;
    if (regionHeight <= 0)
        return;

    std::vector<Cell> firstRow(cols);
    std::copy_n(&currentBuffer().cell(top, 0), cols, firstRow.begin());

    if (regionHeight > 1) {
        Cell* base = &currentBuffer().cell(top, 0);
        std::memmove(base, base + cols, size_t(regionHeight - 1) * size_t(cols) * sizeof(Cell));
    }

    currentBuffer().fillRow(bottom, 0, cols, makeCellForCurrentAttr());

    if (int(m_scrollbackBuffer.size()) == m_scrollbackMax)
        m_scrollbackBuffer.pop_front();
    m_scrollbackBuffer.push_back(std::move(firstRow));

    maybeAdjustScrollBar(+1);

    const int yTop = top * m_charHeight;
    const int yExposed = bottom * m_charHeight;
    viewport()->scroll(0, -m_charHeight, QRect(0, yTop, viewport()->width(), regionHeight * m_charHeight));
    viewport()->update(0, yExposed, viewport()->width(), m_charHeight);
}

void TerminalWidget::scrollDown(int top, int bottom) {
#ifdef ENABLE_DEBUG
    DBG() << "scrollDown top=" << top << "bottom=" << bottom;
#endif

    const int cols = currentBuffer().cols();
    const int regionHeight = bottom - top + 1;
    if (regionHeight <= 0)
        return;

    if (regionHeight > 1) {
        Cell* base = &currentBuffer().cell(top, 0);
        std::memmove(base + cols, base, size_t(regionHeight - 1) * size_t(cols) * sizeof(Cell));
    }

    currentBuffer().fillRow(top, 0, cols, makeCellForCurrentAttr());

    maybeAdjustScrollBar(-1);

    const int yTop = top * m_charHeight;
    viewport()->scroll(0, +m_charHeight, QRect(0, yTop, viewport()->width(), regionHeight * m_charHeight));
    viewport()->update(0, yTop, viewport()->width(), m_charHeight);
}

ScreenBuffer& TerminalWidget::currentBuffer() {
    return m_inAlternateScreen ? *m_alternateScreen : *m_mainScreen;
}

const ScreenBuffer& TerminalWidget::currentBuffer() const {
    return m_inAlternateScreen ? *m_alternateScreen : *m_mainScreen;
}

void TerminalWidget::fillScreen(ScreenBuffer& buf, const Cell& blank) {
#ifdef ENABLE_DEBUG
    DBG() << "fillScreen rows=" << buf.rows() << "cols=" << buf.cols();
#endif
    for (int r = 0; r < buf.rows(); ++r) {
        buf.fillRow(r, 0, buf.cols(), blank);
    }
}

void TerminalWidget::setTerminalSize(int rows, int cols) {
#ifdef ENABLE_DEBUG
    DBG() << "setTerminalSize rows=" << rows << "cols=" << cols;
#endif
    if (m_mainScreen->rows() == rows && m_mainScreen->cols() == cols)
        return;

    ScreenBuffer oldMain = *m_mainScreen;
    ScreenBuffer oldAlternate = *m_alternateScreen;

    m_mainScreen->resize(rows, cols);
    m_alternateScreen->resize(rows, cols);

    int copyRows = std::min(rows, oldMain.rows());
    int copyCols = std::min(cols, oldMain.cols());
    for (int r = 0; r < copyRows; ++r) {
        for (int c = 0; c < copyCols; ++c) {
            m_mainScreen->cell(r, c) = oldMain.cell(r, c);
        }
    }
    Cell blankCell = makeCellForCurrentAttr();
    for (int r = copyRows; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            m_mainScreen->cell(r, c) = blankCell;
        }
    }

    m_scrollRegionTop = 0;
    m_scrollRegionBottom = rows - 1;

    int maxScroll = int(m_scrollbackBuffer.size());
    verticalScrollBar()->setRange(0, maxScroll);
    verticalScrollBar()->setPageStep(rows);

    if (m_ptyMaster >= 0) {
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ws.ws_row = rows;
        ws.ws_col = cols;
        ioctl(m_ptyMaster, TIOCSWINSZ, &ws);
    }
    viewport()->update();
}

QColor TerminalWidget::ansiIndexToColor(int idx, bool bold) {
    if (idx < 16) {
        static const QColor basicTable[16] = {QColor(Qt::black),
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
                                              QColor(Qt::white)};
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
            int b = (offset % 6);
            auto rgb = [&](int v) { return (v == 0) ? 0 : 55 + v * 40; };
            return QColor(rgb(r), rgb(g), rgb(b));
        }
        else {
            int level = idx - 232;
            int gray = 8 + level * 10;
            return QColor(gray, gray, gray);
        }
    }
    return (idx < 0) ? QColor(Qt::black) : QColor(Qt::white);
}

inline void TerminalWidget::drawCell(QPainter& p, int canvasRow, int col, const Cell& cell) {
    if (cell.ch.isNull() || !cell.ch.isPrint() || cell.ch == ' ') {
        return;
    }

#ifdef ENABLE_DEBUG
    DBG() << "Rendering character: " << cell.ch;
#endif

    int x = col * m_charWidth;
    int y = canvasRow * m_charHeight;

    bool isBold = (cell.style & (unsigned char)TextStyle::Bold);
    bool isUnderline = (cell.style & (unsigned char)TextStyle::Underline);
    bool isInverse = (cell.style & (unsigned char)TextStyle::Inverse);

    QColor fg = ansiIndexToColor(cell.fg, isBold);
    QColor bg = ansiIndexToColor(cell.bg, false);

    if (isInverse)
        std::swap(fg, bg);

    p.fillRect(x, y, m_charWidth, m_charHeight, bg);
    p.setPen(fg);

    int baseline = y + fontMetrics().ascent();
    p.drawText(x, baseline, QString(cell.ch));

    if (isUnderline) {
        int underlineY = y + fontMetrics().underlinePos();
        p.drawLine(x, underlineY, x + m_charWidth, underlineY);
    }
}

void TerminalWidget::selectWordAtPosition(int row, int col) {
#ifdef ENABLE_DEBUG
    DBG() << "selectWordAtPosition row=" << row << " col=" << col;
#endif
    const Cell* cells = getCellsAtAbsoluteLine(row);
    if (!cells) {
#ifdef ENABLE_DEBUG
        DBG() << "No cells found at row=" << row;
#endif
        return;
    }

    int startCol = col;
    while (startCol > 0 && !cells[startCol - 1].ch.isSpace())
        startCol--;
    int endCol = col;
    while (endCol < currentBuffer().cols() - 1 && !cells[endCol + 1].ch.isSpace())
        endCol++;

#ifdef ENABLE_DEBUG
    DBG() << "Word selection from col=" << startCol << " to col=" << endCol;
#endif

    m_selAnchorAbsLine = row;
    m_selAnchorCol = startCol;
    m_selActiveAbsLine = row;
    m_selActiveCol = endCol;
    m_hasSelection = true;

#ifdef ENABLE_DEBUG
    DBG() << "Selection anchor set to row=" << m_selAnchorAbsLine << " col=" << m_selAnchorCol;
#endif
    viewport()->update();
}

void TerminalWidget::clearSelection() {
#ifdef ENABLE_DEBUG
    DBG() << "clearSelection called.";
#endif
    m_hasSelection = false;
    viewport()->update();
}

bool TerminalWidget::hasSelection() const {
#ifdef ENABLE_DEBUG
    DBG() << "hasSelection called.";
#endif
    if (!m_hasSelection)
        return false;

    if (m_selAnchorAbsLine == m_selActiveAbsLine && m_selAnchorCol == m_selActiveCol) {
#ifdef ENABLE_DEBUG
        DBG() << "Selection is degenerate (same anchor and active points), returning false.";
#endif
        return false;
    }
    return true;
}

QString TerminalWidget::selectedText() const {
#ifdef ENABLE_DEBUG
    DBG() << "selectedText called.";
#endif

    if (!hasSelection()) {
#ifdef ENABLE_DEBUG
        DBG() << "No selection found.";
#endif
        return QString();
    }

    int startLine = std::min(m_selAnchorAbsLine, m_selActiveAbsLine);
    int endLine = std::max(m_selAnchorAbsLine, m_selActiveAbsLine);

    QStringList lines;
    lines.reserve(endLine - startLine + 1);

#ifdef ENABLE_DEBUG
    DBG() << "Extracting selected text from lines " << startLine << " to " << endLine;
#endif

    for (int absLine = startLine; absLine <= endLine; ++absLine) {
        const Cell* rowCells = getCellsAtAbsoluteLine(absLine);
        if (!rowCells) {
#ifdef ENABLE_DEBUG
            DBG() << "No cells found for line " << absLine;
#endif
            continue;
        }

        int sc =
            (absLine == startLine) ? ((m_selAnchorAbsLine < m_selActiveAbsLine) ? m_selAnchorCol : m_selActiveCol) : 0;
        int ec = (absLine == endLine) ? ((m_selAnchorAbsLine > m_selActiveAbsLine) ? m_selAnchorCol : m_selActiveCol)
                                      : currentBuffer().cols() - 1;
        if (sc > ec)
            std::swap(sc, ec);

        sc = std::clamp(sc, 0, currentBuffer().cols() - 1);
        ec = std::clamp(ec, 0, currentBuffer().cols() - 1);

        QString lineText;
        lineText.reserve(ec - sc + 1);
        for (int c = sc; c <= ec; c++) {
            lineText.append(rowCells[c].ch);
        }
        lines << lineText;
    }
    return lines.join(u"\n"_qs);
}

bool TerminalWidget::isWithinLineSelection(int lineIndex, int col) const {
#ifdef ENABLE_DEBUG
    DBG() << "isWithinLineSelection called for line=" << lineIndex << " col=" << col;
#endif

    if (!m_hasSelection) {
#ifdef ENABLE_DEBUG
        DBG() << "No selection active.";
#endif
        return false;
    }

    int startLine = std::min(m_selAnchorAbsLine, m_selActiveAbsLine);
    int endLine = std::max(m_selAnchorAbsLine, m_selActiveAbsLine);
    if (lineIndex < startLine || lineIndex > endLine) {
#ifdef ENABLE_DEBUG
        DBG() << "Line " << lineIndex << " is outside selection range.";
#endif
        return false;
    }

    int lineStartCol =
        (lineIndex == startLine) ? ((m_selAnchorAbsLine < m_selActiveAbsLine) ? m_selAnchorCol : m_selActiveCol) : 0;
    int lineEndCol = (lineIndex == endLine)
                         ? ((m_selAnchorAbsLine > m_selActiveAbsLine) ? m_selAnchorCol : m_selActiveCol)
                         : currentBuffer().cols() - 1;

    if (lineStartCol > lineEndCol)
        std::swap(lineStartCol, lineEndCol);

#ifdef ENABLE_DEBUG
    DBG() << "Line selection range: startCol=" << lineStartCol << " endCol=" << lineEndCol;
#endif

    return (col >= lineStartCol && col <= lineEndCol);
}

const Cell* TerminalWidget::getCellsAtAbsoluteLine(int absLine) const {
#ifdef ENABLE_DEBUG
    DBG() << "getCellsAtAbsoluteLine called for line=" << absLine;
#endif

    if (absLine < int(m_scrollbackBuffer.size())) {
#ifdef ENABLE_DEBUG
        DBG() << "Fetching cells from scrollback buffer for line " << absLine;
#endif
        return m_scrollbackBuffer[size_t(absLine)].data();
    }
    int offset = absLine - int(m_scrollbackBuffer.size());
    if (offset >= 0 && offset < currentBuffer().rows()) {
#ifdef ENABLE_DEBUG
        DBG() << "Fetching cells from current screen buffer for line " << absLine;
#endif
        return &currentBuffer().cell(offset, 0);
    }
#ifdef ENABLE_DEBUG
    DBG() << "Line " << absLine << " is out of bounds.";
#endif
    return nullptr;
}

void TerminalWidget::handleSpecialKey(int key) {
#ifdef ENABLE_DEBUG
    DBG() << "handleSpecialKey key=" << key;
#endif
    if (m_ptyMaster < 0)
        return;

    switch (key) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
#ifdef ENABLE_DEBUG
            DBG() << "Enter/Return key pressed, sending CR to PTY.";
#endif
            safeWriteToPty("\r");
            break;
        case Qt::Key_Backspace:
#ifdef ENABLE_DEBUG
            DBG() << "Backspace key pressed, sending BS to PTY.";
#endif
            safeWriteToPty("\x7F");
            break;
        case Qt::Key_Tab:
#ifdef ENABLE_DEBUG
            DBG() << "Tab key pressed, sending Tab to PTY.";
#endif
            safeWriteToPty("\t");
            break;
        default:
#ifdef ENABLE_DEBUG
            DBG() << "Unrecognized special key pressed: " << key;
#endif
            break;
    }
}

void TerminalWidget::copyToClipboard() {
#ifdef ENABLE_DEBUG
    DBG() << "copyToClipboard called.";
#endif
    if (!hasSelection()) {
#ifdef ENABLE_DEBUG
        DBG() << "No selection to copy.";
#endif
        return;
    }
    QString sel = selectedText();
    QClipboard* cb = QGuiApplication::clipboard();
    cb->setText(sel, QClipboard::Clipboard);
#ifdef ENABLE_DEBUG
    DBG() << "Copied selected text to clipboard.";
#endif
}

void TerminalWidget::pasteFromClipboard() {
#ifdef ENABLE_DEBUG
    DBG() << "pasteFromClipboard called.";
#endif
    if (m_ptyMaster < 0)
        return;
    QClipboard* cb = QGuiApplication::clipboard();
    QString text = cb->text(QClipboard::Clipboard);
    if (!text.isEmpty()) {
#ifdef ENABLE_DEBUG
        DBG() << "Pasting text from clipboard: " << text;
#endif
        safeWriteToPty(text.toUtf8());
    }
    else {
#ifdef ENABLE_DEBUG
        DBG() << "Clipboard is empty.";
#endif
    }
}

Cell TerminalWidget::makeCellForCurrentAttr() const {
#ifdef ENABLE_DEBUG
    DBG() << "Creating cell with current attributes.";
#endif
    Cell blank;
    blank.ch = QChar(' ');
    blank.fg = m_currentFg;
    blank.bg = m_currentBg;
    blank.style = m_currentStyle;
    return blank;
}

QByteArray TerminalWidget::keyEventToAnsiSequence(QKeyEvent* event) {
#ifdef ENABLE_DEBUG
    DBG() << "keyEventToAnsiSequence called for key: " << event->key();
#endif
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
            break;
    }
    return {};
}

void TerminalWidget::handleIfMouseEnabled(QMouseEvent* event, std::function<void()> fn) {
    if (!m_mouseEnabled) {
        switch (event->type()) {
            case QEvent::MouseButtonPress:
                QAbstractScrollArea::mousePressEvent(event);
                break;
            case QEvent::MouseMove:
                QAbstractScrollArea::mouseMoveEvent(event);
                break;
            case QEvent::MouseButtonRelease:
                QAbstractScrollArea::mouseReleaseEvent(event);
                break;
            default:
                break;
        }
        return;
    }
    fn();
}

void TerminalWidget::safeWriteToPty(const QByteArray& data) {
    if (m_ptyMaster < 0 || data.isEmpty())
        return;
#ifdef ENABLE_DEBUG
    DBG() << "safeWriteToPty bytes=" << data.size();
#endif

    const char* buf = data.constData();
    ssize_t remain = data.size();
    while (remain > 0) {
        ssize_t n = ::write(m_ptyMaster, buf, size_t(remain));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            qWarning() << "Failed to write to PTY:" << strerror(errno);
            break;
        }
        buf += n;
        remain -= n;
    }
}

void TerminalWidget::fullReset() {
#ifdef ENABLE_DEBUG
    DBG() << "fullReset";
#endif
    m_scrollbackBuffer.clear();
    Cell blank = makeCellForCurrentAttr();
    fillScreen(*m_mainScreen, blank);
    fillScreen(*m_alternateScreen, blank);
    m_inAlternateScreen = false;
    m_cursorRow = 0;
    m_cursorCol = 0;
    m_currentFg = 7;
    m_currentBg = 0;
    m_currentStyle = 0;
    m_scrollRegionTop = 0;
    m_scrollRegionBottom = m_mainScreen->rows() - 1;
    update();
}

void TerminalWidget::handleBell() {
#ifdef ENABLE_DEBUG
    DBG() << "handleBell";
#endif
    QApplication::beep();
}

void TerminalWidget::setSGR(const std::vector<int>& params) {
#ifdef ENABLE_DEBUG
    DBG() << "setSGR params size=" << params.size();
#endif
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
            case 90:
            case 91:
            case 92:
            case 93:
            case 94:
            case 95:
            case 96:
            case 97:
                m_currentFg = (p - 90) + 8;
                break;
            case 100:
            case 101:
            case 102:
            case 103:
            case 104:
            case 105:
            case 106:
            case 107:
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
#ifdef ENABLE_DEBUG
                DBG() << "Unknown SGR code" << p;
#endif
                break;
        }
    }
}

void TerminalWidget::mousePressEvent(QMouseEvent* event) {
#ifdef ENABLE_DEBUG
    DBG() << "mousePressEvent pos=" << event->pos();
#endif
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
                m_selAnchorCol = col;
                m_selActiveAbsLine = row;
                m_selActiveCol = col;
                viewport()->update();
            }
        }
    });
}

void TerminalWidget::mouseMoveEvent(QMouseEvent* event) {
    handleIfMouseEnabled(event, [=]() {
        if (m_selecting && (event->buttons() & Qt::LeftButton)) {
            int row = (event->pos().y() / m_charHeight) + verticalScrollBar()->value();
            int col = (event->pos().x() / m_charWidth);
            clampLineCol(row, col);
            m_selActiveAbsLine = row;
            m_selActiveCol = col;
            viewport()->update();
        }
    });
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent* event) {
#ifdef ENABLE_DEBUG
    DBG() << "mouseReleaseEvent pos=" << event->pos();
#endif
    handleIfMouseEnabled(event, [=]() {
        if (event->button() == Qt::LeftButton) {
            m_selecting = false;
            m_hasSelection = true;
            viewport()->update();
        }
    });
}
