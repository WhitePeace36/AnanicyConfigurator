#include "entrymodel.h"

#include <QBrush>
#include <QColor>
#include <QDir>
#include <QFont>

EntryModel::EntryModel(EntryStore *store, QString rootDir, QObject *parent)
    : QAbstractTableModel(parent), m_store(store), m_root(std::move(rootDir))
{
}

void EntryModel::setColumns(QVector<Column> cols)
{
    beginResetModel();
    m_cols = std::move(cols);
    endResetModel();
}

int EntryModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_store->entries().size();
}

int EntryModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_cols.size();
}

QVariant EntryModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_store->entries().size())
        return {};
    const Entry &e = *m_store->entries().at(index.row());
    const Column &c = m_cols.at(index.column());

    switch (role) {
    case Qt::DisplayRole:
    case SortRole:
        return c.display(e);
    case Qt::ToolTipRole: {
        QString tip = c.tooltip ? c.tooltip(e) : QString();
        if (m_warn) {
            const QString w = m_warn(e);
            if (!w.isEmpty())
                tip = tip.isEmpty() ? w : tip + QStringLiteral("\n") + w;
        }
        if (e.modified)
            tip += (tip.isEmpty() ? QString() : QStringLiteral("\n")) + tr("Unsaved changes");
        return tip.isEmpty() ? QVariant() : tip;
    }
    case Qt::FontRole:
        if (e.modified) {
            QFont f;
            f.setBold(true);
            return f;
        }
        return {};
    case Qt::ForegroundRole:
        if (m_warn && !m_warn(e).isEmpty())
            return QBrush(QColor(200, 40, 40));
        return {};
    }
    return {};
}

QVariant EntryModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o == Qt::Horizontal && role == Qt::DisplayRole && section < m_cols.size())
        return m_cols.at(section).header;
    return QAbstractTableModel::headerData(section, o, role);
}

EntryPtr EntryModel::entryAt(int row) const
{
    if (row < 0 || row >= m_store->entries().size())
        return nullptr;
    return m_store->entries().at(row);
}

int EntryModel::rowOf(const EntryPtr &e) const
{
    return m_store->entries().indexOf(e);
}

void EntryModel::reset()
{
    beginResetModel();
    endResetModel();
}

void EntryModel::entryChanged(const EntryPtr &e)
{
    int r = rowOf(e);
    if (r >= 0)
        emit dataChanged(index(r, 0), index(r, columnCount() - 1));
}

void EntryModel::allChanged()
{
    if (rowCount() > 0)
        emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
}

QString EntryModel::relPath(const QString &path) const
{
    return QDir(m_root).relativeFilePath(path);
}

// ---------------------------------------------------------------- proxy

void EntryFilterProxy::setText(const QString &t)
{
    m_text = t.trimmed();
    invalidateFilter();
}

void EntryFilterProxy::setExact(int column, const QString &value)
{
    m_exactCol = column;
    m_exactVal = value;
    invalidateFilter();
}

bool EntryFilterProxy::filterAcceptsRow(int row, const QModelIndex &parent) const
{
    QAbstractItemModel *m = sourceModel();
    if (m_exactCol >= 0) {
        if (m->index(row, m_exactCol, parent).data().toString() != m_exactVal)
            return false;
    }
    if (m_text.isEmpty())
        return true;
    for (int c = 0; c < m->columnCount(parent); ++c)
        if (m->index(row, c, parent).data().toString().contains(m_text, Qt::CaseInsensitive))
            return true;
    return false;
}

bool EntryFilterProxy::lessThan(const QModelIndex &a, const QModelIndex &b) const
{
    const QVariant va = a.data(EntryModel::SortRole);
    const QVariant vb = b.data(EntryModel::SortRole);
    bool oka = false, okb = false;
    const double da = va.toString().toDouble(&oka);
    const double db = vb.toString().toDouble(&okb);
    if (oka && okb)
        return da < db;
    if (oka != okb)
        return oka; // numbers before empty/text
    return QString::localeAwareCompare(va.toString(), vb.toString()) < 0;
}
