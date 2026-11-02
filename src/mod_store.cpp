#include "mod_store.h"
#include <QtXml/QDomDocument>
#include <algorithm>

namespace {
const QString rimworldId = QStringLiteral("294100");

QString installedRoot(const QJsonObject &record) {
    const auto root = record.value(QStringLiteral("root")).toString();
    return record.value(QStringLiteral("disabled")).toBool()
               ? QFileInfo(root).absolutePath() + QStringLiteral("/.workshopdl-disabled/") +
                     record.value(QStringLiteral("app")).toString() + QLatin1Char('/') +
                     record.value(QStringLiteral("item")).toString()
               : root;
}
QString packageIdFor(const QJsonObject &record) {
    const auto profile = profileFor(record.value(QStringLiteral("app")).toString());
    for (const auto &p : record.value(QStringLiteral("paths")).toArray()) {
        const QString path = installedRoot(record) + QLatin1Char('/') + p.toString();
        if (profile.activation == QStringLiteral("rimworld")) {
            const auto mod = readRimMod(path + QStringLiteral("/About/About.xml"));
            if (!mod.id.isEmpty())
                return mod.id;
        } else if (profile.activation == QStringLiteral("factorio")) {
            if (QFileInfo(path).isDir())
                return readObject(path + QStringLiteral("/info.json")).value(QStringLiteral("name")).toString();
            return QFileInfo(path).completeBaseName().remove(
                QRegularExpression(QStringLiteral("_[0-9]+(?:\\.[0-9]+)*$")));
        } else if (profile.activation == QStringLiteral("tmodloader"))
            return QFileInfo(path).completeBaseName();
    }
    return {};
}
void pruneBackups(const QString &path) {
    const QFileInfo info(path);
    auto backups = info.dir().entryInfoList({info.fileName() + QStringLiteral(".workshopdl-*.bak")}, QDir::Files,
                                            QDir::Time);
    for (qsizetype n = 5; n < backups.size(); ++n)
        QFile::remove(backups[n].absoluteFilePath());
}
QString backupConfig(const QString &path) {
    const QString backup = path + QStringLiteral(".workshopdl-") +
                           QString::number(QDateTime::currentMSecsSinceEpoch()) + QStringLiteral(".bak");
    if (!QFile::copy(path, backup))
        return QStringLiteral("Could not back up ") + QFileInfo(path).fileName();
    pruneBackups(path);
    return {};
}
QString writeConfig(const QString &path, const QByteArray &bytes) {
    const QString error = backupConfig(path);
    if (!error.isEmpty())
        return error;
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit())
        return out.errorString();
    return {};
}
QString setActive(const QString &game, const QString &id, const QString &path, bool active) {
    if (path.isEmpty() || !QFileInfo::exists(path))
        return QStringLiteral("The game's mod list was not found. Start the game once, then try again.");
    if (linkedAncestor(path))
        return QStringLiteral("The game's mod list is inside a linked folder.");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024)
        return QStringLiteral("Could not read ") + QFileInfo(path).fileName();
    const QByteArray original = file.readAll();
    file.close();
    const QString activation = profileFor(game).activation;
    if (activation == QStringLiteral("rimworld")) {
        QStringList order = rimworldActive(path);
        const auto existing = std::find_if(order.begin(), order.end(), [&id](const QString &value) {
            return value.compare(id, Qt::CaseInsensitive) == 0;
        });
        if (active == (existing != order.end()))
            return {};
        if (active)
            order.append(id.toLower());
        else
            order.erase(existing);
        return setRimworldOrder(path, order);
    }
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(original, &parse);
    if (parse.error != QJsonParseError::NoError)
        return QStringLiteral("The game's mod list has an unexpected format.");
    if (activation == QStringLiteral("factorio")) {
        auto object = doc.object();
        auto mods = object.value(QStringLiteral("mods")).toArray();
        bool found = false;
        for (int n = 0; n < mods.size(); ++n) {
            auto row = mods[n].toObject();
            if (row.value(QStringLiteral("name")).toString() == id) {
                row.insert(QStringLiteral("enabled"), active);
                mods[n] = row;
                found = true;
            }
        }
        if (!found)
            mods.append(QJsonObject{{QStringLiteral("name"), id}, {QStringLiteral("enabled"), active}});
        object.insert(QStringLiteral("mods"), mods);
        return writeConfig(path, QJsonDocument(object).toJson());
    }
    QJsonArray enabled;
    bool present = false;
    for (const auto &value : doc.array()) {
        if (value.toString() == id) {
            present = true;
            if (!active)
                continue;
        }
        enabled.append(value);
    }
    if (active && !present)
        enabled.append(id);
    return writeConfig(path, QJsonDocument(enabled).toJson());
}
QStringList listOf(const QDomElement &root, const QString &tag) {
    QStringList values;
    const auto list = root.firstChildElement(tag);
    for (auto node = list.firstChildElement(QStringLiteral("li")); !node.isNull();
         node = node.nextSiblingElement(QStringLiteral("li"))) {
        const auto nested = node.firstChildElement(QStringLiteral("packageId"));
        const QString value = (nested.isNull() ? node.text() : nested.text()).trimmed();
        if (!value.isEmpty())
            values << value.toCaseFolded();
    }
    return values;
}
QString profilesPath() {
    return dataRoot() + QStringLiteral("/profiles.json");
}
}

