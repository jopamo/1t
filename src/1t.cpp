#include "1t.h"

#include <QApplication>
#include <QIcon>
#include <QScrollArea>
#include <QDebug>
#include <QSocketNotifier>

#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <memory>

OneTerm::OneTerm(QWidget* parent)
    : QWidget(parent),
      m_terminalWidget(new TerminalWidget(this)),
      m_parser(new EscapeSequenceParser(m_terminalWidget, this)) {
    setWindowTitle("1t");

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 10);
    m_layout->setSpacing(0);

    m_layout->addWidget(m_terminalWidget);
}

void OneTerm::launchShell(const char* shellPath) {
    int masterFD, slaveFD;
    if (openpty(&masterFD, &slaveFD, nullptr, nullptr, nullptr) < 0) {
        qWarning() << "openpty failed:" << strerror(errno);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        qWarning() << "fork failed:" << strerror(errno);
        ::close(masterFD);
        ::close(slaveFD);
        return;
    }
    else if (pid == 0) {
        ::close(masterFD);
        setsid();
        ioctl(slaveFD, TIOCSCTTY, 0);

        if (dup2(slaveFD, STDIN_FILENO) == -1 || dup2(slaveFD, STDOUT_FILENO) == -1 ||
            dup2(slaveFD, STDERR_FILENO) == -1) {
            qWarning() << "Failed to duplicate file descriptors:" << strerror(errno);
            _exit(127);
        }
        ::close(slaveFD);

        execl(shellPath, shellPath, (char*)nullptr);
        qWarning() << "execl failed:" << strerror(errno);
        _exit(127);
    }
    else {
        ::close(slaveFD);
        m_shellPid = pid;
        m_masterFD = masterFD;

        m_notifier = std::make_unique<QSocketNotifier>(masterFD, QSocketNotifier::Read, this);

        connect(m_notifier.get(), &QSocketNotifier::activated, this, [this](int) { readFromPty(); });

        m_terminalWidget->setPtyInfo(masterFD, pid);
    }
}

void OneTerm::readFromPty() {
    char buf[4096];
    ssize_t n = ::read(m_masterFD, buf, sizeof(buf));
    if (n > 0) {
        QByteArray chunk(buf, n);
        m_parser->feed(chunk);
    }
    else {
        if (n == 0) {
            ::waitpid(m_shellPid, nullptr, 0);
            m_notifier->setEnabled(false);
        }
        else {
        }
    }
}

void OneTerm::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    app.setWindowIcon(QIcon("/usr/share/icons/hicolor/256x256/apps/1t.png"));

    OneTerm term;
    term.resize(1200, 300);
    term.show();
    term.launchShell("/bin/bash");

    return app.exec();
}
