#include "ananicydata.h"
#include "rulematch.h"

#include <QJsonArray>
#include <QTest>

#include <filesystem>

using namespace rulematch;

namespace {
EntryPtr rule(const QString &name, const QJsonValue &cmd = QJsonValue(QJsonValue::Undefined),
              const QString &type = QString())
{
    auto e = std::make_shared<Entry>();
    e->set(QStringLiteral("name"), name);
    e->set(kCmdlineKey, cmd);
    if (!type.isEmpty())
        e->set(QStringLiteral("type"), type);
    return e;
}
QJsonValue arr(const QStringList &l) { return QJsonArray::fromStringList(l); }
} // namespace

class TestRuleMatch : public QObject {
    Q_OBJECT
private slots:
    void needlesParsing()
    {
        bool valid = false;
        QCOMPARE(needles(*rule("java", "minecraft"), &valid), QStringList{"minecraft"});
        QVERIFY(valid);
        QCOMPARE(needles(*rule("java", arr({"-jar", "jenkins.war"})), &valid), (QStringList{"-jar", "jenkins.war"}));
        QVERIFY(valid);
        QCOMPARE(needles(*rule("java"), &valid), QStringList{});
        QVERIFY(valid); // plain rule
        for (const QJsonValue &bad : {QJsonValue(""), QJsonValue(arr({})), QJsonValue(42),
                                      QJsonValue(arr({"a", ""}))}) {
            needles(*rule("java", bad), &valid);
            QVERIFY(!valid);
        }
        QJsonArray mixed{QJsonValue("a"), QJsonValue(1)};
        needles(*rule("java", mixed), &valid);
        QVERIFY(!valid);
    }

    void valueRoundTrip()
    {
        QVERIFY(needlesValue({}).isUndefined());
        QVERIFY(needlesValue({"", ""}).isUndefined());
        QCOMPARE(needlesValue({"x"}), QJsonValue("x"));
        QCOMPARE(needlesValue({"a", "", "b", "a"}), arr({"a", "b"}));
        QCOMPARE(needlesText(*rule("java", arr({"-jar", "jenkins.war"}))), QStringLiteral("-jar + jenkins.war"));
    }

    // same cases as ananicy-cpp's unit test "cmdline_contains rules"
    void matchingLikeAnanicyCpp()
    {
        const QVector<EntryPtr> rules = {rule("java", QJsonValue(QJsonValue::Undefined), "plain"),
                                         rule("java", "minecraft", "Game"),
                                         rule("java", arr({"-jar", "jenkins.war"}), "jenkins")};
        auto typeOf = [&](const QString &n, const QString &c) {
            EntryPtr r = match(rules, n, c);
            return r ? r->str("type") : QStringLiteral("<none>");
        };
        QCOMPARE(typeOf("java", "java -Xmx4G -cp minecraft-1.21.jar Main"), QStringLiteral("Game"));
        QCOMPARE(typeOf("java", "java -jar /opt/jenkins.war"), QStringLiteral("jenkins"));
        QCOMPARE(typeOf("java", "java -cp jenkins.war Main"), QStringLiteral("plain")); // needs both
        QCOMPARE(typeOf("java", "java -jar other.jar"), QStringLiteral("plain"));
        QCOMPARE(typeOf("python3", "python3 minecraft.py"), QStringLiteral("<none>"));
        QCOMPARE(typeOf("java", "java -jar jenkins.war minecraft"), QStringLiteral("Game")); // first wins
    }

    void lastPlainWins()
    {
        const QVector<EntryPtr> rules = {rule("gcc", {}, "a"), rule("gcc", {}, "b")};
        QCOMPARE(match(rules, "gcc", "gcc x.c")->str("type"), QStringLiteral("b"));
    }

    void shadowing()
    {
        QVERIFY(shadows(*rule("java", "mine"), *rule("java", "minecraft")));             // substring
        QVERIFY(shadows(*rule("java", "x"), *rule("java", "x")));                        // identical
        QVERIFY(shadows(*rule("java", "-jar"), *rule("java", arr({"-jar", "a.jar"})))); // subset
        QVERIFY(!shadows(*rule("java", arr({"-jar", "a.jar"})), *rule("java", "-jar")));
        QVERIFY(!shadows(*rule("java", "minecraft"), *rule("java", "mine")));
        QVERIFY(!shadows(*rule("java", "x"), *rule("python3", "x")));
        QVERIFY(!shadows(*rule("java"), *rule("java", "x")));
    }

    void analysis()
    {
        const auto p1 = rule("gcc"), p2 = rule("gcc"), solo = rule("make");
        const auto c1 = rule("java", "minecraft"), c2 = rule("java", "jenkins"), c3 = rule("java", "minecraft-server");
        const auto c4 = rule("java", "minecraft"), bad = rule("java", QJsonValue(arr({})));
        const QVector<EntryPtr> rules = {p1, c1, p2, solo, c2, c3, c4, bad};
        const Analysis a = analyze(rules);
        QVERIFY(a.problem.contains(p1.get()) && a.problem.contains(p2.get())); // plain duplicates
        QVERIFY(!a.problem.contains(solo.get()));
        QVERIFY(!a.problem.contains(c1.get()) && !a.problem.contains(c2.get())); // different texts: fine
        QVERIFY(a.problem.contains(c3.get()) && a.shadowedBy.value(c3.get()) == c1); // "minecraft" matches first
        QVERIFY(a.problem.contains(c4.get()) && a.shadowedBy.value(c4.get()) == c1); // identical
        QVERIFY(a.problem.value(bad.get()).contains("Invalid"));
        QCOMPARE(a.duplicatePlainNames(), 1);
        // a cmdline rule next to a single plain rule is not a duplicate
        QVERIFY(analyze({rule("java"), rule("java", "x")}).problem.isEmpty());

        const QVector<EntryPtr> drop = removableDuplicates(rules);
        QCOMPARE(drop, (QVector<EntryPtr>{p1, c3, c4})); // keeps the last plain, the first cmdline rule
        // removing them changes nothing that ananicy-cpp does
        QVector<EntryPtr> kept = rules;
        for (const auto &d : drop)
            kept.removeAll(d);
        for (const QString &cmd : {QString("java minecraft-server.jar"), QString("java jenkins"),
                                   QString("java other"), QString("gcc a.c")})
            for (const QString &n : {QString("java"), QString("gcc")})
                QCOMPARE(match(kept, n, cmd), match(rules, n, cmd));
    }

