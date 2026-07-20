#include "gui/mainwindow.h"

#include "aether/version.h"
#include "core/tftp_client.h"
#include "core/tftp_protocol.h"
#include "core/tftp_server.h"
#include "gui/theme_controller.h"
#include "gui/transfer_model.h"
#include "gui/speed_chart_widget.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QInputDialog>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
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
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QScrollArea>
#include <QTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTreeView>
#include <QVBoxLayout>
#include <QSystemTrayIcon>
#include <QMenu>

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

    setCentralWidget(buildMainView());

    buildMenus();

    setupTrayIcon();

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

QWidget *MainWindow::buildMainView() {
    auto *central = new QWidget(this);
    auto *mainLayout = new QHBoxLayout(central);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(10);

    // Left Column: a Client/Server mode switch above whichever config panel
    // is active. You're driving the client or minding the server, rarely
    // both at once, so only one surface is on screen at a time.
    auto *scrollArea = new QScrollArea(central);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *leftContainer = new QWidget(scrollArea);
    leftContainer->setObjectName(QStringLiteral("leftContainer"));
    auto *leftLayout = new QVBoxLayout(leftContainer);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(10);

    auto *modeBar = new QFrame(leftContainer);
    modeBar->setObjectName(QStringLiteral("segmentedControl"));
    auto *modeBarLayout = new QHBoxLayout(modeBar);
    modeBarLayout->setContentsMargins(3, 3, 3, 3);
    modeBarLayout->setSpacing(0);

    auto *clientModeBtn = new QPushButton(tr("Client"), modeBar);
    auto *serverModeBtn = new QPushButton(tr("Server"), modeBar);
    for (QPushButton *btn : {clientModeBtn, serverModeBtn}) {
        btn->setObjectName(QStringLiteral("segmentButton"));
        btn->setCheckable(true);
        btn->setToolTip(tr("Switch the panel below between client and server configuration."));
    }
    clientModeBtn->setChecked(true);

    m_configModeGroup = new QButtonGroup(modeBar);
    m_configModeGroup->setExclusive(true);
    m_configModeGroup->addButton(clientModeBtn, 0);
    m_configModeGroup->addButton(serverModeBtn, 1);

    modeBarLayout->addWidget(clientModeBtn, 1);
    modeBarLayout->addWidget(serverModeBtn, 1);
    leftLayout->addWidget(modeBar);

    m_configStack = new QStackedWidget(leftContainer);
    connect(m_configModeGroup, &QButtonGroup::idClicked, m_configStack, &QStackedWidget::setCurrentIndex);

    // Group 1: Client Settings
    auto *clientGroup = new QGroupBox(tr("Client Operations"), leftContainer);
    auto *clientForm = new QFormLayout(clientGroup);
    clientForm->setLabelAlignment(Qt::AlignRight);

    // Profile selector row
    auto *profileRow = new QHBoxLayout;
    m_profileCombo = new QComboBox(leftContainer);
    m_profileCombo->addItem(tr("<Default Settings>"));
    m_profileCombo->setToolTip(tr("Select a saved client configuration profile."));

    auto *saveProfileBtn = new QPushButton(tr("&Save"), leftContainer);
    saveProfileBtn->setToolTip(tr("Save the current input fields as a new client profile."));
    connect(saveProfileBtn, &QPushButton::clicked, this, &MainWindow::saveCurrentProfile);

    auto *deleteProfileBtn = new QPushButton(tr("&Delete"), leftContainer);
    deleteProfileBtn->setToolTip(tr("Delete the currently selected configuration profile."));
    connect(deleteProfileBtn, &QPushButton::clicked, this, &MainWindow::deleteCurrentProfile);

    profileRow->addWidget(m_profileCombo, 1);
    profileRow->addWidget(saveProfileBtn);
    profileRow->addWidget(deleteProfileBtn);
    clientForm->addRow(tr("Profile:"), profileRow);
    connect(m_profileCombo, &QComboBox::currentIndexChanged, this, &MainWindow::onProfileChanged);

    m_hostEdit = new QLineEdit(leftContainer);
    m_hostEdit->setPlaceholderText(QStringLiteral("e.g. 192.168.1.10 or host.local"));
    m_hostEdit->setToolTip(tr("Remote TFTP server host name or IP address."));
    clientForm->addRow(tr("&Host:"), m_hostEdit);

    m_clientPortSpin = new QSpinBox(leftContainer);
    m_clientPortSpin->setRange(1, 65535);
    m_clientPortSpin->setValue(kDefaultPort);
    m_clientPortSpin->setToolTip(tr("Remote server port (default 69)."));
    clientForm->addRow(tr("P&ort:"), m_clientPortSpin);

    m_fileEdit = new QLineEdit(leftContainer);
    m_fileEdit->setPlaceholderText(QStringLiteral("Local path to upload, or remote name to download"));
    m_fileEdit->setToolTip(tr("For uploads: the local file to send.\nFor downloads: the remote file name to fetch."));
    auto *browseBtn = new QPushButton(tr("&Browse…"), leftContainer);
    connect(browseBtn, &QPushButton::clicked, this, &MainWindow::browseLocalFile);
    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(m_fileEdit, 1);
    fileRow->addWidget(browseBtn);
    auto *fileLabel = new QLabel(tr("&File:"), leftContainer);
    fileLabel->setBuddy(m_fileEdit);
    clientForm->addRow(fileLabel, fileRow);

    m_blockSizeSpin = new QSpinBox(leftContainer);
    m_blockSizeSpin->setRange(kMinBlockSize, kMaxBlockSize);
    m_blockSizeSpin->setValue(kDefaultBlockSize);
    m_blockSizeSpin->setSuffix(QStringLiteral(" bytes"));
    m_blockSizeSpin->setToolTip(tr("Requested transfer block size (RFC 2348)."));
    clientForm->addRow(tr("Bloc&k size:"), m_blockSizeSpin);

    m_timeoutSpin = new QSpinBox(leftContainer);
    m_timeoutSpin->setRange(500, 60000);
    m_timeoutSpin->setSingleStep(500);
    m_timeoutSpin->setValue(5000);
    m_timeoutSpin->setSuffix(QStringLiteral(" ms"));
    m_timeoutSpin->setToolTip(tr("Per-attempt retransmission timeout."));
    clientForm->addRow(tr("&Timeout:"), m_timeoutSpin);

    m_windowSizeSpin = new QSpinBox(leftContainer);
    m_windowSizeSpin->setRange(1, 64);
    m_windowSizeSpin->setValue(1);
    m_windowSizeSpin->setToolTip(
        tr("Requested transfer window size (RFC 7440). Higher values allow sending multiple blocks before waiting for an ACK."));
    clientForm->addRow(tr("&Window size:"), m_windowSizeSpin);

    m_pskKeyEdit = new QLineEdit(leftContainer);
    m_pskKeyEdit->setPlaceholderText(tr("Optional encryption password"));
    m_pskKeyEdit->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    m_pskKeyEdit->setToolTip(tr("Symmetric pre-shared key (passphrase) to secure data packets. Leave empty for standard TFTP."));
    clientForm->addRow(tr("&Symmetric Key:"), m_pskKeyEdit);

    auto *downloadBtn = new QPushButton(tr("&Download"), leftContainer);
    downloadBtn->setToolTip(tr("Fetch the remote file from the server."));
    connect(downloadBtn, &QPushButton::clicked, this, &MainWindow::startDownload);
    auto *uploadBtn = new QPushButton(tr("&Upload"), leftContainer);
    uploadBtn->setToolTip(tr("Send the local file to the server."));
    connect(uploadBtn, &QPushButton::clicked, this, &MainWindow::startUpload);

    auto *actionRow = new QHBoxLayout;
    actionRow->addWidget(downloadBtn);
    actionRow->addWidget(uploadBtn);
    clientForm->addRow(QString(), actionRow);

    connect(m_hostEdit, &QLineEdit::returnPressed, this, &MainWindow::startDownload);
    connect(m_fileEdit, &QLineEdit::returnPressed, this, &MainWindow::startDownload);

    m_configStack->addWidget(clientGroup);

    // Group 2: Server Settings
    auto *serverGroup = new QGroupBox(tr("Server Settings"), leftContainer);
    auto *serverForm = new QFormLayout(serverGroup);
    serverForm->setLabelAlignment(Qt::AlignRight);

    // Profile selector row (mirrors the client panel's).
    auto *serverProfileRow = new QHBoxLayout;
    m_serverProfileCombo = new QComboBox(leftContainer);
    m_serverProfileCombo->addItem(tr("<Default Settings>"));
    m_serverProfileCombo->setToolTip(tr("Select a saved server configuration profile."));

    auto *saveServerProfileBtn = new QPushButton(tr("Sa&ve"), leftContainer);
    saveServerProfileBtn->setToolTip(tr("Save the current server fields as a new profile."));
    connect(saveServerProfileBtn, &QPushButton::clicked, this, &MainWindow::saveCurrentServerProfile);

    auto *deleteServerProfileBtn = new QPushButton(tr("De&lete"), leftContainer);
    deleteServerProfileBtn->setToolTip(tr("Delete the currently selected server profile."));
    connect(deleteServerProfileBtn, &QPushButton::clicked, this, &MainWindow::deleteCurrentServerProfile);

    serverProfileRow->addWidget(m_serverProfileCombo, 1);
    serverProfileRow->addWidget(saveServerProfileBtn);
    serverProfileRow->addWidget(deleteServerProfileBtn);
    serverForm->addRow(tr("Profile:"), serverProfileRow);
    connect(m_serverProfileCombo, &QComboBox::currentIndexChanged, this, &MainWindow::onServerProfileChanged);

    // Add Status / Toggle inside the settings group box for clean UI!
    auto *serverToggleRow = new QHBoxLayout;
    m_serverStatusLabel = new QLabel(tr("● Stopped"), leftContainer);
    m_serverStatusLabel->setObjectName(QStringLiteral("statusStopped"));
    m_serverToggleBtn = new QPushButton(tr("&Start Server"), leftContainer);
    m_serverToggleBtn->setObjectName(QStringLiteral("primaryButton"));
    connect(m_serverToggleBtn, &QPushButton::clicked, this, &MainWindow::toggleServer);
    serverToggleRow->addWidget(m_serverStatusLabel);
    serverToggleRow->addStretch(1);
    serverToggleRow->addWidget(m_serverToggleBtn);
    serverForm->addRow(tr("Status:"), serverToggleRow);

    m_serverPortSpin = new QSpinBox(leftContainer);
    m_serverPortSpin->setRange(1, 65535);
    m_serverPortSpin->setValue(6969);
    m_serverPortSpin->setToolTip(tr("UDP port for incoming TFTP connection handshakes. Default is 69."));
    serverForm->addRow(tr("Port:"), m_serverPortSpin);

    auto *serverDirLayout = new QHBoxLayout;
    m_serverDirEdit = new QLineEdit(leftContainer);
    m_serverDirEdit->setToolTip(tr("The root folder containing served files, and where uploaded files will be stored."));
    auto *browseDirBtn = new QPushButton(tr("Browse…"), leftContainer);
    browseDirBtn->setToolTip(tr("Browse and select a folder to use as the server root directory."));
    connect(browseDirBtn, &QPushButton::clicked, this, &MainWindow::browseServerDir);
    serverDirLayout->addWidget(m_serverDirEdit, 1);
    serverDirLayout->addWidget(browseDirBtn);
    serverForm->addRow(tr("Root Dir:"), serverDirLayout);

    m_serverMaxSpin = new QSpinBox(leftContainer);
    m_serverMaxSpin->setRange(1, 100);
    m_serverMaxSpin->setValue(4);
    m_serverMaxSpin->setToolTip(tr("The maximum number of simultaneous active client transfer sessions allowed."));
    serverForm->addRow(tr("Max Conn:"), m_serverMaxSpin);

    m_serverSinglePortCheck = new QCheckBox(tr("Single-Port Mode"), leftContainer);
    m_serverSinglePortCheck->setToolTip(
        tr("Handle all traffic on the main listening port (single port multiplexing) instead of allocating temporary ports."));
    serverForm->addRow(QString(), m_serverSinglePortCheck);

    m_serverJsonLoggingCheck = new QCheckBox(tr("JSON Logging"), leftContainer);
    m_serverJsonLoggingCheck->setToolTip(tr("Format all console activity logs into compact structured JSON strings."));
    serverForm->addRow(QString(), m_serverJsonLoggingCheck);

    m_serverAutoStartCheck = new QCheckBox(tr("Auto-Start on Launch"), leftContainer);
    m_serverAutoStartCheck->setToolTip(tr("Automatically start the TFTP server when the application is launched."));
    serverForm->addRow(QString(), m_serverAutoStartCheck);

    m_serverAllowedExtsEdit = new QLineEdit(leftContainer);
    m_serverAllowedExtsEdit->setPlaceholderText(tr("e.g. txt,bin (empty = all)"));
    m_serverAllowedExtsEdit->setToolTip(
        tr("Comma-separated file extensions that clients are allowed to request (e.g. txt,pdf). Leave empty for all."));
    serverForm->addRow(tr("Whitelist:"), m_serverAllowedExtsEdit);

    m_serverBlockedExtsEdit = new QLineEdit(leftContainer);
    m_serverBlockedExtsEdit->setPlaceholderText(tr("e.g. exe,sh"));
    m_serverBlockedExtsEdit->setToolTip(tr("Comma-separated file extensions that are forbidden to transfer."));
    serverForm->addRow(tr("Blacklist:"), m_serverBlockedExtsEdit);

    m_serverIpWhitelistEdit = new QLineEdit(leftContainer);
    m_serverIpWhitelistEdit->setPlaceholderText(tr("e.g. 192.168.1.0/24,10.0.0.1"));
    m_serverIpWhitelistEdit->setToolTip(
        tr("Comma-separated client IPs or CIDR subnets allowed to access the server. Leave empty to allow all."));
    serverForm->addRow(tr("IP Whitelist:"), m_serverIpWhitelistEdit);

    m_serverIpBlacklistEdit = new QLineEdit(leftContainer);
    m_serverIpBlacklistEdit->setPlaceholderText(tr("e.g. 192.168.1.100"));
    m_serverIpBlacklistEdit->setToolTip(tr("Comma-separated client IPs or CIDR subnets blocked from accessing the server."));
    serverForm->addRow(tr("IP Blacklist:"), m_serverIpBlacklistEdit);

    m_serverReadOnlyDirsEdit = new QLineEdit(leftContainer);
    m_serverReadOnlyDirsEdit->setPlaceholderText(tr("e.g. public,docs"));
    m_serverReadOnlyDirsEdit->setToolTip(tr("Comma-separated relative folders inside root where file writes/uploads are blocked."));
    serverForm->addRow(tr("Read-Only:"), m_serverReadOnlyDirsEdit);

    m_serverVirtualMappingsEdit = new QLineEdit(leftContainer);
    m_serverVirtualMappingsEdit->setPlaceholderText(tr("e.g. fw=/tmp/fw,bin=/var/bin"));
    m_serverVirtualMappingsEdit->setToolTip(tr("Comma-separated virtual prefix to physical path mappings (prefix=path)."));
    serverForm->addRow(tr("Virtual Paths:"), m_serverVirtualMappingsEdit);

    m_serverGlobalLimitSpin = new QSpinBox(leftContainer);
    m_serverGlobalLimitSpin->setRange(0, 1000000);
    m_serverGlobalLimitSpin->setSuffix(tr(" KB/s"));
    m_serverGlobalLimitSpin->setSpecialValueText(tr("Unlimited"));
    m_serverGlobalLimitSpin->setToolTip(tr("The maximum cumulative bandwidth speed limit for the entire server (KB/s). 0 is unlimited."));
    serverForm->addRow(tr("Global Limit:"), m_serverGlobalLimitSpin);

    m_serverSessionLimitSpin = new QSpinBox(leftContainer);
    m_serverSessionLimitSpin->setRange(0, 1000000);
    m_serverSessionLimitSpin->setSuffix(tr(" KB/s"));
    m_serverSessionLimitSpin->setSpecialValueText(tr("Unlimited"));
    m_serverSessionLimitSpin->setToolTip(
        tr("The maximum individual bandwidth limit for each separate transfer session (KB/s). 0 is unlimited."));
    serverForm->addRow(tr("Session Limit:"), m_serverSessionLimitSpin);

    m_serverPskKeyEdit = new QLineEdit(leftContainer);
    m_serverPskKeyEdit->setPlaceholderText(tr("Optional encryption password"));
    m_serverPskKeyEdit->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    m_serverPskKeyEdit->setToolTip(tr("Symmetric pre-shared key (passphrase) to secure data packets. Leave empty for standard TFTP."));
    serverForm->addRow(tr("Symmetric Key:"), m_serverPskKeyEdit);

    connect(m_serverPortSpin, &QSpinBox::valueChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverDirEdit, &QLineEdit::textChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverPskKeyEdit, &QLineEdit::textChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverMaxSpin, &QSpinBox::valueChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverSinglePortCheck, &QCheckBox::stateChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverJsonLoggingCheck, &QCheckBox::stateChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverAutoStartCheck, &QCheckBox::stateChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverAllowedExtsEdit, &QLineEdit::textChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverBlockedExtsEdit, &QLineEdit::textChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverIpWhitelistEdit, &QLineEdit::textChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverIpBlacklistEdit, &QLineEdit::textChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverReadOnlyDirsEdit, &QLineEdit::textChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverVirtualMappingsEdit, &QLineEdit::textChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverGlobalLimitSpin, &QSpinBox::valueChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverSessionLimitSpin, &QSpinBox::valueChanged, this, &MainWindow::applyServerConfig);

    m_configStack->addWidget(serverGroup);
    leftLayout->addWidget(m_configStack, 1);

    scrollArea->setWidget(leftContainer);
    mainLayout->addWidget(scrollArea, 30);  // Left sidebar width percentage

    // Right Column: Live Dashboard (Stats card row + Speed Chart) + Active Transfers List + Activity Log
    auto *rightContainer = new QWidget(central);
    auto *rightLayout = new QVBoxLayout(rightContainer);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(10);

    // Live Dashboard Card
    auto *dashGroup = new QGroupBox(tr("Live Dashboard"), rightContainer);
    auto *dashLayout = new QVBoxLayout(dashGroup);

    auto *metricsRow = new QHBoxLayout;
    metricsRow->addWidget(makeMetricCard(tr("Active Sessions"), &m_metricActive), 1);
    metricsRow->addWidget(makeMetricCard(tr("Speed"), &m_metricSpeed), 1);
    metricsRow->addWidget(makeMetricCard(tr("Transferred"), &m_metricBytes), 1);
    metricsRow->addWidget(makeMetricCard(tr("Ratio (Ok/Err)"), &m_metricTransfers), 1);
    metricsRow->addWidget(makeMetricCard(tr("Retransmissions"), &m_metricRetrans), 1);
    dashLayout->addLayout(metricsRow);

    m_speedChart = new SpeedChartWidget(dashGroup);
    m_speedChart->setMinimumHeight(150);
    dashLayout->addWidget(m_speedChart, 1);
    rightLayout->addWidget(dashGroup);

    // Active Transfers Table Card
    auto *transfersGroup = new QGroupBox(tr("Active Client Transfers"), rightContainer);
    auto *transfersLayout = new QVBoxLayout(transfersGroup);

    auto *listHeader = new QHBoxLayout;
    listHeader->addStretch(1);
    auto *clearBtn = new QPushButton(tr("Clear &Completed"), transfersGroup);
    connect(clearBtn, &QPushButton::clicked, this, &MainWindow::clearCompleted);
    listHeader->addWidget(clearBtn);
    transfersLayout->addLayout(listHeader);

    m_view = new QTreeView(transfersGroup);
    m_view->setModel(m_model);
    m_view->setRootIsDecorated(false);
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setItemDelegateForColumn(TransferModel::ColProgress, new ProgressBarDelegate(m_view));
    auto *actions = new TransferActionDelegate(m_view);
    m_view->setItemDelegateForColumn(TransferModel::ColActions, actions);
    connect(actions, &TransferActionDelegate::cancelRequested, this, &MainWindow::cancelTransfer);

    QHeaderView *header = m_view->header();
    header->setSectionResizeMode(TransferModel::ColName, QHeaderView::Interactive);
    header->setSectionResizeMode(TransferModel::ColDirection, QHeaderView::Interactive);
    header->setSectionResizeMode(TransferModel::ColPeer, QHeaderView::Interactive);
    header->setSectionResizeMode(TransferModel::ColProgress, QHeaderView::Interactive);
    header->setSectionResizeMode(TransferModel::ColStatus, QHeaderView::Stretch);
    header->setSectionResizeMode(TransferModel::ColActions, QHeaderView::ResizeToContents);
    header->setStretchLastSection(false);
    header->setSectionsMovable(true);

    m_view->setColumnWidth(TransferModel::ColName, 200);
    m_view->setColumnWidth(TransferModel::ColDirection, 80);
    m_view->setColumnWidth(TransferModel::ColPeer, 130);
    m_view->setColumnWidth(TransferModel::ColProgress, 120);
    m_view->setMinimumHeight(140);
    transfersLayout->addWidget(m_view, 1);
    rightLayout->addWidget(transfersGroup, 1);

    // Server Activity Log Card
    auto *logGroup = new QGroupBox(tr("Server Activity Log"), rightContainer);
    auto *logLayout = new QVBoxLayout(logGroup);

    auto *logHeader = new QHBoxLayout;
    logHeader->addStretch(1);
    auto *copyBtn = new QPushButton(tr("Cop&y"), logGroup);
    auto *clearLogBtn = new QPushButton(tr("C&lear"), logGroup);
    logHeader->addWidget(copyBtn);
    logHeader->addWidget(clearLogBtn);
    logLayout->addLayout(logHeader);

    auto *filterLayout = new QHBoxLayout;
    m_logSearchEdit = new QLineEdit(logGroup);
    m_logSearchEdit->setPlaceholderText(tr("Search logs..."));
    m_logSearchEdit->setClearButtonEnabled(true);

    m_logFilterCombo = new QComboBox(logGroup);
    m_logFilterCombo->addItem(tr("All Severity"), QStringLiteral("all"));
    m_logFilterCombo->addItem(tr("Info Only"), QStringLiteral("info"));
    m_logFilterCombo->addItem(tr("Warning Only"), QStringLiteral("warn"));
    m_logFilterCombo->addItem(tr("Error Only"), QStringLiteral("error"));

    filterLayout->addWidget(new QLabel(tr("Search:"), logGroup));
    filterLayout->addWidget(m_logSearchEdit, 2);
    filterLayout->addWidget(new QLabel(tr("Filter:"), logGroup));
    filterLayout->addWidget(m_logFilterCombo, 1);
    logLayout->addLayout(filterLayout);

    m_log = new QPlainTextEdit(logGroup);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(1000);
    m_log->setPlaceholderText(tr("Server and transfer activity will appear here."));
    m_log->setObjectName(QStringLiteral("logPanel"));
    m_log->setMinimumHeight(100);
    logLayout->addWidget(m_log, 1);

    connect(copyBtn, &QPushButton::clicked, this, [this]() { QApplication::clipboard()->setText(m_log->toPlainText()); });
    connect(clearLogBtn, &QPushButton::clicked, this, [this]() {
        m_allLogLines.clear();
        m_log->clear();
    });

    connect(m_logSearchEdit, &QLineEdit::textChanged, this, &MainWindow::filterLog);
    connect(m_logFilterCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &MainWindow::filterLog);

    rightLayout->addWidget(logGroup, 1);

    mainLayout->addWidget(rightContainer, 70);  // Right columns take remaining space

    return central;
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
    QAction *nord = addThemeAction(tr("&Nord"), ThemeController::Mode::Nord);

    // Reflect the persisted choice loaded into the controller.
    switch (m_theme->mode()) {
        case ThemeController::Mode::Light:
            light->setChecked(true);
            break;
        case ThemeController::Mode::Dark:
            dark->setChecked(true);
            break;
        case ThemeController::Mode::Nord:
            nord->setChecked(true);
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

    auto *langMenu = viewMenu->addMenu(tr("&Language"));
    auto *langGroup = new QActionGroup(this);
    const auto addLangAction = [&](const QString &text, const QString &code) {
        QAction *act = langMenu->addAction(text);
        act->setCheckable(true);
        act->setData(code);
        langGroup->addAction(act);
        return act;
    };
    QAction *langSys = addLangAction(tr("&System Default"), QStringLiteral("system"));
    QAction *langEn = addLangAction(tr("&English"), QStringLiteral("en"));
    QAction *langDe = addLangAction(tr("&German (Deutsch)"), QStringLiteral("de"));
    QAction *langTr = addLangAction(tr("&Turkish (Türkçe)"), QStringLiteral("tr"));
    QAction *langEs = addLangAction(tr("&Spanish (Español)"), QStringLiteral("es"));

    QSettings langSettings;
    QString currentLang = langSettings.value(QStringLiteral("general/language"), QStringLiteral("system")).toString();
    if (currentLang == QStringLiteral("de"))
        langDe->setChecked(true);
    else if (currentLang == QStringLiteral("tr"))
        langTr->setChecked(true);
    else if (currentLang == QStringLiteral("es"))
        langEs->setChecked(true);
    else if (currentLang == QStringLiteral("en"))
        langEn->setChecked(true);
    else
        langSys->setChecked(true);

    connect(langGroup, &QActionGroup::triggered, this, [this](QAction *act) {
        QString code = act->data().toString();
        QSettings settings;
        settings.setValue(QStringLiteral("general/language"), code);
        QMessageBox::information(this, tr("Language Changed"), tr("Please restart the application to apply the language change."));
    });

    auto *profilesMenu = menuBar()->addMenu(tr("&Profiles"));
    QAction *importClientAct = profilesMenu->addAction(tr("Import &Client Profile…"));
    QAction *exportClientAct = profilesMenu->addAction(tr("Export C&lient Profile…"));
    profilesMenu->addSeparator();
    QAction *importServerAct = profilesMenu->addAction(tr("Import &Server Profile…"));
    QAction *exportServerAct = profilesMenu->addAction(tr("Export S&erver Profile…"));

    connect(importClientAct, &QAction::triggered, this, &MainWindow::importClientProfile);
    connect(exportClientAct, &QAction::triggered, this, &MainWindow::exportClientProfile);
    connect(importServerAct, &QAction::triggered, this, &MainWindow::importServerProfile);
    connect(exportServerAct, &QAction::triggered, this, &MainWindow::exportServerProfile);

    auto *helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction *aboutAct = helpMenu->addAction(tr("&About AetherTFTP…"));
    connect(aboutAct, &QAction::triggered, this, [this]() {
        QMessageBox::about(
            this, tr("About AetherTFTP"),
            tr("<h3>AetherTFTP %1</h3>"
               "<p>A modern, lightweight, open-source cross-platform TFTP client and server.</p>"
               "<p>Built with <b>Qt %2</b> and C++17.</p>"
               "<p>GitHub Page: <a href=\"https://github.com/beratatmaca/AetherTFTP\">github.com/beratatmaca/AetherTFTP</a></p>"
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
        if (m_trayToggleServerAction) {
            m_trayToggleServerAction->setText(tr("Start Server"));
        }
        appendLog(QStringLiteral("Server stopped."));
        showStatus(tr("Server stopped."));
    } else {
        m_server->setSinglePortMode(m_serverSinglePort);
        m_server->setJsonLoggingEnabled(m_serverJsonLogging);
        m_server->setAllowedExtensions(m_serverAllowedExts);
        m_server->setBlockedExtensions(m_serverBlockedExts);
        m_server->setWhitelist(m_serverIpWhitelist);
        m_server->setBlacklist(m_serverIpBlacklist);
        m_server->setReadOnlyDirectories(m_serverReadOnlyDirs);
        m_server->setVirtualMappings(m_serverVirtualMappings);
        m_server->setPskKey(m_serverPskKey);
        m_server->setGlobalLimit(qint64(m_serverGlobalLimit) * 1024);
        m_server->setSessionLimit(qint64(m_serverSessionLimit) * 1024);

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
        if (m_trayToggleServerAction) {
            m_trayToggleServerAction->setText(tr("Stop Server"));
        }
        showStatus(tr("Server listening on port %1.").arg(m_server->port()));
    }
    // Re-polish so the object-name-based status colour updates.
    m_serverStatusLabel->style()->unpolish(m_serverStatusLabel);
    m_serverStatusLabel->style()->polish(m_serverStatusLabel);
}

void MainWindow::browseServerDir() {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Select Root Directory"), m_serverDirEdit->text());
    if (!dir.isEmpty()) {
        m_serverDirEdit->setText(dir);
    }
}

void MainWindow::applyServerConfig() {
    m_serverPort = quint16(m_serverPortSpin->value());
    m_serverDir = m_serverDirEdit->text().trimmed();
    m_maxConcurrent = m_serverMaxSpin->value();
    m_serverSinglePort = m_serverSinglePortCheck->isChecked();
    m_serverJsonLogging = m_serverJsonLoggingCheck->isChecked();
    m_serverAutoStart = m_serverAutoStartCheck->isChecked();

    m_serverAllowedExts.clear();
    for (const QString &item : m_serverAllowedExtsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        m_serverAllowedExts.append(item.trimmed());
    }

    m_serverBlockedExts.clear();
    for (const QString &item : m_serverBlockedExtsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        m_serverBlockedExts.append(item.trimmed());
    }

    m_serverIpWhitelist.clear();
    for (const QString &item : m_serverIpWhitelistEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        m_serverIpWhitelist.append(item.trimmed());
    }

    m_serverIpBlacklist.clear();
    for (const QString &item : m_serverIpBlacklistEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        m_serverIpBlacklist.append(item.trimmed());
    }

    m_serverReadOnlyDirs.clear();
    for (const QString &item : m_serverReadOnlyDirsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        m_serverReadOnlyDirs.append(item.trimmed());
    }

    m_serverVirtualMappings.clear();
    for (const QString &item : m_serverVirtualMappingsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        int eq = item.indexOf(QLatin1Char('='));
        if (eq > 0) {
            m_serverVirtualMappings.insert(item.left(eq).trimmed(), item.mid(eq + 1).trimmed());
        }
    }

    m_serverGlobalLimit = m_serverGlobalLimitSpin->value();
    m_serverSessionLimit = m_serverSessionLimitSpin->value();
    m_serverPskKey = m_serverPskKeyEdit ? m_serverPskKeyEdit->text() : QString();

    if (m_server) {
        m_server->setSinglePortMode(m_serverSinglePort);
        m_server->setJsonLoggingEnabled(m_serverJsonLogging);
        m_server->setAllowedExtensions(m_serverAllowedExts);
        m_server->setBlockedExtensions(m_serverBlockedExts);
        m_server->setWhitelist(m_serverIpWhitelist);
        m_server->setBlacklist(m_serverIpBlacklist);
        m_server->setReadOnlyDirectories(m_serverReadOnlyDirs);
        m_server->setVirtualMappings(m_serverVirtualMappings);
        m_server->setPskKey(m_serverPskKey);
        m_server->setGlobalLimit(qint64(m_serverGlobalLimit) * 1024);
        m_server->setSessionLimit(qint64(m_serverSessionLimit) * 1024);
    }
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
    const quint64 id = m_nextId++;
    const QString peer = QStringLiteral("%1:%2").arg(host).arg(port);
    m_model->addTransfer(id, remoteName, isUpload, peer);

    m_clientQueue.append({id, isUpload, host, port, localFile, remoteName, m_pskKeyEdit ? m_pskKeyEdit->text() : QString()});
    m_model->setTransferState(m_model->rowForId(id), TransferState::Queued);

    appendLog(QStringLiteral("Queued %1").arg(remoteName));

    processQueue();
}

void MainWindow::processQueue() {
    while (!m_clientQueue.isEmpty() && activeTransfers() < m_maxConcurrent) {
        QueuedTransfer t = m_clientQueue.takeFirst();

        int row = m_model->rowForId(t.id);
        if (row < 0) {
            continue;
        }
        if (!m_model->isActive(row)) {
            continue;
        }

        m_model->setTransferState(row, TransferState::Pending);

        auto *client = new TftpClient(this);
        client->setBlockSize(m_blockSizeSpin->value());
        client->setTimeout(m_timeoutSpin->value());
        client->setWindowSize(m_windowSizeSpin->value());
        client->setPskKey(t.pskKey);

        m_idOf.insert(client, t.id);
        m_clientById.insert(t.id, client);

        connect(client, &TftpClient::progress, this,
                [this, id = t.id](qint64 done, qint64 total) { m_model->updateProgress(m_model->rowForId(id), done, total); });
        connect(client, &TftpClient::errorOccurred, this, [this, client](const QString &message) { m_lastError.insert(client, message); });
        connect(client, &TftpClient::transferFinished, this, [this, client, id = t.id, remoteName = t.remoteName](bool ok) {
            const QString err = m_lastError.value(client);
            m_model->setFinished(m_model->rowForId(id), ok, err);
            appendLog(QStringLiteral("%1 %2%3").arg(remoteName, ok ? QStringLiteral("completed") : QStringLiteral("failed"),
                                                    (!ok && !err.isEmpty()) ? QStringLiteral(": %1").arg(err) : QString()));
            m_idOf.remove(client);
            m_clientById.remove(id);
            m_lastError.remove(client);
            client->deleteLater();

            processQueue();
        });

        const QString peer = QStringLiteral("%1:%2").arg(t.host).arg(t.port);
        appendLog(
            QStringLiteral("%1 %2 : %3").arg(t.isUpload ? QStringLiteral("Uploading") : QStringLiteral("Downloading"), t.remoteName, peer));
        showStatus(tr("%1 %2…").arg(t.isUpload ? tr("Uploading") : tr("Downloading"), t.remoteName));

        if (t.isUpload)
            client->uploadFile(t.host, t.port, t.localFile, t.remoteName);
        else
            client->downloadFile(t.host, t.port, t.remoteName, t.localFile);
    }
}

void MainWindow::cancelTransfer(int row) {
    const quint64 id = m_model->idForRow(row);
    if (id == 0)
        return;

    TftpClient *client = m_clientById.value(id, nullptr);
    if (client) {
        m_model->setCancelled(row);
        client->abort();
        showStatus(tr("Transfer cancelled."));
        return;
    }

    for (int i = 0; i < m_clientQueue.size(); ++i) {
        if (m_clientQueue.at(i).id == id) {
            m_clientQueue.removeAt(i);
            m_model->setCancelled(row);
            showStatus(tr("Queued transfer cancelled."));
            processQueue();
            return;
        }
    }
}

void MainWindow::clearCompleted() {
    m_model->removeFinished();
    showStatus(tr("Cleared finished transfers."));
}

int MainWindow::activeTransfers() const {
    return m_idOf.size();
}

void MainWindow::appendLog(const QString &message) {
    const QString formatted = QStringLiteral("[%1] %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), message);
    m_allLogLines.append(formatted);
    if (m_allLogLines.size() > 1000) {
        m_allLogLines.removeFirst();
    }

    bool match = true;
    if (m_logSearchEdit && !m_logSearchEdit->text().trimmed().isEmpty()) {
        const QString searchStr = m_logSearchEdit->text().trimmed();
        if (!formatted.contains(searchStr, Qt::CaseInsensitive)) {
            match = false;
        }
    }
    if (match && m_logFilterCombo) {
        const QString severity = m_logFilterCombo->currentData().toString();
        if (severity != QLatin1String("all")) {
            const bool isInfoFiltered =
                (severity == QLatin1String("info") && (formatted.contains(QLatin1String("error"), Qt::CaseInsensitive) ||
                                                       formatted.contains(QLatin1String("warn"), Qt::CaseInsensitive) ||
                                                       formatted.contains(QLatin1String("fail"), Qt::CaseInsensitive)));
            const bool isWarnFiltered =
                (severity == QLatin1String("warn") && !formatted.contains(QLatin1String("warn"), Qt::CaseInsensitive));
            const bool isErrorFiltered =
                (severity == QLatin1String("error") && !(formatted.contains(QLatin1String("error"), Qt::CaseInsensitive) ||
                                                         formatted.contains(QLatin1String("fail"), Qt::CaseInsensitive)));
            if (isInfoFiltered || isWarnFiltered || isErrorFiltered) {
                match = false;
            }
        }
    }

    if (match && m_log) {
        m_log->appendPlainText(formatted);
    }
}

void MainWindow::filterLog() {
    if (!m_log)
        return;

    m_log->clear();

    const QString searchStr = m_logSearchEdit ? m_logSearchEdit->text().trimmed() : QString();
    const QString severity = m_logFilterCombo ? m_logFilterCombo->currentData().toString() : QStringLiteral("all");

    for (const QString &line : m_allLogLines) {
        bool match = true;
        if (!searchStr.isEmpty()) {
            if (!line.contains(searchStr, Qt::CaseInsensitive)) {
                match = false;
            }
        }
        if (match && severity != QLatin1String("all")) {
            const bool isInfoFiltered = (severity == QLatin1String("info") && (line.contains(QLatin1String("error"), Qt::CaseInsensitive) ||
                                                                               line.contains(QLatin1String("warn"), Qt::CaseInsensitive) ||
                                                                               line.contains(QLatin1String("fail"), Qt::CaseInsensitive)));
            const bool isWarnFiltered = (severity == QLatin1String("warn") && !line.contains(QLatin1String("warn"), Qt::CaseInsensitive));
            const bool isErrorFiltered =
                (severity == QLatin1String("error") && !(line.contains(QLatin1String("error"), Qt::CaseInsensitive) ||
                                                         line.contains(QLatin1String("fail"), Qt::CaseInsensitive)));
            if (isInfoFiltered || isWarnFiltered || isErrorFiltered) {
                match = false;
            }
        }
        if (match) {
            m_log->appendPlainText(line);
        }
    }
}

void MainWindow::showStatus(const QString &message) {
    statusBar()->showMessage(message, 6000);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event) {
    // Dropping a file is a client upload; surface those fields.
    if (m_configModeGroup && m_configModeGroup->button(0))
        m_configModeGroup->button(0)->setChecked(true);
    if (m_configStack)
        m_configStack->setCurrentIndex(0);

    const QString host = m_hostEdit->text().trimmed();
    if (host.isEmpty()) {
        showStatus(tr("Enter a host before dropping files to upload."));
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

    if (m_speedChart) {
        m_speedChart->addSpeedSample(speed);
    }
}

void MainWindow::loadSettings() {
    QSettings settings;

    m_hostEdit->setText(settings.value(QStringLiteral("client/host")).toString());
    m_clientPortSpin->setValue(settings.value(QStringLiteral("client/port"), kDefaultPort).toInt());
    m_blockSizeSpin->setValue(settings.value(QStringLiteral("client/blockSize"), kDefaultBlockSize).toInt());
    m_timeoutSpin->setValue(settings.value(QStringLiteral("client/timeoutMs"), 5000).toInt());
    m_windowSizeSpin->setValue(settings.value(QStringLiteral("client/windowSize"), 1).toInt());

    m_serverPort = quint16(settings.value(QStringLiteral("server/port"), m_serverPort).toUInt());
    m_serverDir = settings.value(QStringLiteral("server/dir"), m_serverDir).toString();
    m_maxConcurrent = settings.value(QStringLiteral("server/maxConcurrent"), m_maxConcurrent).toInt();
    m_serverSinglePort = settings.value(QStringLiteral("server/singlePort"), false).toBool();
    m_serverJsonLogging = settings.value(QStringLiteral("server/jsonLogging"), false).toBool();
    m_serverAllowedExts = settings.value(QStringLiteral("server/allowedExts")).toStringList();
    m_serverBlockedExts = settings.value(QStringLiteral("server/blockedExts")).toStringList();
    m_serverIpWhitelist = settings.value(QStringLiteral("server/ipWhitelist")).toStringList();
    m_serverIpBlacklist = settings.value(QStringLiteral("server/ipBlacklist")).toStringList();
    m_serverReadOnlyDirs = settings.value(QStringLiteral("server/readOnlyDirs")).toStringList();
    QVariantMap virtualMappingsMap = settings.value(QStringLiteral("server/virtualMappings")).toMap();
    m_serverVirtualMappings.clear();
    for (auto it = virtualMappingsMap.begin(); it != virtualMappingsMap.end(); ++it) {
        m_serverVirtualMappings.insert(it.key(), it.value().toString());
    }
    m_serverGlobalLimit = settings.value(QStringLiteral("server/globalLimit"), 0).toInt();
    m_serverSessionLimit = settings.value(QStringLiteral("server/sessionLimit"), 0).toInt();
    m_serverAutoStart = settings.value(QStringLiteral("server/autoStart"), false).toBool();
    m_pskKey = settings.value(QStringLiteral("client/pskKey")).toString();
    m_serverPskKey = settings.value(QStringLiteral("server/pskKey")).toString();

    {
        QSignalBlocker b1(m_serverPortSpin);
        QSignalBlocker b2(m_serverDirEdit);
        QSignalBlocker b3(m_serverMaxSpin);
        QSignalBlocker b4(m_serverSinglePortCheck);
        QSignalBlocker b5(m_serverJsonLoggingCheck);
        QSignalBlocker b14(m_serverAutoStartCheck);
        QSignalBlocker b6(m_serverAllowedExtsEdit);
        QSignalBlocker b7(m_serverBlockedExtsEdit);
        QSignalBlocker b11(m_serverIpWhitelistEdit);
        QSignalBlocker b12(m_serverIpBlacklistEdit);
        QSignalBlocker b8(m_serverReadOnlyDirsEdit);
        QSignalBlocker b13(m_serverVirtualMappingsEdit);
        QSignalBlocker b9(m_serverGlobalLimitSpin);
        QSignalBlocker b10(m_serverSessionLimitSpin);
        QSignalBlocker b15(m_serverPskKeyEdit);
        QSignalBlocker b16(m_pskKeyEdit);

        m_serverPortSpin->setValue(m_serverPort == 0 ? 6969 : m_serverPort);
        m_serverDirEdit->setText(m_serverDir);
        m_serverMaxSpin->setValue(m_maxConcurrent);
        m_serverSinglePortCheck->setChecked(m_serverSinglePort);
        m_serverJsonLoggingCheck->setChecked(m_serverJsonLogging);
        m_serverAutoStartCheck->setChecked(m_serverAutoStart);
        m_serverAllowedExtsEdit->setText(m_serverAllowedExts.join(QLatin1Char(',')));
        m_serverBlockedExtsEdit->setText(m_serverBlockedExts.join(QLatin1Char(',')));
        m_serverIpWhitelistEdit->setText(m_serverIpWhitelist.join(QLatin1Char(',')));
        m_serverIpBlacklistEdit->setText(m_serverIpBlacklist.join(QLatin1Char(',')));
        m_serverReadOnlyDirsEdit->setText(m_serverReadOnlyDirs.join(QLatin1Char(',')));

        QStringList mappingStrings;
        for (auto it = m_serverVirtualMappings.begin(); it != m_serverVirtualMappings.end(); ++it) {
            mappingStrings.append(QStringLiteral("%1=%2").arg(it.key(), it.value()));
        }
        m_serverVirtualMappingsEdit->setText(mappingStrings.join(QLatin1Char(',')));

        m_serverGlobalLimitSpin->setValue(m_serverGlobalLimit);
        m_serverSessionLimitSpin->setValue(m_serverSessionLimit);
        if (m_serverPskKeyEdit)
            m_serverPskKeyEdit->setText(m_serverPskKey);
        if (m_pskKeyEdit)
            m_pskKeyEdit->setText(m_pskKey);
    }

    m_theme->setMode(ThemeController::modeFromString(settings.value(QStringLiteral("ui/theme")).toString()));

    const int configMode = settings.value(QStringLiteral("ui/configMode"), 0).toInt() == 1 ? 1 : 0;
    if (m_configModeGroup && m_configModeGroup->button(configMode))
        m_configModeGroup->button(configMode)->setChecked(true);
    if (m_configStack)
        m_configStack->setCurrentIndex(configMode);

    if (settings.contains(QStringLiteral("ui/geometry")))
        restoreGeometry(settings.value(QStringLiteral("ui/geometry")).toByteArray());
    if (settings.contains(QStringLiteral("ui/windowState")))
        restoreState(settings.value(QStringLiteral("ui/windowState")).toByteArray());

    loadProfileList();
    loadServerProfileList();

    if (m_serverAutoStart) {
        QTimer::singleShot(0, this, &MainWindow::toggleServer);
    }
}

void MainWindow::saveSettings() {
    QSettings settings;

    settings.setValue(QStringLiteral("client/host"), m_hostEdit->text());
    settings.setValue(QStringLiteral("client/port"), m_clientPortSpin->value());
    settings.setValue(QStringLiteral("client/blockSize"), m_blockSizeSpin->value());
    settings.setValue(QStringLiteral("client/timeoutMs"), m_timeoutSpin->value());
    settings.setValue(QStringLiteral("client/windowSize"), m_windowSizeSpin->value());
    settings.setValue(QStringLiteral("client/pskKey"), m_pskKeyEdit ? m_pskKeyEdit->text() : QString());

    settings.setValue(QStringLiteral("server/port"), m_serverPort);
    settings.setValue(QStringLiteral("server/dir"), m_serverDir);
    settings.setValue(QStringLiteral("server/maxConcurrent"), m_maxConcurrent);
    settings.setValue(QStringLiteral("server/singlePort"), m_serverSinglePort);
    settings.setValue(QStringLiteral("server/jsonLogging"), m_serverJsonLogging);
    settings.setValue(QStringLiteral("server/autoStart"), m_serverAutoStart);
    settings.setValue(QStringLiteral("server/allowedExts"), m_serverAllowedExts);
    settings.setValue(QStringLiteral("server/blockedExts"), m_serverBlockedExts);
    settings.setValue(QStringLiteral("server/ipWhitelist"), m_serverIpWhitelist);
    settings.setValue(QStringLiteral("server/ipBlacklist"), m_serverIpBlacklist);
    settings.setValue(QStringLiteral("server/readOnlyDirs"), m_serverReadOnlyDirs);
    settings.setValue(QStringLiteral("server/pskKey"), m_serverPskKeyEdit ? m_serverPskKeyEdit->text() : QString());

    QVariantMap virtualMappingsMap;
    for (auto it = m_serverVirtualMappings.begin(); it != m_serverVirtualMappings.end(); ++it) {
        virtualMappingsMap.insert(it.key(), it.value());
    }
    settings.setValue(QStringLiteral("server/virtualMappings"), virtualMappingsMap);

    settings.setValue(QStringLiteral("server/globalLimit"), m_serverGlobalLimit);
    settings.setValue(QStringLiteral("server/sessionLimit"), m_serverSessionLimit);

    settings.setValue(QStringLiteral("ui/theme"), ThemeController::modeToString(m_theme->mode()));
    settings.setValue(QStringLiteral("ui/configMode"), m_configStack ? m_configStack->currentIndex() : 0);
    settings.setValue(QStringLiteral("ui/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("ui/windowState"), saveState());
}

void MainWindow::closeEvent(QCloseEvent *event) {
    saveSettings();
    if (m_trayIcon && m_trayIcon->isVisible()) {
        hide();
        event->ignore();
        static bool firstNotification = true;
        if (firstNotification) {
            m_trayIcon->showMessage(tr("AetherTFTP"),
                                    tr("AetherTFTP is running in the system tray. Use the tray icon to restore or quit the application."),
                                    QSystemTrayIcon::Information, 3000);
            firstNotification = false;
        }
    } else {
        QMainWindow::closeEvent(event);
    }
}

void MainWindow::setupTrayIcon() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }

    m_trayIcon = new QSystemTrayIcon(QIcon(QStringLiteral(":/aether/icon.ico")), this);
    m_trayIcon->setToolTip(QStringLiteral("AetherTFTP"));

    auto *trayMenu = new QMenu(this);

    auto *restoreAct = new QAction(tr("Show Window"), this);
    connect(restoreAct, &QAction::triggered, this, [this]() {
        showNormal();
        activateWindow();
    });
    trayMenu->addAction(restoreAct);

    m_trayToggleServerAction = new QAction(m_serverRunning ? tr("Stop Server") : tr("Start Server"), this);
    connect(m_trayToggleServerAction, &QAction::triggered, this, &MainWindow::toggleServer);
    trayMenu->addAction(m_trayToggleServerAction);

    trayMenu->addSeparator();

    auto *quitAct = new QAction(tr("Quit"), this);
    connect(quitAct, &QAction::triggered, qApp, &QCoreApplication::quit);
    trayMenu->addAction(quitAct);

    m_trayIcon->setContextMenu(trayMenu);

    connect(m_trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
            if (isVisible()) {
                hide();
            } else {
                showNormal();
                activateWindow();
            }
        }
    });

    m_trayIcon->show();
}

void MainWindow::loadProfileList() {
    if (!m_profileCombo)
        return;
    QSignalBlocker blocker(m_profileCombo);
    m_profileCombo->clear();
    m_profileCombo->addItem(tr("<Default Settings>"));

    QSettings settings;
    settings.beginGroup(QStringLiteral("profiles"));
    const QStringList keys = settings.childGroups();
    for (const QString &profileName : keys) {
        m_profileCombo->addItem(profileName);
    }
    settings.endGroup();
}

void MainWindow::onProfileChanged(int index) {
    if (index <= 0) {
        QSettings settings;
        m_hostEdit->setText(settings.value(QStringLiteral("client/host")).toString());
        m_clientPortSpin->setValue(settings.value(QStringLiteral("client/port"), kDefaultPort).toInt());
        m_blockSizeSpin->setValue(settings.value(QStringLiteral("client/blockSize"), kDefaultBlockSize).toInt());
        m_timeoutSpin->setValue(settings.value(QStringLiteral("client/timeoutMs"), 5000).toInt());
        m_windowSizeSpin->setValue(settings.value(QStringLiteral("client/windowSize"), 1).toInt());
        m_pskKeyEdit->setText(settings.value(QStringLiteral("client/pskKey")).toString());
        return;
    }

    const QString profileName = m_profileCombo->itemText(index);
    QSettings settings;
    settings.beginGroup(QStringLiteral("profiles"));
    settings.beginGroup(profileName);
    m_hostEdit->setText(settings.value(QStringLiteral("host")).toString());
    m_clientPortSpin->setValue(settings.value(QStringLiteral("port"), kDefaultPort).toInt());
    m_blockSizeSpin->setValue(settings.value(QStringLiteral("blockSize"), kDefaultBlockSize).toInt());
    m_timeoutSpin->setValue(settings.value(QStringLiteral("timeoutMs"), 5000).toInt());
    m_windowSizeSpin->setValue(settings.value(QStringLiteral("windowSize"), 1).toInt());
    m_pskKeyEdit->setText(settings.value(QStringLiteral("pskKey")).toString());
    settings.endGroup();
    settings.endGroup();
}

void MainWindow::saveCurrentProfile() {
    bool ok = false;
    const QString profileName =
        QInputDialog::getText(this, tr("Save Profile"), tr("Enter profile name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || profileName.trimmed().isEmpty())
        return;

    QSettings settings;
    settings.beginGroup(QStringLiteral("profiles"));
    settings.beginGroup(profileName.trimmed());
    settings.setValue(QStringLiteral("host"), m_hostEdit->text());
    settings.setValue(QStringLiteral("port"), m_clientPortSpin->value());
    settings.setValue(QStringLiteral("blockSize"), m_blockSizeSpin->value());
    settings.setValue(QStringLiteral("timeoutMs"), m_timeoutSpin->value());
    settings.setValue(QStringLiteral("windowSize"), m_windowSizeSpin->value());
    settings.setValue(QStringLiteral("pskKey"), m_pskKeyEdit->text());
    settings.endGroup();
    settings.endGroup();

    loadProfileList();

    int idx = m_profileCombo->findText(profileName.trimmed());
    if (idx >= 0) {
        m_profileCombo->setCurrentIndex(idx);
    }
}

void MainWindow::deleteCurrentProfile() {
    int index = m_profileCombo->currentIndex();
    if (index <= 0) {
        QMessageBox::warning(this, tr("Delete Profile"), tr("Cannot delete the default settings profile."));
        return;
    }

    const QString profileName = m_profileCombo->itemText(index);
    if (QMessageBox::question(this, tr("Delete Profile"), tr("Are you sure you want to delete profile '%1'?").arg(profileName)) !=
        QMessageBox::Yes)
        return;

    QSettings settings;
    settings.beginGroup(QStringLiteral("profiles"));
    settings.remove(profileName);
    settings.endGroup();

    loadProfileList();
    m_profileCombo->setCurrentIndex(0);
}

void MainWindow::loadServerProfileList() {
    if (!m_serverProfileCombo)
        return;
    QSignalBlocker blocker(m_serverProfileCombo);
    m_serverProfileCombo->clear();
    m_serverProfileCombo->addItem(tr("<Default Settings>"));

    QSettings settings;
    settings.beginGroup(QStringLiteral("serverProfiles"));
    const QStringList keys = settings.childGroups();
    for (const QString &profileName : keys) {
        m_serverProfileCombo->addItem(profileName);
    }
    settings.endGroup();
}

void MainWindow::onServerProfileChanged(int index) {
    if (index <= 0) {
        QSettings settings;
        m_serverPortSpin->setValue(settings.value(QStringLiteral("server/port"), 6969).toInt());
        m_serverDirEdit->setText(settings.value(QStringLiteral("server/dir"), QDir::currentPath()).toString());
        m_serverMaxSpin->setValue(settings.value(QStringLiteral("server/maxConcurrent"), 4).toInt());
        m_serverSinglePortCheck->setChecked(settings.value(QStringLiteral("server/singlePort"), false).toBool());
        m_serverJsonLoggingCheck->setChecked(settings.value(QStringLiteral("server/jsonLogging"), false).toBool());
        m_serverAutoStartCheck->setChecked(settings.value(QStringLiteral("server/autoStart"), false).toBool());
        m_serverAllowedExtsEdit->setText(settings.value(QStringLiteral("server/allowedExts")).toStringList().join(QLatin1Char(',')));
        m_serverBlockedExtsEdit->setText(settings.value(QStringLiteral("server/blockedExts")).toStringList().join(QLatin1Char(',')));
        m_serverIpWhitelistEdit->setText(settings.value(QStringLiteral("server/ipWhitelist")).toStringList().join(QLatin1Char(',')));
        m_serverIpBlacklistEdit->setText(settings.value(QStringLiteral("server/ipBlacklist")).toStringList().join(QLatin1Char(',')));
        m_serverReadOnlyDirsEdit->setText(settings.value(QStringLiteral("server/readOnlyDirs")).toStringList().join(QLatin1Char(',')));

        QVariantMap defaultVMap = settings.value(QStringLiteral("server/virtualMappings")).toMap();
        QStringList defaultVStrings;
        for (auto it = defaultVMap.begin(); it != defaultVMap.end(); ++it) {
            defaultVStrings.append(QStringLiteral("%1=%2").arg(it.key(), it.value().toString()));
        }
        m_serverVirtualMappingsEdit->setText(defaultVStrings.join(QLatin1Char(',')));

        m_serverGlobalLimitSpin->setValue(settings.value(QStringLiteral("server/globalLimit"), 0).toInt());
        m_serverSessionLimitSpin->setValue(settings.value(QStringLiteral("server/sessionLimit"), 0).toInt());
        m_serverPskKeyEdit->setText(settings.value(QStringLiteral("server/pskKey")).toString());
        return;
    }

    const QString profileName = m_serverProfileCombo->itemText(index);
    QSettings settings;
    settings.beginGroup(QStringLiteral("serverProfiles"));
    settings.beginGroup(profileName);
    m_serverPortSpin->setValue(settings.value(QStringLiteral("port"), 6969).toInt());
    m_serverDirEdit->setText(settings.value(QStringLiteral("dir"), QDir::currentPath()).toString());
    m_serverMaxSpin->setValue(settings.value(QStringLiteral("maxConcurrent"), 4).toInt());
    m_serverSinglePortCheck->setChecked(settings.value(QStringLiteral("singlePort"), false).toBool());
    m_serverJsonLoggingCheck->setChecked(settings.value(QStringLiteral("jsonLogging"), false).toBool());
    m_serverAutoStartCheck->setChecked(settings.value(QStringLiteral("autoStart"), false).toBool());
    m_serverAllowedExtsEdit->setText(settings.value(QStringLiteral("allowedExts")).toStringList().join(QLatin1Char(',')));
    m_serverBlockedExtsEdit->setText(settings.value(QStringLiteral("blockedExts")).toStringList().join(QLatin1Char(',')));
    m_serverIpWhitelistEdit->setText(settings.value(QStringLiteral("ipWhitelist")).toStringList().join(QLatin1Char(',')));
    m_serverIpBlacklistEdit->setText(settings.value(QStringLiteral("ipBlacklist")).toStringList().join(QLatin1Char(',')));
    m_serverReadOnlyDirsEdit->setText(settings.value(QStringLiteral("readOnlyDirs")).toStringList().join(QLatin1Char(',')));

    QVariantMap vMap = settings.value(QStringLiteral("virtualMappings")).toMap();
    QStringList vStrings;
    for (auto it = vMap.begin(); it != vMap.end(); ++it) {
        vStrings.append(QStringLiteral("%1=%2").arg(it.key(), it.value().toString()));
    }
    m_serverVirtualMappingsEdit->setText(vStrings.join(QLatin1Char(',')));

    m_serverGlobalLimitSpin->setValue(settings.value(QStringLiteral("globalLimit"), 0).toInt());
    m_serverSessionLimitSpin->setValue(settings.value(QStringLiteral("sessionLimit"), 0).toInt());
    m_serverPskKeyEdit->setText(settings.value(QStringLiteral("pskKey")).toString());
    settings.endGroup();
    settings.endGroup();
}

void MainWindow::saveCurrentServerProfile() {
    bool ok = false;
    const QString profileName =
        QInputDialog::getText(this, tr("Save Server Profile"), tr("Enter profile name:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || profileName.trimmed().isEmpty())
        return;

    QSettings settings;
    settings.beginGroup(QStringLiteral("serverProfiles"));
    settings.beginGroup(profileName.trimmed());
    settings.setValue(QStringLiteral("port"), m_serverPortSpin->value());
    settings.setValue(QStringLiteral("dir"), m_serverDirEdit->text());
    settings.setValue(QStringLiteral("maxConcurrent"), m_serverMaxSpin->value());
    settings.setValue(QStringLiteral("singlePort"), m_serverSinglePortCheck->isChecked());
    settings.setValue(QStringLiteral("jsonLogging"), m_serverJsonLoggingCheck->isChecked());
    settings.setValue(QStringLiteral("autoStart"), m_serverAutoStartCheck->isChecked());
    settings.setValue(QStringLiteral("allowedExts"), m_serverAllowedExtsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts));
    settings.setValue(QStringLiteral("blockedExts"), m_serverBlockedExtsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts));
    settings.setValue(QStringLiteral("ipWhitelist"), m_serverIpWhitelistEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts));
    settings.setValue(QStringLiteral("ipBlacklist"), m_serverIpBlacklistEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts));
    settings.setValue(QStringLiteral("readOnlyDirs"), m_serverReadOnlyDirsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts));

    QVariantMap virtualMappingsMap;
    for (const QString &item : m_serverVirtualMappingsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        int eq = item.indexOf(QLatin1Char('='));
        if (eq > 0) {
            virtualMappingsMap.insert(item.left(eq).trimmed(), item.mid(eq + 1).trimmed());
        }
    }
    settings.setValue(QStringLiteral("virtualMappings"), virtualMappingsMap);

    settings.setValue(QStringLiteral("globalLimit"), m_serverGlobalLimitSpin->value());
    settings.setValue(QStringLiteral("sessionLimit"), m_serverSessionLimitSpin->value());
    settings.setValue(QStringLiteral("pskKey"), m_serverPskKeyEdit ? m_serverPskKeyEdit->text() : QString());
    settings.endGroup();
    settings.endGroup();

    loadServerProfileList();

    int idx = m_serverProfileCombo->findText(profileName.trimmed());
    if (idx >= 0) {
        m_serverProfileCombo->setCurrentIndex(idx);
    }
}

void MainWindow::deleteCurrentServerProfile() {
    int index = m_serverProfileCombo->currentIndex();
    if (index <= 0) {
        QMessageBox::warning(this, tr("Delete Server Profile"), tr("Cannot delete the default settings profile."));
        return;
    }

    const QString profileName = m_serverProfileCombo->itemText(index);
    if (QMessageBox::question(this, tr("Delete Server Profile"), tr("Are you sure you want to delete profile '%1'?").arg(profileName)) !=
        QMessageBox::Yes)
        return;

    QSettings settings;
    settings.beginGroup(QStringLiteral("serverProfiles"));
    settings.remove(profileName);
    settings.endGroup();

    loadServerProfileList();
    m_serverProfileCombo->setCurrentIndex(0);
}

void MainWindow::importClientProfile() {
    QString path = QFileDialog::getOpenFileName(this, tr("Import Client Profile"), QString(), tr("JSON Files (*.json)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, tr("Import Profile"), tr("Failed to open file for reading."));
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isNull() || !doc.isObject()) {
        QMessageBox::critical(this, tr("Import Profile"), tr("Invalid profile JSON format."));
        return;
    }

    QJsonObject obj = doc.object();
    QString profileName = obj.value(QStringLiteral("profileName")).toString();
    if (profileName.trimmed().isEmpty()) {
        QMessageBox::critical(this, tr("Import Profile"), tr("Missing profileName field."));
        return;
    }

    QJsonObject client = obj.value(QStringLiteral("client")).toObject();
    QSettings settings;
    settings.beginGroup(QStringLiteral("profiles"));
    settings.beginGroup(profileName.trimmed());
    settings.setValue(QStringLiteral("host"), client.value(QStringLiteral("host")).toString());
    settings.setValue(QStringLiteral("port"), client.value(QStringLiteral("port")).toInt(kDefaultPort));
    settings.setValue(QStringLiteral("blockSize"), client.value(QStringLiteral("blockSize")).toInt(kDefaultBlockSize));
    settings.setValue(QStringLiteral("timeoutMs"), client.value(QStringLiteral("timeoutMs")).toInt(5000));
    settings.setValue(QStringLiteral("windowSize"), client.value(QStringLiteral("windowSize")).toInt(1));
    settings.setValue(QStringLiteral("pskKey"), client.value(QStringLiteral("pskKey")).toString());
    settings.endGroup();
    settings.endGroup();

    loadProfileList();
    int idx = m_profileCombo->findText(profileName.trimmed());
    if (idx >= 0) {
        m_profileCombo->setCurrentIndex(idx);
    }
    QMessageBox::information(this, tr("Import Profile"), tr("Profile '%1' imported successfully.").arg(profileName));
}

void MainWindow::exportClientProfile() {
    int index = m_profileCombo->currentIndex();
    if (index <= 0) {
        QMessageBox::warning(this, tr("Export Profile"), tr("Please select a profile to export (Default settings cannot be exported)."));
        return;
    }

    const QString profileName = m_profileCombo->itemText(index);
    QString path =
        QFileDialog::getSaveFileName(this, tr("Export Client Profile"), profileName + QStringLiteral(".json"), tr("JSON Files (*.json)"));
    if (path.isEmpty())
        return;

    QSettings settings;
    settings.beginGroup(QStringLiteral("profiles"));
    settings.beginGroup(profileName);
    QJsonObject client;
    client.insert(QStringLiteral("host"), settings.value(QStringLiteral("host")).toString());
    client.insert(QStringLiteral("port"), settings.value(QStringLiteral("port"), kDefaultPort).toInt());
    client.insert(QStringLiteral("blockSize"), settings.value(QStringLiteral("blockSize"), kDefaultBlockSize).toInt());
    client.insert(QStringLiteral("timeoutMs"), settings.value(QStringLiteral("timeoutMs"), 5000).toInt());
    client.insert(QStringLiteral("windowSize"), settings.value(QStringLiteral("windowSize"), 1).toInt());
    client.insert(QStringLiteral("pskKey"), settings.value(QStringLiteral("pskKey")).toString());
    settings.endGroup();
    settings.endGroup();

    QJsonObject mainObj;
    mainObj.insert(QStringLiteral("profileName"), profileName);
    mainObj.insert(QStringLiteral("client"), client);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::critical(this, tr("Export Profile"), tr("Failed to open file for writing."));
        return;
    }

    QJsonDocument doc(mainObj);
    file.write(doc.toJson(QJsonDocument::Indented));
    QMessageBox::information(this, tr("Export Profile"), tr("Profile '%1' exported successfully.").arg(profileName));
}

void MainWindow::importServerProfile() {
    QString path = QFileDialog::getOpenFileName(this, tr("Import Server Profile"), QString(), tr("JSON Files (*.json)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, tr("Import Profile"), tr("Failed to open file for reading."));
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isNull() || !doc.isObject()) {
        QMessageBox::critical(this, tr("Import Profile"), tr("Invalid profile JSON format."));
        return;
    }

    QJsonObject obj = doc.object();
    QString profileName = obj.value(QStringLiteral("profileName")).toString();
    if (profileName.trimmed().isEmpty()) {
        QMessageBox::critical(this, tr("Import Profile"), tr("Missing profileName field."));
        return;
    }

    QJsonObject server = obj.value(QStringLiteral("server")).toObject();
    QSettings settings;
    settings.beginGroup(QStringLiteral("serverProfiles"));
    settings.beginGroup(profileName.trimmed());
    settings.setValue(QStringLiteral("port"), server.value(QStringLiteral("port")).toInt(69));
    settings.setValue(QStringLiteral("dir"), server.value(QStringLiteral("dir")).toString());
    settings.setValue(QStringLiteral("maxConcurrent"), server.value(QStringLiteral("maxConcurrent")).toInt(10));
    settings.setValue(QStringLiteral("singlePort"), server.value(QStringLiteral("singlePort")).toBool(false));
    settings.setValue(QStringLiteral("jsonLogging"), server.value(QStringLiteral("jsonLogging")).toBool(false));

    QJsonArray allowedArray = server.value(QStringLiteral("allowedExts")).toArray();
    QStringList allowedList;
    for (const auto &val : allowedArray)
        allowedList.append(val.toString());
    settings.setValue(QStringLiteral("allowedExts"), allowedList);

    QJsonArray blockedArray = server.value(QStringLiteral("blockedExts")).toArray();
    QStringList blockedList;
    for (const auto &val : blockedArray)
        blockedList.append(val.toString());
    settings.setValue(QStringLiteral("blockedExts"), blockedList);

    QJsonArray whitelistArray = server.value(QStringLiteral("ipWhitelist")).toArray();
    QStringList whitelistList;
    for (const auto &val : whitelistArray)
        whitelistList.append(val.toString());
    settings.setValue(QStringLiteral("ipWhitelist"), whitelistList);

    QJsonArray blacklistArray = server.value(QStringLiteral("ipBlacklist")).toArray();
    QStringList blacklistList;
    for (const auto &val : blacklistArray)
        blacklistList.append(val.toString());
    settings.setValue(QStringLiteral("ipBlacklist"), blacklistList);

    QJsonArray roDirsArray = server.value(QStringLiteral("readOnlyDirs")).toArray();
    QStringList roDirsList;
    for (const auto &val : roDirsArray)
        roDirsList.append(val.toString());
    settings.setValue(QStringLiteral("readOnlyDirs"), roDirsList);

    QJsonObject mappingsObj = server.value(QStringLiteral("virtualMappings")).toObject();
    QVariantMap mappingsMap;
    for (auto it = mappingsObj.begin(); it != mappingsObj.end(); ++it) {
        mappingsMap.insert(it.key(), it.value().toString());
    }
    settings.setValue(QStringLiteral("virtualMappings"), mappingsMap);

    settings.setValue(QStringLiteral("globalLimit"), server.value(QStringLiteral("globalLimit")).toInt(0));
    settings.setValue(QStringLiteral("sessionLimit"), server.value(QStringLiteral("sessionLimit")).toInt(0));
    settings.setValue(QStringLiteral("autoStart"), server.value(QStringLiteral("autoStart")).toBool(false));
    settings.setValue(QStringLiteral("pskKey"), server.value(QStringLiteral("pskKey")).toString());
    settings.endGroup();
    settings.endGroup();

    loadServerProfileList();
    int idx = m_serverProfileCombo->findText(profileName.trimmed());
    if (idx >= 0) {
        m_serverProfileCombo->setCurrentIndex(idx);
    }
    QMessageBox::information(this, tr("Import Profile"), tr("Server profile '%1' imported successfully.").arg(profileName));
}

void MainWindow::exportServerProfile() {
    int index = m_serverProfileCombo->currentIndex();
    if (index <= 0) {
        QMessageBox::warning(this, tr("Export Profile"), tr("Please select a profile to export."));
        return;
    }

    const QString profileName = m_serverProfileCombo->itemText(index);
    QString path =
        QFileDialog::getSaveFileName(this, tr("Export Server Profile"), profileName + QStringLiteral(".json"), tr("JSON Files (*.json)"));
    if (path.isEmpty())
        return;

    QSettings settings;
    settings.beginGroup(QStringLiteral("serverProfiles"));
    settings.beginGroup(profileName);

    QJsonObject server;
    server.insert(QStringLiteral("port"), settings.value(QStringLiteral("port"), 69).toInt());
    server.insert(QStringLiteral("dir"), settings.value(QStringLiteral("dir")).toString());
    server.insert(QStringLiteral("maxConcurrent"), settings.value(QStringLiteral("maxConcurrent"), 10).toInt());
    server.insert(QStringLiteral("singlePort"), settings.value(QStringLiteral("singlePort"), false).toBool());
    server.insert(QStringLiteral("jsonLogging"), settings.value(QStringLiteral("jsonLogging"), false).toBool());

    QJsonArray allowedArray;
    for (const auto &val : settings.value(QStringLiteral("allowedExts")).toStringList())
        allowedArray.append(val);
    server.insert(QStringLiteral("allowedExts"), allowedArray);

    QJsonArray blockedArray;
    for (const auto &val : settings.value(QStringLiteral("blockedExts")).toStringList())
        blockedArray.append(val);
    server.insert(QStringLiteral("blockedExts"), blockedArray);

    QJsonArray whitelistArray;
    for (const auto &val : settings.value(QStringLiteral("ipWhitelist")).toStringList())
        whitelistArray.append(val);
    server.insert(QStringLiteral("ipWhitelist"), whitelistArray);

    QJsonArray blacklistArray;
    for (const auto &val : settings.value(QStringLiteral("ipBlacklist")).toStringList())
        blacklistArray.append(val);
    server.insert(QStringLiteral("ipBlacklist"), blacklistArray);

    QJsonArray roDirsArray;
    for (const auto &val : settings.value(QStringLiteral("readOnlyDirs")).toStringList())
        roDirsArray.append(val);
    server.insert(QStringLiteral("readOnlyDirs"), roDirsArray);

    QVariantMap mappingsMap = settings.value(QStringLiteral("virtualMappings")).toMap();
    QJsonObject mappingsObj;
    for (auto it = mappingsMap.begin(); it != mappingsMap.end(); ++it) {
        mappingsObj.insert(it.key(), it.value().toString());
    }
    server.insert(QStringLiteral("virtualMappings"), mappingsObj);

    server.insert(QStringLiteral("globalLimit"), settings.value(QStringLiteral("globalLimit"), 0).toInt());
    server.insert(QStringLiteral("sessionLimit"), settings.value(QStringLiteral("sessionLimit"), 0).toInt());
    server.insert(QStringLiteral("autoStart"), settings.value(QStringLiteral("autoStart"), false).toBool());
    server.insert(QStringLiteral("pskKey"), settings.value(QStringLiteral("pskKey")).toString());
    settings.endGroup();
    settings.endGroup();

    QJsonObject mainObj;
    mainObj.insert(QStringLiteral("profileName"), profileName);
    mainObj.insert(QStringLiteral("server"), server);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::critical(this, tr("Export Profile"), tr("Failed to open file for writing."));
        return;
    }

    QJsonDocument doc(mainObj);
    file.write(doc.toJson(QJsonDocument::Indented));
    QMessageBox::information(this, tr("Export Profile"), tr("Server profile '%1' exported successfully.").arg(profileName));
}

}  // namespace tftp::gui
