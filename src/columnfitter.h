#pragma once

#include <QObject>

#include <QVector>

class QTableView;

// Makes a table's columns fill the whole width of the view: every column gets
// its content width, and the remaining space is shared out in proportion.
class ColumnFitter : public QObject {
    Q_OBJECT
public:
    explicit ColumnFitter(QTableView *view);

    void schedule(); // re-measure contents (coalesced)

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    void measure();
    void apply();

    QTableView *m_view;
    QVector<int> m_natural;
    bool m_pending = false;
};
