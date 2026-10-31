#include "entryeditor.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpressionValidator>
#include <QVBoxLayout>

static const char *kUnset = "(unset)";
static const char *kNoType = "(no type)";

EntryEditor::EntryEditor(Mode mode, QWidget *parent)
    : QWidget(parent), m_mode(mode),
      m_primaryKey(mode == TypeMode ? QStringLiteral("type") : QStringLiteral("name"))
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto *form = new QFormLayout;
    m_name = new QLineEdit;
    m_name->setPlaceholderText(mode == TypeMode ? tr("Type name, e.g. Game") : tr("Process name, e.g. wine"));
    form->addRow(mode == TypeMode ? tr("Type name:") : tr("Process name:"), m_name);
    m_nameHint = new QLabel;
    m_nameHint->setWordWrap(true);
    m_nameHint->setStyleSheet(QStringLiteral("color: #b06000;"));
    m_nameHint->hide();
    form->addRow(QString(), m_nameHint);

    if (mode == RuleMode) {
        m_type = new QComboBox;
        m_type->setToolTip(tr("Type from the types list whose settings are applied to this process"));
        form->addRow(tr("Type:"), m_type);
        m_typeInfo = new QLabel;
        m_typeInfo->setWordWrap(true);
        m_typeInfo->setStyleSheet(QStringLiteral("color: palette(mid);"));
        form->addRow(QString(), m_typeInfo);
        connect(m_type, &QComboBox::currentTextChanged, this, [this] { onTypeChanged(); });
    }
    m_fileLabel = new QLabel;
    m_fileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_fileLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));
    form->addRow(tr("File:"), m_fileLabel);
    outer->addLayout(form);

    m_fieldsBox = new QGroupBox(mode == TypeMode ? tr("Settings")
                                                 : tr("Overrides (optional, take precedence over the type)"));
    auto *grid = new QGridLayout(m_fieldsBox);
    int i = 0;
    for (const FieldSpec &s : typeFieldSpecs()) {
        auto *c = new QComboBox;
        c->setEditable(s.editable);
        c->setToolTip(s.tip);
        c->setMinimumContentsLength(10);
        if (s.editable) {
            c->setInsertPolicy(QComboBox::NoInsert);
            c->lineEdit()->setPlaceholderText(QString::fromLatin1(kUnset));
            if (s.numeric)
                c->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("-?\\d{0,4}")), c));
        }
        auto *lbl = new QLabel(s.label + QLatin1Char(':'));
        lbl->setToolTip(s.tip);
        const int row = i / 2, col = (i % 2) * 2;
        grid->addWidget(lbl, row, col);
        grid->addWidget(c, row, col + 1);
        grid->setColumnStretch(col + 1, 1);
        m_fields.append({s, c});
        connect(c, &QComboBox::currentTextChanged, this, [this, s, c] { onComboChanged(s, c); });
        ++i;
    }
    outer->addWidget(m_fieldsBox);
    outer->addStretch(1);

    connect(m_name, &QLineEdit::editingFinished, this, &EntryEditor::commitPrimary);
    connect(m_name, &QLineEdit::textEdited, this, [this] { updateHints(); });

    setEntry(nullptr);
}

void EntryEditor::setEntry(const EntryPtr &e)
{
    if (m_entry && m_entry != e)
        commitPrimary();
    m_entry = e;
    setEnabled(e != nullptr);
    refresh();
}

void EntryEditor::refresh()
{
    m_loading = true;
    const EntryPtr &e = m_entry;
    m_name->setText(e ? e->str(m_primaryKey) : QString());
    m_fileLabel->setText(e && e->file ? e->file->path : QString());
    if (m_type)
        fillTypeCombo(e ? e->str(QStringLiteral("type")) : QString());
    for (auto &f : m_fields)
        fillCombo(f.second, f.first, e ? e->str(f.first.key) : QString());
    m_loading = false;
    updateHints();
}

