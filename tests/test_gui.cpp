// Drives the real MainWindow on a temporary configuration directory (QT_QPA_PLATFORM=offscreen).
#include "entrydialog.h"
#include "mainwindow.h"
#include "processdialog.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

#include <functional>

namespace {

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
}

// Runs fn on the next modal widget of type T (polls, so it also works for nested dialogs).
template <typename T>
void whenModal(std::function<void(T *)> fn)
{
    auto *t = new QTimer;
    t->setInterval(20);
    auto *clock = new QElapsedTimer;
    clock->start();
    QObject::connect(t, &QTimer::timeout, [t, clock, fn] {
        if (T *w = qobject_cast<T *>(QApplication::activeModalWidget())) {
            t->stop();
            t->deleteLater();
            delete clock;
            fn(w);
        } else if (clock->elapsed() > 8000) {
            t->stop();
            t->deleteLater();
            delete clock;
            if (QWidget *w = QApplication::activeModalWidget())
                w->close(); // do not hang the test
            QFAIL("expected dialog did not appear");
        }
    });
    t->start();
}

// Answers the next message box; returns its text through *text.
void answer(QMessageBox::StandardButton b, QString *text = nullptr)
{
    whenModal<QMessageBox>([b, text](QMessageBox *box) {
        if (text)
            *text = box->text();
        box->button(b)->click();
    });
}

// ANANICY_GUI_SCREENSHOTS=<dir>: save pictures of the dialogs (for checking the layout / docs)
void screenshot(QWidget *w, const QString &file)
{
    const QString dir = qEnvironmentVariable("ANANICY_GUI_SCREENSHOTS");
    if (dir.isEmpty())
        return;
    QApplication::processEvents();
    w->grab().save(dir + QLatin1Char('/') + file);
}

QPushButton *button(QWidget *root, const QString &label, int nth = 0)
{
    int i = 0;
    for (QPushButton *b : root->findChildren<QPushButton *>())
        if (b->text() == label && i++ == nth)
            return b;
    return nullptr;
}

// The rules section's own button with this label (the types section has buttons with the same text).
QPushButton *ruleButton(QWidget *root, const QString &label)
{
    QPushButton *anchor = button(root, QStringLiteral("Add from running process…"));
    if (!anchor)
        return nullptr;
    for (QPushButton *b : anchor->parentWidget()->findChildren<QPushButton *>())
        if (b->text() == label && b->parentWidget() == anchor->parentWidget()
            && b->mapTo(root, QPoint()).y() == anchor->mapTo(root, QPoint()).y()) // same button row
            return b;
    return nullptr;
}

QTableView *ruleView(QWidget *root)
{
    for (QTableView *v : root->findChildren<QTableView *>())
        if (v->model() && v->model()->headerData(1, Qt::Horizontal).toString() == QLatin1String("Command line contains"))
            return v;
    return nullptr;
}

struct Row {
    QString name, cmdline, type, warning;
};
QVector<Row> rows(QTableView *v)
{
    QVector<Row> out;
    QAbstractItemModel *m = v->model();
    for (int r = 0; r < m->rowCount(); ++r) {
        Row row{m->index(r, 0).data().toString(), m->index(r, 1).data().toString(), m->index(r, 2).data().toString(), {}};
        const QString tip = m->index(r, 0).data(Qt::ToolTipRole).toString();
        row.warning = m->index(r, 0).data(Qt::ForegroundRole).isValid() ? tip : QString();
        out << row;
    }
    return out;
}
Row find(const QVector<Row> &rs, const QString &name, const QString &cmdline, const QString &type = QString())
{
    for (const Row &r : rs)
        if (r.name == name && r.cmdline == cmdline && (type.isEmpty() || r.type == type))
            return r;
    return Row{"<not found>", {}, {}, {}};
}

void selectRow(QTableView *v, const QString &name, const QString &cmdline)
{
    QAbstractItemModel *m = v->model();
    for (int r = 0; r < m->rowCount(); ++r)
        if (m->index(r, 0).data().toString() == name && m->index(r, 1).data().toString() == cmdline) {
            v->setCurrentIndex(m->index(r, 0));
            v->selectRow(r);
            return;
        }
    QFAIL("row not found");
}

