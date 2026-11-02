#pragma once
#include "games.h"

struct ModEntry {
    QString id, appId, title, game, status, recordKey, folder, packageId;
    qint64 size = 0, updated = 0;
    bool downloaded = false, installed = false, enabled = false, unmanaged = false, local = false;
    bool configManaged = false, stale = false;
};

struct RimMod {
    QString id, name;
    QStringList dependencies, loadAfter, loadBefore, incompatible;
};

QString gameFolderFor(const QString &game, const QHash<QString, QString> &found);
QString modRootFor(const QString &game, const QHash<QString, QString> &found);
QString activationConfig(const QString &game, const QString &root);
QHash<QString, bool> activationStates(const QString &game, const QString &path);
QList<ModEntry> scanMods(const QList<Item> &library, const QString &game,
                         const QHash<QString, QString> &found);
QString setModActive(const QString &game, const QString &id, const QString &path, bool active);
QString installMod(const Item &item, const QString &destination, bool overwrite);
QString setModEnabled(const ModEntry &entry, bool enabled, const QHash<QString, QString> &found);
QString uninstallMod(const ModEntry &entry, const QHash<QString, QString> &found);

RimMod readRimMod(const QString &aboutPath);
QHash<QString, RimMod> rimworldMods(const QString &gameFolder, const QString &modRoot);
QStringList rimworldSort(const QStringList &order, const QHash<QString, RimMod> &mods, QStringList *warnings);
QStringList rimworldWarnings(const QStringList &order, const QHash<QString, RimMod> &mods);
QStringList rimworldActive(const QString &path);
QString setRimworldOrder(const QString &path, const QStringList &order);

QJsonObject loadModProfiles(const QString &game);
bool saveModProfiles(const QString &game, const QJsonObject &profiles);
