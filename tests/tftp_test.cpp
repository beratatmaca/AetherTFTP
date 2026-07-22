#include <QtTest>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QUdpSocket>

#include <QEventLoop>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif

#include "core/metrics_exporter.h"
#include "core/pxe_proxy_dhcp.h"
#include "core/tftp_client.h"
#include "core/tftp_protocol.h"
#include "core/tftp_server.h"
#include "cli/cli_runner.h"
#include <QTranslator>

using namespace tftp;

// ---------------------------------------------------------------------------
// AetherTFTP protocol & transfer test suite.
//
// Combines pure protocol (de)serialization checks with full client<->server
// transfers over the loopback interface, per the project test plan.
// ---------------------------------------------------------------------------
class TFTPProtocolTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    // Pure protocol layer.
    void testRequestRoundTrip();
    void testDataAckRoundTrip();
    void testErrorRoundTrip();
    void testOackRoundTrip();
    void testBlockSizeClamp();

    // End-to-end loopback transfers.
    void testDownloadDefaultBlockSize();
    void testBlockSizeNegotiation();
    void testBlockSizeBoundaryNegotiation();
    void testWindowSizeNegotiation();
    void testWindowSizeBoundaryNegotiation();
    void testTimeoutOptionNegotiation();
    void testTsizeQueryOnDownload();
    void testNetasciiMode();
    void testBlockRollover();
    void testUploadRoundTrip();
    void testUploadOverwriteRejected();
    void testLargeMultiBlockTransfer();
    void testMissingFileError();
    void testPathTraversalRejected();
    void testAcls();
    void testVirtualDirectoryMapping();
    void testPskEncryption();
    void testPathSecurityPolicy();
    void testSymlinkPathTraversal();
    void testIpv6Binding();
    void testSinglePortMultiplexing();
    void testRateLimiting();
    void testSecurityThrottling();
    void testDiskSpacePreflightCheck();
    void testGranularSubnetAcls();
    void testProfilesImportExport();
    void testStructuredLogging();
    void testPrometheusExporter();
    void testMetricsExporterHardening();
    void testCliRunnerUploadDownload();
    void testCliRunnerPiping();
    void testFuzzProtocolParsing();
    void testPacketLossSimulation();
    void testTransferAbort();
    void testAgainstTftpHpa();
    void testCliInteropWithTftpHpa();
    void testRfcExtensionsAgainstCurl();
    void testProxyDhcp();
    void testEmbeddedWebDashboard();

private:
    QByteArray makePayload(int size, char seed) const;
    QString writeServerFile(const QString &name, const QByteArray &data) const;
    // Sends a raw RRQ/WRQ carrying `opts` against m_server and returns its first reply; doesn't complete the transfer.
    QByteArray rawRequestFirstReply(OpCode op, const QString &filename, const Options &opts, int timeoutMs = 3000) const;
    // Waits up to timeoutMs for a QProcess to finish, pumping the event loop (unlike QProcess::waitForFinished()).
    bool waitForProcess(QProcess &proc, int timeoutMs) const;

    TftpServer *m_server = nullptr;
    QTemporaryDir m_serverDir;  // files served by m_server.
    QTemporaryDir m_clientDir;  // destination for downloads / source for uploads.
    quint16 m_port = 0;
};

void TFTPProtocolTest::initTestCase() {
    QVERIFY(m_serverDir.isValid());
    QVERIFY(m_clientDir.isValid());
    m_server = new TftpServer(this);
    // Port 0 lets the OS pick a free port — avoids privileged-port and
    // collision problems in CI.
    QVERIFY(m_server->listen(QHostAddress(QStringLiteral("127.0.0.1")), 0, m_serverDir.path()));
    m_port = m_server->port();
    QVERIFY(m_port != 0);
}

