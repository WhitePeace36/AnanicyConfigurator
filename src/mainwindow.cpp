#include "mainwindow.h"

#include "columnfitter.h"
#include "entrydialog.h"
#include "entryeditor.h"
#include "entrymodel.h"
#include "processdialog.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonValue>
#include <QRegularExpressionValidator>
#include <QSet>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include <unistd.h>

namespace {

const char *kNotSet = "(not set)";

enum RuleCol { RColName, RColType, RColOverrides, RColFile };

QString shellQuote(const QString &s)
{
    QString r = s;
    r.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + r + QLatin1Char('\'');
}

int runProcess(const QString &program, const QStringList &args, QString *output)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    if (!p.waitForStarted(10000)) {
        if (output)
            *output = p.errorString();
        return -1;
    }
    if (p.state() != QProcess::NotRunning) {
        QEventLoop loop; // keep the UI painting while e.g. the polkit dialog is open
        QObject::connect(&p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), &loop, &QEventLoop::quit);
        loop.exec();
    }
    if (output)
        *output = QString::fromLocal8Bit(p.readAll()).trimmed();
    return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
}

QTableView *makeTable()
{
    auto *v = new QTableView;
    v->setSelectionBehavior(QAbstractItemView::SelectRows);
    v->setSortingEnabled(true);
    v->setAlternatingRowColors(true);
    v->setWordWrap(false);
    v->verticalHeader()->hide();
    v->verticalHeader()->setDefaultSectionSize(v->fontMetrics().height() + 8);
    v->horizontalHeader()->setHighlightSections(false);
    v->setEditTriggers(QAbstractItemView::NoEditTriggers);
    return v;
}

} // namespace

// =====================================================================  construction

MainWindow::MainWindow(const QString &rootDir, QWidget *parent)
    : QMainWindow(parent), m_root(QDir(rootDir).absolutePath())
{
    m_newRulesFile = m_root + QStringLiteral("/00-default/custom.rules");
    m_newTypesFile = m_root + QStringLiteral("/00-types.types");

    resize(1400, 900);

    auto *tabs = new QTabWidget;

    auto *split = new QSplitter(Qt::Vertical);
    split->addWidget(buildTypesPane());
    split->addWidget(buildRulesPane());
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    split->setChildrenCollapsible(false);
    tabs->addTab(split, tr("Types && Rules"));
    tabs->addTab(buildConfTab(), tr("ananicy.conf"));

    auto *central = new QWidget;
    auto *lay = new QVBoxLayout(central);
    lay->addWidget(tabs, 1);
    lay->addWidget(buildBottomBar());
    setCentralWidget(central);

    m_warnLabel = new QLabel;
    m_warnLabel->setStyleSheet(QStringLiteral("color: #b06000;"));
    statusBar()->addPermanentWidget(m_warnLabel);

    loadAll();
}

QWidget *MainWindow::buildTypesPane()
{
    auto *box = new QGroupBox(tr("Types"));
    auto *h = new QHBoxLayout(box);

    // left: table
    auto *left = new QVBoxLayout;
    m_typeFilter = new QLineEdit;
    m_typeFilter->setPlaceholderText(tr("Filter types…"));
    m_typeFilter->setClearButtonEnabled(true);
    left->addWidget(m_typeFilter);

    m_typeModel = new EntryModel(&m_types, m_root, this);
    QVector<EntryModel::Column> cols;
    cols.append({tr("Type"), [](const Entry &e) { return e.str(QStringLiteral("type")); }, {}});
    for (const FieldSpec &s : typeFieldSpecs()) {
        const QString key = s.key;
        cols.append({s.label, [key](const Entry &e) { return e.str(key); }, {}});
    }
    cols.append({tr("Rules"), [this](const Entry &e) { return m_usage.value(e.str(QStringLiteral("type")), 0); },
                 [](const Entry &) { return tr("Number of rules using this type (double-click to show them)"); }});
    cols.append({tr("File"), [this](const Entry &e) { return e.file ? m_typeModel->relPath(e.file->path) : QString(); },
                 [](const Entry &e) { return e.file ? e.file->path : QString(); }});
    m_typeModel->setColumns(cols);
    m_typeModel->setWarnFn([this](const Entry &e) {
        const QString n = e.str(QStringLiteral("type"));
        if (n.isEmpty())
            return tr("Type name is empty");
        int c = 0;
        for (const auto &o : m_types.entries())
            if (o->str(QStringLiteral("type")) == n)
                ++c;
        return c > 1 ? tr("Type \"%1\" is defined %2 times; the last one wins").arg(n).arg(c) : QString();
    });

    m_typeProxy = new EntryFilterProxy(this);
    m_typeProxy->setSourceModel(m_typeModel);
    m_typeView = makeTable();
    m_typeView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_typeView->setModel(m_typeProxy);
    new ColumnFitter(m_typeView);
    m_typeView->sortByColumn(0, Qt::AscendingOrder);
    left->addWidget(m_typeView, 1);

    auto *btns = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add type…"));
    auto *edit = new QPushButton(tr("Edit…"));
    auto *dup = new QPushButton(tr("Duplicate…"));
    auto *del = new QPushButton(tr("Delete"));
    auto *show = new QPushButton(tr("Show its rules"));
    show->setToolTip(tr("Filter the rules list below by the selected type"));
    btns->addWidget(add);
    btns->addWidget(edit);
    btns->addWidget(dup);
    btns->addWidget(del);
    btns->addWidget(show);
    btns->addStretch(1);
    btns->addWidget(new QLabel(tr("Double-click a type to edit it")));
    left->addLayout(btns);
    h->addLayout(left, 1);

    connect(m_typeFilter, &QLineEdit::textChanged, m_typeProxy, &EntryFilterProxy::setText);
    connect(add, &QPushButton::clicked, this, &MainWindow::addType);
    connect(edit, &QPushButton::clicked, this, &MainWindow::editType);
    connect(dup, &QPushButton::clicked, this, &MainWindow::duplicateType);
    connect(del, &QPushButton::clicked, this, &MainWindow::deleteType);
    connect(show, &QPushButton::clicked, this, &MainWindow::showRulesOfType);
    connect(m_typeView, &QTableView::activated, this, &MainWindow::editType); // double-click / Enter
    auto *sc = new QShortcut(QKeySequence::Delete, m_typeView);
    sc->setContext(Qt::WidgetShortcut);
    connect(sc, &QShortcut::activated, this, &MainWindow::deleteType);
    return box;
}

