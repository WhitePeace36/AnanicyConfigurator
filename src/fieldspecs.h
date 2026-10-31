#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// Description of a setting that can appear in a type (and as an override in a rule).
struct FieldSpec {
    QString key;
    QString label;
    QStringList options;   // values offered in the drop-down
    bool numeric = false;  // stored as JSON number
    bool editable = false; // allow typing values not in the list
    int min = 0, max = 0;
    QString tip;
};

inline QStringList intRange(int from, int to, int step = 1)
{
    QStringList l;
    for (int i = from; (step > 0) ? i <= to : i >= to; i += step)
        l << QString::number(i);
    return l;
}

inline const QVector<FieldSpec> &typeFieldSpecs()
{
    static const QVector<FieldSpec> specs = {
        {"nice", "Nice", intRange(-20, 19), true, false, -20, 19,
         "CPU priority: -20 (highest) ... 19 (lowest)"},
        {"latency_nice", "Latency nice", intRange(-20, 19), true, false, -20, 19,
         "Latency nice (ananicy-cpp, needs kernel support): -20 ... 19"},
        {"ioclass", "IO class", {"best-effort", "idle", "realtime", "none"}, false, false, 0, 0,
         "IO scheduling class (ionice -c)"},
        {"ionice", "IO nice", intRange(0, 7), true, false, 0, 7,
         "IO priority within the class: 0 (highest) ... 7 (lowest)"},
        {"sched", "Scheduler", {"other", "normal", "batch", "idle", "fifo", "rr", "iso"}, false, false, 0, 0,
         "CPU scheduling policy"},
        {"rtprio", "RT priority", intRange(1, 99), true, false, 1, 99,
         "Static priority for realtime policies (fifo / rr): 1 ... 99"},
        {"oom_score_adj", "OOM score adj", intRange(-1000, 1000, 100), true, true, -1000, 1000,
         "OOM killer preference: -1000 (never kill) ... 1000 (kill first). You can type any value."},
        {"cgroup", "Cgroup", {}, false, true, 0, 0,
         "cgroup defined in a *.cgroups file"},
    };
    return specs;
}

inline QStringList typeKeyOrder()
{
    QStringList l{"type"};
    for (const auto &s : typeFieldSpecs())
        l << s.key;
    return l;
}

inline QStringList ruleKeyOrder()
{
    QStringList l{"name", "type"};
    for (const auto &s : typeFieldSpecs())
        l << s.key;
    return l;
}

// ananicy.conf keys
struct ConfSpec {
    QString key;
    QStringList options; // empty -> boolean
    bool numeric = false;
    QString tip;
};

inline const QVector<ConfSpec> &confSpecs()
{
    static const QVector<ConfSpec> specs = {
        {"check_freq", {"1", "2", "3", "5", "10", "15", "20", "30", "60"}, true,
         "Seconds between full rescans of all processes (you can type any value)"},
        {"cgroup_load", {}, false, "Load cgroup definitions (*.cgroups)"},
        {"type_load", {}, false, "Load type definitions (*.types)"},
        {"rule_load", {}, false, "Load rules (*.rules)"},
        {"apply_nice", {}, false, "Apply nice values"},
        {"apply_latnice", {}, false, "Apply latency nice values (ananicy-cpp)"},
        {"apply_ioclass", {}, false, "Apply IO classes (ananicy)"},
        {"apply_ionice", {}, false, "Apply IO nice values"},
        {"apply_sched", {}, false, "Apply scheduling policies"},
        {"apply_oom_score_adj", {}, false, "Apply OOM score adjustments"},
        {"apply_cgroup", {}, false, "Move processes into cgroups"},
        {"check_disks_schedulers", {}, false, "Check disk IO schedulers (ananicy)"},
        {"cgroup_realtime_workaround", {}, false,
         "Work around cgroup v2 limitations for realtime processes (ananicy-cpp)"},
        {"log_applied_rule", {}, false, "Log every applied rule (ananicy-cpp)"},
        {"loglevel", {"trace", "debug", "info", "warn", "error", "critical"}, false,
         "Log verbosity (ananicy-cpp)"},
    };
    return specs;
}
