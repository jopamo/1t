/****************************************************************************
** escapeparser.cpp
** Updated EscapeSequenceParser for a newer TerminalWidget design.
****************************************************************************/

#include "escapeparser.h"
#include "terminalwidget.h"

EscapeSequenceParser::EscapeSequenceParser(TerminalWidget* widget, QObject* parent)
    : QObject(parent)
    , m_widget(widget)
    , m_state(State::Normal)
    , m_privateMode(false)
    , m_oscEscape(false)
    , m_savedRow(0)
    , m_savedCol(0)
{
}

void EscapeSequenceParser::feed(const QByteArray& data)
{
    for (unsigned char c : data) {
        processByte(c);
    }

    if (m_widget) {
        // trigger repaint
        m_widget->updateScreen();
    }
}

void EscapeSequenceParser::processByte(unsigned char b)
{
    switch (m_state) {
    case State::Normal:
        handleNormalByte(b);
        break;
    case State::Esc:
        handleEscByte(b);
        break;
    case State::Csi:
        handleCsiByte(b);
        break;
    case State::Osc:
        handleOscByte(b);
        break;
    }
}

void EscapeSequenceParser::handleNormalByte(unsigned char b)
{
    switch (b)
    {
    case 0x1B: // ESC
        changeState(State::Esc);
        break;
    case 0x08: // Backspace
        cursorLeft(1);
        break;
    case 0x09: // Tab
    {
        // naive approach: move to next multiple of 8
        int currentCol = m_widget->getCursorCol();
        int spacesToNextTab = 8 - (currentCol % 8);
        cursorRight(spacesToNextTab);
        break;
    }
    case 0x0D: // CR
        m_widget->setCursorPos(m_widget->getCursorRow(), 0, true);
        break;
    case 0x0A: // LF
        m_widget->lineFeed();
        break;
    default:
        if (b >= 0x20) {
            // printable range
            m_widget->putChar(QChar(b));
        }
        break;
    }
}

void EscapeSequenceParser::handleEscByte(unsigned char b)
{
    switch (b)
    {
    case '[': // CSI
        m_params.clear();
        m_paramString.clear();
        m_privateMode = false;
        changeState(State::Csi);
        break;
    case ']': // OSC
        m_oscBuffer.clear();
        m_oscEscape = false;
        changeState(State::Osc);
        break;
    case 'c': // full reset
        doFullReset();
        resetState();
        break;
    case '7': // save cursor
        if (m_widget) {
            m_widget->saveCursorPos();
        }
        resetState();
        break;
    case '8': // restore cursor
        if (m_widget) {
            m_widget->restoreCursorPos();
        }
        resetState();
        break;
    case 'D': // IND = linefeed
        m_widget->lineFeed();
        resetState();
        break;
    case 'M': // reverse line feed
        m_widget->reverseLineFeed();
        resetState();
        break;
    case 'E': // CR + LF
        m_widget->lineFeed();
        m_widget->setCursorPos(m_widget->getCursorRow(), 0, true);
        resetState();
        break;
    default:
        // unhandled ESC code
        resetState();
        break;
    }
}

void EscapeSequenceParser::handleCsiByte(unsigned char b)
{
    if (b >= '0' && b <= '9')
    {
        m_paramString.append(char(b));
    }
    else if (b == ';')
    {
        storeParam();
    }
    else if (b == '?')
    {
        m_privateMode = true;
    }
    else if (b >= '@' && b <= '~')
    {
        // final byte of CSI
        storeParam();
        handleCsiCommand(b);
        resetState();
    }
    else
    {
        resetState();
    }
}

void EscapeSequenceParser::handleOscByte(unsigned char b)
{
    if (b == 0x07) // BEL
    {
        handleOscCommand();
        resetState();
        return;
    }
    if (b == 0x1B) // ESC in OSC
    {
        m_oscEscape = true;
        return;
    }
    if (!m_oscEscape)
    {
        m_oscBuffer.append(char(b));
    }
    else
    {
        if (b == '\\') {
            handleOscCommand();
            resetState();
        }
        m_oscEscape = false;
    }
}

