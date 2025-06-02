#include "1t.h"
#include "terminalwidget.h"
#include "escapeparser.h"
#include "debug.h"

#include <QApplication>
#include <QIcon>
#include <QSocketNotifier>
#include <QVBoxLayout>
#include <QResizeEvent>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

bool g_debugMode = false;

OneTerm::OneTerm(QWidget* parent)
    : QWidget(parent),
      m_terminalWidget(new TerminalWidget(this)),
      m_parser(new EscapeSequenceParser(m_terminalWidget, this)) {
    setWindowTitle(QStringLiteral("1t"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 10);
    layout->addWidget(m_terminalWidget);
}

OneTerm::~OneTerm() {
    if (m_notifier) {
        m_notifier->setEnabled(false);
    }

    if (m_masterFD >= 0) {
#ifdef ENABLE_DEBUG
        DBG() << "Closing master FD:" << m_masterFD;
#endif
        ::close(m_masterFD);
    }

    if (m_shellPid > 0) {
#ifdef ENABLE_DEBUG
        DBG() << "Waiting on shell PID:" << m_shellPid;
#endif
        ::waitpid(m_shellPid, nullptr, WNOHANG);
    }
}

void OneTerm::launchShell(const char* shellPath) {
    int masterFD, slaveFD;
    if (openpty(&masterFD, &slaveFD, nullptr, nullptr, nullptr) < 0) {
        qWarning() << "openpty failed:" << strerror(errno);
        return;
    }
#ifdef ENABLE_DEBUG
    DBG() << "openpty master FD:" << masterFD << "slave FD:" << slaveFD;
#endif

    fcntl(masterFD, F_SETFL, fcntl(masterFD, F_GETFL) | O_NONBLOCK);

    pid_t pid = fork();
    if (pid < 0) {
        qWarning() << "fork failed:" << strerror(errno);
        ::close(masterFD);
        ::close(slaveFD);
        return;
    }
    if (pid == 0) {
        ::close(masterFD);
        setsid();
        if (ioctl(slaveFD, TIOCSCTTY, 0) < 0) {
            qWarning() << "Failed to set controlling terminal:" << strerror(errno);
            _exit(127);
        }
        dup2(slaveFD, STDIN_FILENO);
        dup2(slaveFD, STDOUT_FILENO);
        dup2(slaveFD, STDERR_FILENO);
        ::close(slaveFD);

        ::setenv("TERM", "xterm-256color", 0);

        if (execl(shellPath, shellPath, "-i", static_cast<char*>(nullptr)) == -1) {
            qWarning() << "execl failed:" << strerror(errno);
            _exit(127);
        }
    }

    ::close(slaveFD);
    m_shellPid = pid;
    m_masterFD = masterFD;

#ifdef ENABLE_DEBUG
    DBG() << "Launched shell PID:" << m_shellPid << "masterFD:" << m_masterFD;
#endif

    m_notifier = std::make_unique<QSocketNotifier>(m_masterFD, QSocketNotifier::Read, this);
    connect(m_notifier.get(), &QSocketNotifier::activated, this, [this] { readFromPty(); });

    m_terminalWidget->setPtyInfo(m_masterFD, m_shellPid);
}

void OneTerm::readFromPty() {
    if (m_masterFD < 0)
        return;

    char buf[4096];
    for (;;) {
        ssize_t n = ::read(m_masterFD, buf, sizeof(buf));
        if (n > 0) {
#ifdef ENABLE_DEBUG
            DBG() << "readFromPty got" << n << "bytes";
#endif
            m_parser->feed(QByteArray(buf, n));
        }
        else if (n == 0) {
#ifdef ENABLE_DEBUG
            DBG() << "PTY EOF, waiting on shell...";
#endif
            ::waitpid(m_shellPid, nullptr, 0);
            m_notifier->setEnabled(false);
            break;
        }
        else if (errno != EAGAIN && errno != EINTR) {
            qWarning() << "read() failed:" << strerror(errno);
            m_notifier->setEnabled(false);
            break;
        }
        else {
            break;
        }
    }
}

void OneTerm::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
#ifdef ENABLE_DEBUG
    DBG() << "OneTerm resized:" << e->size();
#endif
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("1t");
    app.setOrganizationName("MyOrg");

    app.setWindowIcon(QIcon(QStringLiteral("/usr/share/icons/hicolor/256x256/apps/1t.png")));

#ifdef ENABLE_DEBUG
    g_debugMode = true;
    qCDebug(oneTermDbg) << "Debugging enabled";
    QLoggingCategory::setFilterRules("1t.debug=true");
#else
    g_debugMode = false;
#endif

    OneTerm term;
    term.resize(1200, 300);
    term.show();

#ifdef ENABLE_DEBUG
    DBG() << "Launching shell path:" << "/bin/bash";
#endif
    term.launchShell("/bin/bash");

    return app.exec();
}
