#pragma once

#include <QAbstractTableModel>
#include <QStyledItemDelegate>
#include <QString>
#include <QVector>

namespace tftp::gui {

/** @brief One row in the transfer table. */
struct TransferItem {
    QString name;           ///< File name being transferred.
    bool isUpload = false;  ///< true: upload (WRQ); false: download (RRQ).
    QString peer;           ///< Remote host:port.
    qint64 done = 0;        ///< Bytes transferred so far.
    qint64 total = -1;      ///< Total bytes if known, else -1.
    int percent = 0;        ///< Progress percentage [0, 100].
    QString status;         ///< Human-readable status text.
    bool finished = false;  ///< Whether the transfer has ended.
    bool ok = false;        ///< Final result once @ref finished is true.
};

/**
 * @brief Table model backing the transfer list view (Model/View decoupled
 *        from the core engine).
 *
 * Columns: Name, Direction, Peer, Progress, Status. The Progress column's
 * DisplayRole returns an integer percentage consumed by @ref
 * ProgressBarDelegate.
 */
class TransferModel : public QAbstractTableModel {
    Q_OBJECT
public:
    /** @brief Logical column indices. */
    enum Column {
        ColName = 0,
        ColDirection,
        ColPeer,
        ColProgress,
        ColStatus,
        ColumnCount,
    };

    /**
     * @brief Construct an empty model.
     * @param parent Optional QObject parent.
     */
    explicit TransferModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;

    /**
     * @brief Append a new transfer row.
     * @param name File name.
     * @param isUpload @c true for upload, @c false for download.
     * @param peer Remote endpoint string (host:port).
     * @return The row index of the new transfer.
     */
    int addTransfer(const QString &name, bool isUpload, const QString &peer);

    /**
     * @brief Update progress for a row.
     * @param row Row index from @ref addTransfer().
     * @param done Cumulative bytes transferred.
     * @param total Total bytes if known, else -1.
     */
    void updateProgress(int row, qint64 done, qint64 total);

    /**
     * @brief Mark a transfer finished.
     * @param row Row index from @ref addTransfer().
     * @param ok @c true on success.
     * @param message Status text (e.g. failure cause).
     */
    void setFinished(int row, bool ok, const QString &message);

private:
    QVector<TransferItem> m_items;
};

/**
 * @brief Renders the Progress column as an embedded progress bar.
 */
class ProgressBarDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
};

}  // namespace tftp::gui
