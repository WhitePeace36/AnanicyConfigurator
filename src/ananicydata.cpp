#include "ananicydata.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <algorithm>

// ---------------------------------------------------------------- helpers

QString jsonValueText(const QJsonValue &v)
{
    if (v.isString())
        return v.toString();
    if (v.isDouble()) {
        double d = v.toDouble();
        if (d == static_cast<long long>(d))
            return QString::number(static_cast<long long>(d));
        return QString::number(d);
    }
    if (v.isBool())
        return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (v.isUndefined() || v.isNull())
        return QString();
    QJsonArray a; a.append(v);
    QString s = QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
    return s.mid(1, s.size() - 2);
}

static QString jsonLiteral(const QJsonValue &v)
{
    QJsonArray a; a.append(v);
    QString s = QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
    return s.mid(1, s.size() - 2); // strip [ ]
}

static EntryPtr parseLine(const QString &line, QString *error)
{
    const QString t = line.trimmed();
    if (t.isEmpty() || t.startsWith(QLatin1Char('#')))
        return nullptr;

    QString jsonPart = t;
    QString trailing;
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(t.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        // maybe there is a trailing comment: { ... } # comment
        int close = t.lastIndexOf(QLatin1Char('}'));
        if (close > 0) {
            jsonPart = t.left(close + 1);
            trailing = t.mid(close + 1).trimmed();
            doc = QJsonDocument::fromJson(jsonPart.toUtf8(), &pe);
        }
    }
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = pe.errorString();
        return nullptr;
    }

    const QJsonObject obj = doc.object();
    QStringList keys = obj.keys();
    // QJsonObject sorts keys; recover the on-disk order.
    std::stable_sort(keys.begin(), keys.end(), [&](const QString &a, const QString &b) {
        return jsonPart.indexOf(QLatin1Char('"') + a + QLatin1Char('"'))
             < jsonPart.indexOf(QLatin1Char('"') + b + QLatin1Char('"'));
    });

    auto e = std::make_shared<Entry>();
    for (const QString &k : keys)
        e->fields.append({k, obj.value(k)});
    e->originalLine = line;
    e->trailing = trailing;
    return e;
}

// ---------------------------------------------------------------- Entry

bool Entry::has(const QString &key) const
{
    for (const auto &f : fields)
        if (f.first == key)
            return true;
    return false;
}

QJsonValue Entry::value(const QString &key) const
{
    for (const auto &f : fields)
        if (f.first == key)
            return f.second;
    return QJsonValue(QJsonValue::Undefined);
}

QString Entry::str(const QString &key) const
{
    return jsonValueText(value(key));
}

void Entry::set(const QString &key, const QJsonValue &v)
{
    for (int i = 0; i < fields.size(); ++i) {
        if (fields[i].first == key) {
            if (v.isUndefined() || v.isNull())
                fields.remove(i);
            else
                fields[i].second = v;
            return;
        }
    }
    if (!(v.isUndefined() || v.isNull()))
        fields.append({key, v});
}

QString Entry::serialize(const QStringList &keyOrder) const
{
    auto sorted = fields;
    auto rank = [&](const QString &k) {
        int i = keyOrder.indexOf(k);
        return i < 0 ? keyOrder.size() : i;
    };
    std::stable_sort(sorted.begin(), sorted.end(), [&](const auto &a, const auto &b) {
        return rank(a.first) < rank(b.first);
    });

    QStringList parts;
    for (const auto &f : sorted)
        parts << jsonLiteral(QJsonValue(f.first)) + QStringLiteral(": ") + jsonLiteral(f.second);
    QString s = QStringLiteral("{ ") + parts.join(QStringLiteral(", ")) + QStringLiteral(" }");
    if (!trailing.isEmpty())
        s += QLatin1Char(' ') + trailing;
    return s;
}

// ---------------------------------------------------------------- ConfFile

QString ConfFile::render(const QStringList &keyOrder) const
{
    QString out;
    for (const ConfLine &l : lines) {
        if (l.entry)
            out += (l.entry->modified || l.entry->originalLine.isEmpty())
                       ? l.entry->serialize(keyOrder)
                       : l.entry->originalLine;
        else
            out += l.raw;
        out += QLatin1Char('\n');
    }
    return out;
}

QByteArray contentHash(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha1);
}

QByteArray fileHash(const QString &path)
{
    QFile f(path);
    if (!QFileInfo::exists(path))
        return {};
    if (!f.open(QIODevice::ReadOnly))
        return QByteArrayLiteral("unreadable");
    return contentHash(f.readAll());
}

bool ConfFile::hasOtherContent() const
{
    for (const ConfLine &l : lines) {
        if (l.entry)
            continue;
        const QString t = l.raw.trimmed();
        if (!t.isEmpty() && !t.startsWith(QLatin1Char('#')))
            return true;
    }
    return false;
}

