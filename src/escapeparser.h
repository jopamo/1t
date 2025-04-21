#ifndef ESCAPEPARSER_H
#define ESCAPEPARSER_H

#include <QObject>
#include <QByteArray>
#include <vector>

class TerminalWidget;

class EscapeSequenceParser : public QObject {
    Q_OBJECT

   public:
    explicit EscapeSequenceParser(TerminalWidget* widget, QObject* parent = nullptr);
    ~EscapeSequenceParser() override = default;

    void feed(const QByteArray& data);

   private:
    enum class State { Ground, Escape, CsiEntry, CsiParam, CsiIntermediate, CsiIgnore, OscString, SosPmApcString };

    static const char* stateName(State s);

    void processByte(unsigned char b);
    void processCsiSubState(unsigned char b);

    void flushTextBuffer();
    void handleControlChar(unsigned char c0);
    void csiDispatch(unsigned char finalByte);
    void oscDispatch();
    void resetStateMachine();

    void doEraseInDisplay(int mode);
    void doEraseInLine(int mode);
    void doSetMode(int p);
    void doResetMode(int p);

   private:
    TerminalWidget* m_widget{nullptr};
    State m_state{State::Ground};

    bool m_escIntermediate{false};
    bool m_escQuestionMark{false};

    QByteArray m_textBuffer;
    QByteArray m_paramBuffer;
    QByteArray m_intermediate;
    QByteArray m_oscString;

    bool m_lastWasCR{false};
};

#endif
