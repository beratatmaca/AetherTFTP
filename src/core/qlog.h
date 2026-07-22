#pragma once

namespace tftp {

/**
 * @brief Installs the QLog message handler.
 *
 * Intercepts all Qt log messages (qDebug, qInfo, qWarning, qCritical, qFatal),
 * formats them with a timestamp and priority level, and routes them to
 * stderr and to syslog(3) on POSIX platforms.
 */
void installQLog();

}  // namespace tftp
