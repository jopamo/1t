#include "escapeparser.h"
#include "terminalwidget.h"
#include "debug.h"

#include <QDebug>
#include <QStringList>
#include <QStringView>
#include <QChar>
#include <algorithm>
#include <array>
#include <vector>

#if __cplusplus < 201402L
#error "This file requires at least C++14 (or newer) language standard."
#endif

EscapeSequenceParser::EscapeSequenceParser(TerminalWidget* widget, QObject* parent)
    : QObject(parent), m_widget(widget) {
#ifdef ENABLE_DEBUG
    DBG() << "EscapeSequenceParser constructor";
#endif
    resetStateMachine();
}

void EscapeSequenceParser::feed(const QByteArray& data) {
#ifdef ENABLE_DEBUG
    DBG() << "feed" << data.size() << "bytes";
#endif

    for (char c : data) {
        unsigned char b = static_cast<unsigned char>(c);
        processByte(b);
    }

    flushTextBuffer();

    if (m_widget) {
#ifdef ENABLE_DEBUG
        DBG() << "calling updateScreen() after feed";
#endif
        m_widget->updateScreen();
    }
}

void EscapeSequenceParser::processByte(unsigned char b) {
    static constexpr std::array<uint8_t, 256> g_cls = [] {
        std::array<uint8_t, 256> arr{};

        for (auto& x : arr) {
            x = 0;
        }

        for (unsigned i = 0; i < 0x20; ++i) {
            arr[i] = 1;
        }

        arr[0x1B] = 2;
        return arr;
    }();

    State oldState = m_state;
    uint8_t cls = g_cls[b];

    switch (m_state) {
        case State::Ground: {
            if (cls == 0) {
                m_textBuffer.push_back(static_cast<char>(b));
            }
            else if (cls == 1) {
                flushTextBuffer();
                handleControlChar(b);
            }
            else {
                flushTextBuffer();
                m_state = State::Escape;
            }
            break;
        }

        case State::Escape: {
            switch (b) {
                case '[':
                    m_state = State::CsiEntry;
                    m_paramBuffer.clear();
                    m_intermediate.clear();
                    m_escQuestionMark = false;
                    break;
                case ']':
                    m_state = State::OscString;
                    m_oscString.clear();
                    break;
                case '7':
                    if (m_widget)
                        m_widget->saveCursorPos();
                    m_state = State::Ground;
                    break;
                case '8':
                    if (m_widget)
                        m_widget->restoreCursorPos();
                    m_state = State::Ground;
                    break;
                case 'D':
                    if (m_widget)
                        m_widget->lineFeed();
                    m_state = State::Ground;
                    break;
                case 'M':
                    if (m_widget)
                        m_widget->reverseLineFeed();
                    m_state = State::Ground;
                    break;
                case 'E':
                    if (m_widget) {
                        m_widget->lineFeed();
                        m_widget->setCursorPos(m_widget->getCursorRow(), 0, true);
                    }
                    m_state = State::Ground;
                    break;
                case 'c':
                    if (m_widget)
                        m_widget->fullReset();
                    m_state = State::Ground;
                    break;
                default:
#ifdef ENABLE_DEBUG
                    DBG() << "Unrecognized ESC sequence: ESC " << char(b);
#endif
                    m_state = State::Ground;
                    break;
            }
            break;
        }

        case State::CsiEntry:
        case State::CsiParam:
        case State::CsiIntermediate:
        case State::CsiIgnore:
            processCsiSubState(b);
            break;

        case State::OscString: {
            if (b == 0x07) {
                oscDispatch();
                m_state = State::Ground;
            }
            else if (b == 0x1B) {
                m_oscString.push_back('\x1B');
            }
            else if (b == '\\') {
                if (!m_oscString.isEmpty() && m_oscString.back() == '\x1B') {
                    m_oscString.chop(1);
                    oscDispatch();
                    m_state = State::Ground;
                }
                else {
                    m_oscString.push_back('\\');
                }
            }
            else {
                m_oscString.push_back(static_cast<char>(b));
            }
            break;
        }

        case State::SosPmApcString:
            break;
    }

#ifdef ENABLE_DEBUG
    if (oldState != m_state) {
        DBG() << "processByte(" << int(b) << ") state transition: Ground -> " << stateName(oldState) << " -> "
              << stateName(m_state);
    }
#endif
}

