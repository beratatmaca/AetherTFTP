#include "gui/mainwindow.h"

#include "core/tftp_client.h"
#include "core/tftp_server.h"
#include "gui/server_config_dialog.h"
#include "gui/transfer_model.h"

#include <QMainWindow>
#include <QWidget>
#include <QTimer>

#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTime>
#include <QTreeView>
#include <QVBoxLayout>

namespace tftp::gui {

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("AetherTFTP"));
    setAcceptDrops(true);
    m_serverDir = QDir::currentPath();

    m_server = new TftpServer(this);
    connect(m_server, &TftpServer::logMessage, this, &MainWindow::appendLog);
    connect(m_server, &TftpServer::transferStarted, this,
            [this](const QString &name, bool isUpload) {
                appendLog(QStringLiteral("Incoming %1: %2")
                              .arg(isUpload ? QStringLiteral("upload")
                                            : QStringLiteral("download"),
                                   name));
            });

    m_model = new TransferModel(this);
    m_view = new QTreeView(this);
    m_view->setModel(m_model);
    m_view->setRootIsDecorated(false);
    m_view->setAlternatingRowColors(true);
    m_view->setItemDelegateForColumn(TransferModel::ColProgress,
                                     new ProgressBarDelegate(m_view));
    m_view->header()->setSectionResizeMode(TransferModel::ColName,
                                           QHeaderView::Stretch);
    m_view->header()->setSectionResizeMode(TransferModel::ColStatus,
                                           QHeaderView::Stretch);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(1000);
    m_log->setObjectName(QStringLiteral("logPanel"));

    auto *controls = new QHBoxLayout;
    controls->addWidget(buildServerGroup(), 1);
    controls->addWidget(buildClientGroup(), 1);
    controls->addWidget(buildMetricsGroup(), 1);

    auto *lowerSplit = new QSplitter(Qt::Vertical, this);
    lowerSplit->addWidget(m_view);
    lowerSplit->addWidget(m_log);
    lowerSplit->setStretchFactor(0, 3);
    lowerSplit->setStretchFactor(1, 1);

    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->addLayout(controls);
    root->addWidget(lowerSplit, 1);
    setCentralWidget(central);
    resize(920, 600);

    m_metricsTimer = new QTimer(this);
    m_metricsTimer->setInterval(1000);
    connect(m_metricsTimer, &QTimer::timeout, this, &MainWindow::updateMetrics);
    m_metricsTimer->start();
    m_metricsSpeedTimer.start();

    appendLog(QStringLiteral("Ready. Drag files onto the window to upload."));
}

MainWindow::~MainWindow() = default;

QWidget *MainWindow::buildServerGroup() {
    auto *box = new QGroupBox(QStringLiteral("Server"), this);
    auto *layout = new QVBoxLayout(box);

    m_serverStatusLabel = new QLabel(QStringLiteral("Stopped"), box);
    m_serverStatusLabel->setObjectName(QStringLiteral("statusStopped"));

    m_serverToggleBtn = new QPushButton(QStringLiteral("Start Server"), box);
    connect(m_serverToggleBtn, &QPushButton::clicked, this,
            &MainWindow::toggleServer);

    auto *settingsBtn = new QPushButton(QStringLiteral("Settings…"), box);
    connect(settingsBtn, &QPushButton::clicked, this,
            &MainWindow::configureServer);

    auto *btnRow = new QHBoxLayout;
    btnRow->addWidget(m_serverToggleBtn);
    btnRow->addWidget(settingsBtn);

    layout->addWidget(m_serverStatusLabel);
    layout->addLayout(btnRow);
    layout->addStretch(1);
    return box;
}

QWidget *MainWindow::buildClientGroup() {
    auto *box = new QGroupBox(QStringLiteral("Client"), this);
    auto *layout = new QVBoxLayout(box);

    m_hostEdit = new QLineEdit(box);
    m_hostEdit->setPlaceholderText(QStringLiteral("Host (e.g. 192.168.1.10)"));

    m_clientPortSpin = new QSpinBox(box);
    m_clientPortSpin->setRange(1, 65535);
    m_clientPortSpin->setValue(kDefaultPort);

    auto *hostRow = new QHBoxLayout;
    hostRow->addWidget(m_hostEdit, 1);
    hostRow->addWidget(new QLabel(QStringLiteral("Port:"), box));
    hostRow->addWidget(m_clientPortSpin);

    m_fileEdit = new QLineEdit(box);
    m_fileEdit->setPlaceholderText(QStringLiteral(
        "File (local path to upload, or remote name to download)"));
    auto *browseBtn = new QPushButton(QStringLiteral("Browse…"), box);
    connect(browseBtn, &QPushButton::clicked, this,
            &MainWindow::browseLocalFile);

    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(m_fileEdit, 1);
    fileRow->addWidget(browseBtn);

    auto *uploadBtn = new QPushButton(QStringLiteral("Upload"), box);
    uploadBtn->setObjectName(QStringLiteral("primaryButton"));
    connect(uploadBtn, &QPushButton::clicked, this, &MainWindow::startUpload);
    auto *downloadBtn = new QPushButton(QStringLiteral("Download"), box);
    connect(downloadBtn, &QPushButton::clicked, this,
            &MainWindow::startDownload);

    auto *actionRow = new QHBoxLayout;
    actionRow->addWidget(uploadBtn);
    actionRow->addWidget(downloadBtn);

    layout->addLayout(hostRow);
    layout->addLayout(fileRow);
    layout->addLayout(actionRow);
    layout->addStretch(1);
    return box;
}

