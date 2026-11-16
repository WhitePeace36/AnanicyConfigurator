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
#include <QStandardPaths>
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

enum RuleCol { RColName, RColCmdline, RColType, RColOverrides, RColSource, RColFile };

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

bool isPositiveNumber(const QString &t)
{
    bool ok = false;
    const double v = t.toDouble(&ok);
    return ok && v > 0;
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

MainWindow::MainWindow(const QString &rootDir, const QString &packagedDir, QWidget *parent)
    : QMainWindow(parent), m_root(QDir(rootDir).absolutePath())
{
    if (!packagedDir.isEmpty()) {
        const QString p = QDir::cleanPath(QDir(packagedDir).absolutePath());
        if (QFileInfo(p).isDir() && p != QDir::cleanPath(m_root))
            m_packagedRoot = p;
    }
    m_rulesDir = m_root + QStringLiteral("/99-rules");
    m_newTypesFile = m_root + QStringLiteral("/99-types.types");

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
    cols.append({tr("Source"), [this](const Entry &e) { return sourceText(m_types, e); },
                 [this](const Entry &e) { return sourceTip(m_types, e); }});
    cols.append({tr("File"), [this](const Entry &e) { return displayPath(e); },
                 [](const Entry &e) { return e.file ? e.file->path : QString(); }});
    m_typeModel->setColumns(cols);
    m_typeModel->setHiddenFn([this](const Entry &e) { return isHidden(m_types, e); });
    m_typeModel->setRefreshFn([this] { recomputeLayers(); });
    const int typeSourceCol = cols.size() - 2;
    m_typeModel->setWarnFn([this](const Entry &e) {
        if (isPackaged(e))
            return QString(); // the package's business, and never written
        const QString n = e.str(QStringLiteral("type"));
        if (n.isEmpty())
            return tr("Type name is empty");
        int c = 0; // copies in the config directory; one there overriding a packaged type is fine
        for (const auto &o : m_types.entries())
            if (!isPackaged(*o) && o->str(QStringLiteral("type")) == n)
                ++c;
        return c > 1 ? tr("Type \"%1\" is defined %2 times; the last one wins").arg(n).arg(c) : QString();
    });

    m_typeProxy = new EntryFilterProxy(this);
    m_typeProxy->setSourceModel(m_typeModel);
    m_typeView = makeTable();
    m_typeView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_typeView->setModel(m_typeProxy);
    m_typeView->setColumnHidden(typeSourceCol, m_packagedRoot.isEmpty());
    new ColumnFitter(m_typeView);
    m_typeView->sortByColumn(0, Qt::AscendingOrder);
    left->addWidget(m_typeView, 1);

    auto *btns = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add type…"));
    auto *edit = new QPushButton(tr("Edit…"));
    auto *dup = new QPushButton(tr("Duplicate…"));
    auto *del = new QPushButton(tr("Delete"));
    auto *resetType = new QPushButton(tr("Reset to packaged"));
    resetType->setToolTip(tr("Remove your changes to a packaged type, so the package's version applies again"));
    if (m_packagedRoot.isEmpty())
        resetType->hide();
    auto *reorgTypes = new QPushButton(tr("Move all into 99-types.types"));
    reorgTypes->setToolTip(tr("Move every type into %1").arg(m_newTypesFile));
    auto *show = new QPushButton(tr("Show its rules"));
    show->setToolTip(tr("Filter the rules list below by the selected type"));
    btns->addWidget(add);
    btns->addWidget(edit);
    btns->addWidget(dup);
    btns->addWidget(del);
    btns->addWidget(resetType);
    btns->addWidget(show);
    btns->addSpacing(16);
    btns->addWidget(reorgTypes);
    btns->addStretch(1);
    btns->addWidget(new QLabel(tr("Double-click a type to edit it")));
    left->addLayout(btns);
    h->addLayout(left, 1);

    connect(m_typeFilter, &QLineEdit::textChanged, m_typeProxy, &EntryFilterProxy::setText);
    connect(add, &QPushButton::clicked, this, &MainWindow::addType);
    connect(edit, &QPushButton::clicked, this, &MainWindow::editType);
    connect(dup, &QPushButton::clicked, this, &MainWindow::duplicateType);
    connect(del, &QPushButton::clicked, this, &MainWindow::deleteType);
    connect(resetType, &QPushButton::clicked, this, &MainWindow::resetType);
    connect(show, &QPushButton::clicked, this, &MainWindow::showRulesOfType);
    connect(reorgTypes, &QPushButton::clicked, this, &MainWindow::reorganizeTypes);
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
        {tr("Command line contains"), [](const Entry &e) { return rulematch::needlesText(e); },
         [](const Entry &e) {
             return rulematch::hasCmdline(e)
                 ? tr("Only %1 processes whose command line contains: %2")
                       .arg(e.str(QStringLiteral("name")), rulematch::needlesText(e))
                 : QString();
         }},
        {tr("Type"), [](const Entry &e) { return e.str(QStringLiteral("type")); }, {}},
        {tr("Overrides"),
         [](const Entry &e) {
             QStringList parts;
             for (const auto &f : e.fields)
                 if (f.first != QLatin1String("name") && f.first != QLatin1String("type")
                     && f.first != rulematch::kCmdlineKey)
                     parts << f.first + QLatin1Char('=') + jsonValueText(f.second);
             return parts.join(QStringLiteral(", "));
         },
         {}},
        {tr("Source"), [this](const Entry &e) { return sourceText(m_rules, e); },
         [this](const Entry &e) { return sourceTip(m_rules, e); }},
        {tr("File"), [this](const Entry &e) { return displayPath(e); },
         [](const Entry &e) { return e.file ? e.file->path : QString(); }},
    });
    m_ruleModel->setHiddenFn([this](const Entry &e) { return isHidden(m_rules, e); });
    m_ruleModel->setRefreshFn([this] { recomputeLayers(); });
    m_ruleModel->setWarnFn([this](const Entry &e) {
        if (isPackaged(e))
            return QString(); // the package's business, and never written
        if (e.str(QStringLiteral("name")).isEmpty())
            return tr("Process name is empty");
        const QString t = e.str(QStringLiteral("type"));
        if (!t.isEmpty() && !m_types.find(t))
            return tr("Type \"%1\" is not defined").arg(t);
        const QString problem = m_ruleAnalysis.problem.value(&e);
        if (!problem.isEmpty())
            return problem;
        if (!m_cmdlineSupported && rulematch::hasCmdline(e))
            return cmdlineUnsupportedNote();
        return QString();
    });

    m_ruleProxy = new EntryFilterProxy(this);
    m_ruleProxy->setSourceModel(m_ruleModel);
    m_ruleView = makeTable();
    m_ruleView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_ruleView->setModel(m_ruleProxy);
    m_ruleView->setColumnHidden(RColSource, m_packagedRoot.isEmpty());
    new ColumnFitter(m_ruleView);
    m_ruleView->sortByColumn(RColName, Qt::AscendingOrder);
    left->addWidget(m_ruleView, 1);

    auto *btns = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add rule…"));
    auto *addProc = new QPushButton(tr("Add from running process…"));
    auto *edit = new QPushButton(tr("Edit…"));
    auto *del = new QPushButton(tr("Delete"));
    auto *resetRule = new QPushButton(tr("Reset to packaged"));
    resetRule->setToolTip(tr("Remove your changes to the selected packaged rules, so the package's version applies again"));
    if (m_packagedRoot.isEmpty())
        resetRule->hide();
    auto *reorg = new QPushButton(tr("Reorganize into 99-rules/…"));
    reorg->setToolTip(tr("Move every rule into %1/<type>.rules (rules without a type: custom.rules)")
                          .arg(QDir(m_root).relativeFilePath(m_rulesDir)));
    auto *dedup = new QPushButton(tr("Remove duplicates…"));
    dedup->setToolTip(tr("Delete rules that are never used: extra copies of a rule for the same process, and rules "
                         "with \"command line contains\" that an earlier rule always matches first"));
    btns->addWidget(add);
    btns->addWidget(addProc);
    btns->addWidget(edit);
    btns->addWidget(del);
    btns->addWidget(resetRule);
    btns->addSpacing(16);
    btns->addWidget(reorg);
    btns->addWidget(dedup);
    btns->addStretch(1);
    auto *hint = new QLabel(tr("Double-click a rule to edit it · new rules go to %1/<type>.rules")
                                .arg(QDir(m_root).relativeFilePath(m_rulesDir)));
    hint->setForegroundRole(QPalette::PlaceholderText);
    btns->addWidget(hint);
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
    connect(reorg, &QPushButton::clicked, this, &MainWindow::reorganizeRules);
    connect(dedup, &QPushButton::clicked, this, &MainWindow::removeDuplicateRules);
    connect(m_showAllRules, &QPushButton::clicked, this, [this] {
        m_ruleFilter->clear();
        m_ruleTypeFilter->setCurrentIndex(0);
    });
    connect(del, &QPushButton::clicked, this, &MainWindow::deleteRules);
    connect(resetRule, &QPushButton::clicked, this, &MainWindow::resetRules);
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
                if (s.numeric && !isPositiveNumber(t))
                    return; // incomplete while typing (e.g. "0" on the way to "0.5")
                m_conf.set(s.key, t);
            }
            updateDirtyState();
        });
        if (s.numeric) {
            // An invalid value left in the field would show something that is not saved:
            // put the current value back when editing ends.
            connect(c->lineEdit(), &QLineEdit::editingFinished, this, [this, s, c] {
                const QString t = c->currentText().trimmed();
                if (t.isEmpty() || isPositiveNumber(t))
                    return;
                m_confLoading = true;
                c->setEditText(m_conf.get(s.key));
                m_confLoading = false;
            });
        }
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
    if (!m_packagedRoot.isEmpty()) {
        rootLbl->setText(rootLbl->text() + tr(" · packaged (read-only): <b>%1</b>").arg(m_packagedRoot.toHtmlEscaped()));
        rootLbl->setToolTip(tr("The packaged rules are loaded first. Your changes are saved in %1 and override them; "
                               "package updates never touch them.").arg(m_root));
    }
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
    connect(m_service, &QComboBox::currentTextChanged, this, [this](const QString &t) {
        QSettings().setValue(QStringLiteral("service"), t.trimmed());
        updateCmdlineSupport();
    });
    return w;
}

