#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QString>
#include <QTimer>

class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QLabel;
class QTreeView;
class QStackedWidget;
class QButtonGroup;
class QCloseEvent;
class QComboBox;
class QCheckBox;
class QSystemTrayIcon;
class QMenu;

namespace tftp {
class TftpServer;
class TftpClient;
}  // namespace tftp

namespace tftp::gui {

class TransferModel;
class ThemeController;
class SpeedChartWidget;

/**
 * @brief The AetherTFTP main application window.
 *
 * A single-page control surface: a left panel switches between **Client**
 * and **Server** configuration (only one is shown at a time — you're
 * usually driving one or minding the other, rarely both), while the right
 * side keeps the live dashboard, transfer list, and activity log always
 * visible. Transient feedback goes to the status bar; the colour theme
 * follows the OS (View → Theme to override). Supports drag-and-drop upload.
 * Transfers run on the GUI thread's event loop — the core engine is fully
 * asynchronous (no blocking).
 */
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    /**
     * @brief Construct the main window.
     * @param parent Optional parent widget.
     */
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    void toggleServer();  ///< Start or stop the embedded server.
    void browseServerDir();
    void applyServerConfig();
    void browseLocalFile();        ///< Pick a local file for the client controls.
    void startDownload();          ///< Begin a download using the client controls.
    void startUpload();            ///< Begin an upload using the client controls.
    void cancelTransfer(int row);  ///< Abort the transfer shown at @p row.
    void clearCompleted();         ///< Remove finished rows from the list.
    void onProfileChanged(int index);
    void saveCurrentProfile();
    void deleteCurrentProfile();
    void onServerProfileChanged(int index);
    void saveCurrentServerProfile();
    void deleteCurrentServerProfile();
    void importClientProfile();
    void exportClientProfile();
    void importServerProfile();
    void exportServerProfile();

private:
    QWidget *buildMainView();
    QWidget *makeMetricCard(const QString &caption, QLabel **valueOut);
    void buildMenus();

    void appendLog(const QString &message);
    void filterLog();
    void showStatus(const QString &message);
    void updateMetrics();

    /**
     * @brief Create a client, register a transfer row, and wire its signals.
     * @param isUpload @c true for upload (WRQ), @c false for download (RRQ).
     * @param host Remote host.
     * @param port Remote port.
     * @param localFile Local file path (source for upload, dest for download).
     * @param remoteName Remote file name.
     */
    void startTransfer(bool isUpload, const QString &host, quint16 port, const QString &localFile, const QString &remoteName);

    /** @return Number of transfers still in progress. */
    int activeTransfers() const;

    void loadSettings();
    void saveSettings();

    // Server state.
    TftpServer *m_server = nullptr;
    bool m_serverRunning = false;
    quint16 m_serverPort = 6969;
    QString m_serverDir;
    int m_maxConcurrent = 4;
    bool m_serverSinglePort = false;
    bool m_serverJsonLogging = false;
    QStringList m_serverAllowedExts;
    QStringList m_serverBlockedExts;
    QStringList m_serverIpWhitelist;
    QStringList m_serverIpBlacklist;
    QStringList m_serverReadOnlyDirs;
    QMap<QString, QString> m_serverVirtualMappings;
    int m_serverGlobalLimit = 0;
    int m_serverSessionLimit = 0;
    bool m_serverAutoStart = false;
    bool m_serverProxyDhcp = false;
    QString m_serverProxyDhcpBootFile = QStringLiteral("bootx64.efi");

    // Server controls.
    QComboBox *m_serverProfileCombo = nullptr;
    QPushButton *m_serverToggleBtn = nullptr;
    QLabel *m_serverStatusLabel = nullptr;
    QSpinBox *m_serverPortSpin = nullptr;
    QLineEdit *m_serverDirEdit = nullptr;
    QSpinBox *m_serverMaxSpin = nullptr;
    QCheckBox *m_serverSinglePortCheck = nullptr;
    QCheckBox *m_serverJsonLoggingCheck = nullptr;
    QCheckBox *m_serverAutoStartCheck = nullptr;
    QCheckBox *m_serverProxyDhcpCheck = nullptr;
    QLineEdit *m_serverProxyDhcpBootFileEdit = nullptr;
    QLineEdit *m_serverAllowedExtsEdit = nullptr;
    QLineEdit *m_serverBlockedExtsEdit = nullptr;
    QLineEdit *m_serverIpWhitelistEdit = nullptr;
    QLineEdit *m_serverIpBlacklistEdit = nullptr;
    QLineEdit *m_serverReadOnlyDirsEdit = nullptr;
    QLineEdit *m_serverVirtualMappingsEdit = nullptr;
    QSpinBox *m_serverGlobalLimitSpin = nullptr;
    QSpinBox *m_serverSessionLimitSpin = nullptr;
    QLineEdit *m_serverPskKeyEdit = nullptr;
    QString m_serverPskKey;

    // Left panel: Client/Server config switch.
    QStackedWidget *m_configStack = nullptr;
    QButtonGroup *m_configModeGroup = nullptr;

    // Client controls.
    QComboBox *m_profileCombo = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_clientPortSpin = nullptr;
    QLineEdit *m_fileEdit = nullptr;
    QSpinBox *m_blockSizeSpin = nullptr;
    QSpinBox *m_timeoutSpin = nullptr;
    QSpinBox *m_windowSizeSpin = nullptr;
    QLineEdit *m_pskKeyEdit = nullptr;
    QString m_pskKey;

    void loadProfileList();
    void loadServerProfileList();

    // Dashboard metric value labels.
    QLabel *m_metricActive = nullptr;
    QLabel *m_metricBytes = nullptr;
    QLabel *m_metricTransfers = nullptr;
    QLabel *m_metricRetrans = nullptr;
    QLabel *m_metricSpeed = nullptr;
    QTimer *m_metricsTimer = nullptr;
    qint64 m_lastTotalBytes = 0;
    QElapsedTimer m_metricsSpeedTimer;

    // Views.
    TransferModel *m_model = nullptr;
    QTreeView *m_view = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QLineEdit *m_logSearchEdit = nullptr;
    QComboBox *m_logFilterCombo = nullptr;
    QStringList m_allLogLines;
    SpeedChartWidget *m_speedChart = nullptr;

    // Theme.
    ThemeController *m_theme = nullptr;

    // System Tray
    QSystemTrayIcon *m_trayIcon = nullptr;
    QAction *m_trayToggleServerAction = nullptr;
    void setupTrayIcon();

    // Active-transfer bookkeeping. The id is stable across row removals, so it
    // survives "Clear Completed" (which shifts row indices).
    quint64 m_nextId = 1;
    QHash<TftpClient *, quint64> m_idOf;
    QHash<quint64, TftpClient *> m_clientById;
    QHash<TftpClient *, QString> m_lastError;

    // Client Transfer Queue
    struct QueuedTransfer {
        quint64 id;
        bool isUpload;
        QString host;
        quint16 port;
        QString localFile;
        QString remoteName;
        QString pskKey;
    };
    QList<QueuedTransfer> m_clientQueue;
    void processQueue();
};

}  // namespace tftp::gui