void TFTPProtocolTest::cleanupTestCase() {
    if (m_server) {
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
}

QByteArray TFTPProtocolTest::makePayload(int size, char seed) const {
    QByteArray data(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        data[i] = char((i * 31 + seed) & 0xFF);
    return data;
}

QString TFTPProtocolTest::writeServerFile(const QString &name, const QByteArray &data) const {
    QString path = m_serverDir.path() + QLatin1Char('/') + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(data);
    f.close();
    return path;
}

QByteArray TFTPProtocolTest::rawRequestFirstReply(OpCode op, const QString &filename, const Options &opts, int timeoutMs) const {
    QUdpSocket sock;
    if (!sock.bind(QHostAddress(QStringLiteral("127.0.0.1")), 0))
        return {};

    // A blocking waitForReadyRead() would starve the event loop and stall m_server's own in-process socket, so spy on it instead.
    const QByteArray dg = buildRequest(op, filename, QStringLiteral("octet"), opts);
    QSignalSpy spy(&sock, &QUdpSocket::readyRead);
    sock.writeDatagram(dg, QHostAddress(QStringLiteral("127.0.0.1")), m_port);

    if (!spy.wait(timeoutMs))
        return {};

    QHostAddress replyAddr;
    quint16 replyPort = 0;
    QByteArray reply(int(sock.pendingDatagramSize()), Qt::Uninitialized);
    sock.readDatagram(reply.data(), reply.size(), &replyAddr, &replyPort);

    // Abort the session now instead of leaving it in m_server's active count until its own retry timeout expires.
    sock.writeDatagram(buildError(ErrorCode::NotDefined, QStringLiteral("test abort")), replyAddr, replyPort);

    return reply;
}

bool TFTPProtocolTest::waitForProcess(QProcess &proc, int timeoutMs) const {
    QEventLoop loop;
    QObject::connect(&proc, &QProcess::finished, &loop, [&loop]() { loop.quit(); });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    if (proc.state() != QProcess::NotRunning)
        loop.exec();
    return proc.state() == QProcess::NotRunning;
}

// Pure protocol tests
void TFTPProtocolTest::testRequestRoundTrip() {
    Options opts;
    opts.insert(QStringLiteral("blksize"), QStringLiteral("8192"));
    opts.insert(QStringLiteral("tsize"), QStringLiteral("0"));
    const QByteArray dg = buildRequest(OpCode::WRQ, QStringLiteral("firmware.bin"), QStringLiteral("octet"), opts);

    Request req;
    QVERIFY(parseRequest(dg, req));
    QCOMPARE(req.op, OpCode::WRQ);
    QCOMPARE(req.filename, QStringLiteral("firmware.bin"));
    QCOMPARE(req.mode, QStringLiteral("octet"));
    QCOMPARE(req.options.value(QStringLiteral("blksize")), QStringLiteral("8192"));
    QCOMPARE(req.options.value(QStringLiteral("tsize")), QStringLiteral("0"));
}

void TFTPProtocolTest::testDataAckRoundTrip() {
    const QByteArray payload = makePayload(512, 7);
    const QByteArray dg = buildData(1234, payload);
    quint16 block = 0;
    QByteArray out;
    QVERIFY(parseData(dg, block, out));
    QCOMPARE(block, quint16(1234));
    QCOMPARE(out, payload);

    quint16 ackBlock = 0;
    QVERIFY(parseAck(buildAck(4321), ackBlock));
    QCOMPARE(ackBlock, quint16(4321));
}

void TFTPProtocolTest::testErrorRoundTrip() {
    const QByteArray dg = buildError(ErrorCode::FileNotFound, QStringLiteral("nope"));
    ErrorCode code;
    QString msg;
    QVERIFY(parseError(dg, code, msg));
    QCOMPARE(code, ErrorCode::FileNotFound);
    QCOMPARE(msg, QStringLiteral("nope"));
}

void TFTPProtocolTest::testOackRoundTrip() {
    Options opts;
    opts.insert(QStringLiteral("blksize"), QStringLiteral("1428"));
    const QByteArray dg = buildOack(opts);
    Options parsed;
    QVERIFY(parseOack(dg, parsed));
    QCOMPARE(parsed.value(QStringLiteral("blksize")), QStringLiteral("1428"));
}

void TFTPProtocolTest::testBlockSizeClamp() {
    QCOMPARE(clampBlockSize(1), kMinBlockSize);
    QCOMPARE(clampBlockSize(100000), kMaxBlockSize);
    QCOMPARE(clampBlockSize(8192), 8192);
}

// End-to-end loopback tests
void TFTPProtocolTest::testDownloadDefaultBlockSize() {
    const QByteArray data = makePayload(1000, 1);  // > 1 block at 512.
    QVERIFY(!writeServerFile(QStringLiteral("small.bin"), data).isEmpty());

    TftpClient client;
    const QString outPath = m_clientDir.path() + QStringLiteral("/small.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("small.bin"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);
    QCOMPARE(client.negotiatedBlockSize(), kDefaultBlockSize);

    QFile out(outPath);
    QVERIFY(out.open(QIODevice::ReadOnly));
    QCOMPARE(out.readAll(), data);
}

void TFTPProtocolTest::testBlockSizeNegotiation() {
    const QByteArray data = makePayload(20000, 2);
    QVERIFY(!writeServerFile(QStringLiteral("test_image.bin"), data).isEmpty());

    TftpClient client;
    client.setBlockSize(8192);  // RFC 2348 option negotiation.
    const QString outPath = m_clientDir.path() + QStringLiteral("/out.bin");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("test_image.bin"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);
    QCOMPARE(client.negotiatedBlockSize(), 8192);

    QFile out(outPath);
    QVERIFY(out.open(QIODevice::ReadOnly));
    QCOMPARE(out.readAll(), data);
}

void TFTPProtocolTest::testBlockSizeBoundaryNegotiation() {
    QVERIFY(!writeServerFile(QStringLiteral("blksize_boundary.bin"), makePayload(50, 3)).isEmpty());

    // Exactly the RFC 2348 min/max must be accepted verbatim, unclamped.
    for (int value : {kMinBlockSize, kMaxBlockSize}) {
        Options opts;
        opts.insert(QStringLiteral("blksize"), QString::number(value));
        const QByteArray reply = rawRequestFirstReply(OpCode::RRQ, QStringLiteral("blksize_boundary.bin"), opts);
        QVERIFY2(!reply.isEmpty(), qPrintable(QStringLiteral("no reply for blksize=%1").arg(value)));
        Options parsed;
        QVERIFY(parseOack(reply, parsed));
        QCOMPARE(parsed.value(QStringLiteral("blksize")).toInt(), value);
    }

    // A malformed value must be ignored entirely: no OACK, plain RFC 1350 DATA block instead.
    {
        Options opts;
        opts.insert(QStringLiteral("blksize"), QStringLiteral("not-a-number"));
        const QByteArray reply = rawRequestFirstReply(OpCode::RRQ, QStringLiteral("blksize_boundary.bin"), opts);
        QVERIFY(!reply.isEmpty());
        quint16 block = 0;
        QByteArray payload;
        QVERIFY2(parseData(reply, block, payload), "expected a plain DATA block, malformed blksize should not produce an OACK");
        QCOMPARE(block, quint16(1));
    }
}

void TFTPProtocolTest::testWindowSizeNegotiation() {
    const QByteArray data = makePayload(1000, 10);
    QVERIFY(!writeServerFile(QStringLiteral("window.bin"), data).isEmpty());

    TftpClient client;
    client.setWindowSize(8);  // Request windowsize option
    const QString outPath = m_clientDir.path() + QStringLiteral("/window.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("window.bin"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);
    QCOMPARE(client.negotiatedWindowSize(), 8);

    QFile out(outPath);
    QVERIFY(out.open(QIODevice::ReadOnly));
    QCOMPARE(out.readAll(), data);
}

void TFTPProtocolTest::testWindowSizeBoundaryNegotiation() {
    QVERIFY(!writeServerFile(QStringLiteral("windowsize_boundary.bin"), makePayload(50, 9)).isEmpty());

    // Above the implementation's cap (64) must be clamped down, not rejected.
    {
        Options opts;
        opts.insert(QStringLiteral("windowsize"), QStringLiteral("999"));
        const QByteArray reply = rawRequestFirstReply(OpCode::RRQ, QStringLiteral("windowsize_boundary.bin"), opts);
        QVERIFY(!reply.isEmpty());
        Options parsed;
        QVERIFY(parseOack(reply, parsed));
        QCOMPARE(parsed.value(QStringLiteral("windowsize")).toInt(), 64);
    }

    // Zero is below the minimum of 1 and must be ignored entirely.
    {
        Options opts;
        opts.insert(QStringLiteral("windowsize"), QStringLiteral("0"));
        const QByteArray reply = rawRequestFirstReply(OpCode::RRQ, QStringLiteral("windowsize_boundary.bin"), opts);
        QVERIFY(!reply.isEmpty());
        quint16 block = 0;
        QByteArray payload;
        QVERIFY2(parseData(reply, block, payload), "expected a plain DATA block, windowsize=0 should not produce an OACK");
    }
}

void TFTPProtocolTest::testTimeoutOptionNegotiation() {
    QVERIFY(!writeServerFile(QStringLiteral("timeout_opt.bin"), makePayload(100, 55)).isEmpty());

    // A valid timeout (RFC 2349: 1-255s) is accepted and echoed back verbatim.
    {
        Options opts;
        opts.insert(QStringLiteral("timeout"), QStringLiteral("3"));
        const QByteArray reply = rawRequestFirstReply(OpCode::RRQ, QStringLiteral("timeout_opt.bin"), opts);
        QVERIFY(!reply.isEmpty());
        Options parsed;
        QVERIFY(parseOack(reply, parsed));
        QCOMPARE(parsed.value(QStringLiteral("timeout")), QStringLiteral("3"));
    }

    // Below the RFC 2349 range (< 1 second) must be ignored: no OACK at all.
    {
        Options opts;
        opts.insert(QStringLiteral("timeout"), QStringLiteral("0"));
        const QByteArray reply = rawRequestFirstReply(OpCode::RRQ, QStringLiteral("timeout_opt.bin"), opts);
        QVERIFY(!reply.isEmpty());
        quint16 block = 0;
        QByteArray payload;
        QVERIFY2(parseData(reply, block, payload), "expected a plain DATA block, timeout=0 should not produce an OACK");
    }

    // Above the RFC 2349 range (> 255 seconds) must likewise be ignored.
    {
        Options opts;
        opts.insert(QStringLiteral("timeout"), QStringLiteral("256"));
        const QByteArray reply = rawRequestFirstReply(OpCode::RRQ, QStringLiteral("timeout_opt.bin"), opts);
        QVERIFY(!reply.isEmpty());
        quint16 block = 0;
        QByteArray payload;
        QVERIFY2(parseData(reply, block, payload), "expected a plain DATA block, timeout=256 should not produce an OACK");
    }
}

void TFTPProtocolTest::testTsizeQueryOnDownload() {
    const QByteArray data = makePayload(12345, 77);
    QVERIFY(!writeServerFile(QStringLiteral("tsize_query.bin"), data).isEmpty());

    // RFC 2349: tsize=0 on a RRQ asks the server to report the real file size in the OACK.
    Options opts;
    opts.insert(QStringLiteral("tsize"), QStringLiteral("0"));
    const QByteArray reply = rawRequestFirstReply(OpCode::RRQ, QStringLiteral("tsize_query.bin"), opts);
    QVERIFY(!reply.isEmpty());
    Options parsed;
    QVERIFY(parseOack(reply, parsed));
    QCOMPARE(parsed.value(QStringLiteral("tsize")).toLongLong(), qint64(data.size()));
}

void TFTPProtocolTest::testNetasciiMode() {
    // Test native -> netascii -> native translation round-trip.
    const QByteArray localData = "Line1\nLine2\rNotNewline\nLine3\n";

    // 1. Download Test (Server RRQ in netascii, Client GET in netascii)
    QVERIFY(!writeServerFile(QStringLiteral("netascii_download.bin"), localData).isEmpty());

    TftpClient client;
    client.setMode(QStringLiteral("netascii"));
    const QString outPath = m_clientDir.path() + QStringLiteral("/netascii_download.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("netascii_download.bin"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    QFile out(outPath);
    QVERIFY(out.open(QIODevice::ReadOnly));
    // Client should write native line endings. Since we are on Linux/macOS in this environment, it should match localData.
    QCOMPARE(out.readAll(), localData);
    out.close();

    // 2. Upload Test (Client PUT in netascii, Server WRQ in netascii)
    QTemporaryFile tempLocalFile;
    QVERIFY(tempLocalFile.open());
    tempLocalFile.write(localData);
    tempLocalFile.close();

    TftpClient clientUpload;
    clientUpload.setMode(QStringLiteral("netascii"));
    QSignalSpy spyUpload(&clientUpload, &TftpClient::transferFinished);
    clientUpload.uploadFile(QStringLiteral("127.0.0.1"), m_port, tempLocalFile.fileName(), QStringLiteral("netascii_upload.bin"));
    QVERIFY(spyUpload.wait(5000));
    QCOMPARE(spyUpload.takeFirst().at(0).toBool(), true);

    QFile serverOutFile(m_serverDir.path() + QStringLiteral("/netascii_upload.bin"));
    QVERIFY(serverOutFile.open(QIODevice::ReadOnly));
    QCOMPARE(serverOutFile.readAll(), localData);
}

void TFTPProtocolTest::testBlockRollover() {
    // Generate a payload that causes a 16-bit block counter rollover.
    // Minimum blocksize is 8 bytes.
    // 65535 blocks * 8 bytes = 524280 bytes.
    // We send 524300 bytes, which requires 65538 blocks, forcing a rollover.
    const int rolloverSize = 524300;
    const QByteArray data = makePayload(rolloverSize, 12);
    QVERIFY(!writeServerFile(QStringLiteral("rollover.bin"), data).isEmpty());

    TftpClient client;
    client.setBlockSize(8);
    const QString outPath = m_clientDir.path() + QStringLiteral("/rollover.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("rollover.bin"), outPath);
    // Large number of small packets might take slightly longer, give it 30 seconds.
    QVERIFY(spy.wait(30000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    QFile out(outPath);
    QVERIFY(out.open(QIODevice::ReadOnly));
    QCOMPARE(out.readAll(), data);
}

void TFTPProtocolTest::testUploadRoundTrip() {
    const QByteArray data = makePayload(5000, 3);
    const QString srcPath = m_clientDir.path() + QStringLiteral("/upload_src.bin");
    QFile src(srcPath);
    QVERIFY(src.open(QIODevice::WriteOnly));
    src.write(data);
    src.close();

    TftpClient client;
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.uploadFile(QStringLiteral("127.0.0.1"), m_port, srcPath, QStringLiteral("uploaded.bin"));
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    QFile dst(m_serverDir.path() + QStringLiteral("/uploaded.bin"));
    QVERIFY(dst.open(QIODevice::ReadOnly));
    QCOMPARE(dst.readAll(), data);
}

void TFTPProtocolTest::testUploadOverwriteRejected() {
    // First upload succeeds; a second upload of the same remote name must be
    // rejected (RFC 1350 — server replies FileAlreadyExists).
    const QByteArray data = makePayload(1024, 5);
    const QString srcPath = m_clientDir.path() + QStringLiteral("/ow_src.bin");
    QFile src(srcPath);
    QVERIFY(src.open(QIODevice::WriteOnly));
    src.write(data);
    src.close();

    TftpClient first;
    QSignalSpy firstSpy(&first, &TftpClient::transferFinished);
    first.uploadFile(QStringLiteral("127.0.0.1"), m_port, srcPath, QStringLiteral("overwrite.bin"));
    QVERIFY(firstSpy.wait(5000));
    QCOMPARE(firstSpy.takeFirst().at(0).toBool(), true);

    TftpClient second;
    QSignalSpy secondSpy(&second, &TftpClient::transferFinished);
    second.uploadFile(QStringLiteral("127.0.0.1"), m_port, srcPath, QStringLiteral("overwrite.bin"));
    QVERIFY(secondSpy.wait(5000));
    QCOMPARE(secondSpy.takeFirst().at(0).toBool(), false);
}

void TFTPProtocolTest::testLargeMultiBlockTransfer() {
    // Exercise an exact multiple of the block size (needs a final empty block).
    const QByteArray data = makePayload(512 * 4, 9);
    QVERIFY(!writeServerFile(QStringLiteral("exact.bin"), data).isEmpty());

    TftpClient client;
    const QString outPath = m_clientDir.path() + QStringLiteral("/exact.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("exact.bin"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    QFile out(outPath);
    QVERIFY(out.open(QIODevice::ReadOnly));
    QCOMPARE(out.readAll().size(), data.size());
    QCOMPARE(out.readAll(), QByteArray());  // already at end
}

void TFTPProtocolTest::testMissingFileError() {
    TftpClient client;
    const QString outPath = m_clientDir.path() + QStringLiteral("/missing.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("does_not_exist.bin"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), false);
}

void TFTPProtocolTest::testPathTraversalRejected() {
    TftpClient client;
    const QString outPath = m_clientDir.path() + QStringLiteral("/escape.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("../../../etc/passwd"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), false);
}

void TFTPProtocolTest::testAcls() {
    // Read-only mode
    m_server->setReadOnly(true);
    // Download should still work
    {
        const QByteArray data = makePayload(100, 10);
        QVERIFY(!writeServerFile(QStringLiteral("acl_ro.bin"), data).isEmpty());
        TftpClient client;
        const QString outPath = m_clientDir.path() + QStringLiteral("/acl_ro.out");
        QSignalSpy spy(&client, &TftpClient::transferFinished);
        client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("acl_ro.bin"), outPath);
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.takeFirst().at(0).toBool(), true);
    }
    // Upload should fail
    {
        const QString srcPath = m_clientDir.path() + QStringLiteral("/acl_upload_src.bin");
        QFile src(srcPath);
        QVERIFY(src.open(QIODevice::WriteOnly));
        src.write("test");
        src.close();
        TftpClient client;
        QSignalSpy spy(&client, &TftpClient::transferFinished);
        client.uploadFile(QStringLiteral("127.0.0.1"), m_port, srcPath, QStringLiteral("acl_ro_upload.bin"));
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.takeFirst().at(0).toBool(), false);
    }
    m_server->setReadOnly(false);

    // Blacklisting
    m_server->setBlacklist({QStringLiteral("127.0.0.1/32")});
    {
        TftpClient client;
        const QString outPath = m_clientDir.path() + QStringLiteral("/acl_black.out");
        QSignalSpy spy(&client, &TftpClient::transferFinished);
        client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("acl_ro.bin"), outPath);
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.takeFirst().at(0).toBool(), false);
    }
    m_server->setBlacklist({});

    // Whitelisting
    m_server->setWhitelist({QStringLiteral("10.0.0.0/8")});  // does not include 127.0.0.1
    {
        TftpClient client;
        const QString outPath = m_clientDir.path() + QStringLiteral("/acl_white.out");
        QSignalSpy spy(&client, &TftpClient::transferFinished);
        client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("acl_ro.bin"), outPath);
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.takeFirst().at(0).toBool(), false);
    }
    m_server->setWhitelist({});
}

void TFTPProtocolTest::testVirtualDirectoryMapping() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QByteArray data = "Virtual Directory content";
    QFile f(tempDir.path() + QStringLiteral("/mapped_file.bin"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
    f.close();

    QMap<QString, QString> mappings;
    mappings.insert(QStringLiteral("fw"), tempDir.path());
    m_server->setVirtualMappings(mappings);

    // Download from mapped directory
    {
        TftpClient client;
        const QString outPath = m_clientDir.path() + QStringLiteral("/mapped_download.out");
        QSignalSpy spy(&client, &TftpClient::transferFinished);
        client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("fw/mapped_file.bin"), outPath);
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.takeFirst().at(0).toBool(), true);

        QFile destFile(outPath);
        QVERIFY(destFile.open(QIODevice::ReadOnly));
        QCOMPARE(destFile.readAll(), data);
    }

    // Upload into mapped directory
    {
        const QString srcPath = m_clientDir.path() + QStringLiteral("/mapped_upload.src");
        QFile src(srcPath);
        QVERIFY(src.open(QIODevice::WriteOnly));
        src.write("uploaded data");
        src.close();

        TftpClient client;
        QSignalSpy spy(&client, &TftpClient::transferFinished);
        client.uploadFile(QStringLiteral("127.0.0.1"), m_port, srcPath, QStringLiteral("fw/uploaded_mapped.bin"));
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.takeFirst().at(0).toBool(), true);

        QFile uploaded(tempDir.path() + QStringLiteral("/uploaded_mapped.bin"));
        QVERIFY(uploaded.open(QIODevice::ReadOnly));
        QCOMPARE(uploaded.readAll(), QByteArray("uploaded data"));
    }

    // Path traversal attempt in virtual mapped folder
    {
        TftpClient client;
        const QString outPath = m_clientDir.path() + QStringLiteral("/mapped_traversal.out");
        QSignalSpy spy(&client, &TftpClient::transferFinished);
        client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("fw/../invalid.bin"), outPath);
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.takeFirst().at(0).toBool(), false);
    }

    m_server->setVirtualMappings({});
}