void EscapeSequenceParser::processCsiSubState(unsigned char b) {
    State old = m_state;

    switch (m_state) {
        case State::CsiEntry: {
            if (b == '?') {
                m_escQuestionMark = true;
                m_state = State::CsiParam;
            }
            else if ((b >= '0' && b <= '9') || b == ';') {
                m_paramBuffer.push_back(static_cast<char>(b));
                m_state = State::CsiParam;
            }
            else if (b >= 0x20 && b <= 0x2F) {
                m_intermediate.push_back(static_cast<char>(b));
                m_state = State::CsiIntermediate;
            }
            else if (b >= 0x40 && b <= 0x7E) {
                csiDispatch(b);
                m_state = State::Ground;
            }
            else {
                m_state = State::Ground;
            }
            break;
        }
        case State::CsiParam: {
            if ((b >= '0' && b <= '9') || b == ';') {
                m_paramBuffer.push_back(static_cast<char>(b));
            }
            else if (b >= 0x20 && b <= 0x2F) {
                m_intermediate.push_back(static_cast<char>(b));
                m_state = State::CsiIntermediate;
            }
            else if (b >= 0x40 && b <= 0x7E) {
                csiDispatch(b);
                m_state = State::Ground;
            }
            else {
                m_state = State::CsiIgnore;
            }
            break;
        }
        case State::CsiIntermediate: {
            if (b >= 0x20 && b <= 0x2F) {
                m_intermediate.push_back(static_cast<char>(b));
            }
            else if (b >= 0x40 && b <= 0x7E) {
                csiDispatch(b);
                m_state = State::Ground;
            }
            else {
                m_state = State::CsiIgnore;
            }
            break;
        }
        case State::CsiIgnore: {
            if (b >= 0x40 && b <= 0x7E) {
                m_state = State::Ground;
            }
            break;
        }
        default:
            break;
    }

#ifdef ENABLE_DEBUG
    if (old != m_state) {
        DBG() << "processCsiSubState(" << int(b) << ") transition " << stateName(old) << " -> " << stateName(m_state);
    }
#endif
}

void EscapeSequenceParser::flushTextBuffer() {
    if (m_textBuffer.isEmpty()) {
        return;
    }

    if (!m_widget) {
        m_textBuffer.clear();
        return;
    }

    QString chunk = QString::fromUtf8(m_textBuffer.data(), m_textBuffer.size());
    m_textBuffer.clear();

    for (int i = 0; i < chunk.size(); ++i) {
        QChar ch = chunk.at(i);

        if (ch.isHighSurrogate() && (i + 1) < chunk.size()) {
            QChar nextCh = chunk.at(i + 1);
            if (nextCh.isLowSurrogate()) {
                char32_t high = static_cast<char32_t>(ch.unicode() - 0xD800) << 10;
                char32_t low = static_cast<char32_t>(nextCh.unicode() - 0xDC00);
                char32_t ucs4 = 0x10000 + (high | low);

                QString wideChar = QString::fromUcs4(&ucs4, 1);

                if (!wideChar.isEmpty()) {
                    m_widget->putChar(wideChar.at(0));
                }
                ++i;
                continue;
            }
        }

        if (ch == u'\r') {
            m_widget->setCursorPos(m_widget->getCursorRow(), 0, true);
            continue;
        }
        if (ch == u'\n') {
            m_widget->lineFeed();
            continue;
        }

        m_widget->putChar(ch);
    }
}

void EscapeSequenceParser::handleControlChar(unsigned char c0) {
    if (!m_widget)
        return;

    switch (c0) {
        case 0x0D:
            m_widget->setCursorPos(m_widget->getCursorRow(), 0, true);
            break;

        case 0x0A:
            m_widget->lineFeed();
            break;

        case 0x08:
            m_widget->setCursorCol(m_widget->getCursorCol() - 1);
            m_widget->clampCursor();
            break;

        case 0x07:
            m_widget->handleBell();
            break;

        case 0x09: {
            int nextTab = ((m_widget->getCursorCol() / 8) + 1) * 8;
            m_widget->setCursorCol(nextTab);
            m_widget->clampCursor();
            break;
        }

        default:
#ifdef ENABLE_DEBUG
            DBG() << "Unhandled control char: 0x" << std::hex << int(c0);
#endif
            break;
    }
}