// Text of the rules table's "Source" column for a rule ("<not shown>" if it has no row).
QString sourceOf(QTableView *v, const QString &name, const QString &cmdline)
{
    QAbstractItemModel *m = v->model();
    int col = -1;
    for (int c = 0; c < m->columnCount(); ++c)
        if (m->headerData(c, Qt::Horizontal).toString() == QLatin1String("Source"))
            col = c;
    QString out = QStringLiteral("<not shown>");
    for (int r = 0; r < m->rowCount(); ++r)
        if (m->index(r, 0).data().toString() == name && m->index(r, 1).data().toString() == cmdline)
            out = out == QLatin1String("<not shown>") ? m->index(r, col).data().toString() : QStringLiteral("<twice>");
    return out;
}

} // namespace

class TestGui : public QObject {
    Q_OBJECT
    QTemporaryDir home, conf;
    QString root, supported, unsupported;

    // packaged rules (pkg, like /usr/share/ananicy.d) and changes to them (etc, like /etc/ananicy.d)
    QString pkg, etc;
    QByteArray pkgRules;
    void packagedSetup()
    {
        pkg = conf.path() + "/usr-share/ananicy.d";
        etc = conf.path() + "/etc/ananicy.d";
        QDir(pkg).removeRecursively(); // every test starts from the same files
        QDir(etc).removeRecursively();
        writeFile(pkg + "/ananicy.conf", "check_freq = 10\napply_nice = false\nmy_key = 1\n");
        writeFile(pkg + "/99-types.types", "{\"type\": \"Game\", \"nice\": -5}\n{\"type\": \"Heavy\", \"nice\": 10}\n");
        pkgRules = "{\"name\": \"steam\", \"type\": \"Game\"}\n"
                   "{\"name\": \"lutris\", \"type\": \"Game\"}\n"
                   "{\"name\": \"heroic\", \"type\": \"Game\"}\n"
                   "{\"name\": \"java\", \"cmdline_contains\": \"minecraft\", \"type\": \"Game\"}\n";
        writeFile(pkg + "/99-rules/Game.rules", pkgRules);
        writeFile(etc + "/99-rules/Game.rules", "{\"name\": \"lutris\", \"type\": \"Heavy\"}\n{\"name\": \"heroic\"}\n");
        writeFile(etc + "/99-rules/custom.rules", "{\"name\": \"mine\", \"nice\": 1}\n");
    }

private slots:
    void initTestCase()
    {
        QVERIFY(home.isValid() && conf.isValid());
        qputenv("XDG_CONFIG_HOME", home.path().toUtf8()); // QSettings of the app
        QSettings().setValue(QStringLiteral("service"), QStringLiteral("ananicy-cpp"));
        // stand-ins for the daemon: the configurator looks for "cmdline_contains" in the program
        supported = conf.path() + "/ananicy-cpp-new";
        unsupported = conf.path() + "/ananicy-cpp-old";
        writeFile(supported, QByteArray("\x7f" "ELF ... cmdline_contains ... name_regex"));
        writeFile(unsupported, QByteArray("\x7f" "ELF ... name_regex"));
        qputenv("ANANICY_CPP_BINARY", supported.toUtf8());
        root = conf.path() + QStringLiteral("/ananicy.d");
        writeFile(root + "/ananicy.conf", "check_freq = 15\napply_nice = true\n");
        writeFile(root + "/00-types.types", "{\"type\": \"Plain\", \"nice\": 1}\n{\"type\": \"Game\", \"nice\": -5}\n"
                                            "{\"type\": \"Ci\", \"nice\": 10}\n");
        writeFile(root + "/00-default/apps.rules", "# shipped rules\n{\"name\": \"java\", \"type\": \"Plain\"}\n"
                                                   "{\"name\": \"gcc\", \"type\": \"Plain\"} # old\n");
        writeFile(root + "/99-rules/game.rules",
                  "{\"name\": \"java\", \"cmdline_contains\": \"minecraft\", \"type\": \"Game\"}\n"
                  "{\"name\": \"java\", \"cmdline_contains\": \"minecraft-server\", \"type\": \"Game\"}\n"
                  "{\"name\": \"java\",   \"cmdline_contains\": [\"-jar\", \"jenkins.war\"], \"type\": \"Game\"}\n"
                  "{\"name\": \"gcc\", \"type\": \"Game\"}\n"
                  "{\"name\": \"java\", \"cmdline_contains\": [], \"type\": \"Game\"}\n");
    }

