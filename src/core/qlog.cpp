#include "core/qlog.h"
#include <QDateTime>
#include <iostream>

// ---------------------------------------------------------------------------
// Optional native systemd journal integration (libsystemd).
// When CMake finds libsystemd and sets AETHER_HAVE_SYSTEMD, we use
// sd_journal_send() for fully structured key=value journal entries and call
// sd_notify() to signal readiness to the service manager.  When libsystemd is
// absent (Windows, macOS, or stripped-down Linux images) we fall back to the
// plain syslog(3) API on POSIX and a no-op on Windows.
// ---------------------------------------------------------------------------
#ifdef AETHER_HAVE_SYSTEMD
#include <systemd/sd-journal.h>
#include <systemd/sd-daemon.h>
#define AETHER_JOURNAL 1
#elif defined(Q_OS_UNIX)
#include <syslog.h>
#define AETHER_SYSLOG 1
#endif

namespace tftp {

namespace {

void qlogMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
    // Map Qt message type → systemd/syslog priority level + stderr prefix
    int priority = 6;  // LOG_INFO
    const char *levelStr = "INFO";
    const char *sdPrefix = "<6>";  // systemd journal priority prefix for stderr

    switch (type) {
        case QtDebugMsg:
            priority = 7;
            levelStr = "DEBUG";
            sdPrefix = "<7>";
            break;
        case QtInfoMsg:
            priority = 6;
            levelStr = "INFO";
            sdPrefix = "<6>";
            break;
        case QtWarningMsg:
            priority = 4;
            levelStr = "WARNING";
            sdPrefix = "<4>";
            break;
        case QtCriticalMsg:
            priority = 3;
            levelStr = "CRITICAL";
            sdPrefix = "<3>";
            break;
        case QtFatalMsg:
            priority = 2;
            levelStr = "FATAL";
            sdPrefix = "<2>";
            break;
    }

    const QByteArray utf8Msg = msg.toUtf8();
    const QByteArray category = context.category ? QByteArray(context.category) : QByteArray("default");
    const QByteArray timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toUtf8();

    // -----------------------------------------------------------------------
    // 1. stderr — with systemd journal priority prefix so that journald picks
    //    up the level when the service runs with StandardError=journal.
    // -----------------------------------------------------------------------
    std::cerr << sdPrefix << '[' << timestamp.constData() << ']' << " [" << levelStr << ']' << " [" << category.constData() << "] "
              << utf8Msg.constData() << '\n';

    // -----------------------------------------------------------------------
    // 2a. Native sd_journal_send — structured key=value entries visible in
    //     `journalctl -o json` / `journalctl -f` with full field filtering.
    // -----------------------------------------------------------------------
#ifdef AETHER_JOURNAL
    sd_journal_send("MESSAGE=%s", utf8Msg.constData(), "PRIORITY=%d", priority, "SYSLOG_IDENTIFIER=%s", "AetherTFTP", "AETHER_CATEGORY=%s",
                    category.constData(), "AETHER_LEVEL=%s", levelStr, "AETHER_TIMESTAMP=%s", timestamp.constData(), nullptr);

    // -----------------------------------------------------------------------
    // 2b. Fallback: plain POSIX syslog (libsystemd not available)
    // -----------------------------------------------------------------------
#elif defined(AETHER_SYSLOG)
    syslog(priority, "[%s] %s", category.constData(), utf8Msg.constData());
#endif

    if (type == QtFatalMsg) {
        abort();
    }
}

}  // namespace

void installQLog() {
#ifdef AETHER_SYSLOG
    openlog("AetherTFTP", LOG_PID | LOG_NDELAY, LOG_USER);
#endif
    qInstallMessageHandler(qlogMessageHandler);
}

/**
 * @brief Signal service-ready to the systemd service manager.
 *
 * Call this once the server socket is bound and listening.
 * On non-systemd builds this is a no-op.
 */
void notifyReady() {
#ifdef AETHER_JOURNAL
    sd_notify(0, "READY=1");
#endif
}

/**
 * @brief Signal watchdog keep-alive to the systemd service manager.
 *
 * Call periodically when WatchdogSec= is set in the unit file.
 */
void notifyWatchdog() {
#ifdef AETHER_JOURNAL
    sd_notify(0, "WATCHDOG=1");
#endif
}

}  // namespace tftp