// =====================================================================  loading

void MainWindow::loadAll()
{
    m_types.clear();
    m_rules.clear();
    m_cgroups.clear();

    if (!m_packagedRoot.isEmpty()) { // first: ananicy-cpp loads them before the config directory
        m_types.loadTree(m_packagedRoot, QStringLiteral("*.types"), true, true);
        m_rules.loadTree(m_packagedRoot, QStringLiteral("*.rules"), true, true);
        m_cgroups.loadTree(m_packagedRoot, QStringLiteral("*.cgroups"), true, true);
        m_packagedConf.load(m_packagedRoot + QStringLiteral("/ananicy.conf"));
    } else {
        m_packagedConf = MainConf();
    }
    m_types.loadTree(m_root, QStringLiteral("*.types"), true);
    m_rules.loadTree(m_root, QStringLiteral("*.rules"), true);
    m_cgroups.loadTree(m_root, QStringLiteral("*.cgroups"), true);
    m_conf.load(m_root + QStringLiteral("/ananicy.conf"));

    updateCmdlineSupport();
    recomputeUsage();
    m_typeModel->reset();
    m_ruleModel->reset();

    typeNamesChanged();
    refreshConfTab();

    QStringList warnings = m_types.warnings + m_rules.warnings + m_cgroups.warnings;
    if (!QFileInfo(m_root).isDir() && m_packagedRoot.isEmpty()) // with packaged rules it is created on save
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
    int packaged = 0;
    for (const auto &e : m_rules.entries()) {
        files.insert(e->file);
        packaged += isPackaged(*e) ? 1 : 0;
    }
    for (const auto &e : m_types.entries())
        files.insert(e->file);
    QString loaded = tr("Loaded %1 types and %2 rules from %3 files")
                         .arg(m_types.entries().size())
                         .arg(m_rules.entries().size())
                         .arg(files.size());
    if (!m_packagedRoot.isEmpty())
        loaded += tr(" (%n packaged rule(s))", nullptr, packaged);
    statusBar()->showMessage(loaded, 8000);
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
    recomputeLayers();
    m_usage.clear();
    m_ruleAnalysis = rulematch::analyze(m_rules.entries()); // entries() is in load order
    for (const auto &r : m_rules.entries()) {
        if (isHidden(m_rules, *r))
            continue; // a packaged rule replaced by one in the config directory
        const QString t = r->str(QStringLiteral("type"));
        if (!t.isEmpty())
            m_usage[t]++;
    }
}

// =====================================================================  packaged layer

QString MainWindow::entryKey(const EntryStore &store, const Entry &e) const
{
    return &store == &m_rules ? rulematch::ruleKey(e) : e.str(store.primaryKey());
}

void MainWindow::recomputeLayers()
{
    auto fill = [this](const EntryStore &store, Layers &l) {
        l.last.clear();
        l.lastPackaged.clear();
        for (const auto &e : store.entries()) { // load order: the last one per key is used
            const QString k = entryKey(store, *e);
            l.last.insert(k, e);
            if (isPackaged(*e))
                l.lastPackaged.insert(k, e);
        }
    };
    fill(m_types, m_typeLayers);
    fill(m_rules, m_ruleLayers);
}