QWidget *MainWindow::buildRulesPane()
{
    auto *box = new QGroupBox(tr("Rules (process → type)"));
    auto *h = new QHBoxLayout(box);

    auto *left = new QVBoxLayout;
    auto *filterRow = new QHBoxLayout;
    m_ruleFilter = new QLineEdit;
    m_ruleFilter->setPlaceholderText(tr("Filter rules by name, type or file…"));
    m_ruleFilter->setClearButtonEnabled(true);
    m_ruleTypeFilter = new QComboBox;
    m_ruleTypeFilter->setMinimumContentsLength(14);
    m_ruleCount = new QLabel;
    filterRow->addWidget(m_ruleFilter, 1);
    filterRow->addWidget(new QLabel(tr("Type:")));
    filterRow->addWidget(m_ruleTypeFilter);
    filterRow->addWidget(m_ruleCount);
    m_showAllRules = new QPushButton(tr("Show all rules"));
    m_showAllRules->setToolTip(tr("Clear the text and type filters"));
    filterRow->addWidget(m_showAllRules);
    left->addLayout(filterRow);

    m_ruleModel = new EntryModel(&m_rules, m_root, this);
    m_ruleModel->setColumns({
        {tr("Process name"), [](const Entry &e) { return e.str(QStringLiteral("name")); }, {}},
        {tr("Type"), [](const Entry &e) { return e.str(QStringLiteral("type")); }, {}},
        {tr("Overrides"),
         [](const Entry &e) {
             QStringList parts;
             for (const auto &f : e.fields)
                 if (f.first != QLatin1String("name") && f.first != QLatin1String("type"))
                     parts << f.first + QLatin1Char('=') + jsonValueText(f.second);
             return parts.join(QStringLiteral(", "));
         },
         {}},
        {tr("File"), [this](const Entry &e) { return e.file ? m_ruleModel->relPath(e.file->path) : QString(); },
         [](const Entry &e) { return e.file ? e.file->path : QString(); }},
    });
    m_ruleModel->setWarnFn([this](const Entry &e) {
        if (e.str(QStringLiteral("name")).isEmpty())
            return tr("Process name is empty");
        const QString t = e.str(QStringLiteral("type"));
        if (!t.isEmpty() && !m_types.find(t))
            return tr("Type \"%1\" is not defined").arg(t);
        return QString();
    });

    m_ruleProxy = new EntryFilterProxy(this);
    m_ruleProxy->setSourceModel(m_ruleModel);
    m_ruleView = makeTable();
    m_ruleView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_ruleView->setModel(m_ruleProxy);
    new ColumnFitter(m_ruleView);
    m_ruleView->sortByColumn(RColName, Qt::AscendingOrder);
    left->addWidget(m_ruleView, 1);

    auto *btns = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add rule…"));
    auto *addProc = new QPushButton(tr("Add from running process…"));
    auto *edit = new QPushButton(tr("Edit…"));
    auto *del = new QPushButton(tr("Delete"));
    btns->addWidget(add);
    btns->addWidget(addProc);
    btns->addWidget(edit);
    btns->addWidget(del);
    btns->addStretch(1);
    btns->addWidget(new QLabel(tr("Double-click a rule to edit it · new rules go to %1")
                                   .arg(QDir(m_root).relativeFilePath(m_newRulesFile))));
    left->addLayout(btns);
    h->addLayout(left, 1);

    connect(m_ruleFilter, &QLineEdit::textChanged, this, [this](const QString &t) {
        m_ruleProxy->setText(t);
        updateRuleCount();
    });
    connect(m_ruleTypeFilter, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::applyRuleTypeFilter);
    connect(add, &QPushButton::clicked, this, &MainWindow::addRule);
    connect(addProc, &QPushButton::clicked, this, &MainWindow::addRuleFromProcess);
    connect(edit, &QPushButton::clicked, this, &MainWindow::editRule);
    connect(m_showAllRules, &QPushButton::clicked, this, [this] {
        m_ruleFilter->clear();
        m_ruleTypeFilter->setCurrentIndex(0);
    });
    connect(del, &QPushButton::clicked, this, &MainWindow::deleteRules);
    connect(m_ruleView, &QTableView::activated, this, &MainWindow::editRule); // double-click / Enter
    auto *sc = new QShortcut(QKeySequence::Delete, m_ruleView);
    sc->setContext(Qt::WidgetShortcut);
    connect(sc, &QShortcut::activated, this, &MainWindow::deleteRules);
    return box;
}