    void tableShowsCmdlineAndWarnings()
    {
        MainWindow w(root, QString());
        QTableView *v = ruleView(&w);
        QVERIFY(v);
        const auto rs = rows(v);
        QCOMPARE(rs.size(), 7);
        QCOMPARE(find(rs, "java", "").warning, QString());
        QCOMPARE(find(rs, "java", "minecraft").warning, QString());
        QCOMPARE(find(rs, "java", "-jar + jenkins.war").warning, QString());
        QVERIFY2(find(rs, "java", "minecraft-server").warning.startsWith("Never used"),
                 qPrintable(find(rs, "java", "minecraft-server").warning));
        QVERIFY(find(rs, "gcc", "", "Plain").warning.startsWith("Duplicate"));
        QVERIFY(find(rs, "gcc", "", "Game").warning.startsWith("Duplicate"));
        QVERIFY(find(rs, "java", "⚠ []").warning.startsWith("Invalid"));
    }

    void unsupportedDaemonWarns()
    {
        qputenv("ANANICY_CPP_BINARY", unsupported.toUtf8());
        MainWindow w(root, QString());
        const auto rs = rows(ruleView(&w));
        QVERIFY(find(rs, "java", "minecraft").warning.contains("does not support"));
        QCOMPARE(find(rs, "java", "").warning, QString()); // plain rules are fine
        qputenv("ANANICY_CPP_BINARY", supported.toUtf8());
    }

    void removeDuplicatesAndSave()
    {
        const QByteArray gameBefore = readFile(root + "/99-rules/game.rules");
        MainWindow w(root, QString());
        QString q;
        answer(QMessageBox::Yes, &q);
        button(&w, "Remove duplicates…")->click();
        QVERIFY2(q.contains("2 duplicate rule(s)"), qPrintable(q));
        QVERIFY(q.contains("minecraft-server") && q.contains("never used"));
        button(&w, "Save")->click();
        // 00-default lost the plain gcc copy (99-rules wins), game.rules the unreachable rule
        QCOMPARE(readFile(root + "/00-default/apps.rules"),
                 QByteArray("# shipped rules\n{\"name\": \"java\", \"type\": \"Plain\"}\n"));
        QByteArray expect = gameBefore;
        expect.replace("{\"name\": \"java\", \"cmdline_contains\": \"minecraft-server\", \"type\": \"Game\"}\n", "");
        QCOMPARE(readFile(root + "/99-rules/game.rules"), expect); // other lines byte-for-byte
    }

    void addRuleWithCmdline()
    {
        MainWindow w(root, QString());
        QString q;
        whenModal<EntryDialog>([&](EntryDialog *d) {
            auto *name = d->findChildren<QLineEdit *>().first();
            name->setText("java");
            emit name->textEdited("java");
            auto *cmd = d->findChild<QPlainTextEdit *>();
            QVERIFY(cmd);
            cmd->setPlainText("jenkins.war\n-jar"); // same texts as an existing rule, other order
            d->findChildren<QComboBox *>().first()->setCurrentText("Ci");
            answer(QMessageBox::No, &q); // "same command line texts already exists" -> No: stay
            QTimer::singleShot(0, d, [d] { d->accept(); });
            whenModal<EntryDialog>([&](EntryDialog *d2) { // still open after "No"
                d2->findChild<QPlainTextEdit *>()->setPlainText("jenkins.war\n--httpPort\n\n");
                d2->accept();
            });
        });
        button(&w, "Add rule…")->click();
        QVERIFY2(q.contains("same command line texts"), qPrintable(q));
        const auto rs = rows(ruleView(&w));
        QCOMPARE(find(rs, "java", "jenkins.war + --httpPort").type, QString("Ci"));
        button(&w, "Save")->click();
        QCOMPARE(readFile(root + "/99-rules/Ci.rules"),
                 QByteArray("{ \"name\": \"java\", \"cmdline_contains\": [\"jenkins.war\",\"--httpPort\"], \"type\": \"Ci\" }\n"));
    }

