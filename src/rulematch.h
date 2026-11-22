#pragma once

// How ananicy-cpp picks a rule for a process, including "cmdline_contains"
// (github.com/WhitePeace36/ananicy-cpp-lunar). Kept free of GUI code so it can be tested.
//
//   * A rule without cmdline_contains is a plain rule. If several plain rules have the same
//     name, the one loaded last wins.
//   * A rule with cmdline_contains only applies when every text in it occurs in the process's
//     command line (the arguments joined by spaces, case-sensitive). Such rules are tried in
//     load order before the plain rule for that name, and the first one that matches wins.
//     A later one with the same texts (in any order) replaces an earlier one as if that had
//     never been loaded; rules with other texts never replace each other or the plain rule.
//   * Packaged rules (/usr/share/ananicy.d) load before /etc/ananicy.d, so a rule there with
//     the same identity (ruleKey()) overrides the packaged one. Such pairs are not duplicates.

#include "ananicydata.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace rulematch {

inline const QString kCmdlineKey = QStringLiteral("cmdline_contains");

// The texts of a rule's cmdline_contains, empty for a plain rule. *valid is false when the
// value is neither a non-empty string nor a non-empty list of non-empty strings (ananicy-cpp
// then ignores the whole line).
QStringList needles(const Entry &e, bool *valid = nullptr);
bool hasCmdline(const Entry &e);

// JSON value to store for the given texts: Undefined (removes the key) for none, a string for
// one, an array for several. Empty texts are dropped.
QJsonValue needlesValue(const QStringList &texts);

// Short display text, e.g. "minecraft" or "-jar + jenkins.war".
QString needlesText(const Entry &e);

// true if `cmdline` contains every text.
bool matches(const QStringList &needles, const QString &cmdline);

// What makes two rules the same rule for ananicy-cpp (a later one replaces the earlier one):
// the name, and for cmdline rules the set of texts (order and repeats do not matter).
QString ruleKey(const Entry &e);

// true if the earlier cmdline rule matches whenever the later one does (same name, and every
// text of the earlier rule occurs inside one of the later rule's texts), so the later rule
// can never be used. Rules with the same texts do not shadow each other: the later one replaces
// the earlier one instead (see ruleKey()).
bool shadows(const Entry &earlier, const Entry &later);

// Rule that ananicy-cpp applies to a process. `rules` must be in load order.
EntryPtr match(const QVector<EntryPtr> &rules, const QString &name, const QString &cmdline);

// Problems in a rule list (load order): plain rules with the same name, cmdline rules that can
// never match or are replaced, invalid cmdline_contains values. Keyed by entry. Only rules
// outside the packaged directory get problems, and a packaged rule overridden by one in
// /etc/ananicy.d is not a duplicate.
struct Analysis {
    QHash<const Entry *, QString> problem;
    QHash<QString, int> plainCount;            // plain rules per name, not counting packaged ones
    QHash<const Entry *, EntryPtr> shadowedBy; // unreachable cmdline rule -> rule that wins
    QHash<const Entry *, EntryPtr> replacedBy; // rule -> later rule with the same ruleKey()
    int duplicatePlainNames() const;           // names with more than one plain rule
};
Analysis analyze(const QVector<EntryPtr> &rules);

// Rules that "Remove duplicates" can delete without changing what ananicy-cpp does: every rule
// that a later one with the same ruleKey() replaces, and every cmdline rule that can never match.
// Packaged rules are never included.
QVector<EntryPtr> removableDuplicates(const QVector<EntryPtr> &rules);

// Names whose cmdline rules end up in a different order (and may pick a different winner) when
// the rules are re-sorted by `newPathOf`. `rules` in current load order.
QStringList cmdlineOrderChanges(const QVector<EntryPtr> &rules,
                                const std::function<QString(const EntryPtr &)> &newPathOf);

// Order in which ananicy-cpp loads files: std::filesystem::path comparison, i.e. component by
// component ("a/x" before "a-b/x", unlike a plain string comparison).
bool pathLess(const QString &a, const QString &b);
int pathCompare(const QString &a, const QString &b);

// The name ananicy-cpp matches rules against: the file name of argv[0] (for Windows programs
// under Wine also split at '\'; for a rewritten process title, the program it starts with),
// else the file name of /proc/PID/exe, else comm.
QString processName(const QStringList &argv, const QString &exeTarget, const QString &comm);

// A text that identifies this command line for a cmdline_contains rule: the jar after -jar,
// the module after -m, or the first argument that is not an option (without its directory).
// Empty if nothing suitable is found.
QString suggestNeedle(const QStringList &argv);

} // namespace rulematch