QString setModActive(const QString &game, const QString &id, const QString &path, bool active) {
    return setActive(game, id, path, active);
}

QString gameFolderFor(const QString &game, const QHash<QString, QString> &found) {
    QSettings settings(dataRoot() + QStringLiteral("/settings.ini"), QSettings::IniFormat);
    return settings.value(QStringLiteral("gameFolders/") + game, found.value(game)).toString();
}

QString modRootFor(const QString &game, const QHash<QString, QString> &found) {
    QSettings settings(dataRoot() + QStringLiteral("/settings.ini"), QSettings::IniFormat);
    const QString saved = settings.value(QStringLiteral("destinations/") + game).toString();
    if (!saved.isEmpty())
        return saved;
    const auto records = installedRecords();
    for (const auto &value : records)
        if (value.toObject().value(QStringLiteral("app")).toString() == game)
            return value.toObject().value(QStringLiteral("root")).toString();
    return gameDestination(game, gameFolderFor(game, found));
}

QString activationConfig(const QString &game, const QString &root) {
    QSettings settings(dataRoot() + QStringLiteral("/settings.ini"), QSettings::IniFormat);
    const QString custom = settings.value(QStringLiteral("activation/") + game).toString();
    if (!custom.isEmpty())
        return custom;
    const QString activation = profileFor(game).activation;
    if (activation == QStringLiteral("rimworld"))
        return QFileInfo(qEnvironmentVariable("LOCALAPPDATA")).absolutePath() +
               QStringLiteral("/LocalLow/Ludeon Studios/RimWorld by Ludeon Studios/Config/ModsConfig.xml");
    if (activation == QStringLiteral("factorio"))
        return root + QStringLiteral("/mod-list.json");
    if (activation == QStringLiteral("tmodloader"))
        return root + QStringLiteral("/enabled.json");
    return {};
}

QHash<QString, bool> activationStates(const QString &game, const QString &path) {
    QHash<QString, bool> states;
    if (path.isEmpty())
        return states;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 4 * 1024 * 1024)
        return states;
    const auto data = f.readAll();
    const QString activation = profileFor(game).activation;
    if (activation == QStringLiteral("rimworld")) {
        QDomDocument doc;
        if (!doc.setContent(data))
            return states;
        auto active = doc.documentElement().firstChildElement(QStringLiteral("activeMods"));
        for (auto e = active.firstChildElement(QStringLiteral("li")); !e.isNull();
             e = e.nextSiblingElement(QStringLiteral("li")))
            states.insert(e.text().trimmed().toCaseFolded(), true);
    } else if (activation == QStringLiteral("factorio")) {
        for (const auto &v : QJsonDocument::fromJson(data).object().value(QStringLiteral("mods")).toArray()) {
            const auto o = v.toObject();
            states.insert(o.value(QStringLiteral("name")).toString(), o.value(QStringLiteral("enabled")).toBool());
        }
    } else if (activation == QStringLiteral("tmodloader")) {
        for (const auto &v : QJsonDocument::fromJson(data).array())
            states.insert(v.toString(), true);
    }
    return states;
}

