#include "escapeparser.h"
#include "terminalwidget.h"
#include "debug.h"

#include <unistd.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>

EscapeSequenceParser::EscapeSequenceParser(TerminalWidget* widget, QObject* parent)
    : QObject(parent), m_widget(widget) {
    if (g_debug) {
        qDebug() << "EscapeSequenceParser initialized with widget";
    }
}

void EscapeSequenceParser::feed(const QByteArray& data) {
    if (g_debug) {
        qDebug() << "feed called with data of size" << data.size();
    }

    for (unsigned char c : data) {
        processByte(c);
    }
    if (m_widget) {
        m_widget->updateScreen();
    }
}

void EscapeSequenceParser::processByte(unsigned char b) {
    if (g_debug) {
        qDebug() << "processByte called with byte" << QString::number(b, 16);
    }

    switch (m_state) {
        case State::Normal:
            if (b == 0x1B) {
                m_state = State::Esc;
                if (g_debug) {
                    qDebug() << "Switching to Esc state";
                }
            }
            else if (b == 0x08) {
                cursorLeft(1);
            }
            else if (b == 0x09) {
                cursorRight(8 - (m_widget->getCursorCol() % 8));
            }
            else if (b == 0x0D) {
                m_widget->setCursorPos(m_widget->getCursorRow(), 0, true);
            }
            else if (b == 0x0A) {
                m_widget->lineFeed();
            }
            else if (b >= 0x20) {
                m_widget->putChar(QChar(b));
            }
            break;

        case State::Esc:
            if (b == '[') {
                m_state = State::Csi;
                m_params.clear();
                m_privateMode = false;
                m_paramString.clear();
                if (g_debug) {
                    qDebug() << "CSI sequence started";
                }
            }
            else if (b == ']') {
                m_state = State::Osc;
                m_oscBuffer.clear();
                if (g_debug) {
                    qDebug() << "OSC sequence started";
                }
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

void EscapeSequenceParser::handleCsiCommand(unsigned char cmd) {
    if (g_debug) {
        qDebug() << "handleCsiCommand called with cmd" << cmd;
    }

    if (m_params.empty()) {
        m_params.push_back(1);
    }

    std::vector<int> stdParams(m_params.begin(), m_params.end());

    switch (cmd) {
        case 'A':
            cursorUp(stdParams[0]);
            break;
        case 'B':
            cursorDown(stdParams[0]);
            break;
        case 'C':
            cursorRight(stdParams[0]);
            break;
        case 'D':
            cursorLeft(stdParams[0]);
            break;
        case 'E': {
            int n = stdParams[0];
            m_widget->setCursorPos(m_widget->getCursorRow() + n, 0, true);
        } break;
        case 'F': {
            int n = stdParams[0];
            m_widget->setCursorPos(m_widget->getCursorRow() - n, 0, true);
        } break;
        case 'G': {
            int col = stdParams[0] - 1;
            m_widget->setCursorPos(m_widget->getCursorRow(), col, true);
        } break;
        case 'H':
        case 'f': {
            int row = (stdParams.size() > 0 ? stdParams[0] : 1) - 1;
            int col = (stdParams.size() > 1 ? stdParams[1] : 1) - 1;
            m_widget->setCursorPos(row, col, true);
        } break;
        case 'J':
            m_widget->eraseInDisplay(stdParams[0]);
            break;
        case 'K':
            m_widget->eraseInLine(stdParams[0]);
            break;
        case 'm':
            m_widget->setSGR(stdParams);
            break;
        case 'r': {
            int top = (stdParams.size() > 0 ? stdParams[0] : 1) - 1;
            int bot = (stdParams.size() > 1 ? stdParams[1] : m_widget->getMainScreen()->rows()) - 1;
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
    if (g_debug) {
        qDebug() << "handleOscCommand called with oscBuffer" << m_oscBuffer;
    }

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
    if (g_debug) {
        qDebug() << "storeParam called with paramString" << m_paramString;
    }

    if (!m_paramString.isEmpty()) {
        bool ok;
        int param = m_paramString.toInt(&ok);
        if (ok) {
            m_params.push_back(param);
        }
        m_paramString.clear();
    }
}

void EscapeSequenceParser::doFullReset() {
    if (g_debug) {
        qDebug() << "doFullReset called";
    }

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
    if (g_debug) {
        qDebug() << "cursorUp called with n" << n;
    }

    m_widget->setCursorRow(m_widget->getCursorRow() - n);
    m_widget->clampCursor();
}

void EscapeSequenceParser::cursorDown(int n) {
    if (g_debug) {
        qDebug() << "cursorDown called with n" << n;
    }

    m_widget->setCursorRow(m_widget->getCursorRow() + n);
    m_widget->clampCursor();
}

void EscapeSequenceParser::cursorRight(int n) {
    if (g_debug) {
        qDebug() << "cursorRight called with n" << n;
    }

    m_widget->setCursorCol(m_widget->getCursorCol() + n);
    m_widget->clampCursor();
}

void EscapeSequenceParser::cursorLeft(int n) {
    if (g_debug) {
        qDebug() << "cursorLeft called with n" << n;
    }

    m_widget->setCursorCol(m_widget->getCursorCol() - n);
    m_widget->clampCursor();
}
