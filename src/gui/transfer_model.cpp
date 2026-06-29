#include "gui/transfer_model.h"

#include <QApplication>
#include <QPainter>
#include <QStyleOptionProgressBar>

namespace tftp::gui {

TransferModel::TransferModel(QObject *parent) : QAbstractTableModel(parent) {}

int TransferModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_items.size();
}

int TransferModel::columnCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant TransferModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_items.size())
        return {};
    const TransferItem &it = m_items.at(index.row());

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case ColName:
                return it.name;
            case ColDirection:
                return it.isUpload ? QStringLiteral("Upload") : QStringLiteral("Download");
            case ColPeer:
                return it.peer;
            case ColProgress:
                return it.percent;  // consumed by the delegate.
            case ColStatus:
                return it.status;
            default:
                return {};
        }
    }
    if (role == Qt::TextAlignmentRole && index.column() == ColDirection)
        return int(Qt::AlignCenter);
    return {};
}

QVariant TransferModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
        return {};
    switch (section) {
        case ColName:
            return QStringLiteral("File");
        case ColDirection:
            return QStringLiteral("Direction");
        case ColPeer:
            return QStringLiteral("Peer");
        case ColProgress:
            return QStringLiteral("Progress");
        case ColStatus:
            return QStringLiteral("Status");
        default:
            return {};
    }
}

int TransferModel::addTransfer(const QString &name, bool isUpload, const QString &peer) {
    const int row = m_items.size();
    beginInsertRows({}, row, row);
    TransferItem it;
    it.name = name;
    it.isUpload = isUpload;
    it.peer = peer;
    it.status = QStringLiteral("Starting…");
    m_items.append(it);
    endInsertRows();
    return row;
}

void TransferModel::updateProgress(int row, qint64 done, qint64 total) {
    if (row < 0 || row >= m_items.size())
        return;
    TransferItem &it = m_items[row];
    it.done = done;
    it.total = total;
    it.percent = (total > 0) ? int((done * 100) / total) : 0;
    if (!it.finished)
        it.status = QStringLiteral("Transferring…");
    emit dataChanged(index(row, ColProgress), index(row, ColStatus));
}

void TransferModel::setFinished(int row, bool ok, const QString &message) {
    if (row < 0 || row >= m_items.size())
        return;
    TransferItem &it = m_items[row];
    it.finished = true;
    it.ok = ok;
    if (ok)
        it.percent = 100;
    if (ok)
        it.status = QStringLiteral("Completed");
    else if (message.isEmpty())
        it.status = QStringLiteral("Failed");
    else
        it.status = QStringLiteral("Failed: %1").arg(message);
    emit dataChanged(index(row, ColProgress), index(row, ColStatus));
}

void ProgressBarDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    if (index.column() != TransferModel::ColProgress) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    const int percent = qBound(0, index.data(Qt::DisplayRole).toInt(), 100);

    QStyleOptionProgressBar bar;
    bar.rect = option.rect.adjusted(4, 4, -4, -4);
    bar.minimum = 0;
    bar.maximum = 100;
    bar.progress = percent;
    bar.text = QStringLiteral("%1%").arg(percent);
    bar.textVisible = true;
    bar.state = option.state;

    QApplication::style()->drawControl(QStyle::CE_ProgressBar, &bar, painter);
}

}  // namespace tftp::gui