    void editCmdlineToStringAndToPlain()
    {
        MainWindow w(root, QString());
        QTableView *v = ruleView(&w);
        selectRow(v, "java", "jenkins.war + --httpPort");
        whenModal<EntryDialog>([](EntryDialog *d) {
            d->findChild<QPlainTextEdit *>()->setPlainText("  jenkins.war  ");
            d->accept();
        });
        QVERIFY(ruleButton(&w, "Edit…"));
        ruleButton(&w, "Edit…")->click();
        button(&w, "Save")->click();
        QCOMPARE(readFile(root + "/99-rules/Ci.rules"),
                 QByteArray("{ \"name\": \"java\", \"cmdline_contains\": \"jenkins.war\", \"type\": \"Ci\" }\n"));

        selectRow(v, "java", "jenkins.war");
        QString q;
        whenModal<EntryDialog>([&](EntryDialog *d) {
            d->findChild<QPlainTextEdit *>()->clear(); // now a plain java rule: competes with 00-default's
            answer(QMessageBox::Yes, &q);
            QTimer::singleShot(0, d, [d] { d->accept(); });
        });
        ruleButton(&w, "Edit…")->click();
        QVERIFY2(q.contains("loaded last"), qPrintable(q));
        button(&w, "Save")->click();
        QCOMPARE(readFile(root + "/99-rules/Ci.rules"), QByteArray("{ \"name\": \"java\", \"type\": \"Ci\" }\n"));
    }

    void reorganizeWarnsAboutOrder()
    {
        // game.rules: java/minecraft (Game) ... ; add a Ci cmdline rule *after* it in game.rules order:
        writeFile(root + "/99-rules/zz.rules",
                  "{\"name\": \"java\", \"cmdline_contains\": \"mine\", \"type\": \"Ci\"}\n");
        MainWindow w(root, QString());
        QString q;
        answer(QMessageBox::No);      // "remove duplicates first?" (two plain java rules now)
        answer(QMessageBox::Cancel, &q);
        button(&w, "Reorganize into 99-rules/…")->click();
        QVERIFY2(q.contains("different order") && q.contains("java"), qPrintable(q));
        QVERIFY(!w.windowTitle().endsWith(" *")); // cancelled: nothing changed
        QFile::remove(root + "/99-rules/zz.rules");
    }

    // ---- packaged rules (/usr/share/ananicy.d) with changes in /etc/ananicy.d

    void packagedLayerIsShownMerged()
    {
        packagedSetup();
        MainWindow w(etc, pkg);
        QTableView *v = ruleView(&w);
        QCOMPARE(sourceOf(v, "steam", ""), QString("Packaged"));
        QCOMPARE(sourceOf(v, "lutris", ""), QString("Changed")); // one row: the packaged one is replaced
        QCOMPARE(find(rows(v), "lutris", "").type, QString("Heavy"));
        QCOMPARE(sourceOf(v, "heroic", ""), QString("Off"));
        QCOMPARE(sourceOf(v, "java", "minecraft"), QString("Packaged"));
        QCOMPARE(sourceOf(v, "mine", ""), QString("Yours"));
        for (const Row &r : rows(v))
            QVERIFY2(r.warning.isEmpty(), qPrintable(r.name + ": " + r.warning)); // overrides are not duplicates
        QString text;
        whenModal<QMessageBox>([&](QMessageBox *b) {
            text = b->text();
            b->accept();
        });
        button(&w, "Remove duplicates…")->click();
        QCOMPARE(text, QString("There are no duplicate rules."));
        QVERIFY(!w.windowTitle().endsWith(" *"));
    }

