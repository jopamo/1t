#ifndef ONETERM_H
#define ONETERM_H

#include <QWidget>
#include <QSocketNotifier>
#include <QVBoxLayout>
#include <QScrollArea>
#include <memory>
#include <sys/types.h>

#include "escapeparser.h"
#include "terminalwidget.h"

class OneTerm : public QWidget {
    Q_OBJECT

   public:
    explicit OneTerm(QWidget* parent = nullptr);
    ~OneTerm() override;

    void launchShell(const char* shellPath);

   private slots:
    void readFromPty();

   private:
    void resizeEvent(QResizeEvent* event) override;

    QVBoxLayout* m_layout;
    TerminalWidget* m_terminalWidget;
    EscapeSequenceParser* m_parser;

    std::unique_ptr<QSocketNotifier> m_notifier;
    int m_masterFD;
    pid_t m_shellPid;
};

#endif
