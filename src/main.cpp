#include "aether/version.h"
#include "cli/cli_runner.h"
#include "gui/mainwindow.h"

#include <QApplication>
#include <QStringList>

/**
 * @file main.cpp
 * @brief AetherTFTP entry point and CLI/GUI launch dispatch.
 *
 * Dispatch rule:
 *   - No arguments            : launch the Qt6 GUI (QApplication).
 *   - Action args (--server/  : run headless in CLI mode (QCoreApplication).
 *     --get/--put/--help/…)
 *   - Explicit --gui          : force the GUI regardless of other arguments.
 */

int main(int argc, char *argv[]) {
    // arguments() needs a live application; build the raw list manually so we
    // can choose Core-vs-GUI before constructing the application object.
    QStringList rawArgs;
    rawArgs.reserve(argc);
    for (int i = 0; i < argc; ++i)
        rawArgs << QString::fromLocal8Bit(argv[i]);

    if (tftp::CliRunner::wantsGui(rawArgs)) {
        // The GUI resources (themes, icon) live in the aether_gui static
        // library; force their initialiser to link in (Qt static-lib qrc).
        Q_INIT_RESOURCE(resources);

        QApplication app(argc, argv);
        QApplication::setOrganizationName(QStringLiteral("AetherTFTP Project"));
        QApplication::setApplicationName(QStringLiteral("AetherTFTP"));
        QApplication::setApplicationVersion(QStringLiteral(AETHER_VERSION_STRING));

        tftp::gui::MainWindow window;
        window.show();
        return QApplication::exec();
    }

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("AetherTFTP"));
    QCoreApplication::setApplicationVersion(QStringLiteral(AETHER_VERSION_STRING));
    return tftp::CliRunner::run(app);
}
