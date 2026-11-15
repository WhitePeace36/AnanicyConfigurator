#pragma once

#include <QDialog>
#include <QStringList>

#include <functional>

class QLineEdit;
class QCheckBox;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// Lists running processes grouped by the name Ananicy matches against, with one child row per
// distinct command line, and lets the user pick a name (rule for every such process) or one
// command line (rule with "command line contains").
class ProcessDialog : public QDialog {
    Q_OBJECT
public:
    // Text for the "Existing rule" column: describe(name, nullptr) for a name,
    // describe(name, &cmdline) for one command line ("" if no rule applies).
    using Describe = std::function<QString(const QString &name, const QString *cmdline)>;
    struct Selection {
        QString name;
        QStringList argv;      // command line of the picked process (specific == true)
        bool specific = false; // a command line was picked, not the whole name
    };

    // ananicyCpp: group by the file name of argv[0] (what ananicy-cpp matches) instead of comm.
    ProcessDialog(Describe describe, bool ananicyCpp, QWidget *parent = nullptr);

    Selection selection() const { return m_sel; }

private:
    void reload();
    void applyFilter();
    void updateOkButton();
    void accept() override;

    Describe m_describe;
    bool m_ananicyCpp;
    QLineEdit *m_filter;
    QCheckBox *m_hideKernel;
    QCheckBox *m_hideRuled;
    QTreeWidget *m_list;
    QPushButton *m_ok = nullptr;
    Selection m_sel;
};
