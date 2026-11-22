#include "rulematch.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QObject>
#include <QSet>

#include <algorithm>

namespace rulematch {

QStringList needles(const Entry &e, bool *valid)
{
    if (valid)
        *valid = true;
    const QJsonValue v = e.value(kCmdlineKey);
    if (v.isUndefined())
        return {};
    QStringList out;
    bool ok = false;
    if (v.isString()) {
        out << v.toString();
        ok = !out.first().isEmpty();
    } else if (v.isArray()) {
        const QJsonArray a = v.toArray();
        ok = !a.isEmpty();
        for (const QJsonValue &x : a) {
            if (!x.isString() || x.toString().isEmpty()) {
                ok = false;
                break;
            }
            out << x.toString();
        }
    }
    if (!ok) {
        if (valid)
            *valid = false;
        return {};
    }
    return out;
}

bool hasCmdline(const Entry &e)
{
    return e.has(kCmdlineKey);
}

QJsonValue needlesValue(const QStringList &texts)
{
    QStringList t;
    for (const QString &s : texts)
        if (!s.isEmpty() && !t.contains(s))
            t << s;
    if (t.isEmpty())
        return QJsonValue(QJsonValue::Undefined);
    if (t.size() == 1)
        return QJsonValue(t.first());
    return QJsonArray::fromStringList(t);
}

QString needlesText(const Entry &e)
{
    bool valid = true;
    const QStringList n = needles(e, &valid);
    if (!valid)
        return QStringLiteral("⚠ ") + jsonValueText(e.value(kCmdlineKey));
    return n.join(QStringLiteral(" + "));
}

bool matches(const QStringList &n, const QString &cmdline)
{
    return std::all_of(n.begin(), n.end(), [&](const QString &s) { return cmdline.contains(s); });
}

QString ruleKey(const Entry &e)
{
    const QString name = e.str(QStringLiteral("name"));
    if (!hasCmdline(e))
        return name;
    const QChar sep(0x1f);
    bool valid = true;
    QStringList n = needles(e, &valid);
    if (!valid)
        return name + sep + QStringLiteral("invalid") + sep + jsonValueText(e.value(kCmdlineKey));
    n.removeDuplicates();
    n.sort();
    return name + sep + n.join(sep);
}

bool shadows(const Entry &earlier, const Entry &later)
{
    if (earlier.str(QStringLiteral("name")) != later.str(QStringLiteral("name")))
        return false;
    bool v1 = true, v2 = true;
    const QStringList a = needles(earlier, &v1), b = needles(later, &v2);
    if (!v1 || !v2 || a.isEmpty() || b.isEmpty())
        return false;
    if (ruleKey(earlier) == ruleKey(later))
        return false; // same texts: the later rule replaces the earlier one
    return std::all_of(a.begin(), a.end(), [&](const QString &x) {
        return std::any_of(b.begin(), b.end(), [&](const QString &y) { return y.contains(x); });
    });
}

EntryPtr match(const QVector<EntryPtr> &rules, const QString &name, const QString &cmdline)
{
    EntryPtr plain;
    QVector<EntryPtr> byCmdline; // in load order; a later rule with the same texts replaces one
    for (const EntryPtr &r : rules) {
        if (r->str(QStringLiteral("name")) != name)
            continue;
        if (!hasCmdline(*r)) {
            plain = r; // the last plain rule wins
            continue;
        }
        bool valid = true;
        needles(*r, &valid);
        if (!valid)
            continue; // ananicy-cpp ignores the line
        const QString key = ruleKey(*r);
        byCmdline.erase(std::remove_if(byCmdline.begin(), byCmdline.end(),
                                       [&](const EntryPtr &x) { return ruleKey(*x) == key; }),
                        byCmdline.end());
        byCmdline << r;
    }
    for (const EntryPtr &r : byCmdline)
        if (matches(needles(*r), cmdline))
            return r; // the first matching cmdline rule wins
    return plain;
}

int Analysis::duplicatePlainNames() const
{
    int n = 0;
    for (auto it = plainCount.cbegin(); it != plainCount.cend(); ++it)
        if (it.value() > 1)
            ++n;
    return n;
}

Analysis analyze(const QVector<EntryPtr> &rules)
{
    Analysis a;
    QHash<QString, EntryPtr> last; // per ruleKey(): the rule ananicy-cpp keeps
    for (const EntryPtr &r : rules) {
        last[ruleKey(*r)] = r;
        if (!hasCmdline(*r) && !isPackaged(*r))
            a.plainCount[r->str(QStringLiteral("name"))]++;
    }
    QHash<QString, QVector<EntryPtr>> kept; // per name: cmdline rules that are used, load order
    for (const EntryPtr &r : rules) {
        const QString n = r->str(QStringLiteral("name"));
        const EntryPtr winner = last.value(ruleKey(*r));
        if (winner != r)
            a.replacedBy.insert(r.get(), winner);
        if (!hasCmdline(*r)) {
            if (a.plainCount.value(n) > 1 && !isPackaged(*r))
                a.problem.insert(r.get(), QObject::tr("Duplicate: there is more than one rule for this process "
                                                      "without \"command line contains\" (use \"Remove duplicates…\")"));
            continue;
        }
        bool valid = true;
        needles(*r, &valid);
        if (!valid) {
            if (!isPackaged(*r))
                a.problem.insert(r.get(), QObject::tr("Invalid \"cmdline_contains\": it must be a non-empty text or "
                                                      "a list of non-empty texts. ananicy-cpp ignores this rule."));
            continue;
        }
        if (winner != r) {
            if (!isPackaged(*r))
                a.problem.insert(r.get(), QObject::tr("Duplicate: a later rule for \"%1\" with the same command line "
                                                      "texts replaces this one (use \"Remove duplicates…\")")
                                              .arg(n));
            continue;
        }
        for (const EntryPtr &earlier : kept.value(n)) {
            if (shadows(*earlier, *r)) {
                a.shadowedBy.insert(r.get(), earlier);
                if (!isPackaged(*r))
                    a.problem.insert(r.get(), QObject::tr("Never used: an earlier rule for \"%1\" (command line "
                                                          "contains \"%2\") matches first (use \"Remove duplicates…\")")
                                                  .arg(n, needlesText(*earlier)));
                break;
            }
        }
        kept[n] << r;
    }
    return a;
}

QVector<EntryPtr> removableDuplicates(const QVector<EntryPtr> &rules)
{
    const Analysis a = analyze(rules);
    QVector<EntryPtr> out;
    for (const EntryPtr &r : rules) {
        if (isPackaged(*r))
            continue; // never written
        bool valid = true;
        needles(*r, &valid);
        if (!valid)
            continue; // not a duplicate, just broken: shown as a problem
        if (a.replacedBy.contains(r.get()) || a.shadowedBy.contains(r.get()))
            out << r;
    }
    return out;
}

QStringList cmdlineOrderChanges(const QVector<EntryPtr> &rules,
                                const std::function<QString(const EntryPtr &)> &newPathOf)
{
    // new load order: by new file, entries keep their relative order within a file
    QVector<int> idx(rules.size());
    for (int i = 0; i < idx.size(); ++i)
        idx[i] = i;
    QVector<QString> path(rules.size());
    for (int i = 0; i < rules.size(); ++i)
        path[i] = newPathOf(rules[i]);
    // packaged files load first; they never move
    std::stable_sort(idx.begin(), idx.end(), [&](int x, int y) {
        if (isPackaged(*rules[x]) != isPackaged(*rules[y]))
            return isPackaged(*rules[x]);
        return pathLess(path[x], path[y]);
    });

    QHash<QString, QVector<const Entry *>> before, after;
    for (const EntryPtr &r : rules)
        if (hasCmdline(*r))
            before[r->str(QStringLiteral("name"))] << r.get();
    for (int i : idx)
        if (hasCmdline(*rules[i]))
            after[rules[i]->str(QStringLiteral("name"))] << rules[i].get();
    QStringList changed;
    for (auto it = before.cbegin(); it != before.cend(); ++it)
        if (it.value().size() > 1 && it.value() != after.value(it.key()))
            changed << it.key();
    changed.sort(Qt::CaseInsensitive);
    return changed;
}

int pathCompare(const QString &a, const QString &b)
{
    const QStringList pa = a.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const QStringList pb = b.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const bool absA = a.startsWith(QLatin1Char('/')), absB = b.startsWith(QLatin1Char('/'));
    if (absA != absB)
        return absA ? -1 : 1;
    for (int i = 0; i < std::min(pa.size(), pb.size()); ++i) {
        // byte order of the UTF-8 names, like std::filesystem on Linux
        const QByteArray x = pa[i].toUtf8(), y = pb[i].toUtf8();
        if (x != y)
            return x < y ? -1 : 1;
    }
    return pa.size() == pb.size() ? 0 : (pa.size() < pb.size() ? -1 : 1);
}

bool pathLess(const QString &a, const QString &b)
{
    return pathCompare(a, b) < 0;
}

QString processName(const QStringList &argv, const QString &exeTarget, const QString &comm)
{
    if (!argv.isEmpty() && !argv.first().isEmpty()) {
        const QString &a0 = argv.first();
        // Rewritten process title (setproctitle: Chromium, avahi, sshd, ...): ananicy-cpp-lunar
        // names the program, its first word ("/opt/brave-bin/brave --type=zygote ..." -> "brave",
        // "avahi-daemon: running [host]" -> "avahi-daemon"). A first word that is a path only
        // counts if its file name is that of the executable, so "/games/My Game/game" stays as is.
        const int space = a0.indexOf(QLatin1Char(' '));
        if (space > 0 && !a0.endsWith(QLatin1String(".exe"))) {
            QString head = a0.left(space);
            if (head.endsWith(QLatin1Char(':')))
                head.chop(1);
            const int slash = head.lastIndexOf(QLatin1Char('/'));
            if (!head.isEmpty() && slash < 0)
                return head;
            QString exe = exeTarget;
            const int del = exe.indexOf(QLatin1String(" (deleted)"));
            if (del >= 0)
                exe.truncate(del);
            const QString file = head.mid(slash + 1);
            const int exeSlash = exe.lastIndexOf(QLatin1Char('/'));
            if (slash >= 0 && !file.isEmpty() && exeSlash >= 0 && file == exe.mid(exeSlash + 1))
                return file;
        }
        const int cut = a0.endsWith(QLatin1String(".exe"))
                            ? std::max(a0.lastIndexOf(QLatin1Char('/')), a0.lastIndexOf(QLatin1Char('\\')))
                            : a0.lastIndexOf(QLatin1Char('/'));
        return cut >= 0 ? a0.mid(cut + 1) : a0;
    }
    if (!exeTarget.isEmpty()) {
        QString n = QFileInfo(exeTarget).fileName();
        const int d = n.indexOf(QLatin1String(" (deleted)"));
        if (d >= 0)
            n.truncate(d);
        if (!n.isEmpty())
            return n;
    }
    return comm;
}

static QString baseName(const QString &arg)
{
    const int cut = std::max(arg.lastIndexOf(QLatin1Char('/')), arg.lastIndexOf(QLatin1Char('\\')));
    return cut >= 0 ? arg.mid(cut + 1) : arg;
}

QString suggestNeedle(const QStringList &argv)
{
    if (argv.size() < 2)
        return {};
    const QString prog = baseName(argv.first());
    const bool java = prog == QLatin1String("java") || prog == QLatin1String("javaw");
    const bool python = prog.startsWith(QLatin1String("python"));
    // options of java that take a separate value
    static const QSet<QString> javaValueOpts = {
        QStringLiteral("-cp"), QStringLiteral("-classpath"), QStringLiteral("--class-path"),
        QStringLiteral("-p"), QStringLiteral("--module-path"), QStringLiteral("--add-modules"),
        QStringLiteral("--add-opens"), QStringLiteral("--add-exports"), QStringLiteral("--add-reads")};
    for (int i = 1; i < argv.size(); ++i) {
        const QString &a = argv[i];
        if (java && (a == QLatin1String("-jar") || a == QLatin1String("-m") || a == QLatin1String("--module"))) {
            return i + 1 < argv.size() ? baseName(argv[i + 1]) : QString();
        }
        if (python && a == QLatin1String("-m"))
            return i + 1 < argv.size() ? argv[i + 1] : QString();
        if (python && a == QLatin1String("-c"))
            return {}; // inline code: nothing that identifies it reliably
        if (java && javaValueOpts.contains(a)) {
            ++i; // skip the value
            continue;
        }
        if (a.startsWith(QLatin1Char('-')))
            continue;
        const QString b = baseName(a);
        if (!b.isEmpty())
            return b;
    }
    return {};
}

} // namespace rulematch
