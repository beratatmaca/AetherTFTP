#pragma once

#include <QHash>
#include <QMainWindow>
#include <QString>
#include <QWidget>
#include <QTimer>
#include <QElapsedTimer>

class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QLabel;
class QTreeView;

namespace tftp {
class TftpServer;
class TftpClient;
}  // namespace tftp

namespace tftp::gui {

class TransferModel;

/**
 * @brief The AetherTFTP main application window.
 *
 * Hosts the embedded server controls, the client put/get controls, a
 * Model/View transfer list with per-row progress bars, and a log panel.
 * Supports drag-and-drop upload of local files. Transfers run on the GUI
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

private slots:
    void toggleServer();     ///< Start or stop the embedded server.
    void configureServer();  ///< Open the server settings dialog.
    void browseLocalFile();  ///< Pick a local file for the client controls.
    void startDownload();    ///< Begin a download using the client controls.
    void startUpload();      ///< Begin an upload using the client controls.

private:
    QWidget *buildServerGroup();
    QWidget *buildClientGroup();
    QWidget *buildMetricsGroup();
    void appendLog(const QString &message);
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

    // Metrics controls.
    QLabel *m_metricsActiveLabel = nullptr;
    QLabel *m_metricsBytesLabel = nullptr;
    QLabel *m_metricsTransfersLabel = nullptr;
    QLabel *m_metricsRetransLabel = nullptr;
    QLabel *m_metricsSpeedLabel = nullptr;
    QTimer *m_metricsTimer = nullptr;
    qint64 m_lastTotalBytes = 0;
    QElapsedTimer m_metricsSpeedTimer;

    // Transfer list + log.
    TransferModel *m_model = nullptr;
    QTreeView *m_view = nullptr;
    QPlainTextEdit *m_log = nullptr;

    // Active client : its transfer row and last error message.
    QHash<TftpClient *, int> m_rowOf;
    QHash<TftpClient *, QString> m_lastError;
};

}  // namespace tftp::gui
