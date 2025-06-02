#ifndef TERMINALWIDGET_H
#define TERMINALWIDGET_H

#include <QAbstractScrollArea>
#include <QByteArray>
#include <QColor>
#include <QChar>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QString>

#include <cstdint>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <vector>
#include <sys/types.h>

enum class TextStyle : std::uint8_t { None = 0, Bold = 1 << 0, Underline = 1 << 1, Inverse = 1 << 2 };

class EscapeSequenceParser;

struct Cell {
    QChar ch{' '};
    int fg{7};
    int bg{0};
    std::uint8_t style{0};
};

class ScreenBuffer {
   public:
    ScreenBuffer(int rows, int cols);

    void resize(int rows, int cols);
    int rows() const noexcept { return m_rows; }
    int cols() const noexcept { return m_cols; }

    Cell& cell(int r, int c);
    const Cell& cell(int r, int c) const;

    void fillRow(int r, int c0, int c1, const Cell&);

   private:
    int m_rows;
    int m_cols;
    std::vector<Cell> m_data;
};

class TerminalWidget : public QAbstractScrollArea {
    Q_OBJECT
   public:
    explicit TerminalWidget(QWidget* parent = nullptr);
    ~TerminalWidget() override;

    QSize sizeHint() const override;
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

    int getPtyMaster() const noexcept { return m_ptyMaster; }
    int rows() const noexcept { return currentBuffer().rows(); }

    void setMouseEnabled(bool on);
    void updateScreen();
    void useAlternateScreen(bool alt);
    void setScrollingRegion(int top, int bottom);
    void setTerminalSize(int rows, int cols);

    void lineFeed();
    void reverseLineFeed();
    void putChar(QChar ch);
    void setCursorPos(int row, int col, bool clamp = true);
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

    void deleteChars(int n);
    void eraseChars(int n);
    void insertChars(int n);
    void deleteLines(int n);
    void insertLines(int n);
    void scrollUpLines(int n);
    void scrollDownLines(int n);

    int getCursorRow() const noexcept { return m_cursorRow; }
    int getCursorCol() const noexcept { return m_cursorCol; }
    void setCursorRow(int r) {
        m_cursorRow = r;
        clampCursor();
    }
    void setCursorCol(int c) {
        m_cursorCol = c;
        clampCursor();
    }

    void setCurrentFg(int fg) noexcept { m_currentFg = fg; }
    void setCurrentBg(int bg) noexcept { m_currentBg = bg; }
    void setCurrentStyle(std::uint8_t st) noexcept { m_currentStyle = st; }

    void fullReset();
    void handleBell();

    ScreenBuffer* getMainScreen() { return m_mainScreen.get(); }
    ScreenBuffer* getAlternateScreen() { return m_alternateScreen.get(); }

    void fillScreen(ScreenBuffer& buf, const Cell& blank);
    void setPtyInfo(int ptyMaster, pid_t shellPid);

    bool isViewPinnedBottom() const noexcept;
    void maybeAdjustScrollBar(int deltaLines);

   private:
    void clampCursor();
    const Cell* getCellsAtAbsoluteLine(int absLine) const;
    bool isWithinLineSelection(int line, int col) const;
    void drawCursor(QPainter& p, int firstVisibleLine, int visibleRows);
    void handleSpecialKey(int key);
    void copyToClipboard();
    void pasteFromClipboard();
    Cell makeCellForCurrentAttr() const;
    void handleIfMouseEnabled(QMouseEvent*, std::function<void()> fn);
    void clampLineCol(int& line, int& col);
    ScreenBuffer& currentBuffer();
    const ScreenBuffer& currentBuffer() const;

    void safeWriteToPty(const QByteArray& bytes);
    QByteArray keyEventToAnsiSequence(QKeyEvent*);
    void invalidateCell(int row, int col);
    void copyBuffer(const ScreenBuffer& src, ScreenBuffer& dst, int rows, int cols, const Cell& blank);

    std::unique_ptr<ScreenBuffer> m_mainScreen;
    std::unique_ptr<ScreenBuffer> m_alternateScreen;
    bool m_inAlternateScreen{false};

    std::deque<std::vector<Cell>> m_scrollbackBuffer;
    int m_scrollbackMax{1000};

    bool m_showCursor{true};
    int m_cursorRow{0};
    int m_cursorCol{0};
    int m_savedCursorRow{0};
    int m_savedCursorCol{0};
    int m_currentFg{7};
    int m_currentBg{0};
    std::uint8_t m_currentStyle{0};

    int m_scrollRegionTop{0};
    int m_scrollRegionBottom{0};

    int m_ptyMaster{-1};
    pid_t m_shellPid{-1};

    bool m_mouseEnabled{true};
    bool m_selecting{false};
    bool m_hasSelection{false};

    int m_selAnchorAbsLine{0}, m_selAnchorCol{0};
    int m_selActiveAbsLine{0}, m_selActiveCol{0};

    int m_charWidth{0}, m_charHeight{0};
    int m_prevCursorRow{-1}, m_prevCursorCol{-1};

    QColor ansiIndexToColor(int idx, bool bold);
    void drawCell(QPainter&, int canvasRow, int col, const Cell&);
    friend class EscapeSequenceParser;
};

inline ScreenBuffer::ScreenBuffer(int rows, int cols) {
    rows = std::max(rows, 1);
    cols = std::max(cols, 1);
    m_rows = rows;
    m_cols = cols;
    m_data.resize(std::size_t(rows) * std::size_t(cols));
}

inline void ScreenBuffer::resize(int rows, int cols) {
    m_rows = std::max(rows, 1);
    m_cols = std::max(cols, 1);

    m_data.assign(static_cast<std::size_t>(m_rows) * static_cast<std::size_t>(m_cols), Cell{});
}

inline Cell& ScreenBuffer::cell(int r, int c) {
    assert(r >= 0 && r < m_rows && c >= 0 && c < m_cols && "Cell access out of bounds");

    std::size_t idx = static_cast<std::size_t>(r) * static_cast<std::size_t>(m_cols) + static_cast<std::size_t>(c);

    return m_data[idx];
}

inline const Cell& ScreenBuffer::cell(int r, int c) const {
    assert(r >= 0 && r < m_rows && c >= 0 && c < m_cols && "Cell access out of bounds");

    std::size_t idx = static_cast<std::size_t>(r) * static_cast<std::size_t>(m_cols) + static_cast<std::size_t>(c);

    return m_data[idx];
}

inline void ScreenBuffer::fillRow(int r, int c0, int c1, const Cell& cell) {
    if (r < 0 || r >= m_rows)
        return;

    c0 = std::clamp(c0, 0, m_cols);
    c1 = std::clamp(c1, 0, m_cols);

    for (int col = c0; col < c1; ++col) {
        this->cell(r, col) = cell;
    }
}

#endif
