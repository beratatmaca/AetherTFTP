#include "gui/mainwindow.h"

#include "aether/version.h"
#include "core/tftp_client.h"
#include "core/tftp_protocol.h"
#include "core/tftp_server.h"
#include "gui/server_config_dialog.h"
#include "gui/theme_controller.h"
#include "gui/transfer_model.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTime>
#include <QTreeView>
#include <QVBoxLayout>

namespace tftp::gui {

namespace {

/** @brief Human-readable byte count (B / KB / MB). */
QString formatBytes(qint64 bytes) {
    if (bytes < 1024)
        return QString::number(bytes) + QStringLiteral(" B");
    const double kb = double(bytes) / 1024.0;
    if (kb < 1024)
        return QString::number(kb, 'f', 1) + QStringLiteral(" KB");
    return QString::number(kb / 1024.0, 'f', 1) + QStringLiteral(" MB");
}

}  // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("AetherTFTP"));
    setWindowIcon(QIcon(QStringLiteral(":/aether/icon.ico")));
    setAcceptDrops(true);
    setMinimumSize(640, 480);
    m_serverDir = QDir::currentPath();

    m_server = new TftpServer(this);
    connect(m_server, &TftpServer::logMessage, this, &MainWindow::appendLog);
    connect(m_server, &TftpServer::transferStarted, this, [this](const QString &name, bool isUpload) {
        appendLog(QStringLiteral("Incoming %1: %2").arg(isUpload ? QStringLiteral("upload") : QStringLiteral("download"), name));
    });

    m_model = new TransferModel(this);
    m_theme = new ThemeController(qApp, this);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildClientTab(), tr("&Client"));
    m_tabs->addTab(buildServerTab(), tr("&Server"));
    m_tabs->addTab(buildDashboardTab(), tr("&Dashboard"));
    setCentralWidget(m_tabs);

    buildMenus();

    // Status bar with a permanent version readout.
    auto *versionLabel = new QLabel(QString::fromLatin1(AETHER_VERSION_STRING), this);
    statusBar()->addPermanentWidget(versionLabel);
    showStatus(tr("Ready. Drag files onto the window to upload."));

    m_metricsTimer = new QTimer(this);
    m_metricsTimer->setInterval(1000);
    connect(m_metricsTimer, &QTimer::timeout, this, &MainWindow::updateMetrics);
    m_metricsTimer->start();
    m_metricsSpeedTimer.start();

    loadSettings();
    appendLog(QStringLiteral("AetherTFTP %1 ready.").arg(QString::fromLatin1(AETHER_VERSION_STRING)));
}

MainWindow::~MainWindow() = default;