void TFTPProtocolTest::testPskEncryption() {
    const QByteArray plainData = "Secret document content, confidential!";
    const QString serverFile = writeServerFile(QStringLiteral("secret.bin"), plainData);

    m_server->setPskKey(QStringLiteral("SecretKey"));

    // 1. Success case: matching client key
    {
        TftpClient client;
        client.setPskKey(QStringLiteral("SecretKey"));
        const QString outPath = m_clientDir.path() + QStringLiteral("/psk_success.out");
        QSignalSpy spy(&client, &TftpClient::transferFinished);
        client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("secret.bin"), outPath);
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.takeFirst().at(0).toBool(), true);

        QFile destFile(outPath);
        QVERIFY(destFile.open(QIODevice::ReadOnly));
        QCOMPARE(destFile.readAll(), plainData);
    }

    // 2. Wrong client key: decryption should fail (content is garbled)
    {
        TftpClient client;
        client.setPskKey(QStringLiteral("WrongKey"));
        const QString outPath = m_clientDir.path() + QStringLiteral("/psk_wrong.out");
        QSignalSpy spy(&client, &TftpClient::transferFinished);
        client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("secret.bin"), outPath);
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.takeFirst().at(0).toBool(), true);

        QFile destFile(outPath);
        QVERIFY(destFile.open(QIODevice::ReadOnly));
        QVERIFY(destFile.readAll() != plainData);
    }

    // 3. No client key: downloaded content is completely encrypted/scrambled
    {
        TftpClient client;
        const QString outPath = m_clientDir.path() + QStringLiteral("/psk_none.out");
        QSignalSpy spy(&client, &TftpClient::transferFinished);
        client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("secret.bin"), outPath);
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.takeFirst().at(0).toBool(), true);

        QFile destFile(outPath);
        QVERIFY(destFile.open(QIODevice::ReadOnly));
        QVERIFY(destFile.readAll() != plainData);
    }

    m_server->setPskKey(QString());
}

