#ifndef TERMINALWIDGET_H
#define TERMINALWIDGET_H

#include <QAbstractScrollArea>
#include <QTimer>
#include <QProcess> // optional, if you want a QProcess approach
#include <deque>
#include <vector>
#include <memory>
#include <functional>

enum class TextStyle : unsigned char
{
    None      = 0,
    Bold      = 1 << 0,
    Underline = 1 << 1,
    Inverse   = 1 << 2,
    Blink     = 1 << 3  // add a “Blink” bit
};

inline TextStyle operator|(TextStyle lhs, TextStyle rhs)
{
    return static_cast<TextStyle>(
        static_cast<unsigned char>(lhs) | static_cast<unsigned char>(rhs)
    );
}
inline TextStyle& operator|=(TextStyle &lhs, TextStyle rhs)
{
    lhs = lhs | rhs;
    return lhs;
}

struct Cell
{
    QChar ch;
    int   fg;     // ANSI color index
    int   bg;     // ANSI color index
    unsigned char style; // combination of TextStyle bits
};

class ScreenBuffer
{
public:
    ScreenBuffer(int rows, int cols)
        : m_rows(rows), m_cols(cols)
    {
        m_cells.resize(size_t(m_rows * m_cols));
    }

    void resize(int rows, int cols)
    {
        m_rows = rows;
        m_cols = cols;
        m_cells.resize(size_t(m_rows * m_cols));
    }

    int rows() const { return m_rows; }
    int cols() const { return m_cols; }

    Cell& cell(int row, int col)
    {
        return m_cells[size_t(row)*size_t(m_cols) + size_t(col)];
    }
    const Cell& cell(int row, int col) const
    {
        return m_cells[size_t(row)*size_t(m_cols) + size_t(col)];
    }

    // Fill a row’s cells from [startCol..endCol)
    void fillRow(int row, int startCol, int endCol, const Cell &fillCell)
    {
        if (startCol < 0) startCol = 0;
        if (endCol > m_cols) endCol = m_cols;
        if (row < 0 || row >= m_rows) return;

        for (int col = startCol; col < endCol; ++col) {
            cell(row,col) = fillCell;
        }
    }

private:
    int m_rows;
    int m_cols;
    std::vector<Cell> m_cells;
};

//--------------------------------------
// TerminalWidget
//--------------------------------------
class TerminalWidget : public QAbstractScrollArea
{
    Q_OBJECT
public:
    explicit TerminalWidget(QWidget *parent=nullptr);
    ~TerminalWidget() override;

    QSize sizeHint() const override;

    void setPtyInfo(int ptyMaster, pid_t shellPid);

    // optional QProcess approach (uncomment if you want to try QProcess)
    // void setShellProcess(QProcess *proc);

    /// Called by your code whenever there's new data from the shell or pty
    void processIncomingData(const QByteArray &data);

    // Standard terminal controls:
    void setMouseEnabled(bool on);
    void updateScreen();
    void useAlternateScreen(bool alt);
    void setScrollingRegion(int top, int bottom);
    void lineFeed();
    void reverseLineFeed();
    void putChar(QChar ch);

    void setCursorPos(int row, int col, bool doClamp=true);
    void saveCursorPos();
    void restoreCursorPos();
    void eraseInLine(int mode);
    void eraseInDisplay(int mode);
    void setSGR(const std::vector<int> &params);

    void scrollUp(int top, int bottom);
    void scrollDown(int top, int bottom);
    void clampCursor();
    void clampLineCol(int &line, int &col);

    // Returns the current active ScreenBuffer (either main or alternate).
    ScreenBuffer &currentBuffer();
    const ScreenBuffer &currentBuffer() const;

    // Fill an entire ScreenBuffer with the given Cell
    void fillScreen(ScreenBuffer &buf, const Cell &blank);

    // Called after resizing (or from anywhere) to adjust terminal row/col
    void setTerminalSize(int rows, int cols);

    // Helper to map an ANSI color index to a QColor
    QColor ansiIndexToColor(int idx, bool bold);

    // Text selection
    void selectWordAtPosition(int absLine, int col);
    void clearSelection();
    bool hasSelection() const;
    QString selectedText() const;
    const Cell* getCellsAtAbsoluteLine(int absLine) const;

    // Key/mouse
    void handleSpecialKey(int key);
    void copyToClipboard();
    void pasteFromClipboard();

    // Expose cursor row/col
    int  getCursorRow() const { return m_cursorRow; }
    int  getCursorCol() const { return m_cursorCol; }
    void setCursorRow(int row) {
        m_cursorRow = row;
        clampCursor();
    }
    void setCursorCol(int col) {
        m_cursorCol = col;
        clampCursor();
    }

    // Expose setting current color/style
    void setCurrentFg(int fg)             { m_currentFg    = fg; }
    void setCurrentBg(int bg)             { m_currentBg    = bg; }
    void setCurrentStyle(unsigned char s) { m_currentStyle = s; }

    // Provide public getters for main/alternate screens so
    // EscapeSequenceParser can call them.
    ScreenBuffer& getMainScreen()             { return *m_mainScreen; }
    const ScreenBuffer& getMainScreen() const { return *m_mainScreen; }

    ScreenBuffer& getAlternateScreen()             { return *m_alternateScreen; }
    const ScreenBuffer& getAlternateScreen() const { return *m_alternateScreen; }

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void drawCell(QPainter &p, int screenRow, int col, const Cell &cell);
    void drawCursor(QPainter &painter, int firstLine, int visibleLines);

    void handleIfMouseEnabled(QMouseEvent *event, std::function<void()> fn);

    Cell makeCellForCurrentAttr() const;
    QByteArray keyEventToAnsiSequence(QKeyEvent *event);

private slots:
    void blinkEvent(); // For blinking text

private:
    // Buffers
    std::unique_ptr<ScreenBuffer> m_mainScreen;
    std::unique_ptr<ScreenBuffer> m_alternateScreen;
    std::deque< std::vector<Cell> > m_scrollbackBuffer;
    int m_scrollbackMax;

    // Flags
    bool m_inAlternateScreen;
    bool m_showCursor;
    bool m_mouseEnabled;
    bool m_selecting;
    bool m_hasSelection;
    bool m_isClosed;

    // Cursor positions
    int  m_cursorRow;
    int  m_cursorCol;
    int  m_savedCursorRow;
    int  m_savedCursorCol;

    // Text attributes (current)
    int  m_currentFg;
    int  m_currentBg;
    unsigned char m_currentStyle;

    // Scroll region
    int  m_scrollRegionTop;
    int  m_scrollRegionBottom;

    // PTY master FD
    int  m_ptyMaster;
    pid_t m_shellPid;

    // If you want a QProcess approach:
    // QProcess *m_shellProcess {nullptr};

    // Sizing
    int  m_charWidth;
    int  m_charHeight;

    // Selection
    int  m_selAnchorAbsLine;
    int  m_selAnchorCol;
    int  m_selActiveAbsLine;
    int  m_selActiveCol;

    // Minimal parse state
    bool m_escape;
    bool m_bracket;
    QString m_escParams;

    // Blink
    bool m_textBlinkState;
    QTimer m_blinkTimer;

    // Internal methods for parse:
    void processOneChar(char c);
    void doClearScreen();
};

#endif // TERMINALWIDGET_H