const MainWindow::Layers &MainWindow::layersOf(const EntryStore &store) const
{
    return &store == &m_rules ? m_ruleLayers : m_typeLayers;
}

EntryPtr MainWindow::packagedOf(const EntryStore &store, const Entry &e) const
{
    if (isPackaged(e))
        return nullptr;
    return layersOf(store).lastPackaged.value(entryKey(store, e));
}

bool MainWindow::isHidden(const EntryStore &store, const Entry &e) const
{
    if (!isPackaged(e))
        return false;
    const EntryPtr last = layersOf(store).last.value(entryKey(store, e));
    return last && last.get() != &e;
}

QVector<QPair<QString, QJsonValue>> MainWindow::offFields(const EntryStore &store, const Entry &e) const
{
    // Only what identifies the entry: it then replaces the packaged one and sets nothing.
    QVector<QPair<QString, QJsonValue>> f{{store.primaryKey(), e.value(store.primaryKey())}};
    if (&store == &m_rules && rulematch::hasCmdline(e))
        f.append({rulematch::kCmdlineKey, e.value(rulematch::kCmdlineKey)});
    return f;
}

MainWindow::Source MainWindow::sourceOf(const EntryStore &store, const Entry &e) const
{
    if (isPackaged(e))
        return Source::Packaged;
    if (!packagedOf(store, e))
        return Source::Own;
    return e.fields.size() == offFields(store, e).size() ? Source::Off : Source::Changed;
}

QString MainWindow::sourceText(const EntryStore &store, const Entry &e) const
{
    switch (sourceOf(store, e)) {
    case Source::Packaged:
        return tr("Packaged");
    case Source::Changed:
        return tr("Changed");
    case Source::Off:
        return tr("Off");
    case Source::Own:
        break;
    }
    return tr("Yours");
}

QString MainWindow::sourceTip(const EntryStore &store, const Entry &e) const
{
    const QString file = e.file ? e.file->path : QString();
    switch (sourceOf(store, e)) {
    case Source::Packaged:
        return tr("From the package (%1), read-only. Editing it saves your version to %2, which package updates "
                  "never touch.").arg(file, overridePath(e.file));
    case Source::Changed:
        return tr("Your version in %1 replaces the packaged one:\n%2")
            .arg(file, packagedOf(store, e)->originalLine.trimmed());
    case Source::Off:
        return tr("Switched off: this entry in %1 has no settings and replaces the packaged one:\n%2\n"
                  "Use \"Reset to packaged\" to use it again.").arg(file, packagedOf(store, e)->originalLine.trimmed());
    case Source::Own:
        break;
    }
    return tr("Only in %1").arg(m_root);
}

QString MainWindow::overridePath(const ConfFile *packagedFile) const
{
    if (!packagedFile || m_packagedRoot.isEmpty())
        return QString();
    return QDir::cleanPath(m_root + QLatin1Char('/') + QDir(m_packagedRoot).relativeFilePath(packagedFile->path));
}

QString MainWindow::displayPath(const Entry &e) const
{
    if (!e.file)
        return QString();
    return QDir(isPackaged(e) ? m_packagedRoot : m_root).relativeFilePath(e.file->path);
}

EntryPtr MainWindow::addOverride(EntryStore &store, const EntryPtr &packaged,
                                 const QVector<QPair<QString, QJsonValue>> &fields)
{
    EntryPtr e = store.add(overridePath(packaged->file), packaged->str(store.primaryKey()));
    e->fields = fields;
    return e;
}

int MainWindow::resetEntries(EntryStore &store, const QVector<EntryPtr> &sel)
{
    QSet<QString> keys;
    for (const auto &e : sel)
        if (sourceOf(store, *e) == Source::Changed || sourceOf(store, *e) == Source::Off)
            keys.insert(entryKey(store, *e));
    int n = 0;
    const QVector<EntryPtr> all = store.entries();
    for (const auto &e : all) {
        if (!isPackaged(*e) && keys.contains(entryKey(store, *e))) {
            store.remove(e);
            ++n;
        }
    }
    return n;
}

