#include "processdialog.h"

#include "rulematch.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <pwd.h>
#include <sys/stat.h>

namespace {

struct ProcInstance {        // processes with the same command line
    QStringList argv;
    QString cmdline;         // arguments joined by single spaces, as ananicy-cpp matches it
    QStringList pids;
    QStringList users;
};

struct ProcGroup {
    QString name;
    QStringList pids;
    QString exe;
    QStringList users;
    QMap<QString, ProcInstance> instances; // by cmdline
};

QByteArray readSmall(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.read(64 * 1024);
}

QStringList splitArgv(const QByteArray &raw)
{
    QStringList argv;
    for (const QByteArray &a : raw.split('\0'))
        argv << QString::fromLocal8Bit(a);
    while (!argv.isEmpty() && argv.last().isEmpty()) // terminator / setproctitle padding
        argv.removeLast();
    return argv;
}

QString userOf(const QString &path)
{
    struct stat st {};
    if (::stat(QFile::encodeName(path).constData(), &st) != 0)
        return {};
    if (passwd *pw = ::getpwuid(st.st_uid))
        return QString::fromLocal8Bit(pw->pw_name);
    return QString::number(st.st_uid);
}

void addUnique(QStringList &l, const QString &v)
{
    if (!v.isEmpty() && !l.contains(v))
        l << v;
}

enum Col { ColName, ColCount, ColUser, ColRule, ColExe };
enum Role { ArgvRole = Qt::UserRole, SpecificRole, NameRole };

} // namespace

ProcessDialog::ProcessDialog(Describe describe, bool ananicyCpp, QWidget *parent)
    : QDialog(parent), m_describe(std::move(describe)), m_ananicyCpp(ananicyCpp)
{
    setWindowTitle(tr("Select running process"));
    resize(1000, 600);

    auto *lay = new QVBoxLayout(this);
    auto *info = new QLabel(
        ananicyCpp
            ? tr("ananicy-cpp matches rules against the program name (the file name of <tt>argv[0]</tt>), shown in "
                 "the first column. Pick a name for a rule that applies to all of its processes, or unfold it and pick "
                 "one command line for a rule that only applies to processes with that command line "
                 "(<i>command line contains</i>, e.g. one Java or Python program). You can still edit everything "
                 "afterwards.")
            : tr("Ananicy matches rules against the process name (<tt>/proc/PID/comm</tt>), "
                 "shown in the first column. You can still edit the name afterwards."));
    info->setWordWrap(true);
    lay->addWidget(info);

    auto *top = new QHBoxLayout;
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("Filter by name, executable or command line…"));
    m_filter->setClearButtonEnabled(true);
    m_hideKernel = new QCheckBox(tr("Hide kernel threads"));
    m_hideKernel->setChecked(true);
    m_hideRuled = new QCheckBox(tr("Hide processes that already have a rule"));
    auto *refresh = new QPushButton(tr("Refresh"));
    top->addWidget(m_filter, 1);
    top->addWidget(m_hideKernel);
    top->addWidget(m_hideRuled);
    top->addWidget(refresh);
    lay->addLayout(top);

    m_list = new QTreeWidget;
    m_list->setRootIsDecorated(ananicyCpp);
    m_list->setUniformRowHeights(true);
    m_list->setSortingEnabled(true);
    m_list->setAlternatingRowColors(true);
    m_list->setTextElideMode(Qt::ElideMiddle); // keep both the program and the trailing arguments visible
    m_list->setHeaderLabels({ananicyCpp ? tr("Name (argv[0])") : tr("Name (comm)"), tr("Instances"), tr("User"),
                             tr("Existing rule"), tr("Executable")});
    m_list->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_list->header()->setStretchLastSection(true);
    lay->addWidget(m_list, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    m_ok->setText(tr("Create rule"));
    // fixed width: the text changes with the selection and must not resize the dialog
    m_ok->setFixedWidth(qMax(m_ok->fontMetrics().horizontalAdvance(tr("Create rule for this command line")),
                             m_ok->fontMetrics().averageCharWidth() * 48)
                        + m_ok->iconSize().width() + 40);
    lay->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &ProcessDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this] { accept(); });
    connect(m_list, &QTreeWidget::currentItemChanged, this, &ProcessDialog::updateOkButton);
    connect(refresh, &QPushButton::clicked, this, &ProcessDialog::reload);
    connect(m_hideKernel, &QCheckBox::toggled, this, &ProcessDialog::reload);
    connect(m_hideRuled, &QCheckBox::toggled, this, &ProcessDialog::applyFilter);
    connect(m_filter, &QLineEdit::textChanged, this, &ProcessDialog::applyFilter);

    reload();
    m_filter->setFocus();
}