QWidget *MainWindow::buildClientTab() {
    auto *page = new QWidget(this);
    auto *root = new QVBoxLayout(page);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);

    m_hostEdit = new QLineEdit(page);
    m_hostEdit->setPlaceholderText(QStringLiteral("e.g. 192.168.1.10 or host.local"));
    m_hostEdit->setToolTip(tr("Remote TFTP server host name or IP address."));
    form->addRow(tr("&Host:"), m_hostEdit);

    m_clientPortSpin = new QSpinBox(page);
    m_clientPortSpin->setRange(1, 65535);
    m_clientPortSpin->setValue(kDefaultPort);
    m_clientPortSpin->setToolTip(tr("Remote server port (default 69)."));
    form->addRow(tr("P&ort:"), m_clientPortSpin);

    m_fileEdit = new QLineEdit(page);
    m_fileEdit->setPlaceholderText(QStringLiteral("Local path to upload, or remote name to download"));
    m_fileEdit->setToolTip(tr("For uploads: the local file to send.\nFor downloads: the remote file name to fetch."));
    auto *browseBtn = new QPushButton(tr("&Browse…"), page);
    connect(browseBtn, &QPushButton::clicked, this, &MainWindow::browseLocalFile);
    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(m_fileEdit, 1);
    fileRow->addWidget(browseBtn);
    auto *fileLabel = new QLabel(tr("&File:"), page);
    fileLabel->setBuddy(m_fileEdit);
    form->addRow(fileLabel, fileRow);

    m_blockSizeSpin = new QSpinBox(page);
    m_blockSizeSpin->setRange(kMinBlockSize, kMaxBlockSize);
    m_blockSizeSpin->setValue(kDefaultBlockSize);
    m_blockSizeSpin->setSuffix(QStringLiteral(" bytes"));
    m_blockSizeSpin->setToolTip(tr("Requested transfer block size (RFC 2348)."));
    form->addRow(tr("Bloc&k size:"), m_blockSizeSpin);

    m_timeoutSpin = new QSpinBox(page);
    m_timeoutSpin->setRange(500, 60000);
    m_timeoutSpin->setSingleStep(500);
    m_timeoutSpin->setValue(5000);
    m_timeoutSpin->setSuffix(QStringLiteral(" ms"));
    m_timeoutSpin->setToolTip(tr("Per-attempt retransmission timeout."));
    form->addRow(tr("&Timeout:"), m_timeoutSpin);

    root->addLayout(form);

    auto *downloadBtn = new QPushButton(tr("&Download"), page);
    downloadBtn->setToolTip(tr("Fetch the remote file from the server."));
    connect(downloadBtn, &QPushButton::clicked, this, &MainWindow::startDownload);
    auto *uploadBtn = new QPushButton(tr("&Upload"), page);
    uploadBtn->setObjectName(QStringLiteral("primaryButton"));
    uploadBtn->setToolTip(tr("Send the local file to the server."));
    connect(uploadBtn, &QPushButton::clicked, this, &MainWindow::startUpload);

    auto *actionRow = new QHBoxLayout;
    actionRow->addStretch(1);
    actionRow->addWidget(downloadBtn);
    actionRow->addWidget(uploadBtn);
    root->addLayout(actionRow);

    // Enter in the host/file fields triggers a download (the common case).
    connect(m_hostEdit, &QLineEdit::returnPressed, this, &MainWindow::startDownload);
    connect(m_fileEdit, &QLineEdit::returnPressed, this, &MainWindow::startDownload);

    // Transfers toolbar + list.
    auto *listHeader = new QHBoxLayout;
    listHeader->addWidget(new QLabel(tr("Transfers"), page));
    listHeader->addStretch(1);
    auto *clearBtn = new QPushButton(tr("Clear &Completed"), page);
    connect(clearBtn, &QPushButton::clicked, this, &MainWindow::clearCompleted);
    listHeader->addWidget(clearBtn);
    root->addLayout(listHeader);

    m_view = new QTreeView(page);
    m_view->setModel(m_model);
    m_view->setRootIsDecorated(false);
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setItemDelegateForColumn(TransferModel::ColProgress, new ProgressBarDelegate(m_view));
    auto *actions = new TransferActionDelegate(m_view);
    m_view->setItemDelegateForColumn(TransferModel::ColActions, actions);
    connect(actions, &TransferActionDelegate::cancelRequested, this, &MainWindow::cancelTransfer);

    QHeaderView *header = m_view->header();
    header->setSectionResizeMode(TransferModel::ColName, QHeaderView::Stretch);
    header->setSectionResizeMode(TransferModel::ColDirection, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(TransferModel::ColPeer, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(TransferModel::ColProgress, QHeaderView::Fixed);
    m_view->setColumnWidth(TransferModel::ColProgress, 150);
    header->setSectionResizeMode(TransferModel::ColStatus, QHeaderView::Stretch);
    header->setSectionResizeMode(TransferModel::ColActions, QHeaderView::ResizeToContents);
    root->addWidget(m_view, 1);

    return page;
}

QWidget *MainWindow::buildServerTab() {
    auto *page = new QWidget(this);
    auto *root = new QVBoxLayout(page);

    m_serverStatusLabel = new QLabel(tr("● Stopped"), page);
    m_serverStatusLabel->setObjectName(QStringLiteral("statusStopped"));

    m_serverToggleBtn = new QPushButton(tr("&Start Server"), page);
    m_serverToggleBtn->setObjectName(QStringLiteral("primaryButton"));
    connect(m_serverToggleBtn, &QPushButton::clicked, this, &MainWindow::toggleServer);

    auto *settingsBtn = new QPushButton(tr("Se&ttings…"), page);
    connect(settingsBtn, &QPushButton::clicked, this, &MainWindow::configureServer);

    auto *statusRow = new QHBoxLayout;
    statusRow->addWidget(m_serverStatusLabel);
    statusRow->addStretch(1);
    statusRow->addWidget(m_serverToggleBtn);
    statusRow->addWidget(settingsBtn);
    root->addLayout(statusRow);

    auto *logHeader = new QHBoxLayout;
    logHeader->addWidget(new QLabel(tr("Activity Log"), page));
    logHeader->addStretch(1);
    auto *copyBtn = new QPushButton(tr("Cop&y"), page);
    auto *clearLogBtn = new QPushButton(tr("C&lear"), page);
    logHeader->addWidget(copyBtn);
    logHeader->addWidget(clearLogBtn);
    root->addLayout(logHeader);

    m_log = new QPlainTextEdit(page);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(1000);
    m_log->setObjectName(QStringLiteral("logPanel"));
    root->addWidget(m_log, 1);

    connect(copyBtn, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(m_log->toPlainText());
        showStatus(tr("Log copied to clipboard."));
    });
    connect(clearLogBtn, &QPushButton::clicked, m_log, &QPlainTextEdit::clear);

    return page;
}

