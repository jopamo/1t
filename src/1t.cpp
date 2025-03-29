#include "1t.h"
#include "mousehandler.h"

#include <QApplication>
#include <QDebug>
#include <QIcon>

#include <unistd.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>

bool g_debug = false;

void parseArguments(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (QString(argv[i]) == "--debug" || QString(argv[i]) == "-D") {
            g_debug = true;
            qDebug() << "Debug mode enabled.";
        }
    }
}

OneTerm::OneTerm(QWidget* parent)
    : QWidget(parent),
      m_terminalWidget(new TerminalWidget(this)),
      m_parser(new EscapeSequenceParser(m_terminalWidget, this)) {
    if (g_debug) {
        qDebug() << "OneTerm constructor called";
    }

    setWindowTitle("1t");

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);

    QScrollArea* scrollArea = new QScrollArea(this);
    scrollArea->setWidget(m_terminalWidget);
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    m_layout->addWidget(scrollArea);

    updateTerminalSize();

    if (g_debug) {
        qDebug() << "OneTerm initialization complete";
    }
}

void OneTerm::launchShell(const char* shellPath) {
    if (g_debug) {
        qDebug() << "launchShell called with shellPath:" << shellPath;
    }

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
        connect(m_notifier.get(), &QSocketNotifier::activated, this, &OneTerm::readFromPty);

        m_terminalWidget->setPtyInfo(masterFD, pid);
    }
}

void OneTerm::readFromPty() {
    if (g_debug) {
        qDebug() << "readFromPty called";
    }

    char buf[4096];
    ssize_t n = ::read(m_masterFD, buf, sizeof(buf));
    if (n > 0) {
        QByteArray chunk(buf, n);
        m_parser->feed(chunk);

        if (g_debug) {
            qDebug() << "Read" << n << "bytes from PTY";
        }
    }
    else {
        if (n == 0) {
            ::waitpid(m_shellPid, nullptr, 0);
            m_notifier->setEnabled(false);

            if (g_debug) {
                qDebug() << "Child process finished. PTY closed.";
            }
        }
    }
}

void OneTerm::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateTerminalSize();
    if (g_debug) {
        qDebug() << "Window resized, updated terminal size";
    }
}

void OneTerm::updateTerminalSize() {
    int rows = height() / m_terminalWidget->charHeight();
    int cols = width() / m_terminalWidget->charWidth();

    m_terminalWidget->setTerminalSize(rows, cols);
    m_terminalWidget->adjustSize();

    if (g_debug) {
        qDebug() << "Terminal size updated to" << rows << "rows x" << cols << "cols";
    }
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    parseArguments(argc, argv);

    app.setWindowIcon(QIcon("/usr/share/icons/hicolor/256x256/apps/1t.png"));

    OneTerm term;
    term.resize(800, 600);
    term.show();
    term.launchShell("/bin/bash");

    return app.exec();
}
