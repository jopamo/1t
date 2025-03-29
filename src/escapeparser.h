#ifndef ESCAPEPARSER_H
#define ESCAPEPARSER_H

#include <QObject>
#include <QByteArray>
#include <QChar>
#include <QVector>
#include "terminalwidget.h"

class EscapeSequenceParser : public QObject {
    Q_OBJECT

   public:
    explicit EscapeSequenceParser(TerminalWidget* widget, QObject* parent = nullptr);

    void feed(const QByteArray& data);

   private:
    enum class State { Normal, Esc, Csi, Osc };

    State m_state{State::Normal};
    bool m_privateMode{false};
    bool m_oscEscape{false};
    QVector<int> m_params;
    QByteArray m_paramString;
    QByteArray m_oscBuffer;
    TerminalWidget* m_widget;

    int m_savedRow{0};
    int m_savedCol{0};

    void processByte(unsigned char b);
    void handleCsiCommand(unsigned char cmd);
    void handleOscCommand();
    void doFullReset();
    void cursorUp(int n);
    void cursorDown(int n);
    void cursorRight(int n);
    void cursorLeft(int n);

    void storeParam();
};

#endif  // ESCAPEPARSER_H
