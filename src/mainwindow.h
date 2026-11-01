#pragma once

#include "ananicydata.h"
#include "fieldspecs.h"

#include <QMainWindow>
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
    explicit MainWindow(const QString &rootDir, QWidget *parent = nullptr);

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

    // saving
    bool saveAll(bool restart);
    bool writeFiles(const QMap<QString, QByteArray> &writes, const QStringList &deletes, bool restart, QString *log);

    // types
    EntryPtr currentType() const;
    void selectType(const EntryPtr &e);
    void addType();
    void editType();
    void duplicateType();
    void deleteType();
    void showRulesOfType();
    void openTypeDialog(const EntryPtr &original, const EntryPtr &templ);

    // rules
    EntryPtr currentRule() const;
    void selectRule(const EntryPtr &e);
    void addRule();
    void addRuleFromProcess();
    void editRule();
    void createRule(const QString &name);
    void deleteRules();
    void reorganizeRules();
    void removeDuplicateRules();
    QString ruleFileFor(const QString &type) const; // 99-rules/<type>.rules or 99-rules/custom.rules
    void placeNewRule(const EntryPtr &e);           // unsaved rules follow their type's file
    void openRuleDialog(const EntryPtr &original, const QString &newName);
    void refreshRuleTypeFilter();
    void applyRuleTypeFilter();
    void updateRuleCount();

    // ananicy.conf
    void refreshConfTab();
    void addConfKey();
    void removeConfKey();

    QString m_root;
    QString m_rulesDir;
    QString m_newTypesFile;

    EntryStore m_types{QStringLiteral("type"), typeKeyOrder()};
    EntryStore m_rules{QStringLiteral("name"), ruleKeyOrder()};
    EntryStore m_cgroups{QStringLiteral("cgroup"), {QStringLiteral("cgroup")}};
    MainConf m_conf;
    QMap<QString, int> m_usage;
    QHash<QString, int> m_nameCount; // rules per process name (duplicates > 1)

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