void MainWindow::toggleServer() {
    if (m_serverRunning) {
        m_server->close();
        m_serverRunning = false;
        m_serverStatusLabel->setText(QStringLiteral("Stopped"));
        m_serverStatusLabel->setObjectName(QStringLiteral("statusStopped"));
        m_serverToggleBtn->setText(QStringLiteral("Start Server"));
        appendLog(QStringLiteral("Server stopped."));
    } else {
        if (!m_server->listen(QHostAddress::AnyIPv4, m_serverPort,
                              m_serverDir)) {
            appendLog(QStringLiteral("Failed to start server: %1")
                          .arg(m_server->lastError()));
            return;
        }
        m_serverRunning = true;
        m_serverStatusLabel->setText(
            QStringLiteral("Listening on port %1").arg(m_server->port()));
        m_serverStatusLabel->setObjectName(QStringLiteral("statusRunning"));
        m_serverToggleBtn->setText(QStringLiteral("Stop Server"));
    }
    // Re-polish so the object-name-based status colour updates.
    m_serverStatusLabel->style()->unpolish(m_serverStatusLabel);
    m_serverStatusLabel->style()->polish(m_serverStatusLabel);
}

void MainWindow::configureServer() {
    ServerConfigDialog dlg(m_serverPort, m_serverDir, m_maxConcurrent, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const bool wasRunning = m_serverRunning;
    if (wasRunning)
        toggleServer();  // stop with the old settings.

    m_serverPort = dlg.port();
    m_serverDir = dlg.rootDir();
    m_maxConcurrent = dlg.maxConcurrent();
    appendLog(
        QStringLiteral("Server settings updated (port %1, dir %2, max %3).")
            .arg(m_serverPort)
            .arg(m_serverDir)
            .arg(m_maxConcurrent));

    if (wasRunning)
        toggleServer();  // restart with the new settings.
}

void MainWindow::browseLocalFile() {
    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select file to upload"));
    if (!file.isEmpty())
        m_fileEdit->setText(file);
}

void MainWindow::startUpload() {
    const QString host = m_hostEdit->text().trimmed();
    const QString file = m_fileEdit->text().trimmed();
    if (host.isEmpty() || file.isEmpty()) {
        appendLog(QStringLiteral("Upload needs both a host and a file."));
        return;
    }
    startTransfer(true, host, quint16(m_clientPortSpin->value()), file,
                  QFileInfo(file).fileName());
}

void MainWindow::startDownload() {
    const QString host = m_hostEdit->text().trimmed();
    const QString remote = m_fileEdit->text().trimmed();
    if (host.isEmpty() || remote.isEmpty()) {
        appendLog(QStringLiteral(
            "Download needs both a host and a remote file name."));
        return;
    }
    const QString suggested =
        QDir::current().filePath(QFileInfo(remote).fileName());
    const QString dest = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save downloaded file as"), suggested);
    if (dest.isEmpty())
        return;
    startTransfer(false, host, quint16(m_clientPortSpin->value()), dest,
                  remote);
}

void MainWindow::startTransfer(bool isUpload, const QString &host, quint16 port,
                               const QString &localFile,
                               const QString &remoteName) {
    if (activeTransfers() >= m_maxConcurrent) {
        appendLog(QStringLiteral(
                      "Transfer limit reached (%1). Wait for one to finish.")
                      .arg(m_maxConcurrent));
        return;
    }

    auto *client = new TftpClient(this);
    const QString peer = QStringLiteral("%1:%2").arg(host).arg(port);
    const int row = m_model->addTransfer(remoteName, isUpload, peer);
    m_rowOf.insert(client, row);

    connect(client, &TftpClient::progress, this,
            [this, client](qint64 done, qint64 total) {
                m_model->updateProgress(m_rowOf.value(client, -1), done, total);
            });
    connect(client, &TftpClient::errorOccurred, this,
            [this, client](const QString &message) {
                m_lastError.insert(client, message);
            });
    connect(client, &TftpClient::transferFinished, this,
            [this, client, remoteName](bool ok) {
                const int finishedRow = m_rowOf.value(client, -1);
                const QString err = m_lastError.value(client);
                m_model->setFinished(finishedRow, ok, err);
                appendLog(QStringLiteral("%1 %2%3").arg(
                    remoteName,
                    ok ? QStringLiteral("completed") : QStringLiteral("failed"),
                    (!ok && !err.isEmpty()) ? QStringLiteral(": %1").arg(err)
                                            : QString()));
                m_rowOf.remove(client);
                m_lastError.remove(client);
                client->deleteLater();
            });

    appendLog(QStringLiteral("%1 %2 → %3")
                  .arg(isUpload ? QStringLiteral("Uploading")
                                : QStringLiteral("Downloading"),
                       remoteName, peer));

    if (isUpload)
        client->uploadFile(host, port, localFile, remoteName);
    else
        client->downloadFile(host, port, remoteName, localFile);
}

int MainWindow::activeTransfers() const {
    return m_rowOf.size();
}

void MainWindow::appendLog(const QString &message) {
    m_log->appendPlainText(QStringLiteral("[%1] %2").arg(
        QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), message));
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event) {
    const QString host = m_hostEdit->text().trimmed();
    if (host.isEmpty()) {
        appendLog(
            QStringLiteral("Enter a host before dropping files to upload."));
        return;
    }
    const quint16 port = quint16(m_clientPortSpin->value());
    for (const QUrl &url : event->mimeData()->urls()) {
        const QString local = url.toLocalFile();
        if (local.isEmpty() || !QFileInfo(local).isFile())
            continue;
        startTransfer(true, host, port, local, QFileInfo(local).fileName());
    }
    event->acceptProposedAction();
}