QWidget *MainWindow::makeMetricCard(const QString &caption, QLabel **valueOut) {
    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("metricCard"));
    auto *layout = new QVBoxLayout(card);

    auto *value = new QLabel(QStringLiteral("0"), card);
    value->setObjectName(QStringLiteral("metricValue"));
    auto *cap = new QLabel(caption, card);
    cap->setObjectName(QStringLiteral("metricCaption"));

    layout->addWidget(value);
    layout->addWidget(cap);
    *valueOut = value;
    return card;
}

QWidget *MainWindow::buildDashboardTab() {
    auto *page = new QWidget(this);
    auto *grid = new QGridLayout(page);
    grid->addWidget(makeMetricCard(tr("Active Sessions"), &m_metricActive), 0, 0);
    grid->addWidget(makeMetricCard(tr("Transferred"), &m_metricBytes), 0, 1);
    grid->addWidget(makeMetricCard(tr("Transfers (Success / Failed)"), &m_metricTransfers), 1, 0);
    grid->addWidget(makeMetricCard(tr("Retransmissions"), &m_metricRetrans), 1, 1);
    grid->addWidget(makeMetricCard(tr("Current Speed"), &m_metricSpeed), 2, 0);
    grid->setRowStretch(3, 1);
    return page;
}

void MainWindow::buildMenus() {
    auto *viewMenu = menuBar()->addMenu(tr("&View"));
    auto *themeMenu = viewMenu->addMenu(tr("&Theme"));
    auto *group = new QActionGroup(this);

    const auto addThemeAction = [&](const QString &text, ThemeController::Mode mode) {
        QAction *act = themeMenu->addAction(text);
        act->setCheckable(true);
        act->setData(int(mode));
        group->addAction(act);
        return act;
    };
    QAction *sys = addThemeAction(tr("&System"), ThemeController::Mode::System);
    QAction *light = addThemeAction(tr("&Light"), ThemeController::Mode::Light);
    QAction *dark = addThemeAction(tr("&Dark"), ThemeController::Mode::Dark);

    // Reflect the persisted choice loaded into the controller.
    switch (m_theme->mode()) {
        case ThemeController::Mode::Light:
            light->setChecked(true);
            break;
        case ThemeController::Mode::Dark:
            dark->setChecked(true);
            break;
        case ThemeController::Mode::System:
            sys->setChecked(true);
            break;
    }

    connect(group, &QActionGroup::triggered, this, [this](QAction *act) {
        const auto mode = ThemeController::Mode(act->data().toInt());
        m_theme->setMode(mode);
        showStatus(tr("Theme: %1").arg(QString(act->text()).remove(QLatin1Char('&'))));
    });

    auto *helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction *aboutAct = helpMenu->addAction(tr("&About AetherTFTP…"));
    connect(aboutAct, &QAction::triggered, this, [this]() {
        QMessageBox::about(this, tr("About AetherTFTP"),
                           tr("<h3>AetherTFTP %1</h3>"
                              "<p>A modern, lightweight, open-source cross-platform TFTP client and server.</p>"
                              "<p>Built with <b>Qt %2</b> and C++17.</p>"
                              "<p>License: <b>MIT</b> &nbsp;·&nbsp; Copyright &copy; 2026 AetherTFTP Project</p>")
                               .arg(QString::fromLatin1(AETHER_VERSION_STRING), QString::fromLatin1(qVersion())));
    });
}

