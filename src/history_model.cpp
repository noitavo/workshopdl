#include "history_model.h"
#include "storage.h"
#include <QBrush>
#include <QColor>

namespace {
constexpr int maximumEntries = 5000;
constexpr qint64 tailBytes = 4 * 1024 * 1024;

QByteArray readTail(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const qint64 start = qMax(qint64(0), file.size() - tailBytes);
    file.seek(start);
    QByteArray bytes = file.readAll();
    if (start > 0)
        bytes.remove(0, bytes.indexOf('\n') + 1);
    return bytes;
}
QString shortTime(const QDateTime &time) {
    const QDate today = QDate::currentDate();
    if (time.date() == today)
        return time.toString(QStringLiteral("HH:mm"));
    if (time.date().year() == today.year())
        return time.toString(QStringLiteral("MMM d"));
    return time.toString(QStringLiteral("yyyy-MM-dd"));
}
}

HistoryEntry HistoryModel::parse(const QByteArray &line) {
    const auto row = QJsonDocument::fromJson(line).object();
    HistoryEntry entry;
    if (row.isEmpty())
        return entry;
    entry.time = QDateTime::fromString(row.value(QStringLiteral("time")).toString(), Qt::ISODate).toLocalTime();
    entry.id = row.value(QStringLiteral("id")).toString();
    entry.title = row.value(QStringLiteral("title")).toString();
    entry.result = row.value(QStringLiteral("result")).toString();
    entry.method = row.value(QStringLiteral("method")).toString();
    entry.path = row.value(QStringLiteral("path")).toString();
    entry.error = row.value(QStringLiteral("error")).toString();
    entry.seconds = row.value(QStringLiteral("seconds")).toInteger();
    entry.sequence = ++sequence;
    return entry;
}

void HistoryModel::markLatest() {
    QSet<QString> seen;
    for (auto &entry : entries) {
        entry.latest = !seen.contains(entry.id);
        seen.insert(entry.id);
    }
}

void HistoryModel::reload() {
    const QString path = dataRoot() + QStringLiteral("/history.jsonl");
    const qint64 size = QFileInfo(path).size();
    if (offset >= 0 && size >= offset) {
        if (size == offset)
            return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || !file.seek(offset))
            return;
        QList<HistoryEntry> fresh;
        while (!file.atEnd()) {
            const auto entry = parse(file.readLine());
            if (!entry.id.isEmpty())
                fresh.prepend(entry);
        }
        offset = size;
        if (fresh.isEmpty())
            return;
        beginInsertRows({}, 0, int(fresh.size()) - 1);
        entries = fresh + entries;
        markLatest();
        endInsertRows();
        if (entries.size() > maximumEntries) {
            beginRemoveRows({}, maximumEntries, int(entries.size()) - 1);
            entries.resize(maximumEntries);
            endRemoveRows();
        }
        markLatest();
        emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
        return;
    }
    QList<HistoryEntry> loaded;
    for (const auto &suffix : {QStringLiteral(".previous"), QString()}) {
        const auto lines = readTail(path + suffix).split('\n');
        for (const auto &line : lines) {
            const auto entry = parse(line);
            if (!entry.id.isEmpty())
                loaded.prepend(entry);
        }
    }
    if (loaded.size() > maximumEntries)
        loaded.resize(maximumEntries);
    beginResetModel();
    entries = loaded;
    markLatest();
    offset = size;
    endResetModel();
}

int HistoryModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : int(entries.size());
}
int HistoryModel::columnCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : 7;
}
QVariant HistoryModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    const QStringList labels{tr("Time"), tr("Item"), tr("Result"), tr("Method"), tr("Seconds"), tr("Details"), tr("When")};
    return labels.value(section);
}
QVariant HistoryModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= entries.size())
        return {};
    const auto &entry = entries[index.row()];
    if (role == Qt::ToolTipRole) {
        QStringList lines{entry.title.isEmpty() ? entry.id : entry.title,
                          entry.time.toString(QStringLiteral("yyyy-MM-dd HH:mm")), entry.result};
        lines << (entry.error.isEmpty() ? entry.path : entry.error);
        return lines.join(QLatin1Char('\n'));
    }
    if (role == Qt::ForegroundRole && index.column() == 2) {
        if (entry.result == QStringLiteral("Failed"))
            return QBrush(QColor(214, 84, 84));
        if (entry.result == QStringLiteral("Completed"))
            return QBrush(QColor(72, 168, 96));
        return {};
    }
    if (role == Qt::UserRole) {
        if (index.column() == 0 || index.column() == 6)
            return entry.sequence;
        if (index.column() == 4)
            return entry.seconds;
        return data(index, Qt::DisplayRole);
    }
    if (role != Qt::DisplayRole)
        return {};
    switch (index.column()) {
    case 0:
        return entry.time.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    case 1:
        return entry.title.isEmpty() ? entry.id : entry.title;
    case 2:
        return entry.result;
    case 3:
        return entry.method;
    case 4:
        return QString::number(entry.seconds);
    case 5:
        return entry.error.isEmpty() ? entry.path : entry.error;
    case 6:
        return shortTime(entry.time);
    }
    return {};
}

void HistoryFilter::setSearch(const QString &text) {
    const QString trimmed = text.trimmed();
    if (searchText == trimmed)
        return;
    beginFilterChange();
    searchText = trimmed;
    endFilterChange(Direction::Rows);
}
void HistoryFilter::setResult(const QString &result) {
    if (resultName == result)
        return;
    beginFilterChange();
    resultName = result;
    endFilterChange(Direction::Rows);
}
void HistoryFilter::setLatestOnly(bool enabled) {
    if (latestOnly == enabled)
        return;
    beginFilterChange();
    latestOnly = enabled;
    endFilterChange(Direction::Rows);
}
bool HistoryFilter::filterAcceptsRow(int row, const QModelIndex &) const {
    const auto *model = qobject_cast<const HistoryModel *>(sourceModel());
    if (!model || row < 0 || row >= model->entries.size())
        return false;
    const auto &entry = model->entries[row];
    if (latestOnly && !entry.latest)
        return false;
    if (!resultName.isEmpty() && entry.result != resultName)
        return false;
    return searchText.isEmpty() || entry.title.contains(searchText, Qt::CaseInsensitive) ||
           entry.id.contains(searchText) || entry.error.contains(searchText, Qt::CaseInsensitive);
}
