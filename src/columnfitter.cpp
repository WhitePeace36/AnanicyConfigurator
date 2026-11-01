#include "columnfitter.h"

#include <QAbstractItemModel>
#include <QEvent>
#include <QHeaderView>
#include <QTableView>
#include <QTimer>

ColumnFitter::ColumnFitter(QTableView *view) : QObject(view), m_view(view)
{
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    view->horizontalHeader()->setStretchLastSection(false);
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    view->viewport()->installEventFilter(this);

    QAbstractItemModel *m = view->model();
    connect(m, &QAbstractItemModel::modelReset, this, &ColumnFitter::schedule);
    connect(m, &QAbstractItemModel::layoutChanged, this, &ColumnFitter::schedule);
    connect(m, &QAbstractItemModel::rowsInserted, this, &ColumnFitter::schedule);
    connect(m, &QAbstractItemModel::rowsRemoved, this, &ColumnFitter::schedule);
    connect(m, &QAbstractItemModel::dataChanged, this, &ColumnFitter::schedule);
    schedule();
}

void ColumnFitter::schedule()
{
    if (m_pending)
        return;
    m_pending = true;
    QTimer::singleShot(0, this, [this] {
        m_pending = false;
        measure();
        apply();
    });
}

bool ColumnFitter::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj == m_view->viewport() && ev->type() == QEvent::Resize)
        apply();
    return QObject::eventFilter(obj, ev);
}

void ColumnFitter::measure()
{
    QHeaderView *h = m_view->horizontalHeader();
    const int cols = h->count();
    m_natural.resize(cols);
    for (int c = 0; c < cols; ++c) {
        m_view->resizeColumnToContents(c); // considers header text and visible rows
        m_natural[c] = qMax(h->sectionSizeHint(c), m_view->columnWidth(c)) + 8;
    }
}

void ColumnFitter::apply()
{
    const int cols = m_natural.size();
    if (cols == 0 || cols != m_view->horizontalHeader()->count())
        return;
    int total = 0;
    for (int w : m_natural)
        total += w;
    const int avail = m_view->viewport()->width();
    if (total <= 0)
        return;

    if (avail <= total) { // not enough room: content widths, scroll horizontally
        for (int c = 0; c < cols; ++c)
            m_view->setColumnWidth(c, m_natural[c]);
        return;
    }
    // share the extra space in proportion to the content widths; the last
    // column takes the rounding remainder so the columns end exactly at the edge
    const int extra = avail - total;
    int used = 0;
    for (int c = 0; c < cols; ++c) {
        int w = (c == cols - 1) ? avail - used
                                : m_natural[c] + int(qint64(extra) * m_natural[c] / total);
        m_view->setColumnWidth(c, w);
        used += w;
    }
}
