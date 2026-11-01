#pragma once

#include <QJsonValue>
#include <QMap>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>
#include <vector>

struct ConfFile;

// One JSON line of a *.rules / *.types / *.cgroups file.
// Field order is preserved so untouched lines can be written back exactly.
struct Entry {
    QVector<QPair<QString, QJsonValue>> fields;
    QString originalLine;   // exact text as read from disk (empty for new entries)
    QString trailing;       // trailing text after the JSON object (usually a "# comment")
    ConfFile *file = nullptr;
    bool modified = false;

    bool has(const QString &key) const;
    QJsonValue value(const QString &key) const;
    QString str(const QString &key) const;              // value as display text, "" if unset
    void set(const QString &key, const QJsonValue &v);  // Undefined/Null removes the key
    QString serialize(const QStringList &keyOrder) const;
};
using EntryPtr = std::shared_ptr<Entry>;

struct ConfLine {
    QString raw;      // used when entry == nullptr (comments, blank lines, unparsable lines)
    EntryPtr entry;
};

struct ConfFile {
    QString path;
    bool existed = false;
    bool dirty = false;
    int loadedEntries = 0;   // number of entries when read from disk
    QVector<ConfLine> lines;

    QString render(const QStringList &keyOrder) const;
    int entryCount() const;
    // A file that had entries (or is new) and now has none is removed instead of written.
    bool shouldDelete() const { return dirty && existed && loadedEntries > 0 && entryCount() == 0; }
    bool shouldSkip() const { return !existed && entryCount() == 0; }
};

// A collection of JSON-line files (all *.rules, or all *.types ...).
class EntryStore {
public:
    EntryStore(QString primaryKey, QStringList keyOrder)
        : m_primaryKey(std::move(primaryKey)), m_keyOrder(std::move(keyOrder)) {}

    void clear();
    void loadFile(const QString &path);
    void loadTree(const QString &dir, const QString &pattern, bool recursive);

    ConfFile *fileFor(const QString &path); // existing or newly created (empty) file
    EntryPtr add(const QString &filePath, const QString &primaryValue);
    void remove(const EntryPtr &e);
    void moveTo(const EntryPtr &e, const QString &filePath); // keeps the entry's text, moves it to another file
    void markModified(const EntryPtr &e);

    EntryPtr find(const QString &primaryValue) const;
    QStringList names() const;
    bool isDirty() const;

    QMap<QString, QByteArray> pendingWrites() const; // path -> new content for dirty files
    QStringList pendingDeletes() const;              // files that lost all their entries
    void markSaved();

    const QVector<EntryPtr> &entries() const { return m_entries; }
    const QString &primaryKey() const { return m_primaryKey; }
    const QStringList &keyOrder() const { return m_keyOrder; }
    QStringList warnings;

private:
    QString m_primaryKey;
    QStringList m_keyOrder;
    std::vector<std::unique_ptr<ConfFile>> m_files;
    QVector<EntryPtr> m_entries;
};

// ananicy.conf: simple key=value file, comments preserved.
class MainConf {
public:
    struct Line { QString raw; QString key; QString value; bool kv = false; };

    QString path;
    bool existed = false;
    bool dirty = false;

    void load(const QString &path);
    bool has(const QString &key) const;
    QString get(const QString &key) const;
    void set(const QString &key, const QString &value);
    void unset(const QString &key);
    QStringList keys() const;
    QByteArray render() const;

private:
    QVector<Line> m_lines;
    QString m_sep = QStringLiteral("=");
};

QString jsonValueText(const QJsonValue &v);