QWidget *MainWindow::buildConfTab()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);

    m_confPath = new QLabel;
    v->addWidget(m_confPath);

    auto *h = new QHBoxLayout;
    auto *known = new QGroupBox(tr("Settings"));
    auto *form = new QFormLayout(known);
    for (const ConfSpec &s : confSpecs()) {
        auto *c = new QComboBox;
        c->setToolTip(s.tip);
        c->setMinimumContentsLength(12);
        c->setEditable(s.numeric);
        if (s.numeric) {
            c->setInsertPolicy(QComboBox::NoInsert);
            c->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("\\d{0,5}(\\.\\d{0,3})?")), c));
            c->lineEdit()->setPlaceholderText(QString::fromLatin1(kNotSet));
        }
        auto *lbl = new QLabel(s.key + QLatin1Char(':'));
        lbl->setToolTip(s.tip);
        auto *row = new QHBoxLayout;
        row->addWidget(c);
        auto *desc = new QLabel(s.tip);
        desc->setStyleSheet(QStringLiteral("color: palette(mid);"));
        row->addWidget(desc, 1);
        form->addRow(lbl, row);
        m_confCombos.append({s, c});
        connect(c, &QComboBox::currentTextChanged, this, [this, s, c] {
            if (m_confLoading)
                return;
            QString t = c->currentText().trimmed();
            if ((!s.numeric && c->currentIndex() == 0) || t.isEmpty()) {
                m_conf.unset(s.key);
            } else {
                if (s.numeric && t.toDouble() <= 0)
                    return;
                m_conf.set(s.key, t);
            }
            updateDirtyState();
        });
    }
    auto *scroll = new QScrollArea;
    scroll->setWidget(known);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    h->addWidget(scroll, 3);

    auto *otherBox = new QGroupBox(tr("Other keys in the file"));
    auto *ov = new QVBoxLayout(otherBox);
    m_confOther = new QTableWidget(0, 2);
    m_confOther->setHorizontalHeaderLabels({tr("Key"), tr("Value")});
    m_confOther->horizontalHeader()->setStretchLastSection(true);
    m_confOther->verticalHeader()->hide();
    m_confOther->setSelectionBehavior(QAbstractItemView::SelectRows);
    ov->addWidget(m_confOther, 1);
    auto *ob = new QHBoxLayout;
    auto *addKey = new QPushButton(tr("Add key…"));
    auto *rmKey = new QPushButton(tr("Remove key"));
    ob->addWidget(addKey);
    ob->addWidget(rmKey);
    ob->addStretch(1);
    ov->addLayout(ob);
    h->addWidget(otherBox, 2);
    v->addLayout(h, 1);

    connect(m_confOther, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *it) {
        if (m_confLoading || it->column() != 1)
            return;
        const QString key = m_confOther->item(it->row(), 0)->text();
        m_conf.set(key, it->text().trimmed());
        updateDirtyState();
    });
    connect(addKey, &QPushButton::clicked, this, &MainWindow::addConfKey);
    connect(rmKey, &QPushButton::clicked, this, &MainWindow::removeConfKey);
    return page;
}

QWidget *MainWindow::buildBottomBar()
{
    auto *w = new QWidget;
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);

    auto *rootLbl = new QLabel(tr("Config directory: <b>%1</b>").arg(m_root.toHtmlEscaped()));
    h->addWidget(rootLbl);
    h->addStretch(1);

    h->addWidget(new QLabel(tr("Service:")));
    m_service = new QComboBox;
    m_service->setEditable(true);
    m_service->addItems({QStringLiteral("ananicy"), QStringLiteral("ananicy-cpp")});
    QSettings st;
    const QString svc = st.value(QStringLiteral("service")).toString();
    m_service->setCurrentText(svc.isEmpty() ? detectService() : svc);
    m_service->setToolTip(tr("systemd unit that is restarted after saving"));
    h->addWidget(m_service);

    auto *reload = new QPushButton(tr("Reload from disk"));
    auto *save = new QPushButton(tr("Save"));
    auto *saveRestart = new QPushButton(tr("Save && Restart service"));
    saveRestart->setDefault(true);
    QFont f = saveRestart->font();
    f.setBold(true);
    saveRestart->setFont(f);
    save->setShortcut(QKeySequence::Save);
    saveRestart->setToolTip(tr("Write all changes and run: systemctl restart <service>"));
    h->addWidget(reload);
    h->addWidget(save);
    h->addWidget(saveRestart);

    connect(reload, &QPushButton::clicked, this, &MainWindow::reloadFromDisk);
    connect(save, &QPushButton::clicked, this, [this] { saveAll(false); });
    connect(saveRestart, &QPushButton::clicked, this, [this] { saveAll(true); });
    connect(m_service, &QComboBox::currentTextChanged, this, [](const QString &t) {
        QSettings().setValue(QStringLiteral("service"), t.trimmed());
    });
    return w;
}

// =====================================================================  loading