QWidget *MainWindow::buildMetricsGroup() {
    auto *box = new QGroupBox(QStringLiteral("Metrics Dashboard"), this);
    auto *layout = new QVBoxLayout(box);

    m_metricsActiveLabel = new QLabel(QStringLiteral("Active Sessions: 0"), box);
    m_metricsBytesLabel = new QLabel(QStringLiteral("Transferred: 0 B"), box);
    m_metricsTransfersLabel =
        new QLabel(QStringLiteral("Transfers (S/F): 0 / 0"), box);
    m_metricsRetransLabel = new QLabel(QStringLiteral("Retransmissions: 0"), box);
    m_metricsSpeedLabel = new QLabel(QStringLiteral("Current Speed: 0 B/s"), box);

    layout->addWidget(m_metricsActiveLabel);
    layout->addWidget(m_metricsBytesLabel);
    layout->addWidget(m_metricsTransfersLabel);
    layout->addWidget(m_metricsRetransLabel);
    layout->addWidget(m_metricsSpeedLabel);
    layout->addStretch(1);
    return box;
}

void MainWindow::updateMetrics() {
    if (!m_server)
        return;

    int active = m_server->activeSessions();
    qint64 bytes = m_server->totalBytesTransferred();
    qint64 success = m_server->transfersSuccess();
    qint64 failure = m_server->transfersFailure();
    qint64 retrans = m_server->retransmissionCount();

    qint64 elapsedMs = m_metricsSpeedTimer.restart();
    double speed = 0.0;
    if (elapsedMs > 0) {
        speed = double(bytes - m_lastTotalBytes) / (double(elapsedMs) / 1000.0);
    }
    m_lastTotalBytes = bytes;

    auto formatBytes = [](qint64 b) -> QString {
        if (b < 1024)
            return QString::number(b) + QStringLiteral(" B");
        double kb = double(b) / 1024.0;
        if (kb < 1024)
            return QString::number(kb, 'f', 1) + QStringLiteral(" KB");
        double mb = kb / 1024.0;
        return QString::number(mb, 'f', 1) + QStringLiteral(" MB");
    };

    m_metricsActiveLabel->setText(
        QStringLiteral("Active Sessions: %1").arg(active));
    m_metricsBytesLabel->setText(
        QStringLiteral("Transferred: %1").arg(formatBytes(bytes)));
    m_metricsTransfersLabel->setText(
        QStringLiteral("Transfers (S/F): %1 / %2").arg(success).arg(failure));
    m_metricsRetransLabel->setText(
        QStringLiteral("Retransmissions: %1").arg(retrans));
    m_metricsSpeedLabel->setText(
        QStringLiteral("Current Speed: %1/s").arg(formatBytes(qint64(speed))));
}

}  // namespace tftp::gui