void EscapeSequenceParser::csiDispatch(unsigned char finalByte) {
#ifdef ENABLE_DEBUG
    DBG() << "csiDispatch finalByte=" << int(finalByte);
#endif

    if (!m_widget) {
        m_paramBuffer.clear();
        m_intermediate.clear();
        return;
    }

    std::vector<int> params;
    if (!m_paramBuffer.isEmpty()) {
        const QList<QByteArray> parts = QByteArray{m_paramBuffer}.split(';');
        for (const QByteArray& part : parts) {
            bool ok = false;
            int v = part.toInt(&ok);

            if (!ok || v < 0) {
#ifdef ENABLE_DEBUG
                DBG() << "Invalid parameter in CSI sequence:" << part;
#endif
                params.push_back(0);
            }
            else {
                params.push_back(v);
            }
        }
    }
    if (params.empty()) {
        params.emplace_back(0);
    }

    bool priv = m_escQuestionMark;

    auto P = [&](int idx, int def) -> int {
        if (idx >= 0 && idx < static_cast<int>(params.size())) {
            return params[static_cast<size_t>(idx)];
        }
        return def;
    };

    const int rows = m_widget->currentBuffer().rows();
    const int cols = m_widget->currentBuffer().cols();
    const int curR = m_widget->getCursorRow();
    const int curC = m_widget->getCursorCol();

    switch (finalByte) {
        case 'A':
            m_widget->setCursorRow(curR - P(0, 1));
            m_widget->clampCursor();
            break;

        case 'B':
            m_widget->setCursorRow(curR + P(0, 1));
            m_widget->clampCursor();
            break;

        case 'C': {
            int newC = std::min(curC + P(0, 1), cols - 1);
            m_widget->setCursorCol(newC);
            break;
        }
        case 'D': {
            int newC = std::max(curC - P(0, 1), 0);
            m_widget->setCursorCol(newC);
            break;
        }

        case 'G': {
            int col = std::clamp(P(0, 1) - 1, 0, cols - 1);
            m_widget->setCursorPos(curR, col, false);
            break;
        }

        case 'H':
        case 'f': {
            int row = std::clamp(P(0, 1) - 1, 0, rows - 1);
            int col = std::clamp(P(1, 1) - 1, 0, cols - 1);
            m_widget->setCursorPos(row, col, false);
            break;
        }

        case 'J':
            doEraseInDisplay(P(0, 0));
            break;
        case 'K':
            doEraseInLine(P(0, 0));
            break;
        case 'P':
            m_widget->deleteChars(P(0, 1));
            break;
        case 'X':
            m_widget->eraseChars(P(0, 1));
            break;
        case '@':
            m_widget->insertChars(P(0, 1));
            break;

        case 'm':
            m_widget->setSGR(params);
            break;

        case 'r': {
            int top = std::clamp(P(0, 1) - 1, 0, rows - 1);
            int bottom = std::clamp(P(1, rows) - 1, 0, rows - 1);
            if (top > bottom) {
#ifdef ENABLE_DEBUG
                DBG() << "Invalid scrolling region, swapping top/bottom.";
#endif
                std::swap(top, bottom);
            }
            m_widget->setScrollingRegion(top, bottom);
            break;
        }

        default:
#ifdef ENABLE_DEBUG
            DBG() << "Unsupported CSI finalByte:" << char(finalByte) << "params=" << params;
#endif
            break;
    }

    m_paramBuffer.clear();
    m_intermediate.clear();
}