void MainWindow::loadAll()
{
    m_types.clear();
    m_rules.clear();
    m_cgroups.clear();

    m_types.loadTree(m_root, QStringLiteral("*.types"), true);
    m_rules.loadTree(m_root, QStringLiteral("*.rules"), true);
    m_cgroups.loadTree(m_root, QStringLiteral("*.cgroups"), true);
    m_conf.load(m_root + QStringLiteral("/ananicy.conf"));

    recomputeUsage();
    m_typeModel->reset();
    m_ruleModel->reset();

    typeNamesChanged();
    refreshConfTab();

    QStringList warnings = m_types.warnings + m_rules.warnings + m_cgroups.warnings;
    if (!QFileInfo(m_root).isDir())
        warnings.prepend(tr("%1 does not exist").arg(m_root));
    if (warnings.isEmpty()) {
        m_warnLabel->clear();
        m_warnLabel->setToolTip(QString());
    } else {
        m_warnLabel->setText(tr("⚠ %n warning(s) while loading", nullptr, warnings.size()));
        m_warnLabel->setToolTip(warnings.mid(0, 60).join(QLatin1Char('\n'))
                                + (warnings.size() > 60 ? QStringLiteral("\n…") : QString()));
    }

    QSet<ConfFile *> files;
    for (const auto &e : m_rules.entries())
        files.insert(e->file);
    for (const auto &e : m_types.entries())
        files.insert(e->file);
    statusBar()->showMessage(tr("Loaded %1 types and %2 rules from %3 files")
                                 .arg(m_types.entries().size())
                                 .arg(m_rules.entries().size())
                                 .arg(files.size()),
                             8000);
    if (m_typeProxy->rowCount() > 0)
        m_typeView->setCurrentIndex(m_typeProxy->index(0, 0));
    if (m_ruleProxy->rowCount() > 0)
        m_ruleView->setCurrentIndex(m_ruleProxy->index(0, 0));
    updateDirtyState();
}

void MainWindow::reloadFromDisk()
{
    if (isDirty()
        && QMessageBox::question(this, tr("Reload"), tr("Discard all unsaved changes and reload from disk?"))
               != QMessageBox::Yes)
        return;
    loadAll();
}

bool MainWindow::isDirty() const
{
    return m_types.isDirty() || m_rules.isDirty() || m_conf.dirty;
}

void MainWindow::updateDirtyState()
{
    setWindowTitle(tr("Ananicy Configurator — %1%2").arg(m_root, isDirty() ? QStringLiteral(" *") : QString()));
    updateRuleCount();
}

void MainWindow::recomputeUsage()
{
    m_usage.clear();
    for (const auto &r : m_rules.entries()) {
        const QString t = r->str(QStringLiteral("type"));
        if (!t.isEmpty())
            m_usage[t]++;
    }
}

void MainWindow::typeNamesChanged()
{
    const QStringList names = m_types.names();
    refreshRuleTypeFilter();
}

QString MainWindow::describeType(const QString &name) const
{
    EntryPtr t = m_types.find(name);
    if (!t)
        return tr("⚠ type \"%1\" is not defined").arg(name);
    QStringList parts;
    for (const auto &f : t->fields)
        if (f.first != QLatin1String("type"))
            parts << f.first + QLatin1Char('=') + jsonValueText(f.second);
    return tr("Type settings: %1").arg(parts.isEmpty() ? tr("(none)") : parts.join(QStringLiteral(", ")));
}

QString MainWindow::detectService() const
{
    QString out;
    runProcess(QStringLiteral("systemctl"),
               {QStringLiteral("list-unit-files"), QStringLiteral("--no-legend"), QStringLiteral("--no-pager"),
                QStringLiteral("ananicy-cpp.service"), QStringLiteral("ananicy.service")},
               &out);
    QString first;
    for (const QString &line : out.split(QLatin1Char('\n'))) {
        const QStringList cols = line.simplified().split(QLatin1Char(' '));
        if (cols.isEmpty() || !cols[0].endsWith(QLatin1String(".service")))
            continue;
        const QString unit = cols[0].chopped(8);
        if (cols.size() > 1 && cols[1] == QLatin1String("enabled"))
            return unit;
        if (first.isEmpty())
            first = unit;
    }
    return first.isEmpty() ? QStringLiteral("ananicy") : first;
}

// =====================================================================  saving

