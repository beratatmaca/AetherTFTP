#include "core/qlog.h"
#include <QDateTime>
#include <iostream>

// ---------------------------------------------------------------------------
// Logging: stderr always, plus syslog(3) on POSIX platforms.
// ---------------------------------------------------------------------------
#if defined(Q_OS_UNIX)
#include <syslog.h>
#define AETHER_SYSLOG 1
#endif

namespace tftp {

namespace {

void qlogMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
    // Map Qt message type → syslog priority level
    int priority;
    const char *levelStr;

    switch (type) {
        case QtDebugMsg:
            priority = 7;
            levelStr = "DEBUG";
            break;
        case QtInfoMsg:
            priority = 6;
            levelStr = "INFO";
            break;
        case QtWarningMsg:
            priority = 4;
            levelStr = "WARNING";
            break;
        case QtCriticalMsg:
            priority = 3;
            levelStr = "CRITICAL";
            break;
        case QtFatalMsg:
            priority = 2;
            levelStr = "FATAL";
            break;
    }

    const QByteArray utf8Msg = msg.toUtf8();
    const QByteArray category = context.category ? QByteArray(context.category) : QByteArray("default");
    const QByteArray timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toUtf8();

    // -----------------------------------------------------------------------
    // 1. stderr
    // -----------------------------------------------------------------------
    std::cerr << '[' << timestamp.constData() << ']' << " [" << levelStr << ']' << " [" << category.constData() << "] "
              << utf8Msg.constData() << '\n';

    // -----------------------------------------------------------------------
    // 2. Fallback: plain POSIX syslog
    // -----------------------------------------------------------------------
#ifdef AETHER_SYSLOG
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

}  // namespace tftp