    void editingPackagedRuleSavesOverride()
    {
        packagedSetup();
        MainWindow w(etc, pkg);
        QTableView *v = ruleView(&w);
        selectRow(v, "steam", "");
        whenModal<EntryDialog>([](EntryDialog *d) {
            d->findChildren<QComboBox *>().first()->setCurrentText("Heavy");
            d->accept();
        });
        ruleButton(&w, "Edit…")->click();
        QCOMPARE(sourceOf(v, "steam", ""), QString("Changed"));
        button(&w, "Save")->click();
        QCOMPARE(readFile(pkg + "/99-rules/Game.rules"), pkgRules); // never written
        QCOMPARE(readFile(etc + "/99-rules/Game.rules"),
                 QByteArray("{\"name\": \"lutris\", \"type\": \"Heavy\"}\n{\"name\": \"heroic\"}\n"
                            "{ \"name\": \"steam\", \"type\": \"Heavy\" }\n"));
    }

    void deletingPackagedRuleSwitchesItOffAndResetUndoes()
    {
        packagedSetup();
        MainWindow w(etc, pkg);
        QTableView *v = ruleView(&w);
        selectRow(v, "java", "minecraft");
        QString q;
        QPushButton *del = ruleButton(&w, "Delete"), *reset = ruleButton(&w, "Reset to packaged");
        QVERIFY2(del, "Delete");
        QVERIFY2(reset, "Reset to packaged");
        answer(QMessageBox::Yes, &q);
        del->click();
        QVERIFY2(q.startsWith("Switch off the packaged rule"), qPrintable(q));
        QCOMPARE(sourceOf(v, "java", "minecraft"), QString("Off"));
        button(&w, "Save")->click();
        const QByteArray withOff = readFile(etc + "/99-rules/Game.rules");
        QVERIFY2(withOff.endsWith("{ \"name\": \"java\", \"cmdline_contains\": \"minecraft\" }\n"), withOff.constData());

        selectRow(v, "java", "minecraft");
        reset->click();
        QCOMPARE(sourceOf(v, "java", "minecraft"), QString("Packaged"));
        selectRow(v, "lutris", "");
        reset->click();
        QCOMPARE(sourceOf(v, "lutris", ""), QString("Packaged"));
        QCOMPARE(find(rows(v), "lutris", "").type, QString("Game"));
        button(&w, "Save")->click();
        QCOMPARE(readFile(etc + "/99-rules/Game.rules"), QByteArray("{\"name\": \"heroic\"}\n"));
        QCOMPARE(readFile(pkg + "/99-rules/Game.rules"), pkgRules);
    }

    void packagedTypeSwitchOffAndReset()
    {
        packagedSetup();
        MainWindow w(etc, pkg);
        screenshot(&w, "main-window-packaged.png");
        QTableView *types = nullptr;
        for (QTableView *v : w.findChildren<QTableView *>())
            if (v->model() && v->model()->headerData(0, Qt::Horizontal).toString() == QLatin1String("Type"))
                types = v;
        QVERIFY(types);
        QAbstractItemModel *m = types->model();
        int heavy = -1, sourceCol = -1;
        for (int c = 0; c < m->columnCount(); ++c)
            if (m->headerData(c, Qt::Horizontal).toString() == QLatin1String("Source"))
                sourceCol = c;
        for (int r = 0; r < m->rowCount(); ++r)
            if (m->index(r, 0).data().toString() == QLatin1String("Heavy"))
                heavy = r;
        QVERIFY(heavy >= 0 && sourceCol >= 0);
        QCOMPARE(m->index(heavy, sourceCol).data().toString(), QString("Packaged"));
        types->setCurrentIndex(m->index(heavy, 0));

        // the types section's buttons come after the rules section's ones
        QPushButton *del = button(&w, "Delete", 1), *reset = button(&w, "Reset to packaged", 1);
        QVERIFY(del && reset);
        QString q;
        answer(QMessageBox::Yes, &q);
        del->click();
        QVERIFY2(q.startsWith("Switch off the packaged type \"Heavy\""), qPrintable(q));
        QVERIFY2(q.contains("1 rule(s) use this type"), qPrintable(q)); // lutris (changed to Heavy)
        button(&w, "Save")->click();
        QCOMPARE(readFile(etc + "/99-types.types"), QByteArray("{ \"type\": \"Heavy\" }\n"));

        for (int r = 0; r < m->rowCount(); ++r)
            if (m->index(r, 0).data().toString() == QLatin1String("Heavy")) {
                QCOMPARE(m->index(r, sourceCol).data().toString(), QString("Off"));
                types->setCurrentIndex(m->index(r, 0));
            }
        reset->click();
        QString del2;
        answer(QMessageBox::Yes, &del2); // "Saving will delete 1 file(s) that no longer contain any rules or types"
        button(&w, "Save")->click();
        QVERIFY2(del2.contains("delete 1 file"), qPrintable(del2));
        QVERIFY(!QFile::exists(etc + "/99-types.types")); // nothing left: the file is removed
        QCOMPARE(readFile(pkg + "/99-types.types"),
                 QByteArray("{\"type\": \"Game\", \"nice\": -5}\n{\"type\": \"Heavy\", \"nice\": 10}\n"));
    }

