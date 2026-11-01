#include "entrydialog.h"

#include <QDialogButtonBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

EntryDialog::EntryDialog(EntryEditor::Mode mode, const QString &title, const EntryPtr &workingCopy, QWidget *parent)
    : QDialog(parent), m_editor(new EntryEditor(mode)), m_entry(workingCopy)
{
    setWindowTitle(title);
    setMinimumWidth(400);

    auto *lay = new QVBoxLayout(this);
    lay->addWidget(m_editor);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    lay->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &EntryDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // grow / shrink with the overrides section
    connect(m_editor, &EntryEditor::layoutChanged, this, [this] {
        QTimer::singleShot(0, this, [this] { resize(width(), sizeHint().height()); });
    });

    m_editor->setEntry(workingCopy);
}

void EntryDialog::accept()
{
    if (m_validate && !m_validate(*m_entry, this))
        return;
    QDialog::accept();
}