bool MainWindow::saveAll(bool restart)
{
    // validation
    QStringList errors;
    QSet<QString> seen;
    for (const auto &t : m_types.entries()) {
        const QString n = t->str(QStringLiteral("type"));
        if (n.isEmpty())
            errors << tr("A type has an empty name (%1)").arg(t->file ? t->file->path : QString());
        else if (seen.contains(n) && t->modified)
            errors << tr("Type \"%1\" is defined more than once").arg(n);
        seen.insert(n);
    }
    QStringList missing;
    for (const auto &r : m_rules.entries()) {
        if (r->str(QStringLiteral("name")).isEmpty())
            errors << tr("A rule has an empty process name (%1)").arg(r->file ? r->file->path : QString());
        const QString t = r->str(QStringLiteral("type"));
        if (r->modified && !t.isEmpty() && !m_types.find(t))
            missing << QStringLiteral("%1 → %2").arg(r->str(QStringLiteral("name")), t);
    }
    if (!errors.isEmpty()) {
        QMessageBox::warning(this, tr("Cannot save"), errors.mid(0, 20).join(QLatin1Char('\n')));
        return false;
    }
    if (!missing.isEmpty()
        && QMessageBox::question(this, tr("Undefined types"),
                                 tr("These changed rules reference types that do not exist:\n\n%1\n\nSave anyway?")
                                     .arg(missing.mid(0, 20).join(QLatin1Char('\n'))))
               != QMessageBox::Yes)
        return false;

    QMap<QString, QByteArray> writes = m_types.pendingWrites();
    const auto rw = m_rules.pendingWrites();
    for (auto it = rw.begin(); it != rw.end(); ++it)
        writes.insert(it.key(), it.value());
    if (m_conf.dirty)
        writes.insert(m_conf.path, m_conf.render());

    if (writes.isEmpty() && !restart) {
        statusBar()->showMessage(tr("Nothing to save"), 5000);
        return true;
    }

    QString log;
    setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = writeFiles(writes, restart, &log);
    QApplication::restoreOverrideCursor();
    setEnabled(true);

    if (!ok) {
        QMessageBox::critical(this, tr("Save failed"), log.isEmpty() ? tr("Unknown error") : log);
        return false;
    }

    m_types.markSaved();
    m_rules.markSaved();
    if (m_conf.dirty) {
        m_conf.dirty = false;
        m_conf.existed = true;
    }
    m_typeModel->allChanged();
    m_ruleModel->allChanged();
    refreshConfTab();
    updateDirtyState();

    QString msg = writes.isEmpty() ? tr("No changes to write.")
                                   : tr("Saved %n file(s).", nullptr, writes.size());
    if (restart) {
        QString state;
        runProcess(QStringLiteral("systemctl"),
                   {QStringLiteral("is-active"), m_service->currentText().trimmed()}, &state);
        msg += QLatin1Char(' ') + tr("Service %1 restarted (state: %2).").arg(m_service->currentText().trimmed(), state);
        if (state != QLatin1String("active"))
            QMessageBox::warning(this, tr("Service"),
                                 tr("The service %1 is \"%2\" after the restart.\nCheck: journalctl -u %1")
                                     .arg(m_service->currentText().trimmed(), state));
    }
    statusBar()->showMessage(msg, 10000);
    return true;
}

bool MainWindow::writeFiles(const QMap<QString, QByteArray> &writes, bool restart, QString *log)
{
    const QString svc = m_service->currentText().trimmed();
    const bool isRoot = ::geteuid() == 0;

    bool direct = isRoot;
    if (!direct && !writes.isEmpty()) {
        direct = true;
        for (auto it = writes.begin(); it != writes.end(); ++it) {
            QFileInfo fi(it.key());
            const bool writable = fi.exists() ? fi.isWritable() : QFileInfo(fi.absolutePath()).isWritable();
            if (!writable) {
                direct = false;
                break;
            }
        }
    }

    if (direct) {
        for (auto it = writes.begin(); it != writes.end(); ++it) {
            const QString path = it.key();
            QDir().mkpath(QFileInfo(path).absolutePath());
            if (QFile::exists(path)) {
                QFile::remove(path + QStringLiteral(".bak"));
                QFile::copy(path, path + QStringLiteral(".bak"));
            }
            QSaveFile f(path);
            if (!f.open(QIODevice::WriteOnly) || f.write(it.value()) != it.value().size() || !f.commit()) {
                *log = tr("Writing %1 failed: %2").arg(path, f.errorString());
                return false;
            }
        }
        if (restart) {
            QStringList args{QStringLiteral("restart"), svc};
            const int rc = isRoot ? runProcess(QStringLiteral("systemctl"), args, log)
                                  : runProcess(QStringLiteral("pkexec"),
                                               QStringList{QStringLiteral("systemctl")} + args, log);
            if (rc != 0) {
                *log = tr("Files were saved, but restarting %1 failed (exit code %2):\n%3").arg(svc).arg(rc).arg(*log);
                return false;
            }
        }
        return true;
    }

    // Not root: stage files in a temp dir and install them with one pkexec call (one password prompt).
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        *log = tr("Cannot create temporary directory");
        return false;
    }
    QString script = QStringLiteral("set -e\n");
    int n = 0;
    for (auto it = writes.begin(); it != writes.end(); ++it) {
        const QString staged = tmp.filePath(QString::number(n++));
        QFile f(staged);
        if (!f.open(QIODevice::WriteOnly) || f.write(it.value()) != it.value().size()) {
            *log = tr("Cannot write temporary file %1").arg(staged);
            return false;
        }
        f.close();
        const QString dest = shellQuote(it.key());
        const QString bak = shellQuote(it.key() + QStringLiteral(".bak"));
        script += QStringLiteral("if [ -f %1 ]; then cp -f %1 %2; fi\n").arg(dest, bak);
        script += QStringLiteral("install -D -m 0644 %1 %2\n").arg(shellQuote(staged), dest);
    }
    if (restart)
        script += QStringLiteral("systemctl restart %1\n").arg(shellQuote(svc));

    const int rc = runProcess(QStringLiteral("pkexec"), {QStringLiteral("/bin/sh"), QStringLiteral("-c"), script}, log);
    if (rc == 126 || rc == 127) {
        *log = tr("Authorization was cancelled or denied (pkexec exit code %1).\n%2").arg(rc).arg(*log);
        return false;
    }
    if (rc != 0) {
        *log = tr("Saving failed (exit code %1):\n%2").arg(rc).arg(*log);
        return false;
    }
    return true;
}

