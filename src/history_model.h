#pragma once
#include <QAbstractTableModel>
#include <QDateTime>
#include <QSortFilterProxyModel>

struct HistoryEntry {
    QDateTime time;
    QString id, title, result, method, path, error;
    qint64 seconds = 0, sequence = 0;
    bool latest = false;
};

class HistoryModel : public QAbstractTableModel {
    Q_OBJECT
  public:
    using QAbstractTableModel::QAbstractTableModel;
    QList<HistoryEntry> entries;
    void reload();
    int rowCount(const QModelIndex & = {}) const override;
    int columnCount(const QModelIndex & = {}) const override;
    QVariant data(const QModelIndex &, int role = Qt::DisplayRole) const override;
    QVariant headerData(int, Qt::Orientation, int role) const override;

  private:
    qint64 offset = -1, sequence = 0;
    void markLatest();
    HistoryEntry parse(const QByteArray &line);
};

class HistoryFilter : public QSortFilterProxyModel {
    Q_OBJECT
  public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void setSearch(const QString &text);
    void setResult(const QString &result);
    void setLatestOnly(bool enabled);

  protected:
    bool filterAcceptsRow(int row, const QModelIndex &) const override;

  private:
    QString searchText, resultName;
    bool latestOnly = false;
};
