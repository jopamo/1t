#include "1t.h"
#include "terminalwidget.h"
#include "escapeparser.h"

#include <QApplication>
#include <QIcon>
#include <QScrollArea>
#include <QDebug>
#include <QSocketNotifier>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QCommandLineParser>
#include <QCommandLineOption>

#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <memory>
#include <fcntl.h>

static bool g_debugMode = false;

OneTerm::OneTerm(QWidget* parent)
    : QWidget(parent),
      m_terminalWidget(new TerminalWidget(this)),
      m_parser(new EscapeSequenceParser(m_terminalWidget, this)),
      m_masterFD(-1),
      m_shellPid(-1) {
    setWindowTitle(QStringLiteral("1t"));

    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 10);
    m_layout->setSpacing(0);

    m_layout->addWidget(m_terminalWidget);
}

OneTerm::~OneTerm() {
    if (m_notifier) {
        m_notifier->setEnabled(false);
    }
    if (m_masterFD >= 0) {
        if (g_debugMode) {
            qDebug() << "[DEBUG] Closing master FD:" << m_masterFD;
        }
        ::close(m_masterFD);
        m_masterFD = -1;
    }
    if (m_shellPid > 0) {
        if (g_debugMode) {
            qDebug() << "[DEBUG] Waiting on shell pid:" << m_shellPid;
        }

        ::waitpid(m_shellPid, nullptr, WNOHANG);
        m_shellPid = -1;
    }
}

void OneTerm::launchShell(const char* shellPath) {
    int masterFD = -1;
    int slaveFD = -1;

    if (openpty(&masterFD, &slaveFD, nullptr, nullptr, nullptr) < 0) {
        qWarning() << "openpty failed:" << strerror(errno);
        return;
    }
    if (g_debugMode) {
        qDebug() << "[DEBUG] openpty master FD:" << masterFD << "slave FD:" << slaveFD;
    }

    int flags = fcntl(masterFD, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(masterFD, F_SETFL, flags | O_NONBLOCK);
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
        if (ioctl(slaveFD, TIOCSCTTY, 0) < 0) {
            qWarning() << "ioctl(TIOCSCTTY) failed:" << strerror(errno);
            _exit(127);
        }

        if (dup2(slaveFD, STDIN_FILENO) == -1 || dup2(slaveFD, STDOUT_FILENO) == -1 ||
            dup2(slaveFD, STDERR_FILENO) == -1) {
            qWarning() << "Failed to dup2:" << strerror(errno);
            _exit(127);
        }
        ::close(slaveFD);

        execl(shellPath, shellPath, "-i", static_cast<char*>(nullptr));
        qWarning() << "execl failed:" << strerror(errno);
        _exit(127);
    }
    else {
        ::close(slaveFD);
        m_shellPid = pid;
        m_masterFD = masterFD;

        if (g_debugMode) {
            qDebug() << "[DEBUG] Launched shell PID:" << m_shellPid << "masterFD:" << m_masterFD;
        }

        m_notifier = std::make_unique<QSocketNotifier>(m_masterFD, QSocketNotifier::Read, this);
        connect(m_notifier.get(), &QSocketNotifier::activated, this, [this](int) { readFromPty(); });

        m_terminalWidget->setPtyInfo(m_masterFD, m_shellPid);
    }
}

void OneTerm::readFromPty() {
    if (m_masterFD < 0) {
        return;
    }

    while (true) {
        char buf[4096];
        ssize_t n = ::read(m_masterFD, buf, sizeof(buf));
        if (n > 0) {
            if (g_debugMode) {
                qDebug() << "[DEBUG] readFromPty got" << n << "bytes";
            }
            QByteArray chunk(buf, n);
            m_parser->feed(chunk);
        }
        else if (n == 0) {
            if (g_debugMode) {
                qDebug() << "[DEBUG] PTY EOF, waiting on shell...";
            }
            ::waitpid(m_shellPid, nullptr, 0);
            m_notifier->setEnabled(false);
            break;
        }
        else {
            if (errno == EAGAIN || errno == EINTR) {
                break;
            }

            qWarning() << "read() failed:" << strerror(errno);
            m_notifier->setEnabled(false);
            break;
        }
    }
}

void OneTerm::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (g_debugMode) {
        qDebug() << "[DEBUG] OneTerm resized:" << event->size();
    }
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("1t");
    app.setOrganizationName("MyOrg");

    QCommandLineParser parser;
    parser.setApplicationDescription("1t: a minimal PTY-based terminal widget");
    parser.addHelpOption();

    QCommandLineOption debugOption(QStringList() << "D" << "debug", "Enable debug logging.");
    parser.addOption(debugOption);

    QCommandLineOption customHelpOption(QStringList() << "H" << "help", "Show this help text.");
    parser.addOption(customHelpOption);

    parser.process(app);

    if (parser.isSet(customHelpOption)) {
        parser.showHelp(0);
    }

    if (parser.isSet(debugOption)) {
        g_debugMode = true;
        qDebug() << "[DEBUG] Debug mode enabled";
    }

    app.setWindowIcon(QIcon(QStringLiteral("/usr/share/icons/hicolor/256x256/apps/1t.png")));

    OneTerm term;
    term.resize(1200, 300);
    term.show();

    const char* shellPath = "/bin/bash";
    if (g_debugMode) {
        qDebug() << "[DEBUG] Launching shell path:" << shellPath;
    }
    term.launchShell(shellPath);

    return app.exec();
}
