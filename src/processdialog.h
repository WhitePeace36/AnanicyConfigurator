#pragma once

#include <QDialog>

#include <functional>

class QLineEdit;
class QCheckBox;
class QTreeWidget;

// Lists running processes (grouped by name) and lets the user pick one.
class ProcessDialog : public QDialog {
    Q_OBJECT
public:
    // existingRule(name) returns the type of an existing rule ("" if none, "(rule)" if typeless)
    explicit ProcessDialog(std::function<QString(const QString &)> existingRule, QWidget *parent = nullptr);

    QString selectedName() const { return m_selected; }

private:
    void reload();
    void applyFilter();
    void accept() override;

    std::function<QString(const QString &)> m_existing;
    QLineEdit *m_filter;
    QCheckBox *m_hideKernel;
    QCheckBox *m_hideRuled;
    QTreeWidget *m_list;
    QString m_selected;
};
