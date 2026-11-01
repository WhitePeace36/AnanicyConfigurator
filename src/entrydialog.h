#pragma once

#include "entryeditor.h"

#include <QDialog>

#include <functional>

// Pop-up dialog around an EntryEditor. It edits a working copy; the caller applies it on OK.
class EntryDialog : public QDialog {
    Q_OBJECT
public:
    // Return false (after telling the user why) to keep the dialog open.
    using Validator = std::function<bool(const Entry &, QWidget *dialog)>;

    EntryDialog(EntryEditor::Mode mode, const QString &title, const EntryPtr &workingCopy, QWidget *parent = nullptr);

    EntryEditor *editor() const { return m_editor; }
    void setValidator(Validator v) { m_validate = std::move(v); }

    void accept() override;

private:
    EntryEditor *m_editor;
    EntryPtr m_entry;
    Validator m_validate;
};
