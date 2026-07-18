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

    setCentralWidget(buildMainView());

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

    m_serverAllowedExtsEdit = new QLineEdit(leftContainer);
    m_serverAllowedExtsEdit->setPlaceholderText(tr("e.g. txt,bin (empty = all)"));
    m_serverAllowedExtsEdit->setToolTip(
        tr("Comma-separated file extensions that clients are allowed to request (e.g. txt,pdf). Leave empty for all."));
    serverForm->addRow(tr("Whitelist:"), m_serverAllowedExtsEdit);

    m_serverBlockedExtsEdit = new QLineEdit(leftContainer);
    m_serverBlockedExtsEdit->setPlaceholderText(tr("e.g. exe,sh"));
    m_serverBlockedExtsEdit->setToolTip(tr("Comma-separated file extensions that are forbidden to transfer."));
    serverForm->addRow(tr("Blacklist:"), m_serverBlockedExtsEdit);

    m_serverReadOnlyDirsEdit = new QLineEdit(leftContainer);
    m_serverReadOnlyDirsEdit->setPlaceholderText(tr("e.g. public,docs"));
    m_serverReadOnlyDirsEdit->setToolTip(tr("Comma-separated relative folders inside root where file writes/uploads are blocked."));
    serverForm->addRow(tr("Read-Only:"), m_serverReadOnlyDirsEdit);

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

    connect(m_serverPortSpin, &QSpinBox::valueChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverDirEdit, &QLineEdit::textChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverMaxSpin, &QSpinBox::valueChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverSinglePortCheck, &QCheckBox::stateChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverJsonLoggingCheck, &QCheckBox::stateChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverAllowedExtsEdit, &QLineEdit::textChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverBlockedExtsEdit, &QLineEdit::textChanged, this, &MainWindow::applyServerConfig);
    connect(m_serverReadOnlyDirsEdit, &QLineEdit::textChanged, this, &MainWindow::applyServerConfig);
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

    m_log = new QPlainTextEdit(logGroup);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(1000);
    m_log->setPlaceholderText(tr("Server and transfer activity will appear here."));
    m_log->setObjectName(QStringLiteral("logPanel"));
    m_log->setMinimumHeight(100);
    logLayout->addWidget(m_log, 1);

    connect(copyBtn, &QPushButton::clicked, this, [this]() { QApplication::clipboard()->setText(m_log->toPlainText()); });
    connect(clearLogBtn, &QPushButton::clicked, m_log, &QPlainTextEdit::clear);

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
        appendLog(QStringLiteral("Server stopped."));
        showStatus(tr("Server stopped."));
    } else {
        m_server->setSinglePortMode(m_serverSinglePort);
        m_server->setJsonLoggingEnabled(m_serverJsonLogging);
        m_server->setAllowedExtensions(m_serverAllowedExts);
        m_server->setBlockedExtensions(m_serverBlockedExts);
        m_server->setReadOnlyDirectories(m_serverReadOnlyDirs);
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

    m_serverAllowedExts.clear();
    for (const QString &item : m_serverAllowedExtsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        m_serverAllowedExts.append(item.trimmed());
    }

    m_serverBlockedExts.clear();
    for (const QString &item : m_serverBlockedExtsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        m_serverBlockedExts.append(item.trimmed());
    }

    m_serverReadOnlyDirs.clear();
    for (const QString &item : m_serverReadOnlyDirsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        m_serverReadOnlyDirs.append(item.trimmed());
    }

    m_serverGlobalLimit = m_serverGlobalLimitSpin->value();
    m_serverSessionLimit = m_serverSessionLimitSpin->value();

    if (m_server) {
        m_server->setSinglePortMode(m_serverSinglePort);
        m_server->setJsonLoggingEnabled(m_serverJsonLogging);
        m_server->setAllowedExtensions(m_serverAllowedExts);
        m_server->setBlockedExtensions(m_serverBlockedExts);
        m_server->setReadOnlyDirectories(m_serverReadOnlyDirs);
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
    if (activeTransfers() >= m_maxConcurrent) {
        showStatus(tr("Transfer limit reached (%1). Wait for one to finish.").arg(m_maxConcurrent));
        return;
    }

    auto *client = new TftpClient(this);
    client->setBlockSize(m_blockSizeSpin->value());
    client->setTimeout(m_timeoutSpin->value());
    client->setWindowSize(m_windowSizeSpin->value());

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
    m_serverReadOnlyDirs = settings.value(QStringLiteral("server/readOnlyDirs")).toStringList();
    m_serverGlobalLimit = settings.value(QStringLiteral("server/globalLimit"), 0).toInt();
    m_serverSessionLimit = settings.value(QStringLiteral("server/sessionLimit"), 0).toInt();

    {
        QSignalBlocker b1(m_serverPortSpin);
        QSignalBlocker b2(m_serverDirEdit);
        QSignalBlocker b3(m_serverMaxSpin);
        QSignalBlocker b4(m_serverSinglePortCheck);
        QSignalBlocker b5(m_serverJsonLoggingCheck);
        QSignalBlocker b6(m_serverAllowedExtsEdit);
        QSignalBlocker b7(m_serverBlockedExtsEdit);
        QSignalBlocker b8(m_serverReadOnlyDirsEdit);
        QSignalBlocker b9(m_serverGlobalLimitSpin);
        QSignalBlocker b10(m_serverSessionLimitSpin);

        m_serverPortSpin->setValue(m_serverPort == 0 ? 6969 : m_serverPort);
        m_serverDirEdit->setText(m_serverDir);
        m_serverMaxSpin->setValue(m_maxConcurrent);
        m_serverSinglePortCheck->setChecked(m_serverSinglePort);
        m_serverJsonLoggingCheck->setChecked(m_serverJsonLogging);
        m_serverAllowedExtsEdit->setText(m_serverAllowedExts.join(QLatin1Char(',')));
        m_serverBlockedExtsEdit->setText(m_serverBlockedExts.join(QLatin1Char(',')));
        m_serverReadOnlyDirsEdit->setText(m_serverReadOnlyDirs.join(QLatin1Char(',')));
        m_serverGlobalLimitSpin->setValue(m_serverGlobalLimit);
        m_serverSessionLimitSpin->setValue(m_serverSessionLimit);
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
}

void MainWindow::saveSettings() {
    QSettings settings;

    settings.setValue(QStringLiteral("client/host"), m_hostEdit->text());
    settings.setValue(QStringLiteral("client/port"), m_clientPortSpin->value());
    settings.setValue(QStringLiteral("client/blockSize"), m_blockSizeSpin->value());
    settings.setValue(QStringLiteral("client/timeoutMs"), m_timeoutSpin->value());
    settings.setValue(QStringLiteral("client/windowSize"), m_windowSizeSpin->value());

    settings.setValue(QStringLiteral("server/port"), m_serverPort);
    settings.setValue(QStringLiteral("server/dir"), m_serverDir);
    settings.setValue(QStringLiteral("server/maxConcurrent"), m_maxConcurrent);
    settings.setValue(QStringLiteral("server/singlePort"), m_serverSinglePort);
    settings.setValue(QStringLiteral("server/jsonLogging"), m_serverJsonLogging);
    settings.setValue(QStringLiteral("server/allowedExts"), m_serverAllowedExts);
    settings.setValue(QStringLiteral("server/blockedExts"), m_serverBlockedExts);
    settings.setValue(QStringLiteral("server/readOnlyDirs"), m_serverReadOnlyDirs);
    settings.setValue(QStringLiteral("server/globalLimit"), m_serverGlobalLimit);
    settings.setValue(QStringLiteral("server/sessionLimit"), m_serverSessionLimit);

    settings.setValue(QStringLiteral("ui/theme"), ThemeController::modeToString(m_theme->mode()));
    settings.setValue(QStringLiteral("ui/configMode"), m_configStack ? m_configStack->currentIndex() : 0);
    settings.setValue(QStringLiteral("ui/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("ui/windowState"), saveState());
}

void MainWindow::closeEvent(QCloseEvent *event) {
    saveSettings();
    QMainWindow::closeEvent(event);
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
        m_serverAllowedExtsEdit->setText(settings.value(QStringLiteral("server/allowedExts")).toStringList().join(QLatin1Char(',')));
        m_serverBlockedExtsEdit->setText(settings.value(QStringLiteral("server/blockedExts")).toStringList().join(QLatin1Char(',')));
        m_serverReadOnlyDirsEdit->setText(settings.value(QStringLiteral("server/readOnlyDirs")).toStringList().join(QLatin1Char(',')));
        m_serverGlobalLimitSpin->setValue(settings.value(QStringLiteral("server/globalLimit"), 0).toInt());
        m_serverSessionLimitSpin->setValue(settings.value(QStringLiteral("server/sessionLimit"), 0).toInt());
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
    m_serverAllowedExtsEdit->setText(settings.value(QStringLiteral("allowedExts")).toStringList().join(QLatin1Char(',')));
    m_serverBlockedExtsEdit->setText(settings.value(QStringLiteral("blockedExts")).toStringList().join(QLatin1Char(',')));
    m_serverReadOnlyDirsEdit->setText(settings.value(QStringLiteral("readOnlyDirs")).toStringList().join(QLatin1Char(',')));
    m_serverGlobalLimitSpin->setValue(settings.value(QStringLiteral("globalLimit"), 0).toInt());
    m_serverSessionLimitSpin->setValue(settings.value(QStringLiteral("sessionLimit"), 0).toInt());
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
    settings.setValue(QStringLiteral("allowedExts"), m_serverAllowedExtsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts));
    settings.setValue(QStringLiteral("blockedExts"), m_serverBlockedExtsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts));
    settings.setValue(QStringLiteral("readOnlyDirs"), m_serverReadOnlyDirsEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts));
    settings.setValue(QStringLiteral("globalLimit"), m_serverGlobalLimitSpin->value());
    settings.setValue(QStringLiteral("sessionLimit"), m_serverSessionLimitSpin->value());
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

}  // namespace tftp::gui
