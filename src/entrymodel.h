#pragma once

#include "ananicydata.h"

#include <QAbstractTableModel>
#include <QSortFilterProxyModel>

#include <functional>

// Table model over the entries of an EntryStore. Columns are defined by functions.
class EntryModel : public QAbstractTableModel {
    Q_OBJECT
public:
    struct Column {
        QString header;
        std::function<QVariant(const Entry &)> display;
        std::function<QString(const Entry &)> tooltip;
    };

    EntryModel(EntryStore *store, QString rootDir, QObject *parent = nullptr);

    void setColumns(QVector<Column> cols);
    void setWarnFn(std::function<QString(const Entry &)> fn) { m_warn = std::move(fn); }

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

    EntryPtr entryAt(int row) const;
    int rowOf(const EntryPtr &e) const;
    void reset();
    void entryChanged(const EntryPtr &e);
    void allChanged();
    QString relPath(const QString &path) const;

    static constexpr int SortRole = Qt::UserRole + 1;

private:
    EntryStore *m_store;
    QString m_root;
    QVector<Column> m_cols;
    std::function<QString(const Entry &)> m_warn;
};

// Proxy with free-text filter plus an optional exact match on one column.
class EntryFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void setText(const QString &t);
    void setExact(int column, const QString &value); // column < 0 disables

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
    bool lessThan(const QModelIndex &a, const QModelIndex &b) const override;

private:
    QString m_text;
    int m_exactCol = -1;
    QString m_exactVal;
};
