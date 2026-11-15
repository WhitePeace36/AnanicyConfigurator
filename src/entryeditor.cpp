#include "entryeditor.h"

#include "elidedlabel.h"
#include "rulematch.h"

#include <QAction>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QRegularExpressionValidator>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

static const char *kUnset = "(unset)";
static const char *kNoType = "(no type)";

EntryEditor::EntryEditor(Mode mode, QWidget *parent)
    : QWidget(parent), m_mode(mode),
      m_primaryKey(mode == TypeMode ? QStringLiteral("type") : QStringLiteral("name"))
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    // Fixed layout: every row is always present and one line high, so nothing
    // moves when the selected type (and with it the info text) changes.
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_name = new QLineEdit;
    m_name->setPlaceholderText(mode == TypeMode ? tr("Type name, e.g. Game") : tr("Process name, e.g. wine"));
    // warning icon inside the line edit instead of an extra row
    m_nameHint = m_name->addAction(style()->standardIcon(QStyle::SP_MessageBoxWarning), QLineEdit::TrailingPosition);
    m_nameHint->setToolTip(tr("The kernel truncates process names (/proc/PID/comm) to 15 characters, "
                              "so a longer name may never match."));
    m_nameHint->setVisible(false);
    form->addRow(mode == TypeMode ? tr("Type name:") : tr("Process name:"), m_name);

    if (mode == RuleMode) {
        // cmdline_contains: one text per line, all of them must occur in the command line
        m_cmdline = new QPlainTextEdit;
        m_cmdline->setTabChangesFocus(true);
        m_cmdline->setLineWrapMode(QPlainTextEdit::NoWrap);
        m_cmdline->setPlaceholderText(tr("Optional: only processes whose command line contains this text, "
                                         "e.g. minecraft\n(one text per line; then all of them must appear)"));
        m_cmdline->setToolTip(tr("ananicy-cpp \"cmdline_contains\": the rule only applies to processes with this "
                                 "name whose command line (/proc/PID/cmdline, arguments joined by spaces) contains "
                                 "every text listed here. Case-sensitive. Such rules are tried before the rule "
                                 "for the same name without this field, and the first one that matches wins.\n\n"
                                 "Leave it empty for a normal rule that applies to every process with this name."));
        const QFontMetrics fm(m_cmdline->font());
        m_cmdline->setFixedHeight(fm.lineSpacing() * 3 + 2 * m_cmdline->frameWidth()
                                  + static_cast<int>(2 * m_cmdline->document()->documentMargin()));
        form->addRow(tr("Command line contains:"), m_cmdline);
        // two lines, word-wrapped, fixed height: long texts fit and the dialog does not jump
        m_cmdlineNote = new QLabel;
        m_cmdlineNote->setWordWrap(true);
        m_cmdlineNote->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_cmdlineNote->setFixedHeight(m_cmdlineNote->fontMetrics().lineSpacing() * 2 + 2);
        form->addRow(QString(), m_cmdlineNote);
        connect(m_cmdline, &QPlainTextEdit::textChanged, this, [this] {
            if (m_loading || !m_entry)
                return;
            QStringList texts;
            for (const QString &line : m_cmdline->toPlainText().split(QLatin1Char('\n')))
                if (!line.trimmed().isEmpty())
                    texts << line.trimmed();
            m_entry->set(rulematch::kCmdlineKey, rulematch::needlesValue(texts));
            updateHints();
        });
    }

    if (mode == RuleMode) {
        m_type = new QComboBox;
        m_type->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_type->setToolTip(tr("Type from the types list whose settings are applied to this process"));
        form->addRow(tr("Type:"), m_type);
        m_typeInfo = new ElidedLabel(Qt::ElideRight);
        form->addRow(QString(), m_typeInfo);
        connect(m_type, &QComboBox::currentTextChanged, this, [this] { onTypeChanged(); });
    }
    m_fileLabel = new ElidedLabel(Qt::ElideMiddle);
    form->addRow(tr("File:"), m_fileLabel);
    outer->addLayout(form);

    if (mode == RuleMode) {
        m_overridesToggle = new QToolButton;
        m_overridesToggle->setCheckable(true);
        m_overridesToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_overridesToggle->setArrowType(Qt::RightArrow);
        m_overridesToggle->setAutoRaise(true);
        m_overridesToggle->setToolTip(tr("Per-rule settings that take precedence over the type"));
        outer->addWidget(m_overridesToggle);
        connect(m_overridesToggle, &QToolButton::toggled, this, &EntryEditor::setOverridesVisible);
    }

    m_fieldsBox = new QGroupBox(mode == TypeMode ? tr("Settings")
                                                 : tr("Overrides (take precedence over the type)"));
    auto *grid = new QGridLayout(m_fieldsBox);
    int i = 0;
    for (const FieldSpec &s : typeFieldSpecs()) {
        auto *c = new QComboBox;
        c->setEditable(s.editable);
        c->setToolTip(s.tip);
        c->setMinimumContentsLength(6);
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
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
    if (mode == RuleMode)
        m_fieldsBox->hide();

    connect(m_name, &QLineEdit::textEdited, this, [this](const QString &t) {
        if (m_loading || !m_entry)
            return;
        const QString v = t.trimmed();
        m_entry->set(m_primaryKey, v.isEmpty() ? QJsonValue(QJsonValue::Undefined) : QJsonValue(v));
        updateHints();
    });

    setEntry(nullptr);
}