int ConfFile::entryCount() const
{
    int n = 0;
    for (const ConfLine &l : lines)
        if (l.entry)
            ++n;
    return n;
}

// ---------------------------------------------------------------- EntryStore

void EntryStore::clear()
{
    m_files.clear();
    m_entries.clear();
    warnings.clear();
}

void EntryStore::loadFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        warnings << QStringLiteral("Cannot read %1: %2").arg(path, f.errorString());
        return;
    }
    auto file = std::make_unique<ConfFile>();
    file->path = path;
    file->existed = true;

    const QByteArray raw = f.readAll();
    file->diskHash = contentHash(raw);
    QString content = QString::fromUtf8(raw);
    if (content.endsWith(QLatin1Char('\n')))
        content.chop(1);
    const QStringList lines = content.isEmpty() ? QStringList() : content.split(QLatin1Char('\n'));
    int lineNo = 0;
    for (QString line : lines) {
        ++lineNo;
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        QString err;
        EntryPtr e = parseLine(line, &err);
        if (e && !e->has(m_primaryKey)) {
            warnings << QStringLiteral("%1:%2: entry without \"%3\" kept as-is")
                            .arg(path).arg(lineNo).arg(m_primaryKey);
            e.reset();
        }
        if (!e && !err.isEmpty())
            warnings << QStringLiteral("%1:%2: invalid JSON (%3), line kept as-is")
                            .arg(path).arg(lineNo).arg(err);
        ConfLine cl;
        cl.raw = line;
        if (e) {
            e->file = file.get();
            cl.entry = e;
            m_entries.append(e);
        }
        file->lines.append(cl);
    }
    file->loadedEntries = file->entryCount();
    m_files.push_back(std::move(file));
}