void EscapeSequenceParser::oscDispatch() {
#ifdef ENABLE_DEBUG
    DBG() << "oscDispatch";
#endif

    if (!m_widget) {
        m_oscString.clear();
        return;
    }

    QStringView osc = QString::fromLatin1(m_oscString);
    m_oscString.clear();

    qsizetype sep = osc.indexOf(u';');
    if (sep < 0) {
#ifdef ENABLE_DEBUG
        DBG() << "Malformed OSC: missing semicolon";
#endif
        return;
    }

    bool ok = false;
    int ps = osc.left(sep).toInt(&ok);
    if (!ok) {
#ifdef ENABLE_DEBUG
        DBG() << "Malformed OSC: cannot parse ps (before semicolon)";
#endif
        return;
    }

    QStringView pt = osc.mid(sep + 1);

    switch (ps) {
        case 0:
        case 2:
            m_widget->setWindowTitle(pt.toString());
            break;

        case 4:
#ifdef ENABLE_DEBUG
            DBG() << "OSC 4 (set color) not yet implemented. Param=" << pt;
#endif
            break;

        case 8:
#ifdef ENABLE_DEBUG
            DBG() << "OSC 8 (hyperlink) not yet implemented. Param=" << pt;
#endif
            break;

        default:
#ifdef ENABLE_DEBUG
            DBG() << "Ignoring unsupported OSC code: " << ps << ", params=" << pt;
#endif
            break;
    }
}

void EscapeSequenceParser::resetStateMachine() {
#ifdef ENABLE_DEBUG
    DBG() << "resetStateMachine";
#endif
    m_state = State::Ground;

    m_textBuffer.clear();
    m_paramBuffer.clear();
    m_intermediate.clear();
    m_oscString.clear();

    m_escIntermediate = false;
    m_escQuestionMark = false;
}

void EscapeSequenceParser::doEraseInDisplay(int mode) {
#ifdef ENABLE_DEBUG
    DBG() << "doEraseInDisplay mode=" << mode;
#endif
    if (m_widget) {
        m_widget->eraseInDisplay(mode);
    }
}

void EscapeSequenceParser::doEraseInLine(int mode) {
#ifdef ENABLE_DEBUG
    DBG() << "doEraseInLine mode=" << mode;
#endif
    if (m_widget) {
        m_widget->eraseInLine(mode);
    }
}

void EscapeSequenceParser::doSetMode(int p) {
#ifdef ENABLE_DEBUG
    DBG() << "doSetMode p=" << p;
#endif
    if (!m_widget)
        return;

    switch (p) {
        case 25:
#ifdef ENABLE_DEBUG
            DBG() << "Show cursor (not yet implemented in TerminalWidget)";
#endif
            break;

        case 47:
        case 1047:
        case 1049:
            m_widget->useAlternateScreen(true);
            break;

        case 1000:
            m_widget->setMouseEnabled(true);
            break;

        case 2004:
#ifdef ENABLE_DEBUG
            DBG() << "Bracketed paste mode ON (not yet implemented)";
#endif
            break;

        default:
#ifdef ENABLE_DEBUG
            DBG() << "Unrecognized DEC Private Mode: " << p;
#endif
            break;
    }
}

void EscapeSequenceParser::doResetMode(int p) {
#ifdef ENABLE_DEBUG
    DBG() << "doResetMode p=" << p;
#endif
    if (!m_widget)
        return;

    switch (p) {
        case 25:
#ifdef ENABLE_DEBUG
            DBG() << "Hide cursor (not yet implemented in TerminalWidget)";
#endif
            break;

        case 47:
        case 1047:
        case 1049:
            m_widget->useAlternateScreen(false);
            break;

        case 1000:
            m_widget->setMouseEnabled(false);
            break;

        case 2004:
#ifdef ENABLE_DEBUG
            DBG() << "Bracketed paste mode OFF (not yet implemented)";
#endif
            break;

        default:
#ifdef ENABLE_DEBUG
            DBG() << "Unrecognized DEC Private Mode reset: " << p;
#endif
            break;
    }
}

const char* EscapeSequenceParser::stateName(State state) {
    switch (state) {
        case State::Ground: return "Ground";
        case State::Escape: return "Escape";
        case State::CsiEntry: return "CsiEntry";
        case State::CsiParam: return "CsiParam";
        case State::CsiIntermediate: return "CsiIntermediate";
        case State::CsiIgnore: return "CsiIgnore";
        case State::OscString: return "OscString";
        case State::SosPmApcString: return "SosPmApcString";
        default: return "Unknown";
    }
}