void MainWindow::closeEvent(QCloseEvent *ev)
{
    if (!isDirty()) {
        ev->accept();
        return;
    }
    const auto r = QMessageBox::question(this, tr("Unsaved changes"),
                                         tr("There are unsaved changes. Save them and restart the service?"),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel || (r == QMessageBox::Save && !saveAll(true)))
        ev->ignore();
    else
        ev->accept();
}

// =====================================================================  types

EntryPtr MainWindow::currentType() const
{
    const QModelIndex idx = m_typeView->currentIndex();
    if (!idx.isValid())
        return nullptr;
    return m_typeModel->entryAt(m_typeProxy->mapToSource(idx).row());
}

void MainWindow::selectType(const EntryPtr &e)
{
    const int row = m_typeModel->rowOf(e);
    if (row < 0)
        return;
    const QModelIndex idx = m_typeProxy->mapFromSource(m_typeModel->index(row, 0));
    if (!idx.isValid())
        return;
    m_typeView->setCurrentIndex(idx);
    m_typeView->scrollTo(idx);
}

void MainWindow::addType()
{
    openTypeDialog(nullptr, nullptr);
}

void MainWindow::editType()
{
    if (EntryPtr t = currentType())
        openTypeDialog(t, nullptr);
}

void MainWindow::duplicateType()
{
    if (EntryPtr t = currentType())
        openTypeDialog(nullptr, t);
}

void MainWindow::showRulesOfType()
{
    if (EntryPtr t = currentType()) {
        int i = m_ruleTypeFilter->findText(t->str(QStringLiteral("type")));
        if (i >= 0)
            m_ruleTypeFilter->setCurrentIndex(i);
    }
}

void MainWindow::openTypeDialog(const EntryPtr &original, const EntryPtr &templ)
{
    // work on a copy; nothing changes unless the user presses OK
    auto work = std::make_shared<Entry>(original ? *original : templ ? *templ : Entry());
    if (!original) {
        work->file = nullptr;
        work->originalLine.clear();
        work->trailing.clear();
        work->modified = false;
        work->set(QStringLiteral("type"), templ ? QJsonValue(templ->str(QStringLiteral("type")) + QStringLiteral("-copy"))
                                                : QJsonValue(QJsonValue::Undefined));
    }

    EntryDialog dlg(EntryEditor::TypeMode,
                    original ? tr("Edit type \"%1\"").arg(original->str(QStringLiteral("type")))
                             : templ ? tr("Duplicate type \"%1\"").arg(templ->str(QStringLiteral("type")))
                                     : tr("New type"),
                    work, this);
    dlg.editor()->setFileHint(tr("%1 (new)").arg(m_newTypesFile));
    dlg.editor()->setCgroupNames(m_cgroups.names());
    dlg.setValidator([this, original](const Entry &e, QWidget *parent) {
        const QString n = e.str(QStringLiteral("type"));
        if (n.isEmpty()) {
            QMessageBox::warning(parent, tr("Type"), tr("The type name cannot be empty."));
            return false;
        }
        EntryPtr other = m_types.find(n);
        if (other && other != original) {
            QMessageBox::warning(parent, tr("Type"), tr("Type \"%1\" already exists.").arg(n));
            return false;
        }
        return true;
    });
    dlg.editor()->focusPrimary();
    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString newName = work->str(QStringLiteral("type"));
    EntryPtr target = original;
    if (!original) {
        target = m_types.add(m_newTypesFile, newName);
        target->fields = work->fields;
        m_typeFilter->clear();
        m_typeModel->reset();
    } else {
        if (original->fields == work->fields)
            return; // nothing changed
        const QString oldName = original->str(QStringLiteral("type"));
        original->fields = work->fields;
        m_types.markModified(original);
        m_typeModel->entryChanged(original);

        if (oldName != newName && !m_types.find(oldName)) {
            QVector<EntryPtr> refs;
            for (const auto &r : m_rules.entries())
                if (r->str(QStringLiteral("type")) == oldName)
                    refs << r;
            if (!refs.isEmpty()
                && QMessageBox::question(this, tr("Rename type"),
                                         tr("%n rule(s) use \"%1\". Change them to \"%2\" as well?", nullptr,
                                            refs.size()).arg(oldName, newName))
                       == QMessageBox::Yes) {
                for (const auto &r : refs) {
                    r->set(QStringLiteral("type"), newName);
                    m_rules.markModified(r);
                }
            }
        }
    }
    recomputeUsage();
    typeNamesChanged();
    m_typeModel->allChanged();
    m_ruleModel->allChanged();
    selectType(target);
    updateDirtyState();
}

void MainWindow::deleteType()
{
    EntryPtr e = currentType();
    if (!e)
        return;
    const QString name = e->str(QStringLiteral("type"));
    const int used = m_usage.value(name, 0);
    QString q = tr("Delete type \"%1\" from %2?").arg(name, e->file ? e->file->path : QString());
    if (used > 0)
        q += QLatin1String("\n\n") + tr("%n rule(s) use this type and will reference an undefined type.", nullptr, used);
    if (QMessageBox::question(this, tr("Delete type"), q) != QMessageBox::Yes)
        return;
    const int row = m_typeView->currentIndex().row();
    m_types.remove(e);
    m_typeModel->reset();
    typeNamesChanged();
    m_ruleModel->allChanged();
    if (m_typeProxy->rowCount() > 0)
        m_typeView->setCurrentIndex(m_typeProxy->index(qMin(row, m_typeProxy->rowCount() - 1), 0));
    updateDirtyState();
}