void EntryStore::loadTree(const QString &dir, const QString &pattern, bool recursive)
{
    QStringList paths;
    // Like ananicy-cpp (std::filesystem::recursive_directory_iterator), do not descend into
    // symlinked directories. This also guarantees that nothing outside the config
    // directory is ever written or deleted through such a link.
    QDirIterator it(dir, QStringList{pattern}, QDir::Files | QDir::Readable,
                    recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    while (it.hasNext())
        paths << it.next();
    paths.sort(); // ananicy loads in alphabetical order
    for (const QString &p : paths)
        loadFile(p);
}

ConfFile *EntryStore::fileFor(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    for (auto &f : m_files)
        if (QDir::cleanPath(f->path) == clean)
            return f.get();
    auto file = std::make_unique<ConfFile>();
    file->path = clean;
    file->existed = QFileInfo::exists(clean);
    file->diskHash = fileHash(clean); // a not-loaded existing file will fail the safety check on save
    ConfFile *raw = file.get();
    m_files.push_back(std::move(file));
    return raw;
}

EntryPtr EntryStore::add(const QString &filePath, const QString &primaryValue)
{
    ConfFile *file = fileFor(filePath);
    auto e = std::make_shared<Entry>();
    e->fields.append({m_primaryKey, QJsonValue(primaryValue)});
    e->file = file;
    e->modified = true;
    file->lines.append({QString(), e});
    file->dirty = true;
    m_entries.append(e);
    return e;
}

void EntryStore::remove(const EntryPtr &e)
{
    if (!e)
        return;
    if (ConfFile *file = e->file) {
        for (int i = 0; i < file->lines.size(); ++i) {
            if (file->lines[i].entry == e) {
                file->lines.remove(i);
                break;
            }
        }
        file->dirty = true;
    }
    m_entries.removeAll(e);
}

void EntryStore::moveTo(const EntryPtr &e, const QString &filePath, bool reappend)
{
    ConfFile *target = fileFor(filePath);
    if (!e || (e->file == target && !reappend))
        return;
    if (ConfFile *old = e->file) {
        for (int i = 0; i < old->lines.size(); ++i) {
            if (old->lines[i].entry == e) {
                old->lines.remove(i);
                break;
            }
        }
        old->dirty = true;
    }
    target->lines.append({QString(), e});
    target->dirty = true;
    e->file = target;
}

void EntryStore::markModified(const EntryPtr &e)
{
    e->modified = true;
    if (e->file)
        e->file->dirty = true;
}

EntryPtr EntryStore::find(const QString &primaryValue) const
{
    // ananicy: later definitions override earlier ones -> return the last match
    for (int i = m_entries.size() - 1; i >= 0; --i)
        if (m_entries[i]->str(m_primaryKey) == primaryValue)
            return m_entries[i];
    return nullptr;
}

QStringList EntryStore::names() const
{
    QStringList n;
    for (const auto &e : m_entries)
        n << e->str(m_primaryKey);
    n.removeDuplicates();
    n.sort(Qt::CaseInsensitive);
    return n;
}

bool EntryStore::isDirty() const
{
    for (const auto &f : m_files)
        if (f->dirty)
            return true;
    return false;
}

QMap<QString, QByteArray> EntryStore::pendingWrites() const
{
    QMap<QString, QByteArray> out;
    for (const auto &f : m_files)
        if (f->dirty && !f->shouldDelete() && !f->shouldSkip())
            out.insert(f->path, f->render(m_keyOrder).toUtf8());
    return out;
}

bool EntryStore::loadedHash(const QString &path, QByteArray *hash) const
{
    const QString clean = QDir::cleanPath(path);
    for (const auto &f : m_files) {
        if (QDir::cleanPath(f->path) == clean) {
            *hash = f->diskHash;
            return true;
        }
    }
    return false;
}

QStringList EntryStore::pendingDeletes() const
{
    QStringList out;
    for (const auto &f : m_files)
        if (f->shouldDelete())
            out << f->path;
    return out;
}

void EntryStore::markSaved()
{
    for (auto &f : m_files) {
        if (!f->dirty)
            continue;
        if (f->shouldDelete() || f->shouldSkip()) {
            f->lines.clear();
            f->existed = false;
            f->loadedEntries = 0;
            f->diskHash.clear();
            f->dirty = false;
            continue;
        }
        for (ConfLine &l : f->lines) {
            if (l.entry && (l.entry->modified || l.entry->originalLine.isEmpty())) {
                l.entry->originalLine = l.entry->serialize(m_keyOrder);
                l.entry->modified = false;
            }
        }
        f->dirty = false;
        f->existed = true;
        f->loadedEntries = f->entryCount();
        f->diskHash = contentHash(f->render(m_keyOrder).toUtf8());
    }
}

// ---------------------------------------------------------------- MainConf

void MainConf::load(const QString &p)
{
    path = p;
    m_lines.clear();
    dirty = false;
    m_sep = QStringLiteral("=");
    QFile f(p);
    diskHash.clear();
    existed = f.open(QIODevice::ReadOnly | QIODevice::Text);
    if (!existed)
        return;
    const QByteArray raw = f.readAll();
    diskHash = contentHash(raw);
    QString content = QString::fromUtf8(raw);
    if (content.endsWith(QLatin1Char('\n')))
        content.chop(1);
    bool sepDetected = false;
    for (QString raw : content.split(QLatin1Char('\n'))) {
        if (raw.endsWith(QLatin1Char('\r')))
            raw.chop(1);
        Line l;
        l.raw = raw;
        const QString t = raw.trimmed();
        int eq = t.indexOf(QLatin1Char('='));
        if (!t.isEmpty() && !t.startsWith(QLatin1Char('#')) && eq > 0) {
            l.kv = true;
            l.key = t.left(eq).trimmed();
            l.value = t.mid(eq + 1).trimmed();
            if (!sepDetected) {
                sepDetected = true;
                if (t.contains(QStringLiteral(" = ")))
                    m_sep = QStringLiteral(" = ");
            }
        }
        m_lines.append(l);
    }
}

bool MainConf::has(const QString &key) const
{
    for (const Line &l : m_lines)
        if (l.kv && l.key == key)
            return true;
    return false;
}

QString MainConf::get(const QString &key) const
{
    QString v;
    for (const Line &l : m_lines)
        if (l.kv && l.key == key)
            v = l.value; // last one wins
    return v;
}

void MainConf::set(const QString &key, const QString &value)
{
    bool found = false;
    for (int i = m_lines.size() - 1; i >= 0; --i) {
        Line &l = m_lines[i];
        if (l.kv && l.key == key) {
            if (l.value != value) {
                l.value = value;
                l.raw = key + m_sep + value;
                dirty = true;
            }
            found = true;
            break;
        }
    }
    if (!found) {
        Line l;
        l.kv = true;
        l.key = key;
        l.value = value;
        l.raw = key + m_sep + value;
        m_lines.append(l);
        dirty = true;
    }
}

void MainConf::unset(const QString &key)
{
    for (int i = m_lines.size() - 1; i >= 0; --i) {
        if (m_lines[i].kv && m_lines[i].key == key) {
            m_lines.remove(i);
            dirty = true;
        }
    }
}

QStringList MainConf::keys() const
{
    QStringList k;
    for (const Line &l : m_lines)
        if (l.kv && !k.contains(l.key))
            k << l.key;
    return k;
}

QByteArray MainConf::render() const
{
    QString out;
    for (const Line &l : m_lines)
        out += l.raw + QLatin1Char('\n');
    return out.toUtf8();
}