void MainWindow::typeNamesChanged()
{
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

void MainWindow::updateCmdlineSupport()
{
    const QString svc = m_service ? m_service->currentText().trimmed() : QString();
    m_isAnanicyCpp = svc.contains(QLatin1String("ananicy-cpp"));
    bool supported = false;
    if (m_isAnanicyCpp) {
        // ananicy-cpp only knows "cmdline_contains" if it was built with that feature; the
        // key then appears as a string in the program itself.
        QString exe = qEnvironmentVariable("ANANICY_CPP_BINARY");
        if (exe.isEmpty())
            exe = QStandardPaths::findExecutable(QStringLiteral("ananicy-cpp"));
        QFile f(exe);
        if (!exe.isEmpty() && f.open(QIODevice::ReadOnly))
            supported = f.readAll().contains(QByteArrayLiteral("cmdline_contains"));
    }
    if (supported == m_cmdlineSupported)
        return;
    m_cmdlineSupported = supported;
    if (m_ruleModel)
        m_ruleModel->allChanged();
}

QString MainWindow::cmdlineUnsupportedNote() const
{
    const QString daemon = m_isAnanicyCpp ? tr("The installed ananicy-cpp") : tr("Ananicy");
    return tr("%1 does not support \"command line contains\" (cmdline_contains). It ignores the field, so this "
              "rule applies to every process with this name and replaces other rules for the same name. It needs "
              "ananicy-cpp from github.com/WhitePeace36/ananicy-cpp.")
        .arg(daemon);
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
        if (isPackaged(*t))
            continue; // never written; an override of it in the config directory is not a duplicate
        const QString n = t->str(QStringLiteral("type"));
        if (n.isEmpty())
            errors << tr("A type has an empty name (%1)").arg(t->file ? t->file->path : QString());
        else if (seen.contains(n) && t->modified)
            errors << tr("Type \"%1\" is defined more than once").arg(n);
        seen.insert(n);
    }
    for (const auto &p : m_confCombos) {
        const QString t = p.second->currentText().trimmed();
        if (p.first.numeric && !t.isEmpty() && !isPositiveNumber(t))
            errors << tr("ananicy.conf: %1 must be a number greater than 0 (it is \"%2\")").arg(p.first.key, t);
    }
    QStringList missing;
    for (const auto &r : m_rules.entries()) {
        if (isPackaged(*r))
            continue;
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
    const QStringList deletes = m_types.pendingDeletes() + m_rules.pendingDeletes();

    if (writes.isEmpty() && deletes.isEmpty() && !restart) {
        statusBar()->showMessage(tr("Nothing to save"), 5000);
        return true;
    }

    // Safety checks before anything is written or deleted.
    const QStringList problems = checkTargets(writes.keys(), deletes);
    if (!problems.isEmpty()) {
        QMessageBox box(QMessageBox::Critical, tr("Nothing was saved"),
                        tr("Saving was stopped because %n file(s) are not safe to change. "
                           "If files were changed on disk, use \"Reload from disk\" and redo your changes.",
                           nullptr, problems.size()),
                        QMessageBox::Ok, this);
        box.setInformativeText(problems.mid(0, 8).join(QLatin1Char('\n')) + (problems.size() > 8 ? QStringLiteral("\n…") : QString()));
        box.setDetailedText(problems.join(QLatin1Char('\n')));
        box.exec();
        return false;
    }
    if (!deletes.isEmpty()) {
        QStringList rel;
        for (const QString &p : deletes)
            rel << QDir(m_root).relativeFilePath(p);
        rel.sort();
        QMessageBox box(QMessageBox::Question, tr("Delete files"),
                        tr("Saving will delete %n file(s) that no longer contain any rules or types "
                           "(only comments or blank lines are left). Continue?", nullptr, deletes.size()),
                        QMessageBox::Yes | QMessageBox::Cancel, this);
        box.setInformativeText(rel.mid(0, 10).join(QLatin1Char('\n')) + (rel.size() > 10 ? QStringLiteral("\n…") : QString()));
        box.setDetailedText(rel.join(QLatin1Char('\n')));
        if (box.exec() != QMessageBox::Yes)
            return false;
    }

    QString log;
    setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = writeFiles(writes, deletes, restart, &log);
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
        m_conf.diskHash = contentHash(m_conf.render());
    }
    m_typeModel->allChanged();
    m_ruleModel->allChanged();
    refreshConfTab();
    updateDirtyState();

    QString msg = (writes.isEmpty() && deletes.isEmpty()) ? tr("No changes to write.")
                                                          : tr("Saved %n file(s).", nullptr, writes.size());
    if (!deletes.isEmpty())
        msg += QLatin1Char(' ') + tr("Removed %n file(s) that had no entries left.", nullptr, deletes.size());
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

// Every file that is about to be written or deleted must be inside the config directory,
// must not be (or be reached through) a symlink, and must still have exactly the content
// it had when it was loaded. Returns one message per problem.
QStringList MainWindow::checkTargets(const QStringList &writes, const QStringList &deletes) const
{
    QStringList problems;
    const QString root = QDir::cleanPath(m_root);
    auto check = [&](const QString &path, bool deleting) {
        const QString p = QDir::cleanPath(path);
        const QString rel = QDir(root).relativeFilePath(p);
        if (!p.startsWith(root + QLatin1Char('/'))) {
            problems << tr("%1: outside %2").arg(p, root);
            return;
        }
        const QFileInfo fi(p);
        if (fi.isSymLink()) {
            problems << tr("%1: is a symbolic link (to %2); linked files are not changed").arg(rel, fi.symLinkTarget());
            return;
        }
        if (fi.exists() && !fi.isFile()) {
            problems << tr("%1: is not a regular file").arg(rel);
            return;
        }
        for (QString d = QFileInfo(p).absolutePath(); d.startsWith(root + QLatin1Char('/')); d = QFileInfo(d).absolutePath()) {
            if (QFileInfo(d).isSymLink()) {
                problems << tr("%1: its folder %2 is a symbolic link").arg(rel, QDir(root).relativeFilePath(d));
                return;
            }
        }
        QByteArray expected;
        bool known = m_types.loadedHash(p, &expected) || m_rules.loadedHash(p, &expected);
        if (!known && p == QDir::cleanPath(m_conf.path)) {
            expected = m_conf.diskHash;
            known = true;
        }
        if (!known) {
            problems << (QFileInfo::exists(p)
                             ? tr("%1: exists but was not loaded (is it readable?); it is not overwritten").arg(rel)
                             : tr("%1: was never loaded by this program").arg(rel));
            return;
        }
        const QByteArray current = fileHash(p);
        if (current != expected) {
            if (expected.isEmpty())
                problems << tr("%1: did not exist when loading, but exists now").arg(rel);
            else if (current.isEmpty())
                problems << tr("%1: was deleted on disk since it was loaded").arg(rel);
            else
                problems << tr("%1: was changed on disk since it was loaded%2")
                                .arg(rel, deleting ? tr(" (it would have been deleted)") : QString());
        }
    };
    for (const QString &p : writes)
        check(p, false);
    for (const QString &p : deletes)
        check(p, true);
    return problems;
}

// Directories that may have become empty after deleting files: their parents up to (not
// including) the config root, deepest first.
QStringList MainWindow::emptyDirCandidates(const QStringList &deletedFiles) const
{
    QStringList dirs;
    const QString root = QDir::cleanPath(m_root);
    for (const QString &f : deletedFiles) {
        QString d = QDir::cleanPath(QFileInfo(f).absolutePath());
        while (d.startsWith(root + QLatin1Char('/')) && d != root) {
            if (!dirs.contains(d))
                dirs << d;
            d = QFileInfo(d).absolutePath();
        }
    }
    std::sort(dirs.begin(), dirs.end(), [](const QString &a, const QString &b) {
        return a.count(QLatin1Char('/')) > b.count(QLatin1Char('/'));
    });
    return dirs;
}

bool MainWindow::writeFiles(const QMap<QString, QByteArray> &writes, const QStringList &deletes, bool restart,
                            QString *log)
{
    const QString svc = m_service->currentText().trimmed();
    const bool isRoot = ::geteuid() == 0;

    bool direct = isRoot;
    if (!direct && !(writes.isEmpty() && deletes.isEmpty())) {
        direct = true;
        for (auto it = writes.begin(); it != writes.end(); ++it) {
            QFileInfo fi(it.key());
            QString dir = fi.absolutePath();
            while (!QFileInfo::exists(dir) && dir.size() > 1) // new sub-directory: check its parent
                dir = QFileInfo(dir).absolutePath();
            const bool writable = fi.exists() ? fi.isWritable() : QFileInfo(dir).isWritable();
            if (!writable) {
                direct = false;
                break;
            }
        }
        for (const QString &p : deletes)
            if (!QFileInfo(QFileInfo(p).absolutePath()).isWritable())
                direct = false;
    }

    if (direct) {
        for (auto it = writes.begin(); it != writes.end(); ++it) {
            const QString path = it.key();
            QDir().mkpath(QFileInfo(path).absolutePath());
            QSaveFile f(path);
            if (!f.open(QIODevice::WriteOnly) || f.write(it.value()) != it.value().size() || !f.commit()) {
                *log = tr("Writing %1 failed: %2").arg(path, f.errorString());
                return false;
            }
        }
        for (const QString &path : deletes) {
            if (QFile::exists(path) && !QFile::remove(path)) {
                *log = tr("Removing %1 failed").arg(path);
                return false;
            }
        }
        for (const QString &dir : emptyDirCandidates(deletes))
            QDir().rmdir(dir); // only succeeds if the directory is empty
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
        script += QStringLiteral("install -D -m 0644 %1 %2\n").arg(shellQuote(staged), shellQuote(it.key()));
    }
    for (const QString &path : deletes)
        script += QStringLiteral("rm -f %1\n").arg(shellQuote(path));
    for (const QString &dir : emptyDirCandidates(deletes)) // only removed if empty
        script += QStringLiteral("rmdir %1 2>/dev/null || true\n").arg(shellQuote(dir));
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
    if (original && isPackaged(*original))
        dlg.editor()->setFileHint(tr("%1 (your version, replaces the packaged type)").arg(overridePath(original->file)));
    else
        dlg.editor()->setFileHint(tr("%1 (new)").arg(m_newTypesFile));
    dlg.editor()->setCgroupNames(m_cgroups.names());
    dlg.setValidator([this, original](const Entry &e, QWidget *parent) {
        const QString n = e.str(QStringLiteral("type"));
        if (n.isEmpty()) {
            QMessageBox::warning(parent, tr("Type"), tr("The type name cannot be empty."));
            return false;
        }
        // Only check when the name changes: find() returns the last definition, so for a type
        // that is already defined twice the earlier copy would otherwise never pass.
        const bool renamed = !original || original->str(QStringLiteral("type")) != n;
        EntryPtr other = m_types.find(n);
        if (renamed && other && other != original) {
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
    } else if (isPackaged(*original)) {
        // never changed in place: same name -> override in the config directory, else a new type
        if (original->fields == work->fields)
            return;
        if (newName == original->str(QStringLiteral("type"))) {
            target = addOverride(m_types, original, work->fields);
        } else {
            target = m_types.add(m_newTypesFile, newName);
            target->fields = work->fields;
        }
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
                    placeNewRule(r);
                }
                m_rules.sortByLoadOrder();
                m_ruleModel->reset();
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
    const Source src = sourceOf(m_types, *e);
    if (src == Source::Off) {
        statusBar()->showMessage(tr("Type \"%1\" is already switched off. \"Reset to packaged\" uses the package's "
                                    "version again.").arg(name), 10000);
        return;
    }
    if (src == Source::Packaged || src == Source::Changed) {
        // A packaged type cannot be deleted: an entry without settings switches it off.
        QString q = tr("Switch off the packaged type \"%1\"?\n\nAn entry without settings is saved in %2 and replaces "
                       "the package's version. \"Reset to packaged\" undoes this.")
                        .arg(name, src == Source::Packaged ? overridePath(e->file) : e->file->path);
        if (used > 0)
            q += QLatin1String("\n\n") + tr("%n rule(s) use this type and then get no settings from it.", nullptr, used);
        if (QMessageBox::question(this, tr("Switch off type"), q) != QMessageBox::Yes)
            return;
        EntryPtr target = e;
        if (src == Source::Packaged) {
            target = addOverride(m_types, e, offFields(m_types, *e));
        } else {
            e->fields = offFields(m_types, *e);
            m_types.markModified(e);
        }
        m_typeModel->reset();
        recomputeUsage();
        m_ruleModel->allChanged();
        selectType(target);
        updateDirtyState();
        return;
    }
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

void MainWindow::resetType()
{
    EntryPtr e = currentType();
    if (!e)
        return;
    const QString name = e->str(QStringLiteral("type"));
    if (resetEntries(m_types, {e}) == 0) {
        statusBar()->showMessage(tr("Type \"%1\" is not a changed packaged type.").arg(name), 8000);
        return;
    }
    m_typeModel->reset();
    recomputeUsage();
    typeNamesChanged();
    m_ruleModel->allChanged();
    selectType(m_types.find(name));
    updateDirtyState();
    statusBar()->showMessage(tr("Type \"%1\" uses the packaged version again. Press Save to write the change.").arg(name),
                             10000);
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
    // What ananicy-cpp would apply: for a group (cmdline == nullptr) the rule for every process
    // with that name plus how many command-line rules exist; for one process the rule it gets.
    auto describe = [this](const QString &name, const QString *cmdline) -> QString {
        auto text = [this](const EntryPtr &r) {
            const QString t = r->str(QStringLiteral("type"));
            QString s = t.isEmpty() ? tr("(rule without type)") : t;
            if (rulematch::hasCmdline(*r))
                s += tr(" (command line: %1)").arg(rulematch::needlesText(*r));
            return s;
        };
        if (cmdline) {
            const EntryPtr r = rulematch::match(m_rules.entries(), name, *cmdline);
            return r ? text(r) : QString();
        }
        const EntryPtr plain = rulematch::match(m_rules.entries(), name, QString());
        int byCmdline = 0;
        for (const auto &r : m_rules.entries())
            if (rulematch::hasCmdline(*r) && r->str(QStringLiteral("name")) == name)
                ++byCmdline;
        QStringList parts;
        if (plain)
            parts << text(plain);
        if (byCmdline > 0)
            parts << tr("%n rule(s) by command line", nullptr, byCmdline);
        return parts.join(QStringLiteral(", "));
    };
    ProcessDialog dlg(describe, m_isAnanicyCpp, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const ProcessDialog::Selection sel = dlg.selection();
    if (sel.name.isEmpty())
        return;
    if (!sel.specific) {
        createRule(sel.name);
        return;
    }
    const QString needle = rulematch::suggestNeedle(sel.argv);
    if (needle.isEmpty())
        statusBar()->showMessage(tr("No typical part found in this command line: enter the text that identifies it "
                                    "under \"Command line contains\"."),
                                 10000);
    createRule(sel.name, needle.isEmpty() ? QStringList() : QStringList{needle});
}

// The rule with the same name and the same command-line texts (order does not matter), i.e. the
// one a new rule would replace: the last one, which is the one that applies.
static EntryPtr findSameRule(const QVector<EntryPtr> &rules, const QString &name, const QStringList &needles,
                             const Entry *except = nullptr)
{
    const QSet<QString> want(needles.begin(), needles.end());
    EntryPtr found;
    for (const auto &r : rules) {
        if (r.get() == except || r->str(QStringLiteral("name")) != name)
            continue;
        const QStringList n = rulematch::needles(*r);
        if (rulematch::hasCmdline(*r) != !needles.isEmpty() || QSet<QString>(n.begin(), n.end()) != want)
            continue;
        found = r;
    }
    return found;
}

void MainWindow::createRule(const QString &name, const QStringList &needles)
{
    if (EntryPtr ex = findSameRule(m_rules.entries(), name, needles)) {
        const QString what = needles.isEmpty()
            ? tr("A rule for \"%1\" already exists in %2.").arg(name, ex->file ? ex->file->path : QString())
            : tr("A rule for \"%1\" with command line \"%2\" already exists in %3.")
                  .arg(name, needles.join(QStringLiteral(" + ")), ex->file ? ex->file->path : QString());
        const auto r = QMessageBox::question(this, tr("Rule exists"),
                                             what + QStringLiteral("\n\n") + tr("Edit the existing rule instead?"),
                                             QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        if (r == QMessageBox::Cancel)
            return;
        if (r == QMessageBox::Yes) {
            selectRule(ex);
            openRuleDialog(ex, QString());
            return;
        }
    }
    openRuleDialog(nullptr, name, needles);
}

void MainWindow::openRuleDialog(const EntryPtr &original, const QString &newName, const QStringList &newNeedles)
{
    auto work = std::make_shared<Entry>(original ? *original : Entry());
    if (!original && !newName.isEmpty())
        work->set(QStringLiteral("name"), newName);
    if (!original && !newNeedles.isEmpty())
        work->set(rulematch::kCmdlineKey, rulematch::needlesValue(newNeedles));

    QString title = tr("New rule");
    if (original) {
        title = rulematch::hasCmdline(*original)
            ? tr("Edit rule \"%1\" (command line: %2)")
                  .arg(original->str(QStringLiteral("name")), rulematch::needlesText(*original))
            : tr("Edit rule \"%1\"").arg(original->str(QStringLiteral("name")));
    }
    EntryDialog dlg(EntryEditor::RuleMode, title, work, this);
    EntryEditor *ed = dlg.editor();
    ed->setCmdlineSupport(m_cmdlineSupported, cmdlineUnsupportedNote());
    ed->setNameLengthLimited(!m_isAnanicyCpp);
    ed->setFileHintFn([this, original](const Entry &e) {
        if (original && isPackaged(*original)) {
            if (rulematch::ruleKey(e) == rulematch::ruleKey(*original))
                return tr("%1 (your version, replaces the packaged rule)").arg(overridePath(original->file));
            return tr("%1 (new)").arg(ruleFileFor(e.str(QStringLiteral("type"))));
        }
        if (e.file && !e.originalLine.isEmpty())
            return e.file->path; // saved rules stay where they are
        return tr("%1 (new)").arg(ruleFileFor(e.str(QStringLiteral("type"))));
    });
    ed->setTypeDescriber([this](const QString &t) { return describeType(t); });
    ed->setCgroupNames(m_cgroups.names());
    ed->setTypeNames(m_types.names());
    dlg.setValidator([this, original](const Entry &e, QWidget *parent) {
        const QString n = e.str(QStringLiteral("name"));
        if (n.isEmpty()) {
            QMessageBox::warning(parent, tr("Rule"), tr("The process name cannot be empty."));
            return false;
        }
        bool valid = true;
        const QStringList needles = rulematch::needles(e, &valid);
        if (!valid) {
            QMessageBox::warning(parent, tr("Rule"),
                                 tr("\"Command line contains\" is not valid. Enter one text per line, or leave it "
                                    "empty."));
            return false;
        }
        // Only ask when the rule's identity (name + command-line texts) changes.
        auto key = [](const Entry &x) {
            QStringList k = rulematch::needles(x);
            k.sort();
            return QStringList{x.str(QStringLiteral("name")), rulematch::hasCmdline(x) ? QStringLiteral("1") : QString()}
                   + k;
        };
        if (original && key(*original) == key(e))
            return true;
        const EntryPtr other = findSameRule(m_rules.entries(), n, needles, original.get());
        if (!other || isPackaged(*other))
            return true; // a rule in the config directory replaces a packaged one: that is the point
        const QString where = other->file ? other->file->path : QString();
        const QString q = needles.isEmpty()
            ? tr("Another rule for \"%1\" already exists in %2.\nAnanicy will use whichever is loaded last. "
                 "Continue?").arg(n, where)
            : tr("Another rule for \"%1\" with the same command line texts already exists in %2.\n"
                 "ananicy-cpp uses whichever is loaded last, the other one is never used. Continue?")
                  .arg(n, where);
        return QMessageBox::question(parent, tr("Rule"), q) == QMessageBox::Yes;
    });
    if (newName.isEmpty() && !original)
        ed->focusPrimary();
    else
        ed->focusType();
    if (dlg.exec() != QDialog::Accepted)
        return;

    EntryPtr target = original;
    if (!original) {
        target = m_rules.add(ruleFileFor(work->str(QStringLiteral("type"))), work->str(QStringLiteral("name")));
        target->fields = work->fields;
        m_ruleModel->reset();
    } else if (isPackaged(*original)) {
        // never changed in place: same rule -> override in the config directory, else a new rule
        if (original->fields == work->fields)
            return;
        if (rulematch::ruleKey(*work) == rulematch::ruleKey(*original)) {
            target = addOverride(m_rules, original, work->fields);
        } else {
            target = m_rules.add(ruleFileFor(work->str(QStringLiteral("type"))), work->str(QStringLiteral("name")));
            target->fields = work->fields;
        }
        m_ruleModel->reset();
    } else {
        if (original->fields == work->fields)
            return; // nothing changed
        original->fields = work->fields;
        m_rules.markModified(original);
        placeNewRule(original);
        m_rules.sortByLoadOrder(); // an unsaved rule may have moved to another file
        m_ruleModel->reset();
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
    // Own rules are removed; packaged ones cannot be: a rule without settings switches them off.
    QVector<EntryPtr> own, switchOff;
    for (const auto &e : sel) {
        const Source src = sourceOf(m_rules, *e);
        if (src == Source::Own)
            own << e;
        else if (src == Source::Packaged || src == Source::Changed)
            switchOff << e;
    }
    if (own.isEmpty() && switchOff.isEmpty()) {
        statusBar()->showMessage(tr("Already switched off. \"Reset to packaged\" uses the package's version again."),
                                 10000);
        return;
    }
    QString q;
    if (switchOff.isEmpty()) {
        q = own.size() == 1 ? tr("Delete the rule for \"%1\" from %2?").arg(own[0]->str(QStringLiteral("name")),
                                                                          own[0]->file ? own[0]->file->path : QString())
                            : tr("Delete %1 selected rules?").arg(own.size());
    } else if (own.isEmpty() && switchOff.size() == 1) {
        const EntryPtr &e = switchOff[0];
        q = tr("Switch off the packaged rule for \"%1\"?\n\nA rule without settings is saved in %2 and replaces the "
               "package's version. \"Reset to packaged\" undoes this.")
                .arg(e->str(QStringLiteral("name")), isPackaged(*e) ? overridePath(e->file) : e->file->path);
    } else {
        q = tr("Delete %1 of your rules and switch off %2 packaged rules?\n\nPackaged rules are switched off by a rule "
               "without settings in %3. \"Reset to packaged\" undoes this.")
                .arg(own.size()).arg(switchOff.size()).arg(m_root);
    }
    if (QMessageBox::question(this, tr("Delete rules"), q) != QMessageBox::Yes)
        return;
    const int row = m_ruleView->currentIndex().row();
    for (const auto &e : own)
        m_rules.remove(e);
    for (const auto &e : switchOff) {
        if (isPackaged(*e)) {
            addOverride(m_rules, e, offFields(m_rules, *e));
        } else {
            e->fields = offFields(m_rules, *e);
            m_rules.markModified(e);
        }
    }
    m_ruleModel->reset();
    recomputeUsage();
    m_typeModel->allChanged();
    if (m_ruleProxy->rowCount() > 0)
        m_ruleView->setCurrentIndex(m_ruleProxy->index(qMin(row, m_ruleProxy->rowCount() - 1), 0));
    updateDirtyState();
}

void MainWindow::resetRules()
{
    QVector<EntryPtr> sel;
    for (const QModelIndex &idx : m_ruleView->selectionModel()->selectedRows())
        if (EntryPtr e = m_ruleModel->entryAt(m_ruleProxy->mapToSource(idx).row()))
            sel << e;
    if (sel.isEmpty())
        if (EntryPtr e = currentRule())
            sel << e;
    const int n = resetEntries(m_rules, sel);
    if (n == 0) {
        statusBar()->showMessage(tr("None of the selected rules changes or switches off a packaged rule."), 8000);
        return;
    }
    m_ruleModel->reset();
    recomputeUsage();
    m_typeModel->allChanged();
    updateDirtyState();
    statusBar()->showMessage(tr("%n rule(s) use the packaged version again. Press Save to write the change.", nullptr,
                                sel.size()),
                             10000);
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
    if (m_packagedConf.existed)
        m_confPath->setText(m_confPath->text()
                            + tr("<br>Packaged defaults: <b>%1</b> (read-only). Only the settings you change are saved "
                                 "in your file; they win over the packaged ones.").arg(m_packagedConf.path.toHtmlEscaped()));
    QStringList knownKeys;
    for (auto &p : m_confCombos) {
        const ConfSpec &s = p.first;
        QComboBox *c = p.second;
        knownKeys << s.key;
        c->clear();
        // first item: nothing in the config file, so the packaged value (or ananicy's default) applies
        const QString unsetText = m_packagedConf.has(s.key) ? tr("(packaged: %1)").arg(m_packagedConf.get(s.key))
                                                            : QString::fromLatin1(kNotSet);
        c->addItem(s.numeric ? QString() : unsetText);
        if (s.numeric)
            c->lineEdit()->setPlaceholderText(unsetText);
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
    QStringList otherKeys = m_conf.keys();
    for (const QString &k : m_packagedConf.keys())
        if (!otherKeys.contains(k))
            otherKeys << k;
    for (const QString &k : otherKeys) {
        if (knownKeys.contains(k))
            continue;
        const bool packagedOnly = !m_conf.has(k);
        const int r = m_confOther->rowCount();
        m_confOther->insertRow(r);
        auto *ki = new QTableWidgetItem(k);
        ki->setFlags(ki->flags() & ~Qt::ItemIsEditable);
        auto *vi = new QTableWidgetItem(packagedOnly ? m_packagedConf.get(k) : m_conf.get(k));
        if (packagedOnly) {
            QFont f = ki->font();
            f.setItalic(true);
            ki->setFont(f);
            vi->setFont(f);
            const QString tip = tr("Packaged value. Changing it saves your value in %1.").arg(m_conf.path);
            ki->setToolTip(tip);
            vi->setToolTip(tip);
        }
        m_confOther->setItem(r, 0, ki);
        m_confOther->setItem(r, 1, vi);
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
    const QString key = m_confOther->item(r, 0)->text();
    if (!m_conf.has(key)) {
        QMessageBox::information(this, tr("Remove key"),
                                 tr("\"%1\" comes from the packaged %2 and cannot be removed there. Change its value to "
                                    "override it.").arg(key, m_packagedConf.path));
        return;
    }
    m_conf.unset(key); // a packaged value for it applies again
    refreshConfTab();
    updateDirtyState();
}

void MainWindow::updateRuleCount()
{
    if (!m_ruleCount || !m_showAllRules)
        return;
    const bool filtered = !m_ruleFilter->text().trimmed().isEmpty() || m_ruleTypeFilter->currentIndex() > 0;
    int total = 0;
    for (const auto &e : m_rules.entries())
        total += isHidden(m_rules, *e) ? 0 : 1;
    m_ruleCount->setText(filtered ? tr("showing %1 of %2").arg(m_ruleProxy->rowCount()).arg(total)
                                  : tr("%1 rules").arg(total));
    m_showAllRules->setEnabled(filtered);
}

// =====================================================================  rule file structure

QString MainWindow::ruleFileFor(const QString &type) const
{
    QString base = QStringLiteral("custom");
    if (!type.isEmpty()) {
        base.clear();
        for (const QChar c : type) // keep the name readable but safe as a file name
            base += (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_') || c == QLatin1Char('.'))
                        ? c : QLatin1Char('_');
        if (base.compare(QLatin1String("custom"), Qt::CaseInsensitive) == 0 || base.startsWith(QLatin1Char('.')))
            base.prepend(QStringLiteral("type-")); // custom.rules is reserved for rules without a type
    }
    return m_rulesDir + QLatin1Char('/') + base + QStringLiteral(".rules");
}

void MainWindow::placeNewRule(const EntryPtr &e)
{
    if (e && e->originalLine.isEmpty()) // never saved -> follows its type
        m_rules.moveTo(e, ruleFileFor(e->str(QStringLiteral("type"))));
}

void MainWindow::reorganizeRules()
{
    // Duplicates with different types would land in different files, which can change
    // which copy wins. Offer to clean them up first.
    const int dupNames = m_ruleAnalysis.duplicatePlainNames();
    if (dupNames > 0) {
        const auto r = QMessageBox::question(
            this, tr("Reorganize rules"),
            tr("%n process name(s) have more than one rule. After reorganizing, copies with different "
               "types end up in different files, which can change which copy Ananicy uses.\n\n"
               "Remove the duplicates first?", nullptr, dupNames),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        if (r == QMessageBox::Cancel)
            return;
        if (r == QMessageBox::Yes)
            removeDuplicateRules();
    }

    // Rules with "command line contains" are tried in load order and the first match wins, so
    // they must keep their order relative to each other.
    const QStringList reordered = rulematch::cmdlineOrderChanges(m_rules.entries(), [this](const EntryPtr &r) {
        return isPackaged(*r) ? r->file->path : ruleFileFor(r->str(QStringLiteral("type")));
    });
    if (!reordered.isEmpty()) {
        const auto r = QMessageBox::warning(
            this, tr("Reorganize rules"),
            tr("The rules with \"command line contains\" for these processes have different types and would end up "
               "in a different order:\n\n%1\n\nThey are tried in load order and the first one that matches wins, so "
               "a process may then get a different rule. Reorganize anyway?")
                .arg(reordered.mid(0, 15).join(QStringLiteral(", "))),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (r != QMessageBox::Yes)
            return;
    }

    QVector<EntryPtr> toMove;
    QSet<ConfFile *> sources;
    QSet<QString> targets;
    for (const auto &r : m_rules.entries()) {
        if (isPackaged(*r))
            continue; // never moved
        const QString target = ruleFileFor(r->str(QStringLiteral("type")));
        if (!r->file || QDir::cleanPath(r->file->path) != QDir::cleanPath(target)) {
            targets.insert(target);
            toMove << r;
            if (r->file)
                sources.insert(r->file);
        }
    }
    if (toMove.isEmpty()) {
        QMessageBox::information(this, tr("Reorganize rules"),
                                 tr("All rules are already in %1.").arg(m_rulesDir));
        return;
    }
    // files that will have no rules left afterwards
    int emptied = 0;
    for (ConfFile *f : sources) {
        int left = 0;
        for (const ConfLine &l : f->lines)
            if (l.entry && !toMove.contains(l.entry))
                ++left;
        if (left == 0 && f->existed)
            ++emptied;
    }
    const QString q = tr("Move %1 rules into %2/<type>.rules?\n\n"
                         "Rules without a type go to custom.rules. This creates or updates %3 files in %2, "
                         "and %4 old files that end up with no rules are deleted.\n\n"
                         "Nothing is written until you press Save.")
                          .arg(toMove.size()).arg(m_rulesDir).arg(targets.size()).arg(emptied);
    if (QMessageBox::question(this, tr("Reorganize rules"), q) != QMessageBox::Yes)
        return;

    // Re-add every rule of each receiving file in load order, so that for duplicate names the
    // copy that came last before still comes last afterwards.
    const QVector<EntryPtr> all = m_rules.entries();
    for (const auto &r : all) {
        const QString target = ruleFileFor(r->str(QStringLiteral("type")));
        if (!isPackaged(*r) && targets.contains(target))
            m_rules.moveTo(r, target, true);
    }
    m_rules.sortByLoadOrder();
    m_ruleModel->reset();
    recomputeUsage();
    updateDirtyState();
    statusBar()->showMessage(tr("Moved %1 rules into %2. Press Save to write the changes.")
                                 .arg(toMove.size()).arg(m_rulesDir), 10000);
}

void MainWindow::removeDuplicateRules()
{
    // Only rules that can never apply are removed, so ananicy behaves the same afterwards:
    //  * plain rules (no "command line contains"): every copy except the one loaded last (files
    //    load in path order, so a rule in 99-rules wins over one in 00-default),
    //  * rules with "command line contains" that an earlier rule for the same name always
    //    matches first (identical texts, or texts that contain the earlier rule's texts).
    const QVector<EntryPtr> drop = rulematch::removableDuplicates(m_rules.entries());
    if (drop.isEmpty()) {
        QMessageBox::information(this, tr("Remove duplicates"), tr("There are no duplicate rules."));
        return;
    }
    auto fileOf = [this](const EntryPtr &e) { return displayPath(*e); };
    auto typeOf = [this](const EntryPtr &e) {
        const QString t = e->str(QStringLiteral("type"));
        return t.isEmpty() ? tr("no type") : t;
    };
    QStringList preview;
    for (const auto &r : drop) {
        if (preview.size() >= 15)
            break;
        const QString n = rulematch::hasCmdline(*r) ? QStringLiteral("%1 [%2]").arg(r->str(QStringLiteral("name")),
                                                                                  rulematch::needlesText(*r))
                                                    : r->str(QStringLiteral("name"));
        if (const EntryPtr k = m_ruleAnalysis.replacedBy.value(r.get())) {
            preview << tr("%1: keep %2 (%3), remove %4").arg(n, fileOf(k), typeOf(k), fileOf(r));
        } else {
            const EntryPtr s = m_ruleAnalysis.shadowedBy.value(r.get());
            preview << tr("%1: never used, %2 [%3] matches first; remove %4")
                           .arg(n, s ? fileOf(s) : QString(), s ? rulematch::needlesText(*s) : QString(), fileOf(r));
        }
    }
    QString q = tr("%n duplicate rule(s) will be removed. They are never used, so nothing changes for any process. "
                   "For rules without \"command line contains\" the copy in the file that sorts last is kept, so "
                   "a rule in 99-rules wins over one in 00-default:", nullptr, drop.size())
                + QStringLiteral("\n\n") + preview.join(QLatin1Char('\n'));
    if (drop.size() > preview.size())
        q += QStringLiteral("\n…");
    q += QStringLiteral("\n\n") + tr("Nothing is written until you press Save.");
    if (QMessageBox::question(this, tr("Remove duplicates"), q) != QMessageBox::Yes)
        return;

    for (const auto &r : drop)
        m_rules.remove(r);
    m_ruleModel->reset();
    recomputeUsage();
    m_typeModel->allChanged();
    updateDirtyState();
    statusBar()->showMessage(tr("Removed %n duplicate rule(s). Press Save to write the changes.", nullptr, drop.size()),
                             10000);
}

void MainWindow::reorganizeTypes()
{
    const QString target = QDir::cleanPath(m_newTypesFile);
    int toMove = 0;
    QSet<ConfFile *> sources;
    for (const auto &t : m_types.entries()) {
        if (isPackaged(*t))
            continue; // never moved
        if (!t->file || QDir::cleanPath(t->file->path) != target) {
            ++toMove;
            if (t->file)
                sources.insert(t->file);
        }
    }
    if (toMove == 0) {
        QMessageBox::information(this, tr("Reorganize types"), tr("All types are already in %1.").arg(target));
        return;
    }
    QStringList files;
    for (ConfFile *f : sources)
        files << QDir(m_root).relativeFilePath(f->path);
    files.sort();
    const QString q = tr("Move %1 types into %2?\n\n"
                         "These files will have no types left and are deleted on save "
                         "(comments in them are not moved):\n%3\n\n"
                         "Nothing is written until you press Save.")
                          .arg(toMove).arg(target, files.join(QLatin1Char('\n')));
    if (QMessageBox::question(this, tr("Reorganize types"), q) != QMessageBox::Yes)
        return;

    // Re-add all types in load order, so that if a type is defined twice the copy that
    // came last before still comes last (and wins) afterwards.
    const QVector<EntryPtr> all = m_types.entries();
    for (const auto &t : all)
        m_types.moveTo(t, target, true);
    m_types.sortByLoadOrder();
    m_typeModel->reset();
    updateDirtyState();
    statusBar()->showMessage(tr("Moved %1 types into %2. Press Save to write the changes.").arg(toMove).arg(target),
                             10000);
}