void TFTPProtocolTest::testPathSecurityPolicy() {
    // 1. Extension Whitelisting / Blacklisting
    m_server->setAllowedExtensions({QStringLiteral("txt"), QStringLiteral("bin")});
    m_server->setBlockedExtensions({QStringLiteral("exe")});

    // Write allowed and blocked file extensions
    QVERIFY(!writeServerFile(QStringLiteral("ok.bin"), "allowed bin file").isEmpty());
    QVERIFY(!writeServerFile(QStringLiteral("bad.exe"), "blocked exe file").isEmpty());
    QVERIFY(!writeServerFile(QStringLiteral("other.png"), "unlisted png file").isEmpty());

    // Downloading allowed suffix should succeed
    {
        TftpClient client;
        const QString outPath = m_clientDir.path() + QStringLiteral("/ok.out");
        QSignalSpy spy(&client, &TftpClient::transferFinished);
        client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("ok.bin"), outPath);
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.takeFirst().at(0).toBool(), true);
    }

    // Downloading explicitly blocked suffix should fail
    {
        TftpClient client;
        const QString outPath = m_clientDir.path() + QStringLiteral("/bad.out");
        QSignalSpy spy(&client, &TftpClient::transferFinished);
        client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("bad.exe"), outPath);
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.takeFirst().at(0).toBool(), false);
    }

    // Downloading unlisted suffix should fail because we have an allowed whitelist
    {
        TftpClient client;
        const QString outPath = m_clientDir.path() + QStringLiteral("/other.out");
        QSignalSpy spy(&client, &TftpClient::transferFinished);
        client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("other.png"), outPath);
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.takeFirst().at(0).toBool(), false);
    }

    // Reset extension policies
    m_server->setAllowedExtensions({});
    m_server->setBlockedExtensions({});

    // 2. Configurable Read-Only Directories (only block WRQ)
    QDir serverDir(m_serverDir.path());
    QVERIFY(serverDir.mkdir(QStringLiteral("restricted_dir")));
    m_server->setReadOnlyDirectories({QStringLiteral("restricted_dir")});

    // Populate a file inside the restricted directory
    QVERIFY(!writeServerFile(QStringLiteral("restricted_dir/read.txt"), "read content").isEmpty());

    // Reading (RRQ) from restricted directory should succeed
    {
        TftpClient client;
        const QString outPath = m_clientDir.path() + QStringLiteral("/restricted_read.out");
        QSignalSpy spy(&client, &TftpClient::transferFinished);
        client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("restricted_dir/read.txt"), outPath);
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.takeFirst().at(0).toBool(), true);
    }

    // Writing (WRQ) to restricted directory should fail
    {
        const QString srcPath = m_clientDir.path() + QStringLiteral("/write_src.bin");
        QFile src(srcPath);
        QVERIFY(src.open(QIODevice::WriteOnly));
        src.write("restricted upload test");
        src.close();

        TftpClient client;
        QSignalSpy spy(&client, &TftpClient::transferFinished);
        client.uploadFile(QStringLiteral("127.0.0.1"), m_port, srcPath, QStringLiteral("restricted_dir/upload.txt"));
        QVERIFY(spy.wait(5000));
        QCOMPARE(spy.takeFirst().at(0).toBool(), false);
    }

    // Reset read-only directories
    m_server->setReadOnlyDirectories({});
}

void TFTPProtocolTest::testSymlinkPathTraversal() {
    // Create a file outside the server root.
    QTemporaryDir outsideDir;
    QVERIFY(outsideDir.isValid());
    QFile outsideFile(outsideDir.path() + QStringLiteral("/secret.txt"));
    QVERIFY(outsideFile.open(QIODevice::WriteOnly));
    outsideFile.write("classified information");
    outsideFile.close();

    // Create a symlink inside the server root pointing to the outside file.
    QString symlinkPath = m_serverDir.path() + QStringLiteral("/link_to_secret.txt");
#ifdef Q_OS_WIN
    if (!QFile::link(outsideFile.fileName(), symlinkPath)) {
        QSKIP(
            "Symlink creation is not permitted or supported on this Windows "
            "host (requires developer mode / admin privileges).");
    }
#else
    QVERIFY(QFile::link(outsideFile.fileName(), symlinkPath));
#endif

    // A download request for the symlink name should be rejected by path
    // traversal protection.
    TftpClient client;
    const QString outPath = m_clientDir.path() + QStringLiteral("/symlink_traversal.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("link_to_secret.txt"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), false);  // Must fail!
}

void TFTPProtocolTest::testIpv6Binding() {
#ifdef Q_OS_WIN
    QSKIP(
        "IPv6 loopback testing is skipped on Windows due to CI virtual "
        "environment network routing limitations.");
#else
    TftpServer ipv6Server;
    // Bind to LocalHostIPv6 (::1).
    if (!ipv6Server.listen(QHostAddress::LocalHostIPv6, 0, m_serverDir.path())) {
        QSKIP("IPv6 is not supported/configured on this loopback interface");
    }
    quint16 ipv6Port = ipv6Server.port();

    const QByteArray data = makePayload(200, 11);
    QVERIFY(!writeServerFile(QStringLiteral("ipv6.bin"), data).isEmpty());

    TftpClient client;
    const QString outPath = m_clientDir.path() + QStringLiteral("/ipv6.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("::1"), ipv6Port, QStringLiteral("ipv6.bin"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    QFile out(outPath);
    QVERIFY(out.open(QIODevice::ReadOnly));
    QCOMPARE(out.readAll(), data);
#endif
}

void TFTPProtocolTest::testSinglePortMultiplexing() {
    m_server->setSinglePortMode(true);

    const QByteArray data = makePayload(1500, 12);  // multiple blocks
    QVERIFY(!writeServerFile(QStringLiteral("singleport.bin"), data).isEmpty());

    TftpClient client;
    client.setBlockSize(512);
    const QString outPath = m_clientDir.path() + QStringLiteral("/singleport.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("singleport.bin"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    QFile out(outPath);
    QVERIFY(out.open(QIODevice::ReadOnly));
    QCOMPARE(out.readAll(), data);

    m_server->setSinglePortMode(false);
}

void TFTPProtocolTest::testRateLimiting() {
    // Write a test file of 2000 bytes
    const QByteArray data = makePayload(2000, 42);
    QVERIFY(!writeServerFile(QStringLiteral("ratelimit.bin"), data).isEmpty());

    // Session speed limit: 1000 bytes/sec.
    // 2000 bytes should take around 1 second (>= 800 ms with burst allowance)
    m_server->setSessionLimit(1000);
    m_server->setGlobalLimit(0);

    QElapsedTimer timer;
    timer.start();

    TftpClient client;
    client.setBlockSize(512);
    const QString outPath1 = m_clientDir.path() + QStringLiteral("/ratelimit_sess.out");
    QSignalSpy spy1(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("ratelimit.bin"), outPath1);
    QVERIFY(spy1.wait(5000));
    QCOMPARE(spy1.takeFirst().at(0).toBool(), true);

    qint64 elapsedSess = timer.elapsed();
    QVERIFY2(elapsedSess >= 800, qPrintable(QString("Session rate limit took %1 ms, expected >= 800 ms").arg(elapsedSess)));

    // Clean up file
    QFile::remove(outPath1);

    // Wait for server-side session to clean up completely
    while (m_server->activeSessions() > 0) {
        QTest::qWait(10);
    }

    // Global speed limit: 500 bytes/sec.
    // 2000 bytes should take around 2 seconds (>= 1500 ms)
    m_server->setSessionLimit(0);
    m_server->setGlobalLimit(500);

    timer.restart();

    const QString outPath2 = m_clientDir.path() + QStringLiteral("/ratelimit_glob.out");
    QSignalSpy spy2(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("ratelimit.bin"), outPath2);
    QVERIFY(spy2.wait(5000));
    QCOMPARE(spy2.takeFirst().at(0).toBool(), true);

    qint64 elapsedGlob = timer.elapsed();
    QVERIFY2(elapsedGlob >= 1500, qPrintable(QString("Global rate limit took %1 ms, expected >= 1500 ms").arg(elapsedGlob)));

    // Clean up
    QFile::remove(outPath2);
    m_server->setSessionLimit(0);
    m_server->setGlobalLimit(0);

    while (m_server->activeSessions() > 0) {
        QTest::qWait(10);
    }
}

void TFTPProtocolTest::testSecurityThrottling() {
    QVERIFY(!writeServerFile(QStringLiteral("throttling.bin"), "throttling data").isEmpty());
    m_server->setMaxConnections(1);

    TftpClient client1;
    QSignalSpy spy1(&client1, &TftpClient::transferFinished);
    client1.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("throttling.bin"),
                         m_clientDir.path() + QStringLiteral("/throttling1.out"));

    TftpClient client2;
    QSignalSpy spy2(&client2, &TftpClient::transferFinished);
    client2.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("throttling.bin"),
                         m_clientDir.path() + QStringLiteral("/throttling2.out"));

    QVERIFY(spy2.wait(2000));
    QCOMPARE(spy2.takeFirst().at(0).toBool(), false);

    QVERIFY(spy1.wait(5000));
    QCOMPARE(spy1.takeFirst().at(0).toBool(), true);

    m_server->setMaxConnections(0);
    QFile::remove(m_clientDir.path() + QStringLiteral("/throttling1.out"));
    QFile::remove(m_clientDir.path() + QStringLiteral("/throttling2.out"));

    while (m_server->activeSessions() > 0) {
        QTest::qWait(10);
    }
}

void TFTPProtocolTest::testDiskSpacePreflightCheck() {
    QStorageInfo storage(m_clientDir.path());
    QVERIFY(storage.isValid());
    qint64 available = storage.bytesAvailable();
    qint64 required = available + 10LL * 1024LL * 1024LL * 1024LL;  // available + 10 GB

    QFile sparseFile(m_clientDir.path() + QStringLiteral("/sparse.bin"));
    QVERIFY(sparseFile.open(QIODevice::ReadWrite));
    QVERIFY(sparseFile.resize(required));
    sparseFile.close();

    TftpClient client;
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.uploadFile(QStringLiteral("127.0.0.1"), m_port, sparseFile.fileName(), QStringLiteral("sparse_dest.bin"));
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), false);

    QFile::remove(sparseFile.fileName());
}

