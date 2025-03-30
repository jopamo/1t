#ifndef ESCAPEPARSER_H
#define ESCAPEPARSER_H

#include <QObject>
#include <QByteArray>

class TerminalWidget;

class EscapeSequenceParser : public QObject
{
    Q_OBJECT

public:
    explicit EscapeSequenceParser(TerminalWidget* widget, QObject* parent = nullptr);

    void feed(const QByteArray& data);

private:

    enum class State {
        Normal,
        Esc,
        Csi,
        Osc
    };

    State m_state { State::Normal };

    TerminalWidget* m_widget;

    bool m_privateMode { false };
    bool m_oscEscape { false };
    QByteArray m_paramString;
    QVector<int> m_params;
    QByteArray m_oscBuffer;

    int m_savedRow { 0 };
    int m_savedCol { 0 };

private:

    void processByte(unsigned char b);

    void handleNormalByte(unsigned char b);
    void handleEscByte(unsigned char b);
    void handleCsiByte(unsigned char b);
    void handleOscByte(unsigned char b);

    void handleCsiCommand(unsigned char cmd);
    void handleOscCommand();

    void storeParam();

    void doFullReset();

    void cursorUp(int n);
    void cursorDown(int n);
    void cursorRight(int n);
    void cursorLeft(int n);

    void changeState(State newState);
    void resetState();
};

#endif
