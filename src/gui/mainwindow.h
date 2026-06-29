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
class QTabWidget;
class QCloseEvent;

namespace tftp {
class TftpServer;
class TftpClient;
}  // namespace tftp

namespace tftp::gui {

class TransferModel;
class ThemeController;

/**
 * @brief The AetherTFTP main application window.
 *
 * A tabbed control surface: a **Client** tab (transfer controls + the live
 * transfer list with per-row cancel), a **Server** tab (embedded server
 * controls + activity log), and a **Dashboard** tab (metric cards). Transient
 * feedback goes to the status bar; the colour theme follows the OS (View →
 * Theme to override). Supports drag-and-drop upload. Transfers run on the GUI
 * thread's event loop — the core engine is fully asynchronous (no blocking).
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
    void toggleServer();           ///< Start or stop the embedded server.
    void configureServer();        ///< Open the server settings dialog.
    void browseLocalFile();        ///< Pick a local file for the client controls.
    void startDownload();          ///< Begin a download using the client controls.
    void startUpload();            ///< Begin an upload using the client controls.
    void cancelTransfer(int row);  ///< Abort the transfer shown at @p row.
    void clearCompleted();         ///< Remove finished rows from the list.

private:
    QWidget *buildClientTab();
    QWidget *buildServerTab();
    QWidget *buildDashboardTab();
    QWidget *makeMetricCard(const QString &caption, QLabel **valueOut);
    void buildMenus();

    void appendLog(const QString &message);
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

    // Server controls.
    QPushButton *m_serverToggleBtn = nullptr;
    QLabel *m_serverStatusLabel = nullptr;

    // Client controls.
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_clientPortSpin = nullptr;
    QLineEdit *m_fileEdit = nullptr;
    QSpinBox *m_blockSizeSpin = nullptr;
    QSpinBox *m_timeoutSpin = nullptr;

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
    QTabWidget *m_tabs = nullptr;
    TransferModel *m_model = nullptr;
    QTreeView *m_view = nullptr;
    QPlainTextEdit *m_log = nullptr;

    // Theme.
    ThemeController *m_theme = nullptr;

    // Active-transfer bookkeeping. The id is stable across row removals, so it
    // survives "Clear Completed" (which shifts row indices).
    quint64 m_nextId = 1;
    QHash<TftpClient *, quint64> m_idOf;
    QHash<quint64, TftpClient *> m_clientById;
    QHash<TftpClient *, QString> m_lastError;
};

}  // namespace tftp::gui
