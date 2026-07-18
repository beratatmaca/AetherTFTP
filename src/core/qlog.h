#pragma once

namespace tftp {

/**
 * @brief Installs the QLog message handler.
 *
 * Intercepts all Qt log messages (qDebug, qInfo, qWarning, qCritical, qFatal),
 * formats them with a timestamp and priority level, and routes them to both
 * stdout/stderr (with systemd priority prefixes) and to journalctl (via POSIX
 * syslog on Linux/Unix systems).
 */
void installQLog();

}  // namespace tftp
