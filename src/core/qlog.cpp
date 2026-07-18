#include "core/qlog.h"
#include <QDateTime>
#include <QTextStream>
#include <iostream>

#ifdef Q_OS_UNIX
#include <syslog.h>
#endif

namespace tftp {

namespace {

void qlogMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
    QString levelStr;
    int syslogPriority = 6;  // LOG_INFO default
    QString systemdPrefix;

    switch (type) {
        case QtDebugMsg:
            levelStr = QStringLiteral("DEBUG");
            syslogPriority = 7;  // LOG_DEBUG
            systemdPrefix = QStringLiteral("<7>");
            break;
        case QtInfoMsg:
            levelStr = QStringLiteral("INFO");
            syslogPriority = 6;  // LOG_INFO
            systemdPrefix = QStringLiteral("<6>");
            break;
        case QtWarningMsg:
            levelStr = QStringLiteral("WARNING");
            syslogPriority = 4;  // LOG_WARNING
            systemdPrefix = QStringLiteral("<4>");
            break;
        case QtCriticalMsg:
            levelStr = QStringLiteral("CRITICAL");
            syslogPriority = 3;  // LOG_ERR
            systemdPrefix = QStringLiteral("<3>");
            break;
        case QtFatalMsg:
            levelStr = QStringLiteral("FATAL");
            syslogPriority = 2;  // LOG_CRIT
            systemdPrefix = QStringLiteral("<2>");
            break;
    }

    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz"));
    const QString category = context.category ? QString::fromUtf8(context.category) : QStringLiteral("default");

    // Format for console (stdout/stderr)
    const QString formattedMsg = QStringLiteral("[%1] [%2] [%3] %4").arg(timestamp, levelStr, category, msg);

    // 1. Output to standard error (stderr) to protect stdout from being corrupted during pipeline redirections
    std::cerr << qPrintable(systemdPrefix) << qPrintable(formattedMsg) << std::endl;

    // 2. Output to journalctl / syslog (Linux/Unix only)
#ifdef Q_OS_UNIX
    syslog(syslogPriority, "%s", qPrintable(QStringLiteral("[%1] %2").arg(category, msg)));
#endif

    if (type == QtFatalMsg) {
        abort();
    }
}

}  // namespace

void installQLog() {
#ifdef Q_OS_UNIX
    openlog("AetherTFTP", LOG_PID | LOG_NDELAY, LOG_USER);
#endif
    qInstallMessageHandler(qlogMessageHandler);
}

}  // namespace tftp
