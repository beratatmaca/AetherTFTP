#include "gui/transfer_model.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionButton>

namespace tftp::gui {

namespace {

/** @brief Accent colour for a transfer state (readable on light and dark). */
QColor stateColor(TransferState state) {
    switch (state) {
        case TransferState::Completed:
            return {0x10, 0xB9, 0x81};  // mint green
        case TransferState::Failed:
            return {0xEF, 0x44, 0x44};  // red
        case TransferState::Cancelled:
            return {0xF5, 0x9E, 0x0B};  // amber
        case TransferState::Active:
        case TransferState::Pending:
            break;
    }
    return {0x06, 0xB6, 0xD4};  // electric cyan (active / pending)
}

/** @brief Human-readable status text for a transfer row. */
QString statusTextFor(const TransferItem &it) {
    switch (it.state) {
        case TransferState::Pending:
            return QStringLiteral("Starting…");
        case TransferState::Active:
            return QStringLiteral("Transferring…");
        case TransferState::Completed:
            return QStringLiteral("Completed");
        case TransferState::Cancelled:
            return QStringLiteral("Cancelled");
        case TransferState::Failed:
            return it.detail.isEmpty() ? QStringLiteral("Failed") : QStringLiteral("Failed: %1").arg(it.detail);
    }
    return {};
}

}  // namespace

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

    if (role == StateRole)
        return int(it.state);

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
                return statusTextFor(it);
            default:
                return {};
        }
    }
    if (role == Qt::ToolTipRole && index.column() == ColStatus)
        return statusTextFor(it);
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
        default:  // ColActions and any out-of-range section.
            return {};
    }
}

int TransferModel::addTransfer(quint64 id, const QString &name, bool isUpload, const QString &peer) {
    const int row = m_items.size();
    beginInsertRows({}, row, row);
    TransferItem it;
    it.id = id;
    it.name = name;
    it.isUpload = isUpload;
    it.peer = peer;
    it.state = TransferState::Pending;
    m_items.append(it);
    endInsertRows();
    return row;
}

int TransferModel::rowForId(quint64 id) const {
    for (int row = 0; row < m_items.size(); ++row) {
        if (m_items.at(row).id == id)
            return row;
    }
    return -1;
}

quint64 TransferModel::idForRow(int row) const {
    if (row < 0 || row >= m_items.size())
        return 0;
    return m_items.at(row).id;
}

void TransferModel::updateProgress(int row, qint64 done, qint64 total) {
    if (row < 0 || row >= m_items.size())
        return;
    TransferItem &it = m_items[row];
    it.done = done;
    it.total = total;
    it.percent = (total > 0) ? int((done * 100) / total) : 0;
    if (it.state == TransferState::Pending)
        it.state = TransferState::Active;
    emit dataChanged(index(row, ColProgress), index(row, ColActions));
}

void TransferModel::setFinished(int row, bool ok, const QString &message) {
    if (row < 0 || row >= m_items.size())
        return;
    TransferItem &it = m_items[row];
    // A user-cancelled row may also receive transferFinished(false); keep the
    // Cancelled state rather than downgrading it to Failed.
    if (it.state == TransferState::Cancelled)
        return;
    it.state = ok ? TransferState::Completed : TransferState::Failed;
    it.detail = ok ? QString() : message;
    if (ok)
        it.percent = 100;
    emit dataChanged(index(row, ColProgress), index(row, ColActions));
}

void TransferModel::setCancelled(int row) {
    if (row < 0 || row >= m_items.size())
        return;
    TransferItem &it = m_items[row];
    it.state = TransferState::Cancelled;
    emit dataChanged(index(row, ColProgress), index(row, ColActions));
}

bool TransferModel::isActive(int row) const {
    if (row < 0 || row >= m_items.size())
        return false;
    const TransferState s = m_items.at(row).state;
    return s == TransferState::Pending || s == TransferState::Active;
}

void TransferModel::removeFinished() {
    for (int row = m_items.size() - 1; row >= 0; --row) {
        if (isActive(row))
            continue;
        beginRemoveRows({}, row, row);
        m_items.removeAt(row);
        endRemoveRows();
    }
}

// --- Delegates -------------------------------------------------------------

void ProgressBarDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    if (index.column() != TransferModel::ColProgress) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    if (option.state & QStyle::State_Selected)
        painter->fillRect(option.rect, option.palette.highlight());

    const int percent = qBound(0, index.data(Qt::DisplayRole).toInt(), 100);
    const auto state = TransferState(index.data(TransferModel::StateRole).toInt());
    const QColor accent = stateColor(state);

    const QRect bar = option.rect.adjusted(6, 6, -6, -6);
    const qreal radius = 4.0;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    // Groove.
    QColor groove = option.palette.color(QPalette::Text);
    groove.setAlpha(28);
    painter->setPen(Qt::NoPen);
    painter->setBrush(groove);
    painter->drawRoundedRect(bar, radius, radius);

    // Filled chunk.
    if (percent > 0) {
        QRect chunk = bar;
        chunk.setWidth(int(bar.width() * (percent / 100.0)));
        painter->setBrush(accent);
        painter->drawRoundedRect(chunk, radius, radius);
    }

    // Percentage label, centred.
    painter->setPen(option.palette.color(QPalette::Text));
    painter->drawText(bar, Qt::AlignCenter, QStringLiteral("%1%").arg(percent));
    painter->restore();
}

QSize TransferActionDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const {
    QSize base = QStyledItemDelegate::sizeHint(option, index);
    base.setWidth(qMax(base.width(), 84));
    return base;
}

void TransferActionDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    if (option.state & QStyle::State_Selected)
        painter->fillRect(option.rect, option.palette.highlight());

    const auto state = TransferState(index.data(TransferModel::StateRole).toInt());
    const bool active = (state == TransferState::Pending || state == TransferState::Active);
    if (!active)
        return;

    QStyleOptionButton button;
    button.rect = option.rect.adjusted(6, 4, -6, -4);
    button.text = QStringLiteral("Cancel");
    button.state = QStyle::State_Enabled | (option.state & QStyle::State_MouseOver);
    QApplication::style()->drawControl(QStyle::CE_PushButton, &button, painter);
}

bool TransferActionDelegate::editorEvent(QEvent *event, QAbstractItemModel * /*model*/, const QStyleOptionViewItem &option,
                                         const QModelIndex &index) {
    if (event->type() != QEvent::MouseButtonRelease)
        return false;
    const auto state = TransferState(index.data(TransferModel::StateRole).toInt());
    if (state != TransferState::Pending && state != TransferState::Active)
        return false;

    auto *me = static_cast<QMouseEvent *>(event);
    const QRect hit = option.rect.adjusted(6, 4, -6, -4);
    if (hit.contains(me->pos())) {
        emit cancelRequested(index.row());
        return true;
    }
    return false;
}

}  // namespace tftp::gui
