#include "games.h"
#include "gma.h"
#include <algorithm>
#include <qt_windows.h>
#include <tlhelp32.h>

QJsonObject readObject(const QString &path, QString *error) {
    if (error)
        error->clear();
    QFile f(path);
    if (!f.exists())
        return {};
    if (!f.open(QIODevice::ReadOnly) || f.size() > 32 * 1024 * 1024) {
        if (error)
            *error = QStringLiteral("Cannot read %1. Check its size and permissions.").arg(path);
        return {};
    }
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &parse);
    if (f.error() != QFileDevice::NoError || parse.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error =
                QStringLiteral("Invalid data in %1. Restore the file from a backup before changing mods.")
                    .arg(path);
        return {};
    }
    return doc.object();
}
QJsonObject installedRecords(QString *error) {
    return readObject(dataRoot() + QStringLiteral("/installations.json"), error);
}
namespace {
struct Catalog {
    QRecursiveMutex lock;
    QList<GameProfile> profiles;
    bool profilesLoaded = false;
    QJsonObject names;
    bool namesLoaded = false;
};
Catalog &catalog() {
    static Catalog instance;
    return instance;
}
QString gameRulesPath() {
    return dataRoot() + QStringLiteral("/games.json");
}
QString gameNamesPath() {
    return dataRoot() + QStringLiteral("/cache/games.json");
}
GameProfile parseProfile(const QJsonObject &object) {
    return {object.value(QStringLiteral("id")).toString(),
            object.value(QStringLiteral("name")).toString(),
            object.value(QStringLiteral("modFolder")).toString(),
            object.value(QStringLiteral("format")).toString(QStringLiteral("manual")),
            object.value(QStringLiteral("note")).toString(),
            object.value(QStringLiteral("activation")).toString()};
}
QJsonObject profileObject(const GameProfile &profile) {
    return {{QStringLiteral("id"), profile.id},
            {QStringLiteral("name"), profile.name},
            {QStringLiteral("modFolder"), profile.subdir},
            {QStringLiteral("format"), profile.format},
            {QStringLiteral("note"), profile.note},
            {QStringLiteral("activation"), profile.activation}};
}
void mergeProfiles(QList<GameProfile> &profiles, const QJsonArray &rows) {
    for (const auto &row : rows) {
        const auto profile = parseProfile(row.toObject());
        if (!validId(profile.id) || profile.name.isEmpty())
            continue;
        auto it = std::find_if(profiles.begin(), profiles.end(),
                               [&profile](const GameProfile &p) { return p.id == profile.id; });
        if (it != profiles.end())
            *it = profile;
        else
            profiles.append(profile);
    }
}
}
QList<GameProfile> gameProfiles() {
    auto &c = catalog();
    QMutexLocker locker(&c.lock);
    if (!c.profilesLoaded) {
        c.profiles.clear();
        mergeProfiles(c.profiles,
                      readObject(QStringLiteral(":/data/games.json")).value(QStringLiteral("games")).toArray());
        mergeProfiles(c.profiles, readObject(gameRulesPath()).value(QStringLiteral("games")).toArray());
        c.profilesLoaded = true;
    }
    return c.profiles;
}
bool hasProfile(const QString &id) {
    const auto profiles = gameProfiles();
    return std::any_of(profiles.cbegin(), profiles.cend(), [&id](const GameProfile &p) { return p.id == id; });
}
GameProfile profileFor(const QString &id) {
    for (const auto &p : gameProfiles())
        if (p.id == id)
            return p;
    return {id,
            gameName(id),
            {},
            QStringLiteral("manual"),
            QStringLiteral("Download only. Open the folder and follow the game's mod instructions, or set a mod "
                           "folder to install from here."),
            {}};
}
QString supportLevel(const GameProfile &profile) {
    if (!profile.activation.isEmpty())
        return QStringLiteral("Full");
    return profile.format == QStringLiteral("manual") ? QStringLiteral("Download only")
                                                      : QStringLiteral("Install");
}
bool saveGameRule(const GameProfile &profile, QString *error) {
    auto &c = catalog();
    QMutexLocker locker(&c.lock);
    auto rows = readObject(gameRulesPath(), error).value(QStringLiteral("games")).toArray();
    if (error && !error->isEmpty())
        return false;
    for (int n = int(rows.size()) - 1; n >= 0; --n)
        if (rows[n].toObject().value(QStringLiteral("id")).toString() == profile.id)
            rows.removeAt(n);
    rows.append(profileObject(profile));
    if (!writeJson(gameRulesPath(), QJsonDocument(QJsonObject{{QStringLiteral("games"), rows}}), error))
        return false;
    c.profilesLoaded = false;
    return true;
}
QString knownGameName(const QString &id) {
    auto &c = catalog();
    QMutexLocker locker(&c.lock);
    if (!c.namesLoaded) {
        c.names = readObject(gameNamesPath());
        c.namesLoaded = true;
    }
    return c.names.value(id).toObject().value(QStringLiteral("name")).toString();
}
QString knownGameImage(const QString &id) {
    knownGameName(id);
    auto &c = catalog();
    QMutexLocker locker(&c.lock);
    return c.names.value(id).toObject().value(QStringLiteral("image")).toString();
}
void rememberGame(const QString &id, const QString &name, const QString &image) {
    knownGameName(id);
    auto &c = catalog();
    QMutexLocker locker(&c.lock);
    c.names.insert(id, QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("image"), image}});
    QDir().mkpath(QFileInfo(gameNamesPath()).absolutePath());
    writeJson(gameNamesPath(), QJsonDocument(c.names));
}
bool gameRunning(const QString &folder) {
    if (folder.isEmpty())
        return false;
    const QString prefix = QDir::toNativeSeparators(QDir::cleanPath(folder)) + QLatin1Char('\\');
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool running = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !running;
         more = Process32NextW(snapshot, &entry)) {
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process)
            continue;
        wchar_t path[MAX_PATH * 4];
        DWORD size = MAX_PATH * 4;
        if (QueryFullProcessImageNameW(process, 0, path, &size))
            running = QString::fromWCharArray(path, int(size)).startsWith(prefix, Qt::CaseInsensitive);
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return running;
}
QString gameDestination(const QString &id, const QString &folder) {
    QString subdir = profileFor(id).subdir;
    if (subdir.isEmpty())
        return {};
    const QHash<QString, QString> roots{
        {QStringLiteral("@home"), QDir::homePath()},
        {QStringLiteral("@documents"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)},
        {QStringLiteral("@roaming"), qEnvironmentVariable("APPDATA")},
        {QStringLiteral("@local"), qEnvironmentVariable("LOCALAPPDATA")}};
    for (auto it = roots.begin(); it != roots.end(); ++it)
        if (subdir.startsWith(it.key() + QLatin1Char('/')))
            return it.value() + subdir.mid(it.key().size());
    if (folder.isEmpty())
        return {};
    if (id == QStringLiteral("268500") &&
        QDir(folder + QStringLiteral("/XCom2-WarOfTheChosen/XComGame")).exists())
        subdir = QStringLiteral("XCom2-WarOfTheChosen/XComGame/Mods");
    return QDir::cleanPath(folder + QLatin1Char('/') + subdir);
}
QHash<QString, QString> discoverGames() {
    QStringList steamRoots;
    QSettings steam(QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam"), QSettings::NativeFormat);
    steamRoots << steam.value(QStringLiteral("SteamPath")).toString()
               << QStringLiteral("C:/Program Files (x86)/Steam") << QStringLiteral("C:/Program Files/Steam");
    QStringList libraries;
    auto pairs = [](const QString &path) {
        QFile f(path);
        QList<QPair<QString, QString>> result;
        if (!f.open(QIODevice::ReadOnly) || f.size() > 8 * 1024 * 1024)
            return result;
        auto matches = QRegularExpression(QStringLiteral("\"([^\"\\\\]+)\"\\s*\"([^\"]*)\""))
                           .globalMatch(QString::fromUtf8(f.readAll()));
        while (matches.hasNext()) {
            auto m = matches.next();
            result.append(
                {m.captured(1), m.captured(2).replace(QStringLiteral("\\\\"), QStringLiteral("/"))});
        }
        return result;
    };
    for (const auto &root : steamRoots)
        if (!root.isEmpty() && QDir(root).exists()) {
            libraries << root;
            for (const auto &pair : pairs(root + QStringLiteral("/steamapps/libraryfolders.vdf")))
                if (pair.first == QStringLiteral("path"))
                    libraries << pair.second;
        }
    libraries.removeDuplicates();
    QHash<QString, QString> result;
    for (const auto &library : libraries)
        for (const auto &profile : gameProfiles()) {
            for (const auto &pair : pairs(library + QStringLiteral("/steamapps/appmanifest_") + profile.id +
                                          QStringLiteral(".acf")))
                if (pair.first == QStringLiteral("installdir") &&
                    !pair.second.contains(QStringLiteral("..")) && !pair.second.contains(QLatin1Char(':'))) {
                    const QString path = library + QStringLiteral("/steamapps/common/") + pair.second;
                    if (QDir(path).exists())
                        result.insert(profile.id, path);
                }
        }
    auto normal = [](QString s) {
        return s.toCaseFolded().remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
    };
    auto addNamed = [&](const QString &name, const QString &path) {
        if (!QDir(path).exists())
            return;
        for (const auto &p : gameProfiles())
            if (!result.contains(p.id) && normal(name) == normal(p.name))
                result.insert(p.id, path);
    };
    const QDir epic(qEnvironmentVariable("PROGRAMDATA") +
                    QStringLiteral("/Epic/EpicGamesLauncher/Data/Manifests"));
    for (const auto &name : epic.entryList({QStringLiteral("*.item")}, QDir::Files)) {
        const auto o = readObject(epic.filePath(name));
        addNamed(o.value(QStringLiteral("DisplayName")).toString(),
                 o.value(QStringLiteral("InstallLocation")).toString());
    }
    for (const auto &key :
         {QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
          QStringLiteral(
              "HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
          QStringLiteral("HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall")}) {
        QSettings registry(key, QSettings::NativeFormat);
        for (const auto &entry : registry.childGroups()) {
            registry.beginGroup(entry);
            addNamed(registry.value(QStringLiteral("DisplayName")).toString(),
                     registry.value(QStringLiteral("InstallLocation")).toString());
            registry.endGroup();
        }
    }
    return result;
}

namespace {
QString indexPath() {
    return dataRoot() + QStringLiteral("/installations.json");
}
bool removePath(const QString &path) {
    if (!QFileInfo::exists(path))
        return true;
    if (linkedAncestor(path))
        return false;
    return QFileInfo(path).isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
}
QString hashPath(const QString &path) {
    if (linkedAncestor(path))
        return {};
    QStringList files;
    if (QFileInfo(path).isFile())
        files << path;
    else {
        QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (it.hasNext())
            files << it.next();
    }
    files.sort();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto &file : files) {
        if (linkedAncestor(file))
            return {};
        hash.addData(QDir(path).relativeFilePath(file).toUtf8());
        hash.addData(QByteArrayView("\0", 1));
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        while (!f.atEnd()) {
            const auto bytes = f.read(1024 * 1024);
            if (bytes.isEmpty() && f.error() != QFileDevice::NoError)
                return {};
            hash.addData(bytes);
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}
}
QString fingerprintPath(const QString &path) {
    qint64 count = 0, bytes = 0, newest = 0;
    const auto add = [&](const QFileInfo &info) {
        ++count;
        bytes += info.size();
        newest = qMax(newest, info.lastModified().toSecsSinceEpoch());
    };
    if (QFileInfo(path).isFile())
        add(QFileInfo(path));
    else {
        QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            add(it.fileInfo());
        }
    }
    return QStringLiteral("%1:%2:%3").arg(count).arg(bytes).arg(newest);
}
namespace {
bool containsPath(const QString &root, const QString &path) {
    const QString relative = QDir(root).relativeFilePath(path);
    return relative != QStringLiteral("..") && !relative.startsWith(QStringLiteral("../")) &&
           !QDir::isAbsolutePath(relative) && relative != QStringLiteral(".");
}
QString transaction(const QString &key, const QJsonObject &record, const QJsonArray &moves,
                    const QStringList &cleanup) {
    QString error;
    auto index = installedRecords(&error);
    if (!error.isEmpty())
        return error;
    const QString folder = dataRoot() + QStringLiteral("/transactions");
    QDir().mkpath(folder);
    const QString journal =
        folder + QLatin1Char('/') + QUuid::createUuid().toString(QUuid::Id128) + QStringLiteral(".json");
    QJsonArray clean;
    for (const auto &p : cleanup)
        clean.append(p);
    const auto previous = index.value(key).toObject();
    QJsonObject log{{QStringLiteral("key"), key},
                    {QStringLiteral("record"), record},
                    {QStringLiteral("previous"), previous},
                    {QStringLiteral("moves"), moves},
                    {QStringLiteral("cleanup"), clean}};
    if (!writeJson(journal, QJsonDocument(log), &error))
        return error;
    int completed = 0;
    for (const auto &v : moves) {
        const auto m = v.toObject();
        const QString from = m.value(QStringLiteral("from")).toString(),
                      to = m.value(QStringLiteral("to")).toString();
        if (linkedAncestor(from) || linkedAncestor(to) || !QDir().mkpath(QFileInfo(to).absolutePath()) ||
            !renamePath(from, to)) {
            error = QStringLiteral("Could not move %1. Close the game and check permissions.")
                        .arg(QFileInfo(from).fileName());
            break;
        }
        ++completed;
    }
    if (error.isEmpty()) {
        if (record.isEmpty())
            index.remove(key);
        else
            index.insert(key, record);
        if (!writeJson(indexPath(), QJsonDocument(index), &error))
            error.prepend(QStringLiteral("Could not save installation records: "));
    }
    if (!error.isEmpty()) {
        bool restored = true;
        for (int n = completed - 1; n >= 0; --n) {
            const auto m = moves[n].toObject();
            restored = QDir().rename(m.value(QStringLiteral("to")).toString(),
                                     m.value(QStringLiteral("from")).toString()) &&
                       restored;
        }
        if (!restored)
            return error + QStringLiteral(" Recovery will be attempted at next startup.");
    }
    log.insert(QStringLiteral("cleanupOnly"), true);
    QString journalError;
    if (!writeJson(journal, QJsonDocument(log), &journalError))
        return error + QStringLiteral(" Could not save cleanup recovery state: ") + journalError;
    QStringList remaining;
    for (const auto &p : cleanup)
        if (!removePath(p))
            remaining << p;
    if (!remaining.isEmpty())
        return error + QStringLiteral(" Could not remove leftover files: ") +
               remaining.join(QStringLiteral(", ")) +
               QStringLiteral(". Recovery details were kept; cleanup will retry at next startup.");
    if (!QFile::remove(journal))
        return error + QStringLiteral(" Could not remove the completed recovery journal: ") + journal;
    return error;
}
QString disabledRoot(const QJsonObject &r) {
    return QFileInfo(r.value(QStringLiteral("root")).toString()).absolutePath() +
           QStringLiteral("/.workshopdl-disabled/") + r.value(QStringLiteral("app")).toString() +
           QLatin1Char('/') + r.value(QStringLiteral("item")).toString();
}
}

QString recoverInstallations() {
    QString indexError;
    installedRecords(&indexError);
    if (!indexError.isEmpty())
        return indexError;
    const QDir dir(dataRoot() + QStringLiteral("/transactions"));
    QStringList failures;
    for (const auto &file : dir.entryList({QStringLiteral("*.json")}, QDir::Files)) {
        auto log = readObject(dir.filePath(file));
        const QString key = log.value(QStringLiteral("key")).toString();
        const auto record = log.value(QStringLiteral("record")).toObject();
        const auto moves = log.value(QStringLiteral("moves")).toArray();
        if (key.isEmpty() || moves.isEmpty())
            continue;
        bool safe = true;
        const auto old = log.value(QStringLiteral("previous")).toObject();
        const QString root =
            record.value(QStringLiteral("root")).toString(old.value(QStringLiteral("root")).toString());
        const QString disabled = disabledRoot(record.isEmpty() ? old : record);
        for (const auto &v : moves)
            for (const auto &field : {QStringLiteral("from"), QStringLiteral("to")}) {
                const QString path = v.toObject().value(field).toString();
                if (root.isEmpty() || linkedAncestor(path) ||
                    (!containsPath(root, path) && !containsPath(disabled, path)))
                    safe = false;
            }
        if (!safe) {
            failures << file;
            continue;
        }
        const bool committed = record.isEmpty() ? !installedRecords().contains(key)
                                                : installedRecords().value(key).toObject() == record;
        bool ok = true;
        if (!log.value(QStringLiteral("cleanupOnly")).toBool() && !committed)
            for (int n = int(moves.size()) - 1; n >= 0; --n) {
                const auto m = moves[n].toObject();
                const QString from = m.value(QStringLiteral("from")).toString(),
                              to = m.value(QStringLiteral("to")).toString();
                if (!QFileInfo::exists(from) && QFileInfo::exists(to)) {
                    QDir().mkpath(QFileInfo(from).absolutePath());
                    ok = QDir().rename(to, from) && ok;
                }
            }
        if (!ok) {
            failures << file;
            continue;
        }
        log.insert(QStringLiteral("cleanupOnly"), true);
        if (!writeJson(dir.filePath(file), QJsonDocument(log))) {
            failures << file;
            continue;
        }
        for (const auto &v : log.value(QStringLiteral("cleanup")).toArray()) {
            const QString path = v.toString();
            if (containsPath(root, path) &&
                QFileInfo(path).fileName().startsWith(QStringLiteral(".workshopdl-")))
                ok = removePath(path) && ok;
            else
                ok = false;
        }
        if (!ok || !QFile::remove(dir.filePath(file)))
            failures << file;
    }
    return failures.isEmpty()
               ? QString()
               : QStringLiteral(
                     "Some interrupted installations need attention. Recovery records were retained: ") +
                     failures.join(QStringLiteral(", "));
}

QString installContent(const Item &item, const QString &destination, bool overwrite) {
    const auto profile = profileFor(item.appId);
    if (profile.format == QStringLiteral("manual"))
        return profile.note;
    QString error;
    const auto records = installedRecords(&error);
    if (!error.isEmpty())
        return error;
    if (!validId(item.id) || !validId(item.appId) || !safeTree(item.path, &error))
        return error.isEmpty() ? QStringLiteral("Invalid item identity.") : error;
    const QString root = QDir::cleanPath(QFileInfo(destination).absoluteFilePath());
    if (linkedAncestor(root) || !QDir().mkpath(root))
        return QStringLiteral("Choose a writable destination without directory links.");
    if (containsPath(item.path, root) || containsPath(root, item.path) ||
        root.compare(item.path, Qt::CaseInsensitive) == 0)
        return QStringLiteral("The game destination must be separate from the download folder.");
    const QString key = item.appId + QLatin1Char('/') + item.id;
    const auto old = records.value(key).toObject();
    if (!old.isEmpty() &&
        (old.value(QStringLiteral("root")).toString().compare(root, Qt::CaseInsensitive) != 0 ||
         old.value(QStringLiteral("disabled")).toBool()))
        return QStringLiteral("This item is installed elsewhere or disabled. Enable or uninstall it before "
                              "changing its destination.");
    const QString staging =
        root + QStringLiteral("/.workshopdl-stage-") + QUuid::createUuid().toString(QUuid::Id128);
    QDir().mkpath(staging);
    auto fail = [&](const QString &e) {
        removePath(staging);
        return e;
    };
    QList<QPair<QString, QString>> sources;
    const QString name = QStringLiteral("WorkshopDL-") + item.id;
    if (profile.format == QStringLiteral("gma")) {
        QStringList archives;
        QDirIterator it(item.path, {QStringLiteral("*.gma"), QStringLiteral("*.bin")}, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext())
            archives << it.next();
        if (archives.isEmpty()) {
            bool recognized = false;
            for (const auto &folder : {QStringLiteral("lua"), QStringLiteral("materials"),
                                       QStringLiteral("models"), QStringLiteral("maps")})
                recognized = recognized || QDir(item.path + QLatin1Char('/') + folder).exists();
            if (!recognized)
                return fail(QStringLiteral("No GMA archive or addon folders found."));
            sources.append({item.path, name});
        } else
            for (int n = 0; n < archives.size(); ++n) {
                const QString extracted = staging + QStringLiteral("/extracted-%1").arg(n);
                error = extractGma(archives[n], extracted);
                if (!error.isEmpty())
                    return fail(error);
                sources.append(
                    {extracted, name + (archives.size() > 1 ? QStringLiteral("-%1").arg(n + 1) : QString())});
            }
    } else if (profile.format == QStringLiteral("rimworld")) {
        if (!QFileInfo::exists(item.path + QStringLiteral("/About/About.xml")))
            return fail(QStringLiteral("This is not a recognized RimWorld mod: About/About.xml is missing."));
        sources.append({item.path, name});
    } else if (profile.format == QStringLiteral("zomboid")) {
        QDirIterator it(item.path, {QStringLiteral("mod.info")}, QDir::Files, QDirIterator::Subdirectories);
        QSet<QString> roots;
        while (it.hasNext()) {
            QString root = QFileInfo(it.next()).absolutePath();
            const auto leaf = QFileInfo(root).fileName();
            if (leaf == QStringLiteral("common") ||
                QRegularExpression(QStringLiteral("^[0-9]+(?:\\.[0-9]+)*$")).match(leaf).hasMatch())
                root = QFileInfo(root).absolutePath();
            roots.insert(root);
        }
        QStringList ordered = roots.values();
        ordered.sort();
        for (const auto &root : ordered)
            sources.append({root, name + QLatin1Char('-') + QFileInfo(root).fileName()});
        if (sources.isEmpty())
            return fail(QStringLiteral("No Project Zomboid mod.info folders found."));
    } else if (profile.format.startsWith(QStringLiteral("files:"))) {
        QStringList filters;
        for (const auto &ext : profile.format.mid(6).split(QLatin1Char(',')))
            filters << QLatin1Char('*') + ext;
        QDirIterator it(item.path, filters, QDir::Files, QDirIterator::Subdirectories);
        QSet<QString> names;
        while (it.hasNext()) {
            const auto source = it.next();
            const auto filename = QFileInfo(source).fileName();
            if (names.contains(filename.toCaseFolded()))
                return fail(QStringLiteral("The package contains conflicting filenames."));
            names.insert(filename.toCaseFolded());
            sources.append({source, filename});
        }
        if (sources.isEmpty())
            return fail(QStringLiteral("No matching game packages found (%1).").arg(profile.format.mid(6)));
    } else if (profile.format == QStringLiteral("ark")) {
        const QString descriptor = item.path + QLatin1Char('/') + item.id + QStringLiteral(".mod"),
                      content = item.path + QLatin1Char('/') + item.id;
        if (!QFileInfo::exists(descriptor) || !QDir(content).exists())
            return fail(profile.note);
        sources.append({descriptor, item.id + QStringLiteral(".mod")});
        sources.append({content, item.id});
    } else if (profile.format == QStringLiteral("tabletop")) {
        const auto files = QDir(item.path).entryList({QStringLiteral("*.json")}, QDir::Files);
        if (files.size() != 1)
            return fail(QStringLiteral("Expected one Tabletop Simulator JSON save file."));
        if (!readObject(QDir(item.path).filePath(files.first())).contains(QStringLiteral("ObjectStates")))
            return fail(QStringLiteral("The file is not a Tabletop Simulator save."));
        sources.append({QDir(item.path).filePath(files.first()),
                        QStringLiteral("Mods/Workshop/") + item.id + QStringLiteral(".json")});
    } else {
        if (profile.format == QStringLiteral("portal") &&
            QDir(item.path).entryList({QStringLiteral("*.bsp")}, QDir::Files).isEmpty())
            return fail(QStringLiteral("No Portal 2 BSP map found."));
        sources.append({item.path, profile.format == QStringLiteral("portal") ? item.id : name});
    }
    QSet<QString> oldPaths;
    for (const auto &v : old.value(QStringLiteral("paths")).toArray())
        oldPaths.insert(v.toString());
    const auto oldHashes = old.value(QStringLiteral("hashes")).toObject();
    for (const auto &path : oldPaths) {
        const QString full = root + QLatin1Char('/') + path;
        if (!containsPath(root, full) || linkedAncestor(full))
            return fail(QStringLiteral("Installation records contain an unsafe path."));
        if (!overwrite && QFileInfo::exists(full) && hashPath(full) != oldHashes.value(path).toString())
            return fail(
                QStringLiteral(
                    "Installed files were modified. Keep or move those changes before replacing this mod: ") +
                path);
    }
    QJsonArray paths;
    QJsonObject hashes, fingerprints;
    QJsonArray moves;
    auto move = [&](const QString &from, const QString &to) {
        moves.append(QJsonObject{{QStringLiteral("from"), from}, {QStringLiteral("to"), to}});
    };
    int index = 0;
    for (const auto &source : sources) {
        const QString target = root + QLatin1Char('/') + source.second,
                      staged = staging + QStringLiteral("/new-%1").arg(index++);
        if (!containsPath(root, target) || linkedAncestor(target))
            return fail(QStringLiteral("Unsafe installation destination."));
        if (QFileInfo::exists(target) && !oldPaths.contains(source.second))
            return fail(QStringLiteral("Destination contains files this app does not own: ") + source.second);
        if (QFileInfo(source.first).isDir()) {
            if (!copyTree(source.first, staged, &error))
                return fail(error);
        } else if (!QFile::copy(source.first, staged))
            return fail(QStringLiteral("Could not stage package. Check free space."));
        const auto hash = hashPath(staged);
        if (hash.isEmpty())
            return fail(QStringLiteral("Could not verify staged content."));
        paths.append(source.second);
        hashes.insert(source.second, hash);
        fingerprints.insert(source.second, fingerprintPath(staged));
    }
    index = 0;
    for (const auto &path : oldPaths)
        if (QFileInfo::exists(root + QLatin1Char('/') + path))
            move(root + QLatin1Char('/') + path, staging + QStringLiteral("/old-%1").arg(index++));
    index = 0;
    for (const auto &source : sources)
        move(staging + QStringLiteral("/new-%1").arg(index++), root + QLatin1Char('/') + source.second);
    const QJsonObject record{
        {QStringLiteral("app"), item.appId},
        {QStringLiteral("item"), item.id},
        {QStringLiteral("title"), item.title},
        {QStringLiteral("root"), root},
        {QStringLiteral("paths"), paths},
        {QStringLiteral("hashes"), hashes},
        {QStringLiteral("fingerprints"), fingerprints},
        {QStringLiteral("updated"), QString::number(item.updated)},
        {QStringLiteral("disabled"), false},
        {QStringLiteral("installedAt"), QString::number(QDateTime::currentSecsSinceEpoch())}};
    return transaction(key, record, moves, {staging});
}

QString manageInstalled(const QString &key, const QString &action) {
    if (action != QStringLiteral("toggle") && action != QStringLiteral("delete"))
        return QStringLiteral("Unknown mod action.");
    QString indexError;
    auto index = installedRecords(&indexError);
    if (!indexError.isEmpty())
        return indexError;
    auto record = index.value(key).toObject();
    if (record.isEmpty())
        return QStringLiteral("Installation record is missing.");
    const QString root = record.value(QStringLiteral("root")).toString(), disabled = disabledRoot(record);
    const bool off = record.value(QStringLiteral("disabled")).toBool();
    const QString sourceRoot = off ? disabled : root;
    const QString destinationRoot = off ? root : disabled;
    QJsonArray moves;
    QStringList cleanup;
    const QString trash =
        root + QStringLiteral("/.workshopdl-remove-") + QUuid::createUuid().toString(QUuid::Id128);
    if (action == QStringLiteral("delete"))
        cleanup << trash;
    for (const auto &v : record.value(QStringLiteral("paths")).toArray()) {
        const QString relative = v.toString(), from = sourceRoot + QLatin1Char('/') + relative,
                      to = (action == QStringLiteral("delete") ? trash : destinationRoot) + QLatin1Char('/') +
                           relative;
        if (!containsPath(sourceRoot, from) ||
            !containsPath(action == QStringLiteral("delete") ? trash : destinationRoot, to) ||
            linkedAncestor(from) || linkedAncestor(to))
            return QStringLiteral("Unsafe managed mod path.");
        if (!QFileInfo::exists(from)) {
            if (action == QStringLiteral("delete"))
                continue;
            return QStringLiteral("Installed files are missing: ") + relative;
        }
        if (QFileInfo::exists(to))
            return QStringLiteral("Destination is occupied. No files were replaced.");
        moves.append(QJsonObject{{QStringLiteral("from"), from}, {QStringLiteral("to"), to}});
    }
    if (action == QStringLiteral("delete")) {
        if (moves.isEmpty()) {
            index.remove(key);
            QString error;
            writeJson(indexPath(), QJsonDocument(index), &error);
            return error;
        }
        return transaction(key, {}, moves, cleanup);
    }
    record.insert(QStringLiteral("disabled"), !off);
    return transaction(key, record, moves, cleanup);
}
