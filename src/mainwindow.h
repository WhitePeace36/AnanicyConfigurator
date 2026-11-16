#pragma once

#include "ananicydata.h"
#include "fieldspecs.h"
#include "rulematch.h"

#include <QMainWindow>
#include <QHash>
#include <QJsonValue>
#include <QMap>

class EntryModel;
class EntryFilterProxy;
class EntryEditor;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;
class QTableWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // Rules a distribution package ships. Loaded (read-only) before rootDir, like ananicy-cpp
    // does; changes to them are saved in rootDir as overrides. Ignored if it does not exist.
    static constexpr const char *kPackagedDir = "/usr/share/ananicy.d";

    // packagedDir: empty to read rootDir alone (tests).
    explicit MainWindow(const QString &rootDir, const QString &packagedDir = QString::fromLatin1(kPackagedDir),
                        QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *ev) override;

private:
    // ui
    QWidget *buildTypesPane();
    QWidget *buildRulesPane();
    QWidget *buildConfTab();
    QWidget *buildBottomBar();

    // data
    void loadAll();
    void reloadFromDisk();
    bool isDirty() const;
    void updateDirtyState();
    void recomputeUsage();
    void typeNamesChanged();
    QString describeType(const QString &name) const;
    QString detectService() const;
    // Whether the selected daemon understands "cmdline_contains" (ananicy-cpp built with it).
    void updateCmdlineSupport();
    QString cmdlineUnsupportedNote() const;

    // saving
    bool saveAll(bool restart);
    QStringList emptyDirCandidates(const QStringList &deletedFiles) const;
    QStringList checkTargets(const QStringList &writes, const QStringList &deletes) const;
    bool writeFiles(const QMap<QString, QByteArray> &writes, const QStringList &deletes, bool restart, QString *log);

    // packaged layer (/usr/share/ananicy.d) and overrides in the config directory
    enum class Source { Packaged, Changed, Off, Own };
    struct Layers {
        QHash<QString, EntryPtr> last;         // per key: the entry ananicy-cpp uses
        QHash<QString, EntryPtr> lastPackaged; // per key: the packaged entry it would use otherwise
    };
    QString entryKey(const EntryStore &store, const Entry &e) const;
    void recomputeLayers();
    const Layers &layersOf(const EntryStore &store) const;
    Source sourceOf(const EntryStore &store, const Entry &e) const;
    QString sourceText(const EntryStore &store, const Entry &e) const;
    QString sourceTip(const EntryStore &store, const Entry &e) const;
    bool isHidden(const EntryStore &store, const Entry &e) const; // packaged and overridden
    EntryPtr packagedOf(const EntryStore &store, const Entry &e) const;
    QString overridePath(const ConfFile *packagedFile) const;     // same relative path in the config dir
    QString displayPath(const Entry &e) const;                    // relative to its directory
    QVector<QPair<QString, QJsonValue>> offFields(const EntryStore &store, const Entry &e) const;
    EntryPtr addOverride(EntryStore &store, const EntryPtr &packaged, const QVector<QPair<QString, QJsonValue>> &fields);
    // Removes every entry in the config directory with the same key as e, so the packaged one applies.
    int resetEntries(EntryStore &store, const QVector<EntryPtr> &sel);

    // types
    EntryPtr currentType() const;
    void selectType(const EntryPtr &e);
    void addType();
    void editType();
    void duplicateType();
    void deleteType();
    void resetType();
    void showRulesOfType();
    void reorganizeTypes();
    void openTypeDialog(const EntryPtr &original, const EntryPtr &templ);

    // rules
    EntryPtr currentRule() const;
    void selectRule(const EntryPtr &e);
    void addRule();
    void addRuleFromProcess();
    void editRule();
    // needles: texts for cmdline_contains (empty for a rule that applies to every process)
    void createRule(const QString &name, const QStringList &needles = {});
    void deleteRules();
    void resetRules();
    void reorganizeRules();
    void removeDuplicateRules();
    QString ruleFileFor(const QString &type) const; // 99-rules/<type>.rules or 99-rules/custom.rules
    void placeNewRule(const EntryPtr &e);           // unsaved rules follow their type's file (then sortByLoadOrder())
    void openRuleDialog(const EntryPtr &original, const QString &newName, const QStringList &newNeedles = {});
    void refreshRuleTypeFilter();
    void applyRuleTypeFilter();
    void updateRuleCount();

    // ananicy.conf
    void refreshConfTab();
    void addConfKey();
    void removeConfKey();

    QString m_root;
    QString m_packagedRoot; // empty: no packaged rules
    QString m_rulesDir;
    QString m_newTypesFile;

    EntryStore m_types{QStringLiteral("type"), typeKeyOrder()};
    EntryStore m_rules{QStringLiteral("name"), ruleKeyOrder()};
    EntryStore m_cgroups{QStringLiteral("cgroup"), {QStringLiteral("cgroup")}};
    MainConf m_conf;
    MainConf m_packagedConf; // read-only defaults; keys in m_conf win
    Layers m_typeLayers, m_ruleLayers;
    QMap<QString, int> m_usage;
    rulematch::Analysis m_ruleAnalysis; // duplicates / unreachable rules, recomputed with the usage
    bool m_cmdlineSupported = true;     // daemon supports cmdline_contains
    bool m_isAnanicyCpp = true;         // selected service is ananicy-cpp (matches argv[0], not comm)

    EntryModel *m_typeModel = nullptr;
    EntryModel *m_ruleModel = nullptr;
    EntryFilterProxy *m_typeProxy = nullptr;
    EntryFilterProxy *m_ruleProxy = nullptr;
    QTableView *m_typeView = nullptr;
    QTableView *m_ruleView = nullptr;
    QLineEdit *m_typeFilter = nullptr;
    QLineEdit *m_ruleFilter = nullptr;
    QComboBox *m_ruleTypeFilter = nullptr;
    QLabel *m_ruleCount = nullptr;
    QPushButton *m_showAllRules = nullptr;

    QVector<QPair<ConfSpec, QComboBox *>> m_confCombos;
    QTableWidget *m_confOther = nullptr;
    QLabel *m_confPath = nullptr;
    bool m_confLoading = false;

    QComboBox *m_service = nullptr;
    QLabel *m_warnLabel = nullptr;
};
