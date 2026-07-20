#pragma once

namespace tftp {

/**
 * @brief Installs the QLog message handler.
 *
 * Intercepts all Qt log messages (qDebug, qInfo, qWarning, qCritical, qFatal),
 * formats them with a timestamp and priority level, and routes them to both
 * stderr (with systemd sd-daemon priority prefixes) and to the systemd journal
 * via sd_journal_send() when libsystemd is available, or syslog(3) otherwise.
 */
void installQLog();

/**
 * @brief Signal READY=1 to the systemd service manager.
 *
 * Call once the server socket is bound and ready to accept clients.
 * This allows the unit's Type=notify to complete start-up correctly.
 * No-op on non-Linux or when libsystemd is not present.
 */
void notifyReady();

/**
 * @brief Send a WATCHDOG=1 keep-alive ping to the systemd service manager.
 *
 * Call periodically (at least twice per WatchdogSec= interval) to prevent
 * the service manager from restarting the daemon due to liveness timeout.
 * No-op on non-Linux or when libsystemd is not present.
 */
void notifyWatchdog();

}  // namespace tftp
