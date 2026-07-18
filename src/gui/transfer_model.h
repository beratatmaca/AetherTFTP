#pragma once

#include <QAbstractTableModel>
#include <QStyledItemDelegate>
#include <QString>
#include <QVector>

namespace tftp::gui {

/** @brief Lifecycle state of a transfer row. */
enum class TransferState : quint8 {
    Pending,    ///< Created, not yet moving data.
    Queued,     ///< In the transfer queue.
    Active,     ///< Transferring.
    Completed,  ///< Finished successfully.
    Failed,     ///< Finished with an error.
    Cancelled,  ///< Aborted by the user.
};

/** @brief One row in the transfer table. */
struct TransferItem {
    quint64 id = 0;         ///< Stable identifier (survives row removals).
    QString name;           ///< File name being transferred.
    bool isUpload = false;  ///< true: upload (WRQ); false: download (RRQ).
    QString peer;           ///< Remote host:port.
    qint64 done = 0;        ///< Bytes transferred so far.
    qint64 total = -1;      ///< Total bytes if known, else -1.
    int percent = 0;        ///< Progress percentage [0, 100].
    QString detail;         ///< Extra status text (e.g. failure cause).
    TransferState state = TransferState::Pending;
};

/**
 * @brief Table model backing the transfer list view (Model/View decoupled
 *        from the core engine).
 *
 * Columns: Name, Direction, Peer, Progress, Status, Actions. The Progress
 * column's DisplayRole returns an integer percentage consumed by @ref
 * ProgressBarDelegate; @ref StateRole exposes the @ref TransferState to the
 * delegates so they can colour progress and show a cancel affordance.
 */
class TransferModel : public QAbstractTableModel {
    Q_OBJECT
public:
    /** @brief Logical column indices. */
    enum Column : quint8 {
        ColName = 0,
        ColDirection,
        ColPeer,
        ColProgress,
        ColStatus,
        ColActions,
        ColumnCount,
    };

    /** @brief Custom item roles. */
    enum Roles : quint16 {
        StateRole = Qt::UserRole + 1,  ///< Returns int(TransferState).
    };

    /**
     * @brief Construct an empty model.
     * @param parent Optional QObject parent.
     */
    explicit TransferModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    /**
     * @brief Append a new transfer row.
     * @param id Caller-assigned stable identifier for later lookup.
     * @param name File name.
     * @param isUpload @c true for upload, @c false for download.
     * @param peer Remote endpoint string (host:port).
     * @return The row index of the new transfer.
     */
    int addTransfer(quint64 id, const QString &name, bool isUpload, const QString &peer);

    /** @return The current row for @p id, or -1 if no longer present. */
    int rowForId(quint64 id) const;
    /** @return The id at @p row, or 0 if out of range. */
    quint64 idForRow(int row) const;

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

    /**
     * @brief Mark a transfer as cancelled by the user.
     * @param row Row index from @ref addTransfer().
     */
    void setCancelled(int row);

    /**
     * @brief Update the lifecycle state of a row manually.
     * @param row Row index.
     * @param state The target state.
     */
    void setTransferState(int row, TransferState state);

    /** @return @c true if the row exists and its transfer is still in flight. */
    bool isActive(int row) const;

    /** @brief Remove all finished rows (completed, failed, or cancelled). */
    void removeFinished();

private:
    QVector<TransferItem> m_items;
};

/**
 * @brief Renders the Progress column as a state-coloured block ladder (a
 *        row of discrete ticks, echoing TFTP's own lockstep block/ACK
 *        transfer rather than a smooth stream).
 */
class ProgressBarDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

/**
 * @brief Renders a clickable "Cancel" affordance for in-flight rows and emits
 *        @ref cancelRequested when it is clicked.
 */
class TransferActionDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override;

signals:
    /** @brief Emitted when the cancel affordance for @p row is clicked. */
    void cancelRequested(int row);
};

}  // namespace tftp::gui