void EntryEditor::setEntry(const EntryPtr &e)
{
    m_entry = e;
    setEnabled(e != nullptr);
    refresh();
}

void EntryEditor::refresh()
{
    m_loading = true;
    const EntryPtr &e = m_entry;
    m_name->setText(e ? e->str(m_primaryKey) : QString());
    if (m_cmdline) {
        bool valid = true;
        const QStringList n = e ? rulematch::needles(*e, &valid) : QStringList();
        // an invalid value from the file is shown as it is; editing it writes a valid one
        m_cmdline->setPlainText(valid ? n.join(QLatin1Char('\n')) : jsonValueText(e->value(rulematch::kCmdlineKey)));
    }
    m_fileLabel->setFullText(e && e->file ? e->file->path : m_fileHint);
    if (m_type)
        fillTypeCombo(e ? e->str(QStringLiteral("type")) : QString());
    for (auto &f : m_fields)
        fillCombo(f.second, f.first, e ? e->str(f.first.key) : QString());
    m_loading = false;
    updateHints();
}

void EntryEditor::setFileHint(const QString &text)
{
    m_fileHint = text;
    if (!m_entry || !m_entry->file)
        m_fileLabel->setFullText(text);
}

void EntryEditor::setOverridesVisible(bool visible)
{
    if (!m_overridesToggle)
        return;
    if (m_overridesToggle->isChecked() != visible) {
        m_overridesToggle->setChecked(visible); // re-enters via toggled()
        return;
    }
    m_overridesToggle->setArrowType(visible ? Qt::DownArrow : Qt::RightArrow);
    m_fieldsBox->setVisible(visible);
    emit layoutChanged();
}

int EntryEditor::overrideCount() const
{
    int n = 0;
    if (m_entry)
        for (const auto &f : m_fields)
            if (m_entry->has(f.first.key))
                ++n;
    return n;
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
    updateHints();
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

void EntryEditor::setCmdlineSupport(bool supported, const QString &note)
{
    m_cmdlineSupported = supported;
    m_cmdlineUnsupportedNote = note;
    updateHints();
}

void EntryEditor::setCgroupNames(const QStringList &names)
{
    m_cgroupNames = names;
    refresh();
}

void EntryEditor::updateHints()
{
    if (m_mode == RuleMode && m_nameLimited && m_name->text().trimmed().size() > 15) {
        m_nameHint->setVisible(true);
    } else {
        m_nameHint->setVisible(false);
    }
    if (m_cmdlineNote) {
        bool valid = true;
        const QStringList n = m_entry ? rulematch::needles(*m_entry, &valid) : QStringList();
        const QString name = m_name->text().trimmed();
        QString note, tip;
        bool warn = false;
        if (!valid) {
            note = tr("⚠ The value in the file is not valid (ananicy-cpp ignores this rule). Edit it to fix it.");
            warn = true;
        } else if (!n.isEmpty() && !m_cmdlineSupported) {
            note = QStringLiteral("⚠ ") + m_cmdlineUnsupportedNote;
            warn = true;
        } else if (!n.isEmpty()) {
            const QString who = name.isEmpty() ? tr("Only processes") : tr("Only \"%1\" processes").arg(name);
            note = n.size() == 1 ? tr("%1 with \"%2\" in the command line.").arg(who, n.first())
                                 : tr("%1 with all %n texts in the command line.", nullptr, n.size()).arg(who);
            tip = note + QLatin1Char(' ')
                  + tr("Other %1 processes use the rule without this field, if there is one.")
                        .arg(name.isEmpty() ? QString() : QStringLiteral("\"%1\"").arg(name)).simplified();
        } else {
            note = tr("Empty: the rule applies to every process with this name.");
        }
        m_cmdlineNote->setText(note);
        m_cmdlineNote->setToolTip(tip.isEmpty() ? note : tip);
        m_cmdlineNote->setStyleSheet(warn ? QStringLiteral("color: #b06000;") : QString());
        m_cmdlineNote->setForegroundRole(warn ? QPalette::WindowText : QPalette::PlaceholderText);
    }
    if (m_typeInfo) {
        const QString t = m_entry ? m_entry->str(QStringLiteral("type")) : QString();
        QString info;
        if (!t.isEmpty() && m_describe)
            info = m_describe(t);
        m_typeInfo->setFullText(t.isEmpty() ? tr("No type: only the overrides apply") : info);
    }
    if (m_fileFn && m_entry)
        m_fileLabel->setFullText(m_fileFn(*m_entry));
    if (m_overridesToggle) {
        const int n = overrideCount();
        m_overridesToggle->setText(n == 0 ? tr("Overrides (optional)")
                                          : tr("Overrides (%n set)", nullptr, n));
    }
}