void MainWindow::toggleServer() {
    if (m_serverRunning) {
        m_server->close();
        m_serverRunning = false;
        m_serverStatusLabel->setText(tr("● Stopped"));
        m_serverStatusLabel->setObjectName(QStringLiteral("statusStopped"));
        m_serverToggleBtn->setText(tr("&Start Server"));
        appendLog(QStringLiteral("Server stopped."));
        showStatus(tr("Server stopped."));
    } else {
        if (!m_server->listen(QHostAddress::AnyIPv4, m_serverPort, m_serverDir)) {
            const QString err = m_server->lastError();
            appendLog(QStringLiteral("Failed to start server: %1").arg(err));
            showStatus(tr("Failed to start server: %1").arg(err));
            return;
        }
        m_serverRunning = true;
        m_serverStatusLabel->setText(tr("● Listening on port %1").arg(m_server->port()));
        m_serverStatusLabel->setObjectName(QStringLiteral("statusRunning"));
        m_serverToggleBtn->setText(tr("Sto&p Server"));
        showStatus(tr("Server listening on port %1.").arg(m_server->port()));
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
    appendLog(QStringLiteral("Server settings updated (port %1, dir %2, max %3).").arg(m_serverPort).arg(m_serverDir).arg(m_maxConcurrent));

    if (wasRunning)
        toggleServer();  // restart with the new settings.
}

void MainWindow::browseLocalFile() {
    const QString file = QFileDialog::getOpenFileName(this, tr("Select file to upload"));
    if (!file.isEmpty())
        m_fileEdit->setText(file);
}

void MainWindow::startUpload() {
    const QString host = m_hostEdit->text().trimmed();
    const QString file = m_fileEdit->text().trimmed();
    if (host.isEmpty() || file.isEmpty()) {
        showStatus(tr("Upload needs both a host and a local file."));
        return;
    }
    if (!QFileInfo::exists(file)) {
        showStatus(tr("Local file not found: %1").arg(file));
        return;
    }
    startTransfer(true, host, quint16(m_clientPortSpin->value()), file, QFileInfo(file).fileName());
}

void MainWindow::startDownload() {
    const QString host = m_hostEdit->text().trimmed();
    const QString remote = m_fileEdit->text().trimmed();
    if (host.isEmpty() || remote.isEmpty()) {
        showStatus(tr("Download needs both a host and a remote file name."));
        return;
    }
    const QString suggested = QDir::current().filePath(QFileInfo(remote).fileName());
    const QString dest = QFileDialog::getSaveFileName(this, tr("Save downloaded file as"), suggested);
    if (dest.isEmpty())
        return;
    startTransfer(false, host, quint16(m_clientPortSpin->value()), dest, remote);
}

void MainWindow::startTransfer(bool isUpload, const QString &host, quint16 port, const QString &localFile, const QString &remoteName) {
    if (activeTransfers() >= m_maxConcurrent) {
        showStatus(tr("Transfer limit reached (%1). Wait for one to finish.").arg(m_maxConcurrent));
        return;
    }

    auto *client = new TftpClient(this);
    client->setBlockSize(m_blockSizeSpin->value());
    client->setTimeout(m_timeoutSpin->value());

    const quint64 id = m_nextId++;
    const QString peer = QStringLiteral("%1:%2").arg(host).arg(port);
    m_model->addTransfer(id, remoteName, isUpload, peer);
    m_idOf.insert(client, id);
    m_clientById.insert(id, client);

    connect(client, &TftpClient::progress, this,
            [this, id](qint64 done, qint64 total) { m_model->updateProgress(m_model->rowForId(id), done, total); });
    connect(client, &TftpClient::errorOccurred, this, [this, client](const QString &message) { m_lastError.insert(client, message); });
    connect(client, &TftpClient::transferFinished, this, [this, client, id, remoteName](bool ok) {
        const QString err = m_lastError.value(client);
        m_model->setFinished(m_model->rowForId(id), ok, err);
        appendLog(QStringLiteral("%1 %2%3").arg(remoteName, ok ? QStringLiteral("completed") : QStringLiteral("failed"),
                                                (!ok && !err.isEmpty()) ? QStringLiteral(": %1").arg(err) : QString()));
        m_idOf.remove(client);
        m_clientById.remove(id);
        m_lastError.remove(client);
        client->deleteLater();
    });

    appendLog(QStringLiteral("%1 %2 : %3").arg(isUpload ? QStringLiteral("Uploading") : QStringLiteral("Downloading"), remoteName, peer));
    showStatus(tr("%1 %2…").arg(isUpload ? tr("Uploading") : tr("Downloading"), remoteName));

    if (isUpload)
        client->uploadFile(host, port, localFile, remoteName);
    else
        client->downloadFile(host, port, remoteName, localFile);
}

void MainWindow::cancelTransfer(int row) {
    const quint64 id = m_model->idForRow(row);
    if (id == 0)
        return;
    TftpClient *client = m_clientById.value(id, nullptr);
    if (!client)
        return;
    m_model->setCancelled(row);
    client->abort();  // synchronously finishes; the row stays "Cancelled".
    showStatus(tr("Transfer cancelled."));
}

void MainWindow::clearCompleted() {
    m_model->removeFinished();
    showStatus(tr("Cleared finished transfers."));
}

int MainWindow::activeTransfers() const {
    return m_idOf.size();
}

void MainWindow::appendLog(const QString &message) {
    if (m_log)
        m_log->appendPlainText(QStringLiteral("[%1] %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), message));
}

void MainWindow::showStatus(const QString &message) {
    statusBar()->showMessage(message, 6000);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event) {
    const QString host = m_hostEdit->text().trimmed();
    if (host.isEmpty()) {
        showStatus(tr("Enter a host before dropping files to upload."));
        m_tabs->setCurrentIndex(0);
        return;
    }
    const auto port = quint16(m_clientPortSpin->value());
    for (const QUrl &url : event->mimeData()->urls()) {
        const QString local = url.toLocalFile();
        if (local.isEmpty() || !QFileInfo(local).isFile())
            continue;
        startTransfer(true, host, port, local, QFileInfo(local).fileName());
    }
    event->acceptProposedAction();
}

void MainWindow::updateMetrics() {
    if (!m_server)
        return;

    const int active = m_server->activeSessions();
    const qint64 bytes = m_server->totalBytesTransferred();
    const qint64 success = m_server->transfersSuccess();
    const qint64 failure = m_server->transfersFailure();
    const qint64 retrans = m_server->retransmissionCount();

    const qint64 elapsedMs = m_metricsSpeedTimer.restart();
    double speed = 0.0;
    if (elapsedMs > 0)
        speed = double(bytes - m_lastTotalBytes) / (double(elapsedMs) / 1000.0);
    m_lastTotalBytes = bytes;

    m_metricActive->setText(QString::number(active));
    m_metricBytes->setText(formatBytes(bytes));
    m_metricTransfers->setText(QStringLiteral("%1 / %2").arg(success).arg(failure));
    m_metricRetrans->setText(QString::number(retrans));
    m_metricSpeed->setText(formatBytes(qint64(speed)) + QStringLiteral("/s"));
}

void MainWindow::loadSettings() {
    QSettings settings;

    m_hostEdit->setText(settings.value(QStringLiteral("client/host")).toString());
    m_clientPortSpin->setValue(settings.value(QStringLiteral("client/port"), kDefaultPort).toInt());
    m_blockSizeSpin->setValue(settings.value(QStringLiteral("client/blockSize"), kDefaultBlockSize).toInt());
    m_timeoutSpin->setValue(settings.value(QStringLiteral("client/timeoutMs"), 5000).toInt());

    m_serverPort = quint16(settings.value(QStringLiteral("server/port"), m_serverPort).toUInt());
    m_serverDir = settings.value(QStringLiteral("server/dir"), m_serverDir).toString();
    m_maxConcurrent = settings.value(QStringLiteral("server/maxConcurrent"), m_maxConcurrent).toInt();

    m_theme->setMode(ThemeController::modeFromString(settings.value(QStringLiteral("ui/theme")).toString()));

    if (settings.contains(QStringLiteral("ui/geometry")))
        restoreGeometry(settings.value(QStringLiteral("ui/geometry")).toByteArray());
    if (settings.contains(QStringLiteral("ui/windowState")))
        restoreState(settings.value(QStringLiteral("ui/windowState")).toByteArray());
    m_tabs->setCurrentIndex(settings.value(QStringLiteral("ui/tab"), 0).toInt());
}

void MainWindow::saveSettings() {
    QSettings settings;

    settings.setValue(QStringLiteral("client/host"), m_hostEdit->text());
    settings.setValue(QStringLiteral("client/port"), m_clientPortSpin->value());
    settings.setValue(QStringLiteral("client/blockSize"), m_blockSizeSpin->value());
    settings.setValue(QStringLiteral("client/timeoutMs"), m_timeoutSpin->value());

    settings.setValue(QStringLiteral("server/port"), m_serverPort);
    settings.setValue(QStringLiteral("server/dir"), m_serverDir);
    settings.setValue(QStringLiteral("server/maxConcurrent"), m_maxConcurrent);

    settings.setValue(QStringLiteral("ui/theme"), ThemeController::modeToString(m_theme->mode()));
    settings.setValue(QStringLiteral("ui/tab"), m_tabs->currentIndex());
    settings.setValue(QStringLiteral("ui/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("ui/windowState"), saveState());
}

void MainWindow::closeEvent(QCloseEvent *event) {
    saveSettings();
    QMainWindow::closeEvent(event);
}

}  // namespace tftp::gui
