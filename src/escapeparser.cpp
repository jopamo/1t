#include "escapeparser.h"
#include "terminalwidget.h"

#include <QDebug>
#include <QString>
#include <QChar>
#include <algorithm>

#include <unistd.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>

EscapeSequenceParser::EscapeSequenceParser(TerminalWidget* widget, QObject* parent)
    : QObject(parent), m_widget(widget) {}

void EscapeSequenceParser::feed(const QByteArray& data) {
    for (unsigned char b : data) {
        processByte(b);
    }

    flushTextBuffer();

    if (m_widget) {
        m_widget->updateScreen();
    }
}

void EscapeSequenceParser::processByte(unsigned char b) {
    switch (m_state) {
        case State::Normal:
            if (b == 0x1B) {
                flushTextBuffer();
                m_state = State::Esc;
            }
            else if (b < 0x20 || b == 0x7F) {
                flushTextBuffer();
                handleControlChar(b);
            }
            else {
                m_textBuffer.append(char(b));
            }
            break;

        case State::Esc:
            if (b == '[') {
                m_state = State::Csi;
                m_params.clear();
                m_privateMode = false;
                m_paramString.clear();
            }
            else if (b == ']') {
                m_state = State::Osc;
                m_oscBuffer.clear();
            }
            else if (b == 'c') {
                doFullReset();
                m_state = State::Normal;
            }
            else if (b == '7') {
                m_widget->saveCursorPos();
                m_state = State::Normal;
            }
            else if (b == '8') {
                m_widget->restoreCursorPos();
                m_state = State::Normal;
            }
            else if (b == '(' || b == ')' || b == '*') {
                m_state = State::Normal;
            }
            else if (b == 'D') {
                m_widget->lineFeed();
                m_state = State::Normal;
            }
            else if (b == 'M') {
                m_widget->reverseLineFeed();
                m_state = State::Normal;
            }
            else if (b == 'E') {
                m_widget->lineFeed();
                m_widget->setCursorPos(m_widget->getCursorRow(), 0, true);
                m_state = State::Normal;
            }
            else {
                m_state = State::Normal;
            }
            break;

        case State::Csi:
            if (b >= '0' && b <= '9') {
                m_paramString.append(char(b));
            }
            else if (b == ';') {
                storeParam();
            }
            else if (b == '?') {
                m_privateMode = true;
            }
            else if ((b >= 0x40 && b <= 0x7E) || b == '@') {
                storeParam();
                handleCsiCommand(b);
                m_state = State::Normal;
            }
            break;

        case State::Osc:

            if (b == 0x07) {
                handleOscCommand();
                m_state = State::Normal;
            }
            else if (b == 0x1B) {
                m_oscEscape = true;
            }
            else {
                if (!m_oscEscape) {
                    m_oscBuffer.append(char(b));
                }
                else {
                    if (b == '\\') {
                        handleOscCommand();
                        m_state = State::Normal;
                    }
                    m_oscEscape = false;
                }
            }
            break;
    }
}

void EscapeSequenceParser::flushTextBuffer() {
    if (m_textBuffer.isEmpty()) {
        return;
    }

    const QString decoded = QString::fromUtf8(m_textBuffer);
    m_textBuffer.clear();

    for (QChar ch : decoded) {
        if (ch == '\n') {
            m_widget->lineFeed();
        }
        else if (ch == '\r') {
            m_widget->setCursorPos(m_widget->getCursorRow(), 0, true);
        }
        else {
            m_widget->putChar(ch);
        }
    }
}

void EscapeSequenceParser::handleControlChar(unsigned char b) {
    switch (b) {
        case 0x08:
            cursorLeft(1);
            break;
        case 0x09:
            cursorRight(8 - (m_widget->getCursorCol() % 8));
            break;
        case 0x0D:
            m_widget->setCursorPos(m_widget->getCursorRow(), 0, true);
            break;
        case 0x0A:
            m_widget->lineFeed();
            break;
        default:

            break;
    }
}