void TFTPProtocolTest::testGranularSubnetAcls() {
    QVERIFY(!writeServerFile(QStringLiteral("acl_test.bin"), "acl data").isEmpty());
    m_server->clearSubnetRules();
    m_server->setDefaultAccessLevel(TftpServer::AccessLevel::Blocked);
    m_server->addSubnetRule(QStringLiteral("127.0.0.1"), TftpServer::AccessLevel::ReadOnly);

    TftpClient client;
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("acl_test.bin"),
                        m_clientDir.path() + QStringLiteral("/acl_test.out"));
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    QTemporaryFile tempLocalFile;
    QVERIFY(tempLocalFile.open());
    tempLocalFile.write("data");
    tempLocalFile.close();

    TftpClient clientUpload;
    QSignalSpy spyUpload(&clientUpload, &TftpClient::transferFinished);
    clientUpload.uploadFile(QStringLiteral("127.0.0.1"), m_port, tempLocalFile.fileName(), QStringLiteral("acl_upload.bin"));
    QVERIFY(spyUpload.wait(5000));
    QCOMPARE(spyUpload.takeFirst().at(0).toBool(), false);

    m_server->clearSubnetRules();
    m_server->setDefaultAccessLevel(TftpServer::AccessLevel::ReadWrite);
    QFile::remove(m_clientDir.path() + QStringLiteral("/acl_test.out"));
}

void TFTPProtocolTest::testProfilesImportExport() {
    // The previous test verified MapTranslator functionality. Since we moved to
    // standard Qt Linguist translations (.qm files), testing QTranslator loading
    // from a static library inside a standalone test executable requires forced
    // resource initialization. We skip this as it's testing Qt's resource system,
    // not our app logic.

    // 2. Test JSON Serialization logic for a Client Profile
    QJsonObject client;
    client.insert(QStringLiteral("host"), QStringLiteral("10.0.0.1"));
    client.insert(QStringLiteral("port"), 69);
    client.insert(QStringLiteral("blockSize"), 1024);
    client.insert(QStringLiteral("timeoutMs"), 3000);
    client.insert(QStringLiteral("windowSize"), 4);
    client.insert(QStringLiteral("pskKey"), QStringLiteral("secret"));

    QJsonObject mainObj;
    mainObj.insert(QStringLiteral("profileName"), QStringLiteral("TestProfile"));
    mainObj.insert(QStringLiteral("client"), client);

    QJsonDocument doc(mainObj);
    QByteArray jsonBytes = doc.toJson();

    // Now parse it back (simulating import)
    QJsonDocument docImport = QJsonDocument::fromJson(jsonBytes);
    QVERIFY(!docImport.isNull());
    QJsonObject objImport = docImport.object();
    QCOMPARE(objImport.value(QStringLiteral("profileName")).toString(), QStringLiteral("TestProfile"));

    QJsonObject clientImport = objImport.value(QStringLiteral("client")).toObject();
    QCOMPARE(clientImport.value(QStringLiteral("host")).toString(), QStringLiteral("10.0.0.1"));
    QCOMPARE(clientImport.value(QStringLiteral("port")).toInt(), 69);
    QCOMPARE(clientImport.value(QStringLiteral("blockSize")).toInt(), 1024);
    QCOMPARE(clientImport.value(QStringLiteral("timeoutMs")).toInt(), 3000);
    QCOMPARE(clientImport.value(QStringLiteral("windowSize")).toInt(), 4);
    QCOMPARE(clientImport.value(QStringLiteral("pskKey")).toString(), QStringLiteral("secret"));
}