// =====================================================================  rules

EntryPtr MainWindow::currentRule() const
{
    const QModelIndex idx = m_ruleView->currentIndex();
    if (!idx.isValid())
        return nullptr;
    return m_ruleModel->entryAt(m_ruleProxy->mapToSource(idx).row());
}

void MainWindow::selectRule(const EntryPtr &e)
{
    int row = m_ruleModel->rowOf(e);
    if (row < 0)
        return;
    QModelIndex idx = m_ruleProxy->mapFromSource(m_ruleModel->index(row, 0));
    if (!idx.isValid()) { // hidden by filter
        m_ruleFilter->clear();
        m_ruleTypeFilter->setCurrentIndex(0);
        idx = m_ruleProxy->mapFromSource(m_ruleModel->index(row, 0));
    }
    m_ruleView->clearSelection();
    m_ruleView->setCurrentIndex(idx);
    m_ruleView->scrollTo(idx, QAbstractItemView::PositionAtCenter);
}

void MainWindow::addRule()
{
    openRuleDialog(nullptr, QString());
}

void MainWindow::editRule()
{
    if (EntryPtr r = currentRule())
        openRuleDialog(r, QString());
}

void MainWindow::addRuleFromProcess()
{
    ProcessDialog dlg([this](const QString &name) -> QString {
        EntryPtr r = m_rules.find(name);
        if (!r)
            return {};
        const QString t = r->str(QStringLiteral("type"));
        return t.isEmpty() ? tr("(rule without type)") : t;
    }, this);
    if (dlg.exec() == QDialog::Accepted && !dlg.selectedName().isEmpty())
        createRule(dlg.selectedName());
}