void ProcessDialog::reload()
{
    QMap<QString, ProcGroup> groups;
    const QStringList dirs = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &pid : dirs) {
        bool isNum = false;
        pid.toInt(&isNum);
        if (!isNum)
            continue;
        const QString base = QStringLiteral("/proc/") + pid;
        const QString comm = QString::fromLocal8Bit(readSmall(base + QStringLiteral("/comm"))).trimmed();
        if (comm.isEmpty())
            continue;
        const QByteArray raw = readSmall(base + QStringLiteral("/cmdline"));
        const bool kernel = raw.isEmpty();
        if (kernel && m_hideKernel->isChecked())
            continue;
        const QStringList argv = splitArgv(raw);
        const QString exe = QFileInfo(base + QStringLiteral("/exe")).symLinkTarget();
        const QString name = m_ananicyCpp ? rulematch::processName(argv, exe, comm) : comm;
        if (name.isEmpty())
            continue;

        ProcGroup &g = groups[name];
        g.name = name;
        g.pids << pid;
        const QString user = userOf(base);
        addUnique(g.users, user);
        if (g.exe.isEmpty())
            g.exe = exe;
        if (!kernel) {
            const QString cmdline = argv.join(QLatin1Char(' '));
            ProcInstance &in = g.instances[cmdline];
            in.argv = argv;
            in.cmdline = cmdline;
            in.pids << pid;
            addUnique(in.users, user);
        }
    }

    m_list->setSortingEnabled(false);
    m_list->clear();
    for (const ProcGroup &g : groups) {
        auto *it = new QTreeWidgetItem;
        it->setText(ColName, g.name);
        it->setData(ColName, NameRole, g.name);
        it->setData(ColName, SpecificRole, false);
        it->setData(ColCount, Qt::DisplayRole, g.pids.size());
        it->setText(ColUser, g.users.join(QStringLiteral(", ")));
        it->setText(ColRule, m_describe ? m_describe(g.name, nullptr) : QString());
        it->setText(ColExe, g.exe);
        it->setToolTip(ColName, tr("PIDs: %1").arg(g.pids.join(QStringLiteral(", "))));
        if (m_ananicyCpp) {
            for (const ProcInstance &in : g.instances) {
                auto *child = new QTreeWidgetItem(it);
                child->setData(ColName, NameRole, g.name);
                child->setData(ColName, SpecificRole, true);
                child->setData(ColName, ArgvRole, in.argv);
                // show the command line itself, so the rows of one name can be told apart
                child->setText(ColName, in.cmdline.simplified());
                child->setData(ColCount, Qt::DisplayRole, in.pids.size());
                child->setText(ColUser, in.users.join(QStringLiteral(", ")));
                child->setText(ColRule, m_describe ? m_describe(g.name, &in.cmdline) : QString());
                child->setToolTip(ColName, tr("Only processes with this command line:\n%1\n\nPIDs: %2")
                                               .arg(in.cmdline, in.pids.join(QStringLiteral(", "))));
            }
        }
        m_list->addTopLevelItem(it);
    }
    m_list->setSortingEnabled(true);
    m_list->sortByColumn(ColName, Qt::AscendingOrder);
    for (int c = 0; c < ColExe; ++c)
        m_list->resizeColumnToContents(c);
    // one very long argv[0] (Wine, setproctitle) must not push the other columns off-screen
    m_list->setColumnWidth(ColName, qBound(200, m_list->columnWidth(ColName), 420));
    applyFilter();
}

void ProcessDialog::applyFilter()
{
    const QString f = m_filter->text().trimmed();
    auto textMatch = [&](const QTreeWidgetItem *it) {
        return f.isEmpty() || it->text(ColName).contains(f, Qt::CaseInsensitive)
               || it->text(ColExe).contains(f, Qt::CaseInsensitive);
    };
    QTreeWidgetItem *firstVisible = nullptr;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = m_list->topLevelItem(i);
        const bool groupMatch = textMatch(it);
        bool anyChild = false;
        for (int c = 0; c < it->childCount(); ++c) {
            QTreeWidgetItem *ch = it->child(c);
            bool show = groupMatch || textMatch(ch);
            if (m_hideRuled->isChecked() && !ch->text(ColRule).isEmpty())
                show = false;
            ch->setHidden(!show);
            anyChild = anyChild || show;
        }
        bool show = groupMatch || anyChild;
        if (m_hideRuled->isChecked() && !it->text(ColRule).isEmpty() && !anyChild)
            show = false;
        it->setHidden(!show);
        // a filter that only matches a command line unfolds its name
        it->setExpanded(show && !groupMatch && anyChild && !f.isEmpty());
        if (show && !firstVisible)
            firstVisible = it;
    }
    QTreeWidgetItem *cur = m_list->currentItem();
    if ((!cur || cur->isHidden() || (cur->parent() && cur->parent()->isHidden())) && firstVisible)
        m_list->setCurrentItem(firstVisible);
    updateOkButton();
}

void ProcessDialog::updateOkButton()
{
    const QTreeWidgetItem *it = m_list->currentItem();
    if (!m_ok)
        return;
    m_ok->setToolTip(QString());
    if (it && it->data(ColName, SpecificRole).toBool()) {
        m_ok->setText(tr("Create rule for this command line"));
    } else if (it && m_ananicyCpp) {
        const QString pattern = tr("Create rule for all \"%1\" processes");
        const QString name = it->data(ColName, NameRole).toString();
        const QFontMetrics fm = m_ok->fontMetrics();
        const int room = m_ok->width() - m_ok->iconSize().width() - 48 - fm.horizontalAdvance(pattern.arg(QString()));
        m_ok->setText(pattern.arg(fm.elidedText(name, Qt::ElideMiddle, qMax(room, 0))));
        m_ok->setToolTip(name);
    } else {
        m_ok->setText(tr("Create rule"));
    }
}

void ProcessDialog::accept()
{
    QTreeWidgetItem *it = m_list->currentItem();
    if (!it || it->isHidden())
        return;
    m_sel.name = it->data(ColName, NameRole).toString();
    m_sel.specific = it->data(ColName, SpecificRole).toBool();
    m_sel.argv = it->data(ColName, ArgvRole).toStringList();
    QDialog::accept();
}