    void packagedConfShowsDefaultsAndSavesOnlyChanges()
    {
        packagedSetup();
        MainWindow w(etc, pkg);
        QComboBox *applyNice = nullptr;
        for (QComboBox *c : w.findChildren<QComboBox *>())
            if (c->count() > 0 && c->itemText(0) == QLatin1String("(packaged: false)"))
                applyNice = c;
        QVERIFY(applyNice);
        QCOMPARE(applyNice->currentIndex(), 0); // nothing set in /etc: the packaged value applies
        applyNice->setCurrentIndex(applyNice->findText("true"));
        button(&w, "Save")->click();
        QCOMPARE(readFile(etc + "/ananicy.conf"), QByteArray("apply_nice=true\n"));
        QVERIFY(readFile(pkg + "/ananicy.conf").startsWith("check_freq = 10"));
        applyNice->setCurrentIndex(0); // back to the packaged value: the key is removed again
        button(&w, "Save")->click();
        QCOMPARE(readFile(etc + "/ananicy.conf"), QByteArray(""));
    }

    void addFromRunningProcess()
    {
        const QString script = conf.path() + "/cctest_gui_app.py";
        writeFile(script, "import time\ntime.sleep(30)\n");
        QProcess p;
        p.start("python3", {script, "--port", "8080"});
        QVERIFY(p.waitForStarted());
        QTest::qWait(300);

        MainWindow w(root, QString());
        QString ruleCol, okText;
        whenModal<ProcessDialog>([&](ProcessDialog *d) {
            auto *tree = d->findChild<QTreeWidget *>();
            QTreeWidgetItem *hit = nullptr;
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                QTreeWidgetItem *g = tree->topLevelItem(i);
                if (g->text(0) != "python3")
                    continue;
                for (int c = 0; c < g->childCount(); ++c)
                    if (g->child(c)->text(5).contains("cctest_gui_app.py"))
                        hit = g->child(c);
            }
            QVERIFY2(hit, "python3 / cctest_gui_app.py not listed");
            hit->parent()->setExpanded(true);
            tree->setCurrentItem(hit);
            tree->scrollToItem(hit);
            screenshot(d, "process-dialog.png");
            ruleCol = hit->text(3);
            okText = d->findChildren<QPushButton *>().first()->text();
            for (QPushButton *b : d->findChildren<QPushButton *>())
                if (b->text().startsWith("Create rule"))
                    okText = b->text();
            whenModal<EntryDialog>([](EntryDialog *e) {
                QCOMPARE(e->findChildren<QLineEdit *>().first()->text(), QString("python3"));
                QCOMPARE(e->findChild<QPlainTextEdit *>()->toPlainText(), QString("cctest_gui_app.py"));
                screenshot(e, "rule-dialog-cmdline.png");
                e->reject();
            });
            QTimer::singleShot(0, d, [d] { static_cast<QDialog *>(d)->accept(); });
        });
        button(&w, "Add from running process…")->click();
        p.kill();
        p.waitForFinished();
        QCOMPARE(ruleCol, QString()); // no rule for python3 yet
        QCOMPARE(okText, QString("Create rule for this command line"));
    }
};

QTEST_MAIN(TestGui)
#include "test_gui.moc"