void EscapeSequenceParser::handleCsiCommand(unsigned char cmd) {
    auto ensure = [&](int def) {
        if (m_params.empty()) {
            m_params.push_back(def);
        }
    };

    switch (cmd) {
        case 'A':
        case 'B':
        case 'C':
        case 'D':
        case 'E':
        case 'F':
            ensure(1);
            break;

        case 'J':
        case 'K':
            ensure(0);
            break;

        default:
            break;
    }

    std::vector<int> stdParams(m_params.begin(), m_params.end());

    auto P = [&](int i, int defVal = 0) { return (i < int(stdParams.size()) ? stdParams[i] : defVal); };

    switch (cmd) {
        case 'A':
            cursorUp(P(0, 1));
            break;
        case 'B':
            cursorDown(P(0, 1));
            break;
        case 'C':
            cursorRight(P(0, 1));
            break;
        case 'D':
            cursorLeft(P(0, 1));
            break;

        case 'E':
            m_widget->setCursorPos(m_widget->getCursorRow() + P(0, 1), 0, true);
            break;
        case 'F':
            m_widget->setCursorPos(m_widget->getCursorRow() - P(0, 1), 0, true);
            break;

        case 'G': {
            int col = P(0, 1) - 1;
            m_widget->setCursorPos(m_widget->getCursorRow(), col, true);
        } break;

        case 'H':
        case 'f': {
            int row = P(0, 1) - 1;
            int col = P(1, 1) - 1;
            m_widget->setCursorPos(row, col, true);
        } break;

        case 'J':
            m_widget->eraseInDisplay(P(0, 0));
            break;
        case 'K':
            m_widget->eraseInLine(P(0, 0));
            break;

        case 'm':
            m_widget->setSGR(stdParams);
            break;

        case 'r': {
            int top = P(0, 1) - 1;
            int bot = P(1, m_widget->getMainScreen()->rows()) - 1;
            m_widget->setScrollingRegion(top, bot);
        } break;

        case 'h':
            if (m_privateMode) {
                for (int p : stdParams) {
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
            if (m_privateMode) {
                for (int p : stdParams) {
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

void EscapeSequenceParser::handleOscCommand() {
    auto parts = m_oscBuffer.split(';');
    if (!parts.isEmpty()) {
        int ps = parts[0].toInt();
        if (ps == 0 || ps == 2) {
            if (parts.size() > 1) {
                QByteArray t = parts[1];
                for (int i = 2; i < parts.size(); i++) {
                    t += ';';
                    t += parts[i];
                }
                if (m_widget->window()) {
                    m_widget->window()->setWindowTitle(QString::fromUtf8(t));
                }
            }
        }
    }
}

void EscapeSequenceParser::storeParam() {
    if (!m_paramString.isEmpty()) {
        bool ok = false;
        int param = m_paramString.toInt(&ok);
        if (ok) {
            m_params.push_back(param);
        }
        m_paramString.clear();
    }
}

void EscapeSequenceParser::doFullReset() {
    m_widget->setCurrentFg(7);
    m_widget->setCurrentBg(0);
    m_widget->setCurrentStyle(0);

    Cell blankCell;
    m_widget->fillScreen(*m_widget->getMainScreen(), blankCell);
    m_widget->fillScreen(*m_widget->getAlternateScreen(), blankCell);

    m_widget->setCursorPos(0, 0, true);
    m_widget->useAlternateScreen(false);
}

void EscapeSequenceParser::cursorUp(int n) {
    m_widget->setCursorRow(m_widget->getCursorRow() - n);
    m_widget->clampCursor();
}

void EscapeSequenceParser::cursorDown(int n) {
    m_widget->setCursorRow(m_widget->getCursorRow() + n);
    m_widget->clampCursor();
}

void EscapeSequenceParser::cursorRight(int n) {
    m_widget->setCursorCol(m_widget->getCursorCol() + n);
    m_widget->clampCursor();
}

void EscapeSequenceParser::cursorLeft(int n) {
    m_widget->setCursorCol(m_widget->getCursorCol() - n);
    m_widget->clampCursor();
}