void TFTPProtocolTest::testStructuredLogging() {
    m_server->setJsonLoggingEnabled(true);

    QList<QString> jsonLogs;
    auto conn = connect(m_server, &TftpServer::logMessage, this, [&jsonLogs](const QString &msg) {
        if (msg.startsWith('{')) {
            jsonLogs.append(msg);
        }
    });

    const QByteArray data = makePayload(1000, 43);
    QVERIFY(!writeServerFile(QStringLiteral("jsonlog.bin"), data).isEmpty());

    TftpClient client;
    const QString outPath = m_clientDir.path() + QStringLiteral("/jsonlog.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("jsonlog.bin"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    // Wait for server to finish logging before disconnecting and asserting
    while (m_server->activeSessions() > 0) {
        QTest::qWait(10);
    }

    disconnect(conn);
    m_server->setJsonLoggingEnabled(false);

    QFile::remove(outPath);

    QVERIFY(!jsonLogs.isEmpty());

    bool foundStart = false;
    bool foundComplete = false;

    for (const QString &logStr : jsonLogs) {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(logStr.toUtf8(), &err);
        QCOMPARE(err.error, QJsonParseError::NoError);
        QVERIFY(doc.isObject());

        QJsonObject obj = doc.object();
        QVERIFY(obj.contains(QStringLiteral("timestamp")));
        QVERIFY(obj.contains(QStringLiteral("event_type")));
        QVERIFY(obj.contains(QStringLiteral("session_id")));
        QVERIFY(obj.contains(QStringLiteral("client_ip")));
        QVERIFY(obj.contains(QStringLiteral("file_name")));
        QVERIFY(obj.contains(QStringLiteral("block_count")));
        QVERIFY(obj.contains(QStringLiteral("status")));

        QString evType = obj.value(QStringLiteral("event_type")).toString();
        if (evType == QLatin1String("transfer_start")) {
            foundStart = true;
            QCOMPARE(obj.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        } else if (evType == QLatin1String("transfer_complete")) {
            foundComplete = true;
            QCOMPARE(obj.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
            QCOMPARE(obj.value(QStringLiteral("block_count")).toInt(),
                     2);  // 1000 bytes with 512 block size is 2 blocks
        }
    }

    QVERIFY(foundStart);
    QVERIFY(foundComplete);
}

void TFTPProtocolTest::testPrometheusExporter() {
    // Start metrics exporter on dynamic port
    QVERIFY(m_server->startMetricsServer(0));
    quint16 mPort = m_server->metricsServerPort();
    QVERIFY(mPort > 0);

    const QByteArray data = makePayload(2000, 44);
    QVERIFY(!writeServerFile(QStringLiteral("metrics.bin"), data).isEmpty());

    TftpClient client;
    const QString outPath = m_clientDir.path() + QStringLiteral("/metrics.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("metrics.bin"), outPath);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    QFile::remove(outPath);

    // Wait for server session cleanup so active sessions counts go to 0 and
    // totals are recorded
    while (m_server->activeSessions() > 0) {
        QTest::qWait(10);
    }

    // Query metrics exporter using QTcpSocket
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, mPort);
    QVERIFY(socket.waitForConnected(2000));

    // Send HTTP GET request and flush
    socket.write("GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
    socket.flush();

    // Use a QEventLoop to wait for readyRead asynchronously so the server side
    // gets events processed
    QEventLoop loop;
    connect(&socket, &QTcpSocket::readyRead, &loop, &QEventLoop::quit);
    connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();

    QByteArray response = socket.readAll();
    while (socket.state() == QAbstractSocket::ConnectedState && socket.waitForReadyRead(100)) {
        response.append(socket.readAll());
    }

    socket.close();

    QString respStr = QString::fromUtf8(response);
    QVERIFY(respStr.contains(QStringLiteral("HTTP/1.1 200 OK")));
    QVERIFY(respStr.contains(QStringLiteral("tftp_active_sessions")));
    QVERIFY(respStr.contains(QStringLiteral("tftp_bytes_transferred_total")));
    QVERIFY(respStr.contains(QStringLiteral("tftp_transfers_total{status=\"success\"}")));
    QVERIFY(respStr.contains(QStringLiteral("tftp_transfers_total{status=\"failure\"}")));
    QVERIFY(respStr.contains(QStringLiteral("tftp_retransmissions_total")));

    m_server->stopMetricsServer();
}

void TFTPProtocolTest::testMetricsExporterHardening() {
    QVERIFY(m_server->startMetricsServer(0));
    const quint16 mPort = m_server->metricsServerPort();
    QVERIFY(mPort > 0);

    // Issue a raw HTTP request and return the full response text. Uses a
    // QEventLoop so the in-process exporter gets its events serviced.
    auto request = [mPort](const QByteArray &raw) -> QString {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, mPort);
        if (!socket.waitForConnected(2000))
            return {};
        socket.write(raw);
        socket.flush();

        QEventLoop loop;
        QObject::connect(&socket, &QTcpSocket::readyRead, &loop, &QEventLoop::quit);
        QObject::connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        loop.exec();

        QByteArray response = socket.readAll();
        while (socket.state() == QAbstractSocket::ConnectedState && socket.waitForReadyRead(100)) {
            response.append(socket.readAll());
        }
        socket.close();
        return QString::fromUtf8(response);
    };

    // Non-GET method is rejected with 405 (and not served metrics).
    const QString post = request("POST /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
    QVERIFY2(post.contains(QStringLiteral("405")), qPrintable(post));
    QVERIFY(!post.contains(QStringLiteral("tftp_active_sessions")));

    // Unknown paths return 404.
    const QString notFound = request("GET /not-here HTTP/1.1\r\nHost: localhost\r\n\r\n");
    QVERIFY2(notFound.contains(QStringLiteral("404")), qPrintable(notFound));

    // A query string on /metrics is tolerated and still served.
    const QString ok = request("GET /metrics?foo=bar HTTP/1.1\r\nHost: localhost\r\n\r\n");
    QVERIFY2(ok.contains(QStringLiteral("200 OK")), qPrintable(ok));
    QVERIFY(ok.contains(QStringLiteral("tftp_active_sessions")));

    m_server->stopMetricsServer();
}

void TFTPProtocolTest::testCliRunnerUploadDownload() {
    QTemporaryDir cliServerDir;
    QTemporaryDir cliClientDir;
    QVERIFY(cliServerDir.isValid());
    QVERIFY(cliClientDir.isValid());

    // 1. Create file for download test
    QByteArray downloadData = "Hello from CLI Server!";
    QFile dFile(cliServerDir.filePath("server_file.txt"));
    QVERIFY(dFile.open(QIODevice::WriteOnly));
    dFile.write(downloadData);
    dFile.close();

    // 2. Create file for upload test
    QByteArray uploadData = "Hello from CLI Client!";
    QFile uFile(cliClientDir.filePath("client_file.txt"));
    QVERIFY(uFile.open(QIODevice::WriteOnly));
    uFile.write(uploadData);
    uFile.close();

    // Schedule the client operations and server shutdown
    QTimer::singleShot(100, [&]() {
        // Run get/download
        QStringList getArgs = {"--get",           "--host",   "127.0.0.1",
                               "--port",          "12347",    "--file",
                               "server_file.txt", "--output", cliClientDir.filePath("server_file_downloaded.txt")};
        int getResult = CliRunner::run(*qApp, getArgs);
        QCOMPARE(getResult, 0);

        // Verify downloaded file content
        QFile downloadedFile(cliClientDir.filePath("server_file_downloaded.txt"));
        QVERIFY(downloadedFile.open(QIODevice::ReadOnly));
        QCOMPARE(downloadedFile.readAll(), downloadData);
        downloadedFile.close();

        // Run put/upload
        QStringList putArgs = {"--put", "--host", "127.0.0.1", "--port", "12347", "--file", cliClientDir.filePath("client_file.txt")};
        int putResult = CliRunner::run(*qApp, putArgs);
        QCOMPARE(putResult, 0);

        // Verify uploaded file content (should be in server dir with the remote name 'client_file.txt')
        QFile uploadedFile(cliServerDir.filePath("client_file.txt"));
        QVERIFY(uploadedFile.open(QIODevice::ReadOnly));
        QCOMPARE(uploadedFile.readAll(), uploadData);
        uploadedFile.close();

        // Stop the server by exiting the main event loop
        qApp->quit();
    });

    // Run the server command. This starts the server and runs QCoreApplication::exec()
    QStringList serverArgs = {"--server", "--port", "12347", "--dir", cliServerDir.path(), "--bind", "127.0.0.1"};
    int serverResult = CliRunner::run(*qApp, serverArgs);
    QCOMPARE(serverResult, 0);
}

void TFTPProtocolTest::testCliRunnerPiping() {
    const QByteArray downloadData = "Pipe transfer verification!";
    QVERIFY(!writeServerFile(QStringLiteral("pipe.txt"), downloadData).isEmpty());

    // Temporarily disable message handler so QTest doesn't capture logging to stdout
    auto oldHandler = qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &) {});

    // Redirect stdout to a temporary file
    QTemporaryFile tempOut;
    QVERIFY(tempOut.open());
#ifdef Q_OS_WIN
    int oldStdout = _dup(1);
    _dup2(tempOut.handle(), 1);
#else
    int oldStdout = dup(1);
    dup2(tempOut.handle(), 1);
#endif

    TftpClient client;
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("pipe.txt"), QStringLiteral("-"));
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    // Restore stdout and message handler
    fflush(stdout);
#ifdef Q_OS_WIN
    _dup2(oldStdout, 1);
    _close(oldStdout);
#else
    dup2(oldStdout, 1);
    ::close(oldStdout);
#endif
    qInstallMessageHandler(oldHandler);

    tempOut.seek(0);
    QCOMPARE(tempOut.readAll(), downloadData);
}

void TFTPProtocolTest::testFuzzProtocolParsing() {
    QList<QByteArray> inputs;
    inputs.append(QByteArray());
    inputs.append(QByteArray("\x00", 1));
    inputs.append(QByteArray("\x00\x01", 2));
    inputs.append(QByteArray("\x00\x05\x00\x01", 4));
    inputs.append(QByteArray("\x00\x03\x00", 3));
    inputs.append(QByteArray("\x00\x04\x00\x05", 4));

    for (int i = 0; i < 50; ++i) {
        QByteArray garbage;
        int len = QRandomGenerator::global()->bounded(100) + 1;
        garbage.resize(len);
        for (int j = 0; j < len; ++j) {
            garbage[j] = char(QRandomGenerator::global()->bounded(256));
        }
        inputs.append(garbage);
    }

    for (const QByteArray &in : inputs) {
        Request req;
        parseRequest(in, req);

        quint16 block = 0;
        QByteArray payload;
        parseData(in, block, payload);

        parseAck(in, block);

        ErrorCode errCode;
        QString errMsg;
        parseError(in, errCode, errMsg);

        Options opts;
        parseOack(in, opts);
    }
}

void TFTPProtocolTest::testPacketLossSimulation() {
    m_server->setSinglePortMode(true);
    m_server->setPacketDropRate(0.05);  // Drop 5% of packets

    const QByteArray data = makePayload(5000, 101);
    QVERIFY(!writeServerFile(QStringLiteral("loss.bin"), data).isEmpty());

    TftpClient client;
    client.setBlockSize(512);
    client.setTimeout(1000);  // 1000 ms timeout to handle slow ASan/CI environments safely

    const QString outPath = m_clientDir.path() + QStringLiteral("/loss.out");
    QSignalSpy spy(&client, &TftpClient::transferFinished);
    client.downloadFile(QStringLiteral("127.0.0.1"), m_port, QStringLiteral("loss.bin"), outPath);

    QVERIFY(spy.wait(10000));
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    QFile out(outPath);
    QVERIFY(out.open(QIODevice::ReadOnly));
    QCOMPARE(out.readAll(), data);

    m_server->setSinglePortMode(false);
    m_server->setPacketDropRate(0.0);
}

void TFTPProtocolTest::testTransferAbort() {
    // Point at a port with no live server so begin() succeeds (it binds a
    // local socket and creates the destination file) without any peer session
    // being spawned — keeps the test deterministic and side-effect free.
    TftpClient client;
    const QString outPath = m_clientDir.path() + QStringLiteral("/aborted.out");
    QSignalSpy finishedSpy(&client, &TftpClient::transferFinished);

    client.downloadFile(QStringLiteral("127.0.0.1"), quint16(1), QStringLiteral("nothing.bin"), outPath);
    QVERIFY(client.isRunning());
    QVERIFY(QFile::exists(outPath));  // created (empty) when the file is opened.

    client.abort();
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.takeFirst().at(0).toBool(), false);
    QVERIFY(!client.isRunning());
    QVERIFY(!QFile::exists(outPath));  // partial download discarded.

    // Aborting again is a harmless no-op.
    client.abort();
    QCOMPARE(finishedSpy.count(), 0);
}

void TFTPProtocolTest::testAgainstTftpHpa() {
    QString tftpBin = QStandardPaths::findExecutable(QStringLiteral("tftp"));
    if (tftpBin.isEmpty()) {
        QSKIP("tftp command-line tool not found, skipping integration test");
    }

    QTemporaryDir serverDir;
    QTemporaryDir clientDir;
    QVERIFY(serverDir.isValid());
    QVERIFY(clientDir.isValid());

    // 1. Start our in-process server
    TftpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 12349, serverDir.path()));

    // 2. Prepare a file in the server directory to download
    QByteArray downloadData = "Data for tftp-hpa client to download!";
    QFile dFile(serverDir.filePath(QStringLiteral("tftp_hpa_down.txt")));
    QVERIFY(dFile.open(QIODevice::WriteOnly));
    dFile.write(downloadData);
    dFile.close();

    // 3. Prepare a file in the client directory to upload
    QByteArray uploadData = "Data for tftp-hpa client to upload!";
    QFile uFile(clientDir.filePath(QStringLiteral("tftp_hpa_up.txt")));
    QVERIFY(uFile.open(QIODevice::WriteOnly));
    uFile.write(uploadData);
    uFile.close();

    // 4. Run external tftp-hpa client to download
    QProcess downloadProcess;
    downloadProcess.setWorkingDirectory(clientDir.path());
    downloadProcess.start(tftpBin, {QStringLiteral("-m"), QStringLiteral("binary"), QStringLiteral("127.0.0.1"), QStringLiteral("12349"),
                                    QStringLiteral("-c"), QStringLiteral("get"), QStringLiteral("tftp_hpa_down.txt")});
    QVERIFY2(waitForProcess(downloadProcess, 8000), "tftp-hpa download did not finish");
    QByteArray downloadErrors = downloadProcess.readAllStandardError() + "\n" + downloadProcess.readAllStandardOutput();
    QVERIFY2(downloadProcess.exitCode() == 0, downloadErrors.constData());

    // Verify downloaded file content
    QFile downloadedFile(clientDir.filePath(QStringLiteral("tftp_hpa_down.txt")));
    QVERIFY(downloadedFile.open(QIODevice::ReadOnly));
    QCOMPARE(downloadedFile.readAll(), downloadData);
    downloadedFile.close();

    // 5. Run external tftp-hpa client to upload
    QProcess uploadProcess;
    uploadProcess.setWorkingDirectory(clientDir.path());
    uploadProcess.start(tftpBin, {QStringLiteral("-m"), QStringLiteral("binary"), QStringLiteral("127.0.0.1"), QStringLiteral("12349"),
                                  QStringLiteral("-c"), QStringLiteral("put"), QStringLiteral("tftp_hpa_up.txt")});
    QVERIFY2(waitForProcess(uploadProcess, 8000), "tftp-hpa upload did not finish");
    QByteArray uploadErrors = uploadProcess.readAllStandardError() + "\n" + uploadProcess.readAllStandardOutput();
    QVERIFY2(uploadProcess.exitCode() == 0, uploadErrors.constData());

    // Verify uploaded file content
    QFile uploadedFile(serverDir.filePath(QStringLiteral("tftp_hpa_up.txt")));
    QVERIFY(uploadedFile.open(QIODevice::ReadOnly));
    QCOMPARE(uploadedFile.readAll(), uploadData);
    uploadedFile.close();

    server.close();
}

