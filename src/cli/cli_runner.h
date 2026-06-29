#pragma once

#include <QStringList>
#include <QtGlobal>

class QCoreApplication;
class QCommandLineParser;

namespace tftp {

/**
 * @brief QCommandLineParser-based headless front-end.
 *
 * Parses command-line arguments and executes the requested headless action:
 * run a server (@c --server), or perform a download (@c --get) / upload
 * (@c --put). The CLI is the default mode; the GUI is opt-in via
 * @c --gui (see @ref wantsGui()).
 */
class CliRunner {
public:
    /**
     * @brief Parse arguments and execute the requested operation.
     * @param app The application whose event loop is used for async transfers.
     * @return A process exit code (0 on success).
     */
    static int run(QCoreApplication &app);

    /**
     * @brief Detect whether the GUI was requested.
     * @param args The raw argument list.
     * @return @c true if @c --gui is present; @c false runs the CLI.
     */
    static bool wantsGui(const QStringList &args);

private:
    /**
     * @brief Run the headless server until interrupted.
     * @param parser The processed command-line parser.
     * @param port UDP port to bind.
     * @return A process exit code.
     */
    static int runServer(QCommandLineParser &parser, quint16 port);

    /**
     * @brief Perform a single client transfer (put or get).
     * @param parser The processed command-line parser.
     * @param port Server port to connect to.
     * @param isPut @c true for upload (WRQ); @c false for download (RRQ).
     * @return A process exit code.
     */
    static int runTransfer(QCommandLineParser &parser, quint16 port,
                           bool isPut);
};

}  // namespace tftp