QList<ModEntry> scanMods(const QList<Item> &library, const QString &game,
                         const QHash<QString, QString> &found) {
    const auto records = installedRecords();
    QHash<QString, QHash<QString, bool>> stateCache;
    QHash<QString, QString> configCache;
    const auto statesFor = [&](const QString &app, const QString &root) -> const QHash<QString, bool> & {
        if (!stateCache.contains(app)) {
            const QString config = activationConfig(app, root);
            const bool exists = !config.isEmpty() && QFileInfo::exists(config);
            configCache.insert(app, exists ? config : QString());
            stateCache.insert(app, exists ? activationStates(app, config) : QHash<QString, bool>{});
        }
        return stateCache[app];
    };
    const auto describe = [&](ModEntry &entry, const Item *item, const QJsonObject &record) {
        const auto profile = profileFor(entry.appId);
        entry.game = profile.name;
        if (!record.isEmpty()) {
            entry.installed = true;
            entry.recordKey = entry.appId + QLatin1Char('/') + entry.id;
            const QString root = installedRoot(record);
            const bool disabled = record.value(QStringLiteral("disabled")).toBool();
            bool missing = false;
            for (const auto &p : record.value(QStringLiteral("paths")).toArray())
                missing = missing || !QFileInfo::exists(root + QLatin1Char('/') + p.toString());
            entry.packageId = packageIdFor(record);
            const auto &states = statesFor(entry.appId, record.value(QStringLiteral("root")).toString());
            entry.configManaged = !entry.packageId.isEmpty() && !configCache.value(entry.appId).isEmpty();
            entry.enabled = !disabled;
            if (entry.configManaged && !disabled)
                entry.enabled = states.value(entry.packageId.toCaseFolded(), states.value(entry.packageId, false));
            const auto installedPaths = record.value(QStringLiteral("paths")).toArray();
            entry.folder = installedPaths.isEmpty() ? root : root + QLatin1Char('/') + installedPaths.first().toString();
            bool modified = false;
            if (!game.isEmpty() && !missing) {
                const auto fingerprints = record.value(QStringLiteral("fingerprints")).toObject();
                for (auto it = fingerprints.begin(); it != fingerprints.end() && !modified; ++it)
                    modified = fingerprintPath(root + QLatin1Char('/') + it.key()) != it.value().toString();
            }
            const qint64 installedVersion = record.value(QStringLiteral("updated")).toString().toLongLong();
            entry.stale = item && installedVersion > 0 && item->updated > installedVersion;
            if (missing)
                entry.status = QStringLiteral("Missing files");
            else if (modified)
                entry.status = QStringLiteral("Modified");
            else if (entry.stale || (item && item->status == QStringLiteral("Update available")))
                entry.status = QStringLiteral("Update available");
            else
                entry.status = entry.enabled ? QStringLiteral("Enabled") : QStringLiteral("Disabled");
            if (entry.title.isEmpty())
                entry.title = record.value(QStringLiteral("title")).toString();
            return;
        }
        if (!item)
            return;
        if (item->status == QStringLiteral("Update available") || item->status == QStringLiteral("Missing files") ||
            item->status == QStringLiteral("Incomplete") || item->status == QStringLiteral("Unavailable"))
            entry.status = item->status;
        else
            entry.status = profile.format == QStringLiteral("manual") ? QStringLiteral("Downloaded")
                                                                      : QStringLiteral("Not installed");
    };
    QList<ModEntry> entries;
    QSet<QString> seen;
    for (const auto &item : library) {
        if (!game.isEmpty() && item.appId != game)
            continue;
        ModEntry entry;
        entry.id = item.id;
        entry.appId = item.appId;
        entry.title = item.title.isEmpty() ? QStringLiteral("Workshop item ") + item.id : item.title;
        entry.size = item.size;
        entry.updated = item.updated;
        entry.downloaded = true;
        entry.local = item.local;
        entry.folder = item.path;
        const QString key = item.appId + QLatin1Char('/') + item.id;
        seen.insert(key);
        describe(entry, &item, records.value(key).toObject());
        entries.append(entry);
    }
    for (auto it = records.begin(); it != records.end(); ++it) {
        if (seen.contains(it.key()))
            continue;
        const auto record = it.value().toObject();
        const QString app = record.value(QStringLiteral("app")).toString();
        if (!game.isEmpty() && app != game)
            continue;
        ModEntry entry;
        entry.id = record.value(QStringLiteral("item")).toString();
        entry.appId = app;
        describe(entry, nullptr, record);
        entries.append(entry);
    }
    const auto profile = profileFor(game);
    if (game.isEmpty() || profile.format == QStringLiteral("manual") ||
        profile.format == QStringLiteral("tabletop"))
        return entries;
    const QString root = modRootFor(game, found);
    if (root.isEmpty() || !QDir(root).exists())
        return entries;
    QSet<QString> owned;
    for (const auto &value : records) {
        const auto record = value.toObject();
        if (record.value(QStringLiteral("app")).toString() != game)
            continue;
        for (const auto &p : record.value(QStringLiteral("paths")).toArray())
            owned.insert(p.toString().section(QLatin1Char('/'), 0, 0).toCaseFolded());
    }
    QStringList extensions;
    if (profile.format.startsWith(QStringLiteral("files:")))
        extensions = profile.format.mid(6).split(QLatin1Char(','));
    const auto names = QDir(root).entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    int listed = 0;
    for (const auto &info : names) {
        if (listed >= 2000)
            break;
        const QString name = info.fileName();
        if (name.startsWith(QStringLiteral(".workshopdl")) || owned.contains(name.toCaseFolded()))
            continue;
        if (extensions.isEmpty() ? !info.isDir()
                                 : !std::any_of(extensions.cbegin(), extensions.cend(), [&name](const QString &e) {
                                       return name.endsWith(e, Qt::CaseInsensitive);
                                   }))
            continue;
        ModEntry entry;
        entry.id = QStringLiteral("unmanaged:") + name;
        entry.appId = game;
        entry.game = profile.name;
        entry.title = name;
        entry.status = QStringLiteral("Unmanaged");
        entry.unmanaged = true;
        entry.folder = info.absoluteFilePath();
        entry.size = info.isFile() ? info.size() : 0;
        entry.updated = info.lastModified().toSecsSinceEpoch();
        if (profile.activation == QStringLiteral("rimworld")) {
            const auto mod = readRimMod(info.absoluteFilePath() + QStringLiteral("/About/About.xml"));
            if (!mod.name.isEmpty())
                entry.title = mod.name;
        }
        entries.append(entry);
        ++listed;
    }
    return entries;
}

