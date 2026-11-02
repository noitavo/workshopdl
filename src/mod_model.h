#pragma once
#include "mod_store.h"
#include <QAbstractTableModel>
#include <QSortFilterProxyModel>

class ModListModel : public QAbstractTableModel {
    Q_OBJECT
  public:
    using QAbstractTableModel::QAbstractTableModel;
    QList<ModEntry> entries;
    void setEntries(const QList<ModEntry> &values);
    int rowCount(const QModelIndex & = {}) const override;
    int columnCount(const QModelIndex & = {}) const override;
    QVariant data(const QModelIndex &, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &, const QVariant &, int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex &) const override;
    QVariant headerData(int, Qt::Orientation, int role) const override;
  signals:
    void enableRequested(const QString &id, bool enabled);
};

class ModFilter : public QSortFilterProxyModel {
    Q_OBJECT
  public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void setSearch(const QString &text);
    void setStatusGroup(int group);

  protected:
    bool filterAcceptsRow(int row, const QModelIndex &) const override;

  private:
    QString searchText;
    int statusGroup = 0;
};
