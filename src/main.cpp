#include "cli/cli_runner.h"
#include "gui/mainwindow.h"

#include <QApplication>
#include <QFile>
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

namespace {

/** @brief Apply the bundled brand accent stylesheet to the application. */
void applyTheme(QApplication &app) {
    QFile qss(QStringLiteral(":/aether/theme.qss"));
    if (qss.open(QIODevice::ReadOnly | QIODevice::Text))
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));
}

}  // namespace

int main(int argc, char *argv[]) {
    // arguments() needs a live application; build the raw list manually so we
    // can choose Core-vs-GUI before constructing the application object.
    QStringList rawArgs;
    rawArgs.reserve(argc);
    for (int i = 0; i < argc; ++i)
        rawArgs << QString::fromLocal8Bit(argv[i]);

    if (tftp::CliRunner::wantsGui(rawArgs)) {
        QApplication app(argc, argv);
        QApplication::setApplicationName(QStringLiteral("AetherTFTP"));
        QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
        applyTheme(app);

        tftp::gui::MainWindow window;
        window.show();
        return QApplication::exec();
    }

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("AetherTFTP"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    return tftp::CliRunner::run(app);
}
