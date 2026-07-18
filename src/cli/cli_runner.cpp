#include "cli/cli_runner.h"

#include "core/tftp_client.h"
#include "core/tftp_protocol.h"
#include "core/tftp_server.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QHostAddress>
#include <QTextStream>

namespace tftp {

namespace {

QTextStream &out() {
    static QTextStream s(stdout);
    return s;
}
QTextStream &err() {
    static QTextStream s(stderr);
    return s;
}

}  // namespace

bool CliRunner::wantsGui(const QStringList &args) {
    // Dispatch rule: the GUI is the default when launched with
    // no arguments, and an explicit --gui always forces it. Any other arguments
    // (--server / --get / --put / --help / …) run headless in CLI mode.
    if (args.contains(QStringLiteral("--gui")))
        return true;
    return args.size() <= 1;  // program name only : no arguments supplied.
}

int CliRunner::run(QCoreApplication &app, const QStringList &args) {
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("AetherTFTP — cross-platform TFTP client/server"));
    parser.addHelpOption();
    parser.addVersionOption();

    parser.addOptions({
        {QStringLiteral("server"), QStringLiteral("Run in headless server mode.")},
        {QStringLiteral("put"), QStringLiteral("Upload a file to a server (WRQ).")},
        {QStringLiteral("get"), QStringLiteral("Download a file from a server (RRQ).")},
        {QStringLiteral("gui"), QStringLiteral("Open the graphical interface.")},
        {{QStringLiteral("H"), QStringLiteral("host")}, QStringLiteral("Server host (client modes)."), QStringLiteral("host")},
        {{QStringLiteral("p"), QStringLiteral("port")}, QStringLiteral("Port (default 69)."), QStringLiteral("port")},
        {{QStringLiteral("d"), QStringLiteral("dir")}, QStringLiteral("Served directory (server mode)."), QStringLiteral("dir")},
        {{QStringLiteral("f"), QStringLiteral("file")}, QStringLiteral("File name to transfer (client modes)."), QStringLiteral("file")},
        {{QStringLiteral("o"), QStringLiteral("output")},
         QStringLiteral("Local save path (get mode; default current dir)."),
         QStringLiteral("output")},
        {{QStringLiteral("b"), QStringLiteral("blocksize")},
         QStringLiteral("Block size (RFC 2348, 8-65464)."),
         QStringLiteral("blocksize")},
        {{QStringLiteral("t"), QStringLiteral("timeout")},
         QStringLiteral("Per-block timeout in seconds (RFC 2349, default 5)."),
         QStringLiteral("timeout")},
        {QStringLiteral("bind"), QStringLiteral("Server bind address (default 0.0.0.0)."), QStringLiteral("address")},
    });

    QStringList argsToParse = args;
    if (argsToParse.isEmpty()) {
        argsToParse = QCoreApplication::arguments();
    } else {
        argsToParse.prepend(QStringLiteral("AetherTFTP"));
    }
    parser.process(argsToParse);

    const quint16 port =
        parser.isSet(QStringLiteral("port")) ? quint16(parser.value(QStringLiteral("port")).toUInt()) : quint16(kDefaultPort);

    if (parser.isSet(QStringLiteral("server")))
        return runServer(parser, port);

    const bool isPut = parser.isSet(QStringLiteral("put"));
    const bool isGet = parser.isSet(QStringLiteral("get"));
    if (isPut && isGet) {
        err() << "Error: --put and --get are mutually exclusive\n";
        err().flush();
        return 2;
    }
    if (isPut || isGet)
        return runTransfer(parser, port, isPut);

    err() << "Error: no action specified (use --server, --put, or --get)\n";
    err().flush();
    return 2;
}

int CliRunner::runServer(QCommandLineParser &parser, quint16 port) {
    const QString dir = parser.isSet(QStringLiteral("dir")) ? parser.value(QStringLiteral("dir")) : QStringLiteral(".");
    const QString bindAddr = parser.isSet(QStringLiteral("bind")) ? parser.value(QStringLiteral("bind")) : QStringLiteral("0.0.0.0");

    TftpServer server;
    QObject::connect(&server, &TftpServer::logMessage, [](const QString &m) {
        out() << "[server] " << m << "\n";
        out().flush();
    });

    if (!server.listen(QHostAddress(bindAddr), port, dir)) {
        err() << "Error: " << server.lastError() << "\n";
        err().flush();
        return 1;
    }
    out() << "AetherTFTP server running on " << bindAddr << ":" << port << " (Ctrl+C to stop)\n";
    out().flush();

    // Run until interrupted.
    return QCoreApplication::exec();
}

int CliRunner::runTransfer(QCommandLineParser &parser, quint16 port, bool isPut) {
    if (!parser.isSet(QStringLiteral("host"))) {
        err() << "Error: --host is required for client transfers\n";
        err().flush();
        return 2;
    }
    if (!parser.isSet(QStringLiteral("file"))) {
        err() << "Error: --file is required for client transfers\n";
        err().flush();
        return 2;
    }

    const QString host = parser.value(QStringLiteral("host"));
    const QString file = parser.value(QStringLiteral("file"));
    const bool isPipe = (file == QLatin1String("-") ||
                         (parser.isSet(QStringLiteral("output")) && parser.value(QStringLiteral("output")) == QLatin1String("-")));

    TftpClient client;
    if (parser.isSet(QStringLiteral("blocksize")))
        client.setBlockSize(parser.value(QStringLiteral("blocksize")).toInt());
    if (parser.isSet(QStringLiteral("timeout")))
        client.setTimeout(parser.value(QStringLiteral("timeout")).toInt() * 1000);

    QObject::connect(&client, &TftpClient::errorOccurred, [](const QString &m) {
        err() << "Error: " << m << "\n";
        err().flush();
    });
    QObject::connect(&client, &TftpClient::progress, [isPipe](qint64 done, qint64 total) {
        QTextStream &stream = isPipe ? err() : out();
        if (total > 0)
            stream << "\r" << done << " / " << total << " bytes";
        else
            stream << "\r" << done << " bytes";
        stream.flush();
    });

    bool ok = false;
    QEventLoop loop;
    QObject::connect(&client, &TftpClient::transferFinished, [&ok, &loop](bool result) {
        ok = result;
        loop.quit();
    });

    if (isPut) {
        QString remote = QStringLiteral("upload_stdin.bin");
        if (parser.isSet(QStringLiteral("output"))) {
            remote = parser.value(QStringLiteral("output"));
        } else if (file != QLatin1String("-")) {
            remote = QFileInfo(file).fileName();
        }
        client.uploadFile(host, port, file, remote);
    } else {
        const QString output = parser.isSet(QStringLiteral("output")) ? parser.value(QStringLiteral("output")) : QStringLiteral(".");
        QString localPath = output;
        if (output != QLatin1String("-")) {
            QFileInfo outInfo(output);
            if (output.isEmpty() || outInfo.isDir() || output.endsWith(QLatin1Char('/')))
                localPath = QDir(output.isEmpty() ? QStringLiteral(".") : output).filePath(QFileInfo(file).fileName());
        }
        client.downloadFile(host, port, file, localPath);
    }

    loop.exec();

    QTextStream &finalStream = isPipe ? err() : out();
    finalStream << "\n" << (ok ? "Transfer complete" : "Transfer failed") << "\n";
    finalStream.flush();
    return ok ? 0 : 1;
}

}  // namespace tftp