QString installMod(const Item &item, const QString &destination, bool overwrite) {
    const QString key = item.appId + QLatin1Char('/') + item.id;
    QString error;
    const auto records = installedRecords(&error);
    if (!error.isEmpty())
        return error;
    const auto old = records.value(key).toObject();
    const bool first = old.isEmpty(), disabled = old.value(QStringLiteral("disabled")).toBool();
    if (disabled) {
        error = manageInstalled(key, QStringLiteral("toggle"));
        if (!error.isEmpty())
            return error;
    }
    error = installContent(item, destination, overwrite);
    if (disabled) {
        const QString restore = manageInstalled(key, QStringLiteral("toggle"));
        if (error.isEmpty())
            error = restore;
    }
    if (!error.isEmpty() || !first || profileFor(item.appId).activation.isEmpty())
        return error;
    const auto record = installedRecords().value(key).toObject();
    const QString package = packageIdFor(record);
    const QString config = activationConfig(item.appId, record.value(QStringLiteral("root")).toString());
    if (!package.isEmpty() && QFileInfo::exists(config))
        setActive(item.appId, package, config, true);
    return {};
}

QString setModEnabled(const ModEntry &entry, bool enabled, const QHash<QString, QString> &found) {
    if (!entry.installed)
        return QStringLiteral("This mod is not installed.");
    const auto record = installedRecords().value(entry.recordKey).toObject();
    const bool movedAway = record.value(QStringLiteral("disabled")).toBool();
    if (entry.configManaged) {
        if (movedAway) {
            const QString error = manageInstalled(entry.recordKey, QStringLiteral("toggle"));
            if (!error.isEmpty())
                return error;
        }
        Q_UNUSED(found)
        return setActive(entry.appId, entry.packageId,
                         activationConfig(entry.appId, record.value(QStringLiteral("root")).toString()), enabled);
    }
    if (movedAway == !enabled)
        return {};
    return manageInstalled(entry.recordKey, QStringLiteral("toggle"));
}

