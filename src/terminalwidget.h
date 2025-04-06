#ifndef TERMINALWIDGET_H
#define TERMINALWIDGET_H

#include <QAbstractScrollArea>
#include <QColor>
#include <QChar>
#include <deque>
#include <vector>
#include <memory>
#include <functional>
#include <sys/types.h>

enum class TextStyle : unsigned char { None = 0, Bold = 1 << 0, Underline = 1 << 1, Inverse = 1 << 2 };

struct Cell {
    QChar ch = QChar(' ');
    int fg = 7;
    int bg = 0;
    unsigned char style = 0;
};

class ScreenBuffer {
   public:
    ScreenBuffer(int rows, int cols) : m_rows(rows), m_cols(cols), m_data(rows * cols) {}

    void resize(int rows, int cols) {
        if (rows < 1)
            rows = 1;
        if (cols < 1)
            cols = 1;
        m_rows = rows;
        m_cols = cols;
        m_data.assign(size_t(rows * cols), Cell());
    }

    int rows() const { return m_rows; }
    int cols() const { return m_cols; }

    Cell& cell(int row, int col) { return m_data[size_t(row * m_cols + col)]; }

    const Cell& cell(int row, int col) const { return m_data[size_t(row * m_cols + col)]; }

    void fillRow(int row, int colStart, int colEnd, const Cell& c) {
        if (row < 0 || row >= m_rows)
            return;
        if (colStart < 0)
            colStart = 0;
        if (colEnd > m_cols)
            colEnd = m_cols;
        for (int col = colStart; col < colEnd; ++col) {
            cell(row, col) = c;
        }
    }

   private:
    int m_rows;
    int m_cols;
    std::vector<Cell> m_data;
};

class TerminalWidget : public QAbstractScrollArea {
    Q_OBJECT

   public:
    explicit TerminalWidget(QWidget* parent = nullptr);

    QSize sizeHint() const override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

    void setPtyInfo(int ptyMaster, pid_t shellPid);
    int getPtyMaster() const { return m_ptyMaster; }

    void setMouseEnabled(bool on);

    void updateScreen();
    void useAlternateScreen(bool alt);
    void setScrollingRegion(int top, int bottom);
    void setTerminalSize(int rows, int cols);

    void lineFeed();
    void reverseLineFeed();
    void putChar(QChar ch);
    void setCursorPos(int row, int col, bool doClamp = true);
    void saveCursorPos();
    void restoreCursorPos();
    void eraseInLine(int mode);
    void eraseInDisplay(int mode);
    void setSGR(const std::vector<int>& params);

    void scrollUp(int top, int bottom);
    void scrollDown(int top, int bottom);

    void selectWordAtPosition(int row, int col);
    void clearSelection();
    bool hasSelection() const;
    QString selectedText() const;

    int getCursorRow() const { return m_cursorRow; }
    int getCursorCol() const { return m_cursorCol; }

    void setCursorRow(int row) {
        m_cursorRow = row;
        clampCursor();
    }
    void setCursorCol(int col) {
        m_cursorCol = col;
        clampCursor();
    }

    void setCurrentFg(int fg) { m_currentFg = fg; }
    void setCurrentBg(int bg) { m_currentBg = bg; }
    void setCurrentStyle(unsigned char s) { m_currentStyle = s; }

    void clampCursor();

    ScreenBuffer* getMainScreen() { return m_mainScreen.get(); }
    ScreenBuffer* getAlternateScreen() { return m_alternateScreen.get(); }

    void fillScreen(ScreenBuffer& buf, const Cell& blank);

   protected:
   private:
    const Cell* getCellsAtAbsoluteLine(int absLine) const;

    bool isWithinLineSelection(int lineIndex, int col) const;

    void drawCursor(QPainter& p, int firstVisibleLine, int visibleRows);

    void handleSpecialKey(int key);

    void copyToClipboard();

    void pasteFromClipboard();

    Cell makeCellForCurrentAttr() const;

    void handleIfMouseEnabled(QMouseEvent* event, std::function<void()> fn);
    void clampLineCol(int& line, int& col);

    ScreenBuffer& currentBuffer();
    const ScreenBuffer& currentBuffer() const;

    QByteArray keyEventToAnsiSequence(QKeyEvent* event);

    std::unique_ptr<ScreenBuffer> m_mainScreen;
    std::unique_ptr<ScreenBuffer> m_alternateScreen;
    bool m_inAlternateScreen;

    std::deque<std::vector<Cell>> m_scrollbackBuffer;
    int m_scrollbackMax;

    bool m_showCursor;
    int m_cursorRow;
    int m_cursorCol;
    int m_savedCursorRow;
    int m_savedCursorCol;

    int m_currentFg;
    int m_currentBg;
    unsigned char m_currentStyle;

    int m_scrollRegionTop;
    int m_scrollRegionBottom;

    int m_ptyMaster = -1;
    pid_t m_shellPid = -1;

    bool m_mouseEnabled;
    bool m_selecting;
    bool m_hasSelection;
    int m_selAnchorAbsLine;
    int m_selAnchorCol;
    int m_selActiveAbsLine;
    int m_selActiveCol;

    int m_charWidth;
    int m_charHeight;

    QColor ansiIndexToColor(int idx, bool bold);
    void drawCell(QPainter& painter, int canvasRow, int col, const Cell& cell);
};

#endif