    void reorganizeOrder()
    {
        const auto a = rule("java", "minecraft", "Game"), b = rule("java", "minecraft-server", "Server");
        const auto c = rule("java", "jenkins", "Ci");
        QVector<EntryPtr> rules = {b, a, c}; // b first now
        auto file = [](const EntryPtr &e) { return QStringLiteral("/etc/ananicy.d/99-rules/") + e->str("type") + ".rules"; };
        // after reorganizing: Ci.rules, Game.rules, Server.rules -> c, a, b: order of a and b flips
        QCOMPARE(cmdlineOrderChanges(rules, file), QStringList{"java"});
        rules = {c, a, b};
        QVERIFY(cmdlineOrderChanges(rules, file).isEmpty());
    }

    void pathOrderLikeStdFilesystem()
    {
        QVERIFY(pathLess("/etc/ananicy.d/a/x.rules", "/etc/ananicy.d/a-b/x.rules")); // string order says the opposite
        QVERIFY(QString("/etc/ananicy.d/a-b/x.rules") < QString("/etc/ananicy.d/a/x.rules"));
        QVERIFY(pathLess("/d/00-default/z.rules", "/d/99-rules/a.rules"));
        QVERIFY(pathLess("/d/a.rules", "/d/b.rules"));
        QVERIFY(pathLess("/d/x", "/d/x/y"));
        QCOMPARE(pathCompare("/d/x.rules", "/d/x.rules"), 0);
        QVERIFY(pathLess("/d/B.rules", "/d/a.rules")); // byte order: upper case first
    }

    // the order ananicy-cpp uses: std::sort over std::filesystem::path
    void pathOrderMatchesStdFilesystemExactly()
    {
        const QStringList names = {"a", "a-b", "a.b", "a_b", "A", "ab", "a b", "a~", "00-default", "00-default-x",
                                   "99-rules", "99", "x.rules", "x-y.rules", "é", "z"};
        QStringList paths;
        for (const QString &d : names)
            for (const QString &f : {QString("x.rules"), QString("a/x.rules"), QString("a-b.rules")})
                paths << "/etc/ananicy.d/" + d + "/" + f;
        for (const QString &x : paths)
            for (const QString &y : paths) {
                const bool ours = pathLess(x, y);
                const bool std_ = std::filesystem::path(x.toStdString()) < std::filesystem::path(y.toStdString());
                if (ours != std_)
                    QFAIL(qPrintable(QString("%1 < %2: ours %3, std %4").arg(x, y).arg(ours).arg(std_)));
            }
    }

    void processNames()
    {
        QCOMPARE(processName({"/usr/bin/java", "-jar", "x.jar"}, "/usr/lib/jvm/bin/java", "java"), QStringLiteral("java"));
        QCOMPARE(processName({"C:\\Games\\game.exe"}, "", "game.exe"), QStringLiteral("game.exe"));
        QCOMPARE(processName({"C:\\Games\\tool"}, "", "tool"), QStringLiteral("C:\\Games\\tool")); // only .exe splits at '\'
        QCOMPARE(processName({"", "-c", "x"}, "/usr/bin/bash", "sh"), QStringLiteral("bash")); // empty argv[0]
        QCOMPARE(processName({}, "/opt/app (deleted)", "app"), QStringLiteral("app"));
        QCOMPARE(processName({}, "", "kworker/0:1"), QStringLiteral("kworker/0:1"));
        QCOMPARE(processName({"renamed-game-with-a-long-name.exe"}, "", "renamed-game-wi"),
                 QStringLiteral("renamed-game-with-a-long-name.exe")); // not cut at 15 like comm
    }

    void needleSuggestions()
    {
        QCOMPARE(suggestNeedle({"java", "-Xmx4G", "-jar", "/srv/mc/server.jar", "nogui"}), QStringLiteral("server.jar"));
        QCOMPARE(suggestNeedle({"java", "-cp", "lib/*:app.jar", "net.minecraft.client.Main"}),
                 QStringLiteral("net.minecraft.client.Main"));
        QCOMPARE(suggestNeedle({"/usr/bin/python3", "/usr/bin/jupyter-lab"}), QStringLiteral("jupyter-lab"));
        QCOMPARE(suggestNeedle({"python3", "-u", "-m", "http.server", "8000"}), QStringLiteral("http.server"));
        QCOMPARE(suggestNeedle({"python3", "-c", "print(1)"}), QString());
        QCOMPARE(suggestNeedle({"node", "/opt/app/server.js"}), QStringLiteral("server.js"));
        QCOMPARE(suggestNeedle({"java"}), QString());
    }
};

QTEST_GUILESS_MAIN(TestRuleMatch)
#include "test_rulematch.moc"
