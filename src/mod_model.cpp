#include "mod_model.h"
#include <QBrush>
#include <QColor>
#include <QDateTime>

void ModListModel::setEntries(const QList<ModEntry> &values) {
    beginResetModel();
    entries = values;
    endResetModel();
}
int ModListModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : int(entries.size());
}
int ModListModel::columnCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : 5;
}
QVariant ModListModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    const QStringList labels{tr("Mod"), tr("Game"), tr("Updated"), tr("Size"), tr("Status")};
    return labels.value(section);
}
Qt::ItemFlags ModListModel::flags(const QModelIndex &index) const {
    auto flags = QAbstractTableModel::flags(index);
    if (index.isValid() && index.column() == 0 && index.row() < entries.size() &&
        entries[index.row()].installed)
        flags |= Qt::ItemIsUserCheckable;
    return flags;
}
QVariant ModListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= entries.size())
        return {};
    const auto &entry = entries[index.row()];
    if (role == Qt::CheckStateRole && index.column() == 0 && entry.installed)
        return entry.enabled ? Qt::Checked : Qt::Unchecked;
    if (role == Qt::ToolTipRole)
        return entry.folder.isEmpty() ? entry.title : entry.title + QLatin1Char('\n') + entry.folder;
    if (role == Qt::ForegroundRole && index.column() == 4) {
        if (entry.status == QStringLiteral("Missing files") || entry.status == QStringLiteral("Modified") ||
            entry.status == QStringLiteral("Incomplete"))
            return QBrush(QColor(214, 84, 84));
        if (entry.status == QStringLiteral("Update available"))
            return QBrush(QColor(224, 160, 48));
        return {};
    }
    if (role == Qt::UserRole) {
        if (index.column() == 2)
            return entry.updated;
        if (index.column() == 3)
            return entry.size;
        return data(index, Qt::DisplayRole);
    }
    if (role != Qt::DisplayRole)
        return {};
    switch (index.column()) {
    case 0:
        return entry.title;
    case 1:
        return entry.game;
    case 2:
        return entry.updated > 0 ? QDateTime::fromSecsSinceEpoch(entry.updated).toString(QStringLiteral("yyyy-MM-dd"))
                                 : QString();
    case 3:
        return entry.size > 0 ? sizeText(entry.size) : QString();
    case 4:
        return entry.status;
    }
    return {};
}
bool ModListModel::setData(const QModelIndex &index, const QVariant &value, int role) {
    if (role != Qt::CheckStateRole || !index.isValid() || index.row() >= entries.size())
        return false;
    emit enableRequested(entries[index.row()].id, value.toInt() == Qt::Checked);
    return false;
}

void ModFilter::setSearch(const QString &text) {
    const auto trimmed = text.trimmed();
    if (searchText == trimmed)
        return;
    beginFilterChange();
    searchText = trimmed;
    endFilterChange(Direction::Rows);
}
void ModFilter::setStatusGroup(int group) {
    if (statusGroup == group)
        return;
    beginFilterChange();
    statusGroup = group;
    endFilterChange(Direction::Rows);
}
bool ModFilter::filterAcceptsRow(int row, const QModelIndex &) const {
    const auto *model = qobject_cast<const ModListModel *>(sourceModel());
    if (!model || row < 0 || row >= model->entries.size())
        return false;
    const auto &entry = model->entries[row];
    const QString &status = entry.status;
    switch (statusGroup) {
    case 1:
        if (entry.installed || entry.unmanaged)
            return false;
        break;
    case 2:
        if (!entry.installed || !entry.enabled)
            return false;
        break;
    case 3:
        if (!entry.installed || entry.enabled)
            return false;
        break;
    case 4:
        if (status != QStringLiteral("Update available"))
            return false;
        break;
    case 5:
        if (status != QStringLiteral("Missing files") && status != QStringLiteral("Modified") &&
            status != QStringLiteral("Incomplete") && status != QStringLiteral("Unavailable"))
            return false;
        break;
    case 6:
        if (!entry.unmanaged)
            return false;
        break;
    default:
        break;
    }
    if (searchText.isEmpty())
        return true;
    return entry.title.contains(searchText, Qt::CaseInsensitive) ||
           entry.game.contains(searchText, Qt::CaseInsensitive) || entry.id.contains(searchText);
}