QString uninstallMod(const ModEntry &entry, const QHash<QString, QString> &found) {
    Q_UNUSED(found)
    const auto record = installedRecords().value(entry.recordKey).toObject();
    if (entry.configManaged)
        setActive(entry.appId, entry.packageId,
                  activationConfig(entry.appId, record.value(QStringLiteral("root")).toString()), false);
    return manageInstalled(entry.recordKey, QStringLiteral("delete"));
}

RimMod readRimMod(const QString &aboutPath) {
    RimMod mod;
    QFile file(aboutPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
        return mod;
    QDomDocument doc;
    if (!doc.setContent(file.readAll()))
        return mod;
    const auto root = doc.documentElement();
    mod.id = root.firstChildElement(QStringLiteral("packageId")).text().trimmed();
    mod.name = root.firstChildElement(QStringLiteral("name")).text().trimmed();
    mod.dependencies = listOf(root, QStringLiteral("modDependencies"));
    mod.loadAfter = listOf(root, QStringLiteral("loadAfter")) + listOf(root, QStringLiteral("forceLoadAfter"));
    mod.loadBefore = listOf(root, QStringLiteral("loadBefore")) + listOf(root, QStringLiteral("forceLoadBefore"));
    mod.incompatible = listOf(root, QStringLiteral("incompatibleWith"));
    return mod;
}

QHash<QString, RimMod> rimworldMods(const QString &gameFolder, const QString &modRoot) {
    QStringList folders;
    if (!gameFolder.isEmpty()) {
        folders << gameFolder + QStringLiteral("/Data") << gameFolder + QStringLiteral("/Mods");
        const int common = gameFolder.indexOf(QStringLiteral("/steamapps/common/"), 0, Qt::CaseInsensitive);
        if (common > 0)
            folders << gameFolder.left(common) + QStringLiteral("/steamapps/workshop/content/294100");
    }
    if (!modRoot.isEmpty())
        folders << modRoot;
    folders.removeDuplicates();
    QHash<QString, RimMod> mods;
    for (const auto &folder : folders) {
        const auto names = QDir(folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto &name : names) {
            if (name.startsWith(QStringLiteral(".workshopdl")))
                continue;
            const auto mod = readRimMod(folder + QLatin1Char('/') + name + QStringLiteral("/About/About.xml"));
            if (!mod.id.isEmpty() && !mods.contains(mod.id.toCaseFolded()))
                mods.insert(mod.id.toCaseFolded(), mod);
        }
    }
    return mods;
}

QStringList rimworldSort(const QStringList &order, const QHash<QString, RimMod> &mods, QStringList *warnings) {
    const qsizetype count = order.size();
    QHash<QString, int> position;
    for (int n = 0; n < count; ++n)
        position.insert(order[n].toCaseFolded(), n);
    QList<QSet<int>> before(count);
    const QString core = QStringLiteral("ludeon.rimworld");
    const auto add = [&](const QString &first, int second) {
        const auto it = position.constFind(first);
        if (it != position.cend() && it.value() != second)
            before[second].insert(it.value());
    };
    for (int n = 0; n < count; ++n) {
        const QString id = order[n].toCaseFolded();
        const auto mod = mods.value(id);
        if (id != core && !mod.loadBefore.contains(core))
            add(core, n);
        for (const auto &dependency : mod.dependencies)
            add(dependency, n);
        for (const auto &first : mod.loadAfter)
            add(first, n);
        for (const auto &later : mod.loadBefore) {
            const auto it = position.constFind(later);
            if (it != position.cend() && it.value() != n)
                before[it.value()].insert(n);
        }
    }
    QStringList sorted;
    QSet<int> placed;
    while (sorted.size() < count) {
        bool progressed = false;
        for (int n = 0; n < count; ++n) {
            if (placed.contains(n))
                continue;
            bool ready = true;
            for (const int required : before[n])
                ready = ready && placed.contains(required);
            if (ready) {
                placed.insert(n);
                sorted << order[n];
                progressed = true;
                break;
            }
        }
        if (progressed)
            continue;
        if (warnings)
            warnings->append(QStringLiteral("Some mods have circular load rules, so their order was kept."));
        for (int n = 0; n < count; ++n)
            if (!placed.contains(n))
                sorted << order[n];
        break;
    }
    return sorted;
}

QStringList rimworldWarnings(const QStringList &order, const QHash<QString, RimMod> &mods) {
    QStringList warnings;
    QHash<QString, int> position;
    for (int n = 0; n < order.size(); ++n)
        position.insert(order[n].toCaseFolded(), n);
    const auto nameOf = [&mods](const QString &id) {
        const auto mod = mods.value(id.toCaseFolded());
        return mod.name.isEmpty() ? id : mod.name;
    };
    for (int n = 0; n < order.size(); ++n) {
        const auto mod = mods.value(order[n].toCaseFolded());
        const QString name = nameOf(order[n]);
        for (const auto &dependency : mod.dependencies) {
            if (!mods.contains(dependency))
                warnings << QStringLiteral("%1 requires %2, which was not found.").arg(name, dependency);
            else if (!position.contains(dependency))
                warnings << QStringLiteral("%1 requires %2, which is not enabled.").arg(name, nameOf(dependency));
            else if (position.value(dependency) > n)
                warnings << QStringLiteral("%1 must load after %2.").arg(name, nameOf(dependency));
        }
        for (const auto &first : mod.loadAfter)
            if (position.value(first, -1) > n)
                warnings << QStringLiteral("%1 should load after %2.").arg(name, nameOf(first));
        for (const auto &later : mod.loadBefore)
            if (position.contains(later) && position.value(later) < n)
                warnings << QStringLiteral("%1 should load before %2.").arg(name, nameOf(later));
        for (const auto &other : mod.incompatible)
            if (position.contains(other))
                warnings << QStringLiteral("%1 is incompatible with %2.").arg(name, nameOf(other));
    }
    warnings.removeDuplicates();
    return warnings;
}

QStringList rimworldActive(const QString &path) {
    QStringList order;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return order;
    QDomDocument doc;
    if (!doc.setContent(file.readAll()))
        return order;
    const auto active = doc.documentElement().firstChildElement(QStringLiteral("activeMods"));
    for (auto node = active.firstChildElement(QStringLiteral("li")); !node.isNull();
         node = node.nextSiblingElement(QStringLiteral("li")))
        order << node.text().trimmed();
    return order;
}

QString setRimworldOrder(const QString &path, const QStringList &order) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024)
        return QStringLiteral("Could not read ") + QFileInfo(path).fileName();
    QDomDocument doc;
    if (!doc.setContent(file.readAll()))
        return QStringLiteral("The game's mod list has an unexpected format.");
    file.close();
    auto active = doc.documentElement().firstChildElement(QStringLiteral("activeMods"));
    if (active.isNull())
        return QStringLiteral("The game's mod list has no activeMods section.");
    while (!active.firstChild().isNull())
        active.removeChild(active.firstChild());
    for (const auto &id : order) {
        auto li = doc.createElement(QStringLiteral("li"));
        li.appendChild(doc.createTextNode(id));
        active.appendChild(li);
    }
    return writeConfig(path, doc.toByteArray(2));
}

QJsonObject loadModProfiles(const QString &game) {
    return readObject(profilesPath()).value(game).toObject();
}

bool saveModProfiles(const QString &game, const QJsonObject &profiles) {
    auto all = readObject(profilesPath());
    all.insert(game, profiles);
    return writeJson(profilesPath(), QJsonDocument(all));
}
