#pragma once

#include "ananicydata.h"
#include "fieldspecs.h"

#include <QWidget>

#include <functional>

class QComboBox;
class QAction;
class ElidedLabel;
class QLineEdit;
class QGroupBox;
class QToolButton;

// Form for editing one type or one rule. Every setting except the name is a combo box.
// The editor writes straight into the entry it is given (the dialog passes a working copy).
class EntryEditor : public QWidget {
    Q_OBJECT
public:
    enum Mode { TypeMode, RuleMode };

    explicit EntryEditor(Mode mode, QWidget *parent = nullptr);

    void setEntry(const EntryPtr &e);
    EntryPtr entry() const { return m_entry; }
    void refresh();          // reload widgets from the current entry
    void focusPrimary();
    void focusType();

    void setFileHint(const QString &text); // shown instead of the entry's file (for new entries)
    // computes the file shown in the "File:" row from the edited entry (e.g. follows the type)
    void setFileHintFn(std::function<QString(const Entry &)> fn) { m_fileFn = std::move(fn); updateHints(); }
    void setTypeNames(const QStringList &names);
    void setCgroupNames(const QStringList &names);
    void setTypeDescriber(std::function<QString(const QString &)> fn) { m_describe = std::move(fn); }
    void setOverridesVisible(bool visible);

signals:
    void layoutChanged(); // overrides shown/hidden

private:
    void fillCombo(QComboBox *c, const FieldSpec &s, const QString &value);
    void fillTypeCombo(const QString &value);
    void onComboChanged(const FieldSpec &s, QComboBox *c);
    void onTypeChanged();
    void updateHints();
    int overrideCount() const;

    Mode m_mode;
    EntryPtr m_entry;
    bool m_loading = false;
    QString m_primaryKey;
    QString m_fileHint;

    QLineEdit *m_name = nullptr;
    QAction *m_nameHint = nullptr;
    QComboBox *m_type = nullptr;
    ElidedLabel *m_typeInfo = nullptr;
    ElidedLabel *m_fileLabel = nullptr;
    QToolButton *m_overridesToggle = nullptr;
    QGroupBox *m_fieldsBox = nullptr;
    QVector<QPair<FieldSpec, QComboBox *>> m_fields;
    QStringList m_typeNames;
    QStringList m_cgroupNames;
    std::function<QString(const QString &)> m_describe;
    std::function<QString(const Entry &)> m_fileFn;
};