void EntryEditor::fillCombo(QComboBox *c, const FieldSpec &s, const QString &value)
{
    c->clear();
    c->addItem(s.editable ? QString() : QString::fromLatin1(kUnset));
    QStringList opts = (s.key == QLatin1String("cgroup")) ? m_cgroupNames : s.options;
    c->addItems(opts);
    if (value.isEmpty()) {
        c->setCurrentIndex(0);
        if (s.editable)
            c->setEditText(QString());
        return;
    }
    int idx = c->findText(value);
    if (idx < 0) {
        if (s.editable) {
            c->setEditText(value);
            return;
        }
        c->addItem(value); // keep unknown value from the file visible
        idx = c->count() - 1;
    }
    c->setCurrentIndex(idx);
}

void EntryEditor::fillTypeCombo(const QString &value)
{
    m_type->clear();
    m_type->addItem(QString::fromLatin1(kNoType));
    m_type->addItems(m_typeNames);
    if (value.isEmpty()) {
        m_type->setCurrentIndex(0);
        return;
    }
    int idx = m_type->findText(value);
    if (idx < 0) {
        m_type->addItem(value);
        idx = m_type->count() - 1;
        m_type->setItemData(idx, tr("This type is not defined in any *.types file"), Qt::ToolTipRole);
    }
    m_type->setCurrentIndex(idx);
}

void EntryEditor::onComboChanged(const FieldSpec &s, QComboBox *c)
{
    if (m_loading || !m_entry)
        return;
    QString t = c->currentText().trimmed();
    if (!s.editable && c->currentIndex() == 0)
        t.clear();
    QJsonValue v(QJsonValue::Undefined);
    if (!t.isEmpty()) {
        if (s.numeric) {
            bool ok = false;
            int n = t.toInt(&ok);
            if (!ok || (s.min != s.max && (n < s.min || n > s.max)))
                return; // incomplete / out of range while typing
            v = QJsonValue(n);
        } else {
            v = QJsonValue(t);
        }
    }
    if (jsonValueText(v) == m_entry->str(s.key))
        return;
    m_entry->set(s.key, v);
    emit fieldChanged(m_entry, s.key);
}

void EntryEditor::onTypeChanged()
{
    if (m_loading || !m_entry)
        return;
    const QString t = m_type->currentIndex() == 0 ? QString() : m_type->currentText();
    if (t == m_entry->str(QStringLiteral("type")))
        return;
    m_entry->set(QStringLiteral("type"), t.isEmpty() ? QJsonValue(QJsonValue::Undefined) : QJsonValue(t));
    updateHints();
    emit fieldChanged(m_entry, QStringLiteral("type"));
}

void EntryEditor::commitPrimary()
{
    if (m_loading || !m_entry)
        return;
    const QString oldName = m_entry->str(m_primaryKey);
    const QString newName = m_name->text().trimmed();
    if (newName == oldName)
        return;
    emit renameRequested(m_entry, oldName, newName); // receiver validates, applies and calls refresh()
}

void EntryEditor::focusPrimary()
{
    m_name->setFocus();
    m_name->selectAll();
}

void EntryEditor::focusType()
{
    if (m_type)
        m_type->setFocus();
}

void EntryEditor::setTypeNames(const QStringList &names)
{
    m_typeNames = names;
    if (m_type) {
        m_loading = true;
        fillTypeCombo(m_entry ? m_entry->str(QStringLiteral("type")) : QString());
        m_loading = false;
        updateHints();
    }
}

void EntryEditor::setCgroupNames(const QStringList &names)
{
    m_cgroupNames = names;
    refresh();
}

void EntryEditor::updateHints()
{
    if (m_mode == RuleMode && m_name->text().trimmed().size() > 15) {
        m_nameHint->setText(tr("Note: the kernel truncates process names (/proc/PID/comm) to 15 characters."));
        m_nameHint->show();
    } else {
        m_nameHint->hide();
    }
    if (m_typeInfo) {
        const QString t = m_entry ? m_entry->str(QStringLiteral("type")) : QString();
        QString info;
        if (!t.isEmpty() && m_describe)
            info = m_describe(t);
        m_typeInfo->setText(info);
        m_typeInfo->setVisible(!info.isEmpty());
    }
}