void EscapeSequenceParser::handleCsiCommand(unsigned char cmd)
{
    // if no params parsed, default = 1
    if (m_params.empty()) {
        m_params.push_back(1);
    }
    std::vector<int> stdParams(m_params.begin(), m_params.end());

    switch (cmd)
    {
    case 'A': // up
        cursorUp(stdParams[0]);
        break;
    case 'B': // down
        cursorDown(stdParams[0]);
        break;
    case 'C': // right
        cursorRight(stdParams[0]);
        break;
    case 'D': // left
        cursorLeft(stdParams[0]);
        break;
    case 'E': // next line
    {
        int n = stdParams[0];
        int row = m_widget->getCursorRow() + n;
        m_widget->setCursorPos(row, 0, true);
        break;
    }
    case 'F': // prev line
    {
        int n = stdParams[0];
        int row = m_widget->getCursorRow() - n;
        m_widget->setCursorPos(row, 0, true);
        break;
    }
    case 'G': // set column
    {
        int col = stdParams[0] - 1;
        m_widget->setCursorPos(m_widget->getCursorRow(), col, true);
        break;
    }
    case 'H': // set row/col
    case 'f':
    {
        int row = (stdParams.size() >= 1 ? stdParams[0] : 1) - 1;
        int col = (stdParams.size() >= 2 ? stdParams[1] : 1) - 1;
        m_widget->setCursorPos(row, col, true);
        break;
    }
    case 'J': // erase display
        m_widget->eraseInDisplay(stdParams[0]);
        break;
    case 'K': // erase line
        m_widget->eraseInLine(stdParams[0]);
        break;
    case 'm': // set attributes
        m_widget->setSGR(stdParams);
        break;
    case 'r': // set scrolling region
    {
        // Use dot (.) because getMainScreen() returns ScreenBuffer&
        int top = (stdParams.size() > 0 ? stdParams[0] : 1) - 1;
        int bot = (stdParams.size() > 1 ? stdParams[1]
                                        : m_widget->getMainScreen().rows()) - 1;
        m_widget->setScrollingRegion(top, bot);
        break;
    }
    case 'h':
        if (m_privateMode)
        {
            for (int p : stdParams) {
                // ?1049 => alt screen
                // ?1000 => mouse on
                if (p == 1049) {
                    m_widget->useAlternateScreen(true);
                    m_savedRow = m_widget->getCursorRow();
                    m_savedCol = m_widget->getCursorCol();
                }
                else if (p == 1000) {
                    m_widget->setMouseEnabled(true);
                }
            }
        }
        break;
    case 'l':
        if (m_privateMode)
        {
            for (int p : stdParams) {
                // ?1049 => normal screen
                // ?1000 => mouse off
                if (p == 1049) {
                    m_widget->useAlternateScreen(false);
                    m_widget->setCursorPos(m_savedRow, m_savedCol, true);
                }
                else if (p == 1000) {
                    m_widget->setMouseEnabled(false);
                }
            }
        }
        break;
    default:
        break;
    }
}

void EscapeSequenceParser::handleOscCommand()
{
    // e.g. ESC ] 0;titleBEL
    if (m_oscBuffer.isEmpty())
        return;

    QList<QByteArray> parts = m_oscBuffer.split(';');
    if (parts.isEmpty())
        return;

    int ps = parts[0].toInt();
    if (ps == 0 || ps == 2)
    {
        // window title
        QByteArray title;
        for (int i = 1; i < parts.size(); i++) {
            if (!title.isEmpty()) title.append(';');
            title.append(parts[i]);
        }
        if (m_widget->window()) {
            m_widget->window()->setWindowTitle(QString::fromUtf8(title));
        }
    }
}

void EscapeSequenceParser::storeParam()
{
    if (!m_paramString.isEmpty())
    {
        bool ok = false;
        int param = m_paramString.toInt(&ok);
        if (ok) {
            m_params.push_back(param);
        }
        m_paramString.clear();
    }
}

void EscapeSequenceParser::doFullReset()
{
    if (!m_widget) return;

    // Reset attributes
    m_widget->setCurrentFg(7);
    m_widget->setCurrentBg(0);
    m_widget->setCurrentStyle(0);

    // Clear main & alternate screens
    Cell blank;
    blank.ch    = QChar(' ');
    blank.fg    = 7;
    blank.bg    = 0;
    blank.style = 0;

    // Use dot, no * needed:
    m_widget->fillScreen(m_widget->getMainScreen(), blank);
    m_widget->fillScreen(m_widget->getAlternateScreen(), blank);

    // Place cursor at (0,0)
    m_widget->setCursorPos(0, 0, true);

    // Switch back to normal screen
    m_widget->useAlternateScreen(false);
}

void EscapeSequenceParser::cursorUp(int n)
{
    int row = m_widget->getCursorRow() - n;
    m_widget->setCursorPos(row, m_widget->getCursorCol(), true);
}

void EscapeSequenceParser::cursorDown(int n)
{
    int row = m_widget->getCursorRow() + n;
    m_widget->setCursorPos(row, m_widget->getCursorCol(), true);
}

void EscapeSequenceParser::cursorRight(int n)
{
    int col = m_widget->getCursorCol() + n;
    m_widget->setCursorPos(m_widget->getCursorRow(), col, true);
}

void EscapeSequenceParser::cursorLeft(int n)
{
    int col = m_widget->getCursorCol() - n;
    m_widget->setCursorPos(m_widget->getCursorRow(), col, true);
}

void EscapeSequenceParser::changeState(State newState)
{
    m_state = newState;
}

void EscapeSequenceParser::resetState()
{
    m_state = State::Normal;
    m_paramString.clear();
    m_params.clear();
    m_privateMode = false;
    m_oscEscape   = false;
    // m_oscBuffer is left intact only until we process it
}