// Unlike testAgainstTftpHpa() (in-process TftpServer), this spawns the real aethertftp binary via QProcess, both directions.
void TFTPProtocolTest::testCliInteropWithTftpHpa() {
    QString tftpBin = QStandardPaths::findExecutable(QStringLiteral("tftp"));
    if (tftpBin.isEmpty()) {
        QSKIP("tftp-hpa client (tftp) not found, skipping CLI interop test");
    }

    const QString aetherBin = QStringLiteral(AETHERTFTP_BINARY);
    QVERIFY2(QFile::exists(aetherBin), qPrintable(QStringLiteral("aethertftp binary not found: %1").arg(aetherBin)));

    // Direction 1: aethertftp CLI (--server) is the SERVER, tftp-hpa's `tftp` client performs the get/put.
    {
        const quint16 port = 12460;
        QTemporaryDir serverDir;
        QTemporaryDir clientDir;
        QVERIFY(serverDir.isValid());
        QVERIFY(clientDir.isValid());

        QByteArray downloadData = "Data served by the aethertftp CLI for tftp-hpa to download!";
        QFile dFile(serverDir.filePath(QStringLiteral("aether_cli_down.txt")));
        QVERIFY(dFile.open(QIODevice::WriteOnly));
        dFile.write(downloadData);
        dFile.close();

        QByteArray uploadData = "Data uploaded by tftp-hpa to the aethertftp CLI!";
        QFile uFile(clientDir.filePath(QStringLiteral("aether_cli_up.txt")));
        QVERIFY(uFile.open(QIODevice::WriteOnly));
        uFile.write(uploadData);
        uFile.close();

        QProcess serverProcess;
        serverProcess.start(aetherBin, {QStringLiteral("--server"), QStringLiteral("--bind"), QStringLiteral("127.0.0.1"),
                                        QStringLiteral("--port"), QString::number(port), QStringLiteral("--dir"), serverDir.path()});
        QVERIFY2(serverProcess.waitForStarted(3000), "aethertftp --server failed to start");
        QTest::qWait(500);  // let it bind before hammering it with requests.
        QVERIFY2(serverProcess.state() == QProcess::Running, "aethertftp --server exited immediately");

        QProcess downloadProcess;
        downloadProcess.setWorkingDirectory(clientDir.path());
        downloadProcess.start(tftpBin, {QStringLiteral("-m"), QStringLiteral("binary"), QStringLiteral("127.0.0.1"), QString::number(port),
                                        QStringLiteral("-c"), QStringLiteral("get"), QStringLiteral("aether_cli_down.txt")});
        QVERIFY2(waitForProcess(downloadProcess, 8000), "tftp-hpa download from aethertftp CLI did not finish");
        QCOMPARE(downloadProcess.exitCode(), 0);

        QFile downloadedFile(clientDir.filePath(QStringLiteral("aether_cli_down.txt")));
        QVERIFY(downloadedFile.open(QIODevice::ReadOnly));
        QCOMPARE(downloadedFile.readAll(), downloadData);
        downloadedFile.close();

        QProcess uploadProcess;
        uploadProcess.setWorkingDirectory(clientDir.path());
        uploadProcess.start(tftpBin, {QStringLiteral("-m"), QStringLiteral("binary"), QStringLiteral("127.0.0.1"), QString::number(port),
                                      QStringLiteral("-c"), QStringLiteral("put"), QStringLiteral("aether_cli_up.txt")});
        QVERIFY2(waitForProcess(uploadProcess, 8000), "tftp-hpa upload to aethertftp CLI did not finish");
        QCOMPARE(uploadProcess.exitCode(), 0);

        QFile uploadedFile(serverDir.filePath(QStringLiteral("aether_cli_up.txt")));
        QVERIFY(uploadedFile.open(QIODevice::ReadOnly));
        QCOMPARE(uploadedFile.readAll(), uploadData);
        uploadedFile.close();

        serverProcess.terminate();
        QVERIFY2(waitForProcess(serverProcess, 3000), "aethertftp --server did not exit after terminate()");
    }

    // Direction 2: tftp-hpa's `in.tftpd` is the SERVER, aethertftp CLI is the CLIENT.
    // in.tftpd only works started as root (drops privileges, needs CAP_SYS_CHROOT); skip gracefully otherwise.
    QString tftpdBin = QStandardPaths::findExecutable(QStringLiteral("in.tftpd"));
    if (tftpdBin.isEmpty())
        tftpdBin = QStringLiteral("/usr/sbin/in.tftpd");
#ifdef Q_OS_WIN
    const bool haveRoot = false;
#else
    const bool haveRoot = (::geteuid() == 0);
#endif
    bool haveSudo = false;
    if (!haveRoot) {
        QProcess sudoCheck;
        sudoCheck.start(QStringLiteral("sudo"), {QStringLiteral("-n"), QStringLiteral("true")});
        haveSudo = sudoCheck.waitForFinished(3000) && sudoCheck.exitCode() == 0;
    }
    if (!QFile::exists(tftpdBin) || (!haveRoot && !haveSudo)) {
        QSKIP("in.tftpd not usable without root/passwordless sudo, skipping reverse-direction CLI interop test");
    }

    const quint16 port2 = 12461;
    QTemporaryDir hpaDir;
    QTemporaryDir aetherClientDir;
    QVERIFY(hpaDir.isValid());
    QVERIFY(aetherClientDir.isValid());
    // in.tftpd's worker drops to an unprivileged user, so this needs to be world read/write/execute.
    QVERIFY(QFile::setPermissions(hpaDir.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                                                     QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
                                                     QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther));

    QByteArray downloadData2 = "Data served by tftp-hpa for the aethertftp CLI to download!";
    QFile dFile2(hpaDir.filePath(QStringLiteral("hpa_down.txt")));
    QVERIFY(dFile2.open(QIODevice::WriteOnly));
    dFile2.write(downloadData2);
    dFile2.close();

    QStringList tftpdArgs{QStringLiteral("-L"), QStringLiteral("-c"), QStringLiteral("--secure"),
                          hpaDir.path(),        QStringLiteral("-a"), QStringLiteral("127.0.0.1:") + QString::number(port2)};
    QProcess tftpdProcess;
    if (haveRoot) {
        tftpdProcess.start(tftpdBin, tftpdArgs);
    } else {
        tftpdProcess.start(QStringLiteral("sudo"), QStringList{QStringLiteral("-n"), tftpdBin} + tftpdArgs);
    }
    QVERIFY2(tftpdProcess.waitForStarted(3000), "in.tftpd failed to start");
    QTest::qWait(500);
    QVERIFY2(tftpdProcess.state() == QProcess::Running, "in.tftpd exited immediately");

    QProcess aetherGet;
    aetherGet.start(aetherBin, {QStringLiteral("--get"), QStringLiteral("--host"), QStringLiteral("127.0.0.1"), QStringLiteral("--port"),
                                QString::number(port2), QStringLiteral("--file"), QStringLiteral("hpa_down.txt"),
                                QStringLiteral("--output"), aetherClientDir.filePath(QStringLiteral("hpa_down.txt"))});
    QVERIFY2(waitForProcess(aetherGet, 8000), "aethertftp --get from in.tftpd did not finish");
    QCOMPARE(aetherGet.exitCode(), 0);

    QFile downloadedFile2(aetherClientDir.filePath(QStringLiteral("hpa_down.txt")));
    QVERIFY(downloadedFile2.open(QIODevice::ReadOnly));
    QCOMPARE(downloadedFile2.readAll(), downloadData2);
    downloadedFile2.close();

    QByteArray uploadData2 = "Data uploaded by the aethertftp CLI to tftp-hpa!";
    QFile uFile2(aetherClientDir.filePath(QStringLiteral("hpa_up.txt")));
    QVERIFY(uFile2.open(QIODevice::WriteOnly));
    uFile2.write(uploadData2);
    uFile2.close();

    QProcess aetherPut;
    aetherPut.start(aetherBin, {QStringLiteral("--put"), QStringLiteral("--host"), QStringLiteral("127.0.0.1"), QStringLiteral("--port"),
                                QString::number(port2), QStringLiteral("--file"), aetherClientDir.filePath(QStringLiteral("hpa_up.txt"))});
    QVERIFY2(waitForProcess(aetherPut, 8000), "aethertftp --put to in.tftpd did not finish");
    QCOMPARE(aetherPut.exitCode(), 0);

    QFile uploadedFile2(hpaDir.filePath(QStringLiteral("hpa_up.txt")));
    QVERIFY(uploadedFile2.open(QIODevice::ReadOnly));
    QCOMPARE(uploadedFile2.readAll(), uploadData2);
    uploadedFile2.close();

    // Modern sudo forwards SIGTERM, so terminate() works whether or not in.tftpd is wrapped in sudo.
    tftpdProcess.terminate();
    waitForProcess(tftpdProcess, 3000);
    if (tftpdProcess.state() != QProcess::NotRunning) {
        if (haveRoot)
            tftpdProcess.kill();
        else
            QProcess::execute(QStringLiteral("sudo"), {QStringLiteral("-n"), QStringLiteral("pkill"), QStringLiteral("-KILL"),
                                                       QStringLiteral("-f"), QStringLiteral("in.tftpd.*") + QString::number(port2)});
        waitForProcess(tftpdProcess, 3000);
    }
}

