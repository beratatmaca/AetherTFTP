#include "aether/version.h"
#include "cli/cli_runner.h"
#include "core/qlog.h"
#include "gui/mainwindow.h"
#include "gui/map_translator.h"

#include <QApplication>
#include <QIcon>
#include <QStringList>
#include <QSettings>
#include <QLocale>

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

#ifdef Q_OS_WIN
#include <windows.h>

SERVICE_STATUS g_ServiceStatus = {0};
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
HANDLE g_ServiceStopEvent = INVALID_HANDLE_VALUE;

VOID WINAPI ServiceMain(DWORD argc, LPTSTR *argv);
VOID WINAPI ServiceCtrlHandler(DWORD request);

VOID WINAPI ServiceMain(DWORD argc, LPTSTR *argv) {
    Q_UNUSED(argc);
    Q_UNUSED(argv);
    g_StatusHandle = RegisterServiceCtrlHandlerW(L"AetherTFTP", ServiceCtrlHandler);
    if (g_StatusHandle == NULL) {
        return;
    }

    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (g_ServiceStopEvent == NULL) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }

    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    int fakeArgc = 2;
    char *fakeArgv[] = {(char *)"aethertftp.exe", (char *)"--server", NULL};

    QCoreApplication app(fakeArgc, fakeArgv);
    QCoreApplication::setApplicationName(QStringLiteral("AetherTFTP"));
    QCoreApplication::setApplicationVersion(QStringLiteral(AETHER_VERSION_STRING));

    tftp::CliRunner::run(app, {QStringLiteral("aethertftp.exe"), QStringLiteral("--server")});

    CloseHandle(g_ServiceStopEvent);
    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

VOID WINAPI ServiceCtrlHandler(DWORD request) {
    switch (request) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
            SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
            QCoreApplication::quit();
            break;
        default:
            break;
    }
}
#endif

int main(int argc, char *argv[]) {
    tftp::installQLog();

    // arguments() needs a live application; build the raw list manually so we
    // can choose Core-vs-GUI before constructing the application object.
    QStringList rawArgs;
    rawArgs.reserve(argc);
    for (int i = 0; i < argc; ++i)
        rawArgs << QString::fromLocal8Bit(argv[i]);

#ifdef Q_OS_WIN
    if (rawArgs.contains(QStringLiteral("--service"))) {
        SERVICE_TABLE_ENTRYW ServiceTable[] = {{(LPWSTR)L"AetherTFTP", (LPSERVICE_MAIN_FUNCTIONW)ServiceMain}, {NULL, NULL}};
        if (StartServiceCtrlDispatcherW(ServiceTable)) {
            return 0;
        }
        return 1;
    }
#endif

    if (tftp::CliRunner::wantsGui(rawArgs)) {
        // The GUI resources (themes, icon) live in the aether_gui static
        // library; force their initialiser to link in (Qt static-lib qrc).
        Q_INIT_RESOURCE(resources);

        QApplication app(argc, argv);
        QApplication::setOrganizationName(QStringLiteral("AetherTFTP Project"));
        QApplication::setApplicationName(QStringLiteral("AetherTFTP"));
        QApplication::setApplicationVersion(QStringLiteral(AETHER_VERSION_STRING));
        QApplication::setWindowIcon(QIcon(QStringLiteral(":/aether/icon.ico")));

        QSettings settings;
        QString lang = settings.value(QStringLiteral("general/language"), QStringLiteral("system")).toString();
        if (lang != QStringLiteral("system")) {
            auto *translator = tftp::gui::MapTranslator::create(lang, &app);
            app.installTranslator(translator);
        } else {
            QString systemLang = QLocale::system().name().left(2);
            if (systemLang == QStringLiteral("de") || systemLang == QStringLiteral("tr") || systemLang == QStringLiteral("es")) {
                auto *translator = tftp::gui::MapTranslator::create(systemLang, &app);
                app.installTranslator(translator);
            }
        }

        tftp::gui::MainWindow window;
        window.show();
        return QApplication::exec();
    }

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("AetherTFTP"));
    QCoreApplication::setApplicationVersion(QStringLiteral(AETHER_VERSION_STRING));
    return tftp::CliRunner::run(app);
}
