#include "processdialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

struct ProcGroup {
    QString comm;
    QStringList pids;
    QString exe;
    QString cmdline;
    QStringList users;
};

QByteArray readSmall(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.read(64 * 1024);
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

enum Col { ColName, ColCount, ColUser, ColRule, ColExe, ColCmd };

} // namespace

ProcessDialog::ProcessDialog(std::function<QString(const QString &)> existingRule, QWidget *parent)
    : QDialog(parent), m_existing(std::move(existingRule))
{
    setWindowTitle(tr("Select running process"));
    resize(1000, 600);

    auto *lay = new QVBoxLayout(this);
    auto *info = new QLabel(tr("Ananicy matches rules against the process name (<tt>/proc/PID/comm</tt>), "
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
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setSortingEnabled(true);
    m_list->setAlternatingRowColors(true);
    m_list->setHeaderLabels({tr("Name (comm)"), tr("Instances"), tr("User"), tr("Existing rule"),
                             tr("Executable"), tr("Command line")});
    m_list->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_list->header()->setStretchLastSection(true);
    lay->addWidget(m_list, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Create rule"));
    lay->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &ProcessDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this] { accept(); });
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
        QByteArray cmd = readSmall(base + QStringLiteral("/cmdline"));
        const bool kernel = cmd.isEmpty();
        if (kernel && m_hideKernel->isChecked())
            continue;
        cmd.replace('\0', ' ');

        ProcGroup &g = groups[comm];
        g.comm = comm;
        g.pids << pid;
        const QString user = userOf(base);
        if (!user.isEmpty() && !g.users.contains(user))
            g.users << user;
        if (g.exe.isEmpty())
            g.exe = QFileInfo(base + QStringLiteral("/exe")).symLinkTarget();
        if (g.cmdline.isEmpty())
            g.cmdline = QString::fromLocal8Bit(cmd).simplified();
    }

    m_list->setSortingEnabled(false);
    m_list->clear();
    for (const ProcGroup &g : groups) {
        auto *it = new QTreeWidgetItem;
        it->setText(ColName, g.comm);
        it->setData(ColCount, Qt::DisplayRole, g.pids.size());
        it->setText(ColUser, g.users.join(QStringLiteral(", ")));
        it->setText(ColRule, m_existing ? m_existing(g.comm) : QString());
        it->setText(ColExe, g.exe);
        it->setText(ColCmd, g.cmdline);
        it->setToolTip(ColName, tr("PIDs: %1").arg(g.pids.join(QStringLiteral(", "))));
        it->setToolTip(ColCmd, g.cmdline);
        m_list->addTopLevelItem(it);
    }
    m_list->setSortingEnabled(true);
    m_list->sortByColumn(ColName, Qt::AscendingOrder);
    for (int c = 0; c < ColCmd; ++c)
        m_list->resizeColumnToContents(c);
    m_list->setColumnWidth(ColExe, qMin(m_list->columnWidth(ColExe), 250));
    applyFilter();
}

void ProcessDialog::applyFilter()
{
    const QString f = m_filter->text().trimmed();
    QTreeWidgetItem *firstVisible = nullptr;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *it = m_list->topLevelItem(i);
        bool show = f.isEmpty() || it->text(ColName).contains(f, Qt::CaseInsensitive)
                    || it->text(ColExe).contains(f, Qt::CaseInsensitive)
                    || it->text(ColCmd).contains(f, Qt::CaseInsensitive);
        if (m_hideRuled->isChecked() && !it->text(ColRule).isEmpty())
            show = false;
        it->setHidden(!show);
        if (show && !firstVisible)
            firstVisible = it;
    }
    QTreeWidgetItem *cur = m_list->currentItem();
    if ((!cur || cur->isHidden()) && firstVisible)
        m_list->setCurrentItem(firstVisible);
}

void ProcessDialog::accept()
{
    QTreeWidgetItem *it = m_list->currentItem();
    if (!it || it->isHidden())
        return;
    m_selected = it->text(ColName);
    QDialog::accept();
}
