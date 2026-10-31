#pragma once

#include "ananicydata.h"
#include "fieldspecs.h"

#include <QWidget>

#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QGroupBox;

// Form for editing one type or one rule. Every setting except the name is a combo box.
class EntryEditor : public QWidget {
    Q_OBJECT
public:
    enum Mode { TypeMode, RuleMode };

    explicit EntryEditor(Mode mode, QWidget *parent = nullptr);

    void setEntry(const EntryPtr &e);
    EntryPtr entry() const { return m_entry; }
    void refresh();          // reload widgets from the current entry
    void commitPrimary();    // apply a pending name edit
    void focusPrimary();
    void focusType();

    void setTypeNames(const QStringList &names);
    void setCgroupNames(const QStringList &names);
    void setTypeDescriber(std::function<QString(const QString &)> fn) { m_describe = std::move(fn); }

signals:
    void fieldChanged(const EntryPtr &e, const QString &key);
    void renameRequested(const EntryPtr &e, const QString &oldName, const QString &newName);

private:
    void fillCombo(QComboBox *c, const FieldSpec &s, const QString &value);
    void fillTypeCombo(const QString &value);
    void onComboChanged(const FieldSpec &s, QComboBox *c);
    void onTypeChanged();
    void updateHints();

    Mode m_mode;
    EntryPtr m_entry;
    bool m_loading = false;
    QString m_primaryKey;

    QLineEdit *m_name = nullptr;
    QLabel *m_nameHint = nullptr;
    QComboBox *m_type = nullptr;
    QLabel *m_typeInfo = nullptr;
    QLabel *m_fileLabel = nullptr;
    QGroupBox *m_fieldsBox = nullptr;
    QVector<QPair<FieldSpec, QComboBox *>> m_fields;
    QStringList m_typeNames;
    QStringList m_cgroupNames;
    std::function<QString(const QString &)> m_describe;
};