void MainWindow::createRule(const QString &name)
{
    if (EntryPtr ex = m_rules.find(name)) {
        const auto r = QMessageBox::question(
            this, tr("Rule exists"),
            tr("A rule for \"%1\" already exists in %2.\n\nEdit the existing rule instead?")
                .arg(name, ex->file ? ex->file->path : QString()),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        if (r == QMessageBox::Cancel)
            return;
        if (r == QMessageBox::Yes) {
            selectRule(ex);
            openRuleDialog(ex, QString());
            return;
        }
    }
    openRuleDialog(nullptr, name);
}

void MainWindow::openRuleDialog(const EntryPtr &original, const QString &newName)
{
    auto work = std::make_shared<Entry>(original ? *original : Entry());
    if (!original && !newName.isEmpty())
        work->set(QStringLiteral("name"), newName);

    EntryDialog dlg(EntryEditor::RuleMode,
                    original ? tr("Edit rule \"%1\"").arg(original->str(QStringLiteral("name"))) : tr("New rule"),
                    work, this);
    EntryEditor *ed = dlg.editor();
    ed->setFileHint(tr("%1 (new)").arg(m_newRulesFile));
    ed->setTypeDescriber([this](const QString &t) { return describeType(t); });
    ed->setCgroupNames(m_cgroups.names());
    ed->setTypeNames(m_types.names());
    dlg.setValidator([this, original](const Entry &e, QWidget *parent) {
        const QString n = e.str(QStringLiteral("name"));
        if (n.isEmpty()) {
            QMessageBox::warning(parent, tr("Rule"), tr("The process name cannot be empty."));
            return false;
        }
        const bool renamed = !original || original->str(QStringLiteral("name")) != n;
        EntryPtr other = m_rules.find(n);
        if (renamed && other && other != original)
            return QMessageBox::question(parent, tr("Rule"),
                                         tr("Another rule for \"%1\" already exists in %2.\n"
                                            "Ananicy will use whichever is loaded last. Continue?")
                                             .arg(n, other->file ? other->file->path : QString()))
                == QMessageBox::Yes;
        return true;
    });
    if (newName.isEmpty() && !original)
        ed->focusPrimary();
    else
        ed->focusType();
    if (dlg.exec() != QDialog::Accepted)
        return;

    EntryPtr target = original;
    if (!original) {
        target = m_rules.add(m_newRulesFile, work->str(QStringLiteral("name")));
        target->fields = work->fields;
        m_ruleModel->reset();
    } else {
        if (original->fields == work->fields)
            return; // nothing changed
        original->fields = work->fields;
        m_rules.markModified(original);
        m_ruleModel->entryChanged(original);
    }
    recomputeUsage();
    m_typeModel->allChanged();
    refreshRuleTypeFilter();
    selectRule(target);
    updateDirtyState();
}

void MainWindow::deleteRules()
{
    QVector<EntryPtr> sel;
    for (const QModelIndex &idx : m_ruleView->selectionModel()->selectedRows())
        if (EntryPtr e = m_ruleModel->entryAt(m_ruleProxy->mapToSource(idx).row()))
            sel << e;
    if (sel.isEmpty())
        if (EntryPtr e = currentRule())
            sel << e;
    if (sel.isEmpty())
        return;
    const QString q = sel.size() == 1
        ? tr("Delete the rule for \"%1\" from %2?").arg(sel[0]->str(QStringLiteral("name")),
                                                      sel[0]->file ? sel[0]->file->path : QString())
        : tr("Delete %1 selected rules?").arg(sel.size());
    if (QMessageBox::question(this, tr("Delete rules"), q) != QMessageBox::Yes)
        return;
    const int row = m_ruleView->currentIndex().row();
    for (const auto &e : sel)
        m_rules.remove(e);
    m_ruleModel->reset();
    recomputeUsage();
    m_typeModel->allChanged();
    if (m_ruleProxy->rowCount() > 0)
        m_ruleView->setCurrentIndex(m_ruleProxy->index(qMin(row, m_ruleProxy->rowCount() - 1), 0));
    updateDirtyState();
}

void MainWindow::refreshRuleTypeFilter()
{
    const QString cur = m_ruleTypeFilter->currentIndex() > 0 ? m_ruleTypeFilter->currentText() : QString();
    const bool noType = m_ruleTypeFilter->currentIndex() == 1;
    m_ruleTypeFilter->blockSignals(true);
    m_ruleTypeFilter->clear();
    m_ruleTypeFilter->addItem(tr("(all)"));
    m_ruleTypeFilter->addItem(tr("(no type)"));
    QStringList names = m_types.names();
    for (const auto &k : m_usage.keys())
        if (!names.contains(k))
            names << k; // undefined but referenced
    names.sort(Qt::CaseInsensitive);
    m_ruleTypeFilter->addItems(names);
    int idx = noType ? 1 : (cur.isEmpty() ? 0 : m_ruleTypeFilter->findText(cur));
    m_ruleTypeFilter->setCurrentIndex(qMax(0, idx));
    m_ruleTypeFilter->blockSignals(false);
    applyRuleTypeFilter();
}

void MainWindow::applyRuleTypeFilter()
{
    const int i = m_ruleTypeFilter->currentIndex();
    if (i <= 0)
        m_ruleProxy->setExact(-1, QString());
    else if (i == 1)
        m_ruleProxy->setExact(RColType, QString());
    else
        m_ruleProxy->setExact(RColType, m_ruleTypeFilter->currentText());
    updateRuleCount();
}

// =====================================================================  ananicy.conf

void MainWindow::refreshConfTab()
{
    m_confLoading = true;
    m_confPath->setText(m_conf.existed
                            ? tr("File: <b>%1</b>").arg(m_conf.path.toHtmlEscaped())
                            : tr("File: <b>%1</b> (does not exist yet, will be created on save)")
                                  .arg(m_conf.path.toHtmlEscaped()));
    QStringList knownKeys;
    for (auto &p : m_confCombos) {
        const ConfSpec &s = p.first;
        QComboBox *c = p.second;
        knownKeys << s.key;
        c->clear();
        c->addItem(s.numeric ? QString() : QString::fromLatin1(kNotSet));
        c->addItems(s.options.isEmpty() ? QStringList{QStringLiteral("true"), QStringLiteral("false")} : s.options);
        if (!m_conf.has(s.key)) {
            c->setCurrentIndex(0);
            if (s.numeric)
                c->setEditText(QString());
            continue;
        }
        const QString v = m_conf.get(s.key);
        int i = c->findText(v);
        if (i < 0) {
            if (s.numeric) {
                c->setEditText(v);
                continue;
            }
            c->addItem(v);
            i = c->count() - 1;
        }
        c->setCurrentIndex(i);
    }

    m_confOther->setRowCount(0);
    for (const QString &k : m_conf.keys()) {
        if (knownKeys.contains(k))
            continue;
        const int r = m_confOther->rowCount();
        m_confOther->insertRow(r);
        auto *ki = new QTableWidgetItem(k);
        ki->setFlags(ki->flags() & ~Qt::ItemIsEditable);
        m_confOther->setItem(r, 0, ki);
        m_confOther->setItem(r, 1, new QTableWidgetItem(m_conf.get(k)));
    }
    m_confOther->resizeColumnToContents(0);
    m_confLoading = false;
}

void MainWindow::addConfKey()
{
    bool ok = false;
    const QString key = QInputDialog::getText(this, tr("Add key"), tr("Key:"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || key.isEmpty())
        return;
    if (key.contains(QLatin1Char('=')) || key.contains(QLatin1Char(' '))) {
        QMessageBox::warning(this, tr("Add key"), tr("Invalid key."));
        return;
    }
    const QString val = QInputDialog::getText(this, tr("Add key"), tr("Value for %1:").arg(key),
                                              QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok)
        return;
    m_conf.set(key, val);
    refreshConfTab();
    updateDirtyState();
}

void MainWindow::removeConfKey()
{
    const int r = m_confOther->currentRow();
    if (r < 0)
        return;
    m_conf.unset(m_confOther->item(r, 0)->text());
    refreshConfTab();
    updateDirtyState();
}

void MainWindow::updateRuleCount()
{
    if (!m_ruleCount || !m_showAllRules)
        return;
    const bool filtered = !m_ruleFilter->text().trimmed().isEmpty() || m_ruleTypeFilter->currentIndex() > 0;
    m_ruleCount->setText(filtered ? tr("showing %1 of %2").arg(m_ruleProxy->rowCount()).arg(m_rules.entries().size())
                                  : tr("%1 rules").arg(m_rules.entries().size()));
    m_showAllRules->setEnabled(filtered);
}