// Unlike tftp-hpa's `tftp` (no blksize/tsize/timeout/windowsize support at all), curl negotiates RFC 2348/2349 by default.
void TFTPProtocolTest::testRfcExtensionsAgainstCurl() {
    QString curlBin = QStandardPaths::findExecutable(QStringLiteral("curl"));
    if (curlBin.isEmpty()) {
        QSKIP("curl not found, skipping RFC extension interop test");
    }

    const QString aetherBin = QStringLiteral(AETHERTFTP_BINARY);
    QVERIFY2(QFile::exists(aetherBin), qPrintable(QStringLiteral("aethertftp binary not found: %1").arg(aetherBin)));

    const quint16 port = 12462;
    const QString baseUrl = QStringLiteral("tftp://127.0.0.1:") + QString::number(port) + QLatin1Char('/');

    QTemporaryDir serverDir;
    QTemporaryDir clientDir;
    QVERIFY(serverDir.isValid());
    QVERIFY(clientDir.isValid());

    const QByteArray downloadData = makePayload(3000, 42);
    QFile dFile(serverDir.filePath(QStringLiteral("curl_down.bin")));
    QVERIFY(dFile.open(QIODevice::WriteOnly));
    dFile.write(downloadData);
    dFile.close();

    QProcess serverProcess;
    serverProcess.start(aetherBin, {QStringLiteral("--server"), QStringLiteral("--bind"), QStringLiteral("127.0.0.1"),
                                    QStringLiteral("--port"), QString::number(port), QStringLiteral("--dir"), serverDir.path()});
    QVERIFY2(serverProcess.waitForStarted(3000), "aethertftp --server failed to start");
    QTest::qWait(500);  // let it bind before hammering it with requests.
    QVERIFY2(serverProcess.state() == QProcess::Running, "aethertftp --server exited immediately");

    // GET with a non-default blksize; curl also sends tsize=0, confirming the server reports the real file size.
    {
        const QString outPath = clientDir.filePath(QStringLiteral("curl_down.bin"));
        QProcess getProcess;
        getProcess.start(curlBin, {QStringLiteral("-sS"), QStringLiteral("-v"), QStringLiteral("--tftp-blksize"), QStringLiteral("1024"),
                                   baseUrl + QStringLiteral("curl_down.bin"), QStringLiteral("-o"), outPath});
        QVERIFY2(waitForProcess(getProcess, 8000), "curl GET did not finish");
        const QByteArray log = getProcess.readAllStandardError();
        QVERIFY2(getProcess.exitCode() == 0, log.constData());
        QVERIFY2(log.contains("got option=(blksize) value=(1024)"), log.constData());
        QVERIFY2(log.contains(QByteArray("got option=(tsize) value=(") + QByteArray::number(downloadData.size()) + ")"), log.constData());

        QFile downloaded(outPath);
        QVERIFY(downloaded.open(QIODevice::ReadOnly));
        QCOMPARE(downloaded.readAll(), downloadData);
    }

    // PUT: confirms the server accepts and echoes the tsize curl declares for the upload.
    {
        const QByteArray uploadData = makePayload(2000, 84);
        const QString localPath = clientDir.filePath(QStringLiteral("curl_up.bin"));
        QFile uFile(localPath);
        QVERIFY(uFile.open(QIODevice::WriteOnly));
        uFile.write(uploadData);
        uFile.close();

        QProcess putProcess;
        putProcess.start(curlBin, {QStringLiteral("-sS"), QStringLiteral("-v"), QStringLiteral("-T"), localPath,
                                   baseUrl + QStringLiteral("curl_up.bin")});
        QVERIFY2(waitForProcess(putProcess, 8000), "curl PUT did not finish");
        const QByteArray log = putProcess.readAllStandardError();
        QVERIFY2(putProcess.exitCode() == 0, log.constData());
        QVERIFY2(log.contains(QByteArray("got option=(tsize) value=(") + QByteArray::number(uploadData.size()) + ")"), log.constData());

        QFile uploaded(serverDir.filePath(QStringLiteral("curl_up.bin")));
        QVERIFY(uploaded.open(QIODevice::ReadOnly));
        QCOMPARE(uploaded.readAll(), uploadData);
    }

    serverProcess.terminate();
    QVERIFY2(waitForProcess(serverProcess, 3000), "aethertftp --server did not exit after terminate()");
}

void TFTPProtocolTest::testProxyDhcp() {
    // 1. Construct a dummy BOOTP request header (236 bytes minimum + magic cookie)
    QByteArray req;
    req.resize(240);
    req.fill(0);
    req[0] = 1;  // BOOTREQUEST
    req[1] = 1;  // Ethernet
    req[2] = 6;  // hlen
    // Set Transaction ID (xid)
    quint32 xid = 0x12345678;
    quint32 xidBE = qToBigEndian(xid);
    memcpy(req.data() + 4, &xidBE, 4);
    // Set Magic Cookie at offset 236
    quint32 magicBE = qToBigEndian(0x63825363);
    memcpy(req.data() + 236, &magicBE, 4);

    // Option 53: DHCPDISCOVER (1)
    req.append(static_cast<char>(53));
    req.append(static_cast<char>(1));
    req.append(static_cast<char>(1));

    // Non-PXE Client test -> Should be ignored
    QHostAddress serverIp("192.168.1.100");
    QByteArray resNonPxe = PxeProxyDhcp::processDhcpPacket(req, serverIp, "bootx64.efi", serverIp, "tftp.local");
    QVERIFY(resNonPxe.isEmpty());

    // Add Option 60: PXEClient
    req.append(static_cast<char>(60));
    const char pxeVendor[] = "PXEClient:Arch:00000:UNDI:002001";
    req.append(static_cast<char>(sizeof(pxeVendor) - 1));
    req.append(pxeVendor, sizeof(pxeVendor) - 1);
    req.append(static_cast<char>(255));  // END

    // PXE Client test -> Should return ProxyDHCP DHCPOFFER response
    QString macStr;
    QByteArray resPxe = PxeProxyDhcp::processDhcpPacket(req, serverIp, "pxelinux.0", serverIp, "tftp.local", &macStr);
    QVERIFY(!resPxe.isEmpty());
    QVERIFY(resPxe.size() >= 240);
    QCOMPARE(static_cast<quint8>(resPxe.at(0)), 2);  // BOOTREPLY

    // Check siaddr in BOOTREPLY header (offset 20)
    quint32 siaddrBE = 0;
    memcpy(&siaddrBE, resPxe.constData() + 20, 4);
    QCOMPARE(qFromBigEndian(siaddrBE), serverIp.toIPv4Address());

    // Check filename in BOOTREPLY header (offset 108)
    const char *fileInHeader = resPxe.constData() + 108;
    QCOMPARE(QString::fromUtf8(fileInHeader), QStringLiteral("pxelinux.0"));

    // Check Option 53 (DHCPOFFER = 2) in response
    QVERIFY(resPxe.contains("\x35\x01\x02"));

    // 2. Test server integration
    TftpServer server;
    server.setProxyDhcpEnabled(true);
    server.setProxyDhcpBootFile("ipxe.efi");
    QVERIFY(server.isProxyDhcpEnabled());
    QCOMPARE(server.proxyDhcpBootFile(), QStringLiteral("ipxe.efi"));
}

void TFTPProtocolTest::testEmbeddedWebDashboard() {
    QTemporaryDir dir;
    TftpServer server;
    server.setWebDashboardEnabled(true);
    server.setWebDashboardPort(18080);
    QVERIFY(server.listen(QHostAddress::LocalHost, 12356, dir.path()));
    QVERIFY(server.isWebDashboardEnabled());
    QCOMPARE(server.webDashboardPort(), static_cast<quint16>(18080));

    // Test GET /
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, 18080);
    QVERIFY(client.waitForConnected(3000));
    QSignalSpy spyHtml(&client, &QTcpSocket::readyRead);
    client.write("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    QVERIFY(spyHtml.wait(3000));
    QByteArray resHtml = client.readAll();
    QVERIFY(resHtml.contains("200 OK"));
    QVERIFY(resHtml.contains("AetherTFTP"));

    // Test GET /api/status
    QTcpSocket clientApi;
    clientApi.connectToHost(QHostAddress::LocalHost, 18080);
    QVERIFY(clientApi.waitForConnected(3000));
    QSignalSpy spyApi(&clientApi, &QTcpSocket::readyRead);
    clientApi.write("GET /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n");
    QVERIFY(spyApi.wait(3000));
    QByteArray resApi = clientApi.readAll();
    QVERIFY(resApi.contains("200 OK"));
    QVERIFY(resApi.contains("isListening"));
    QVERIFY(resApi.contains("true"));
    QVERIFY(resApi.contains("webDashboardPort"));

    server.close();

    // Test custom parametric port
    TftpServer serverParametric;
    serverParametric.setWebDashboardEnabled(true);
    serverParametric.setWebDashboardPort(18081);
    QVERIFY(serverParametric.listen(QHostAddress::LocalHost, 12357, dir.path()));
    QCOMPARE(serverParametric.webDashboardPort(), static_cast<quint16>(18081));
    serverParametric.close();
}

QTEST_MAIN(TFTPProtocolTest)
#include "tftp_test.moc"
