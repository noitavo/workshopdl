#pragma once
#include "item_model.h"
#include "storage.h"

struct GameProfile {
    QString id, name, subdir, format, note, activation;
};
QList<GameProfile> gameProfiles();
GameProfile profileFor(const QString &id);
bool hasProfile(const QString &id);
QString supportLevel(const GameProfile &profile);
bool saveGameRule(const GameProfile &profile, QString *error = nullptr);
QString knownGameName(const QString &id);
QString knownGameImage(const QString &id);
void rememberGame(const QString &id, const QString &name, const QString &image);
bool gameRunning(const QString &folder);
QHash<QString, QString> discoverGames();
QString gameDestination(const QString &id, const QString &folder);
QJsonObject readObject(const QString &path, QString *error = nullptr);
QJsonObject installedRecords(QString *error = nullptr);
QString fingerprintPath(const QString &path);
QString installContent(const Item &item, const QString &destination, bool overwrite = false);
QString manageInstalled(const QString &key, const QString &action);
QString recoverInstallations();
