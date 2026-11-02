#pragma once
#include "item_model.h"
#include "storage.h"
#include <QtNetwork>
#include <QtConcurrent>
#include <functional>

struct WorkshopEntry {
    QJsonObject details;
    QStringList children;
    QString error;
    bool collection = false, transient = false;
};
using WorkshopEntries = QHash<QString, WorkshopEntry>;

struct StoreGame {
    QString id, name, image;
};

void applyWorkshopDetails(Item &item, const QJsonObject &details);

class Engine : public QObject {
    Q_OBJECT
  public:
    explicit Engine(ItemModel *queue, ItemModel *library, QObject *parent = nullptr);
    QNetworkAccessManager network;
    QSettings settings;
    QString root, steamExe, username, password;
    bool busy = false, stopAfter = false, checkingUpdates = false;
    int resolving = 0;
    void resolve(const QStringList &ids);
    void start(const QStringList &only = {});
    void cancel();
    void retryCurrent();
    void submitInput(const QString &input);
    void persist();
    void bootstrap();
    void repairSteamcmd();
    void prepareSteamcmd();
    bool needsSteamcmdSetup() const;
    void setSteamcmdPath(const QString &path);
    QString apiKey() const;
    bool setApiKey(const QString &key);
    bool authPending() const;
    bool inSession(const QString &id) const;
    void signOut();
    QStringList gamesNeedingAccount(const QStringList &ids) const;
    void searchGames(const QString &term, QObject *context, std::function<void(QList<StoreGame>)> callback);
    void lookupGame(const QString &id, QObject *context, std::function<void(QString, QString)> callback);
    void backfillLibrary();
    void preview(const QStringList &ids, QObject *context,
                 std::function<void(QHash<QString, QJsonObject>, QString)> callback);
    void inspectWorkshop(const QStringList &ids, QObject *context,
                         std::function<void(WorkshopEntries)> callback);
    void checkLibrary();
    void reviewDependencies(const QStringList &ids, QObject *context,
                            std::function<void(QMap<QString, QString>, QString)> callback);
    QString diagnostics() const;
    QString stateLoadError() const;
  signals:
    void message(const QString &text);
    void activity(const QString &text);
    void busyChanged(bool busy);
    void authRequired(const QString &prompt);
    void authExpired();
    void collectionReady(const QString &id, const QString &title, const QStringList &children);
    void storageError(const QString &text);
    void itemSaved(const QString &id);
    void historyChanged();
    void transferStatus(const QString &text);
    void setupProgress(const QString &text);
    void setupDownloadProgress(qint64 received, qint64 total);
    void setupFinished(const QString &error, bool cancelled);

  private:
    ItemModel *queue, *library;
    QProcess process;
    QTimer timeout;
    QTimer retryTimer;
    QString loadError, webApiKey;
    QString active, outputBuffer, lineBuffer, phase, setupZip;
    struct SessionResult {
        QString id, error;
        bool success = false, retry = false;
    };
    QStringList sessionIds;
    QSet<QString> sessionDone, sessionStarted;
    QHash<QString, QString> sessionErrors;
    QHash<QString, qint64> sessionSeconds;
    QList<SessionResult> sessionResults;
    qint64 attemptSeconds = -1;
    void beginSessionItem(const QString &id);
    void abortSession(const QString &error);
    QString failureText(const QString &output) const;
    bool cancelled = false, bootstrapping = false, authPrompted = false;
    bool steamReady = false, clientReady = false;
    QString steamcmdKey() const;
    bool steamcmdInstalled() const;
    bool passwordSubmitted = false, timedOut = false;
    bool retryRequested = false;
    QTimer transferTimer;
    QElapsedTimer attemptClock;
    qint64 lastOutput = 0, lastBytes = 0, lastSample = 0, transferBytes = -1;
    qint64 transferBaseline = -1, lastFolderSample = 0;
    int runTotal = 0;
    QString attemptMethod, transferSpeed;
    void resetTransfer();
    bool sampling = false;
    quint64 attemptGeneration = 0;
    void sampleTransfer();
    void recordAttempt(const QString &result, const QString &error = {});
    QString downloadError() const;
    int setupAttempts = 0;
    void updateSteamcmd();
    void finishSetup(const QString &error = {});
    int attempts = 0;
    QHash<QString, int> retries;
    QSet<QString> pending;
    QStringList runIds;
    QStringList secrets;
    QPointer<QNetworkReply> setupReply;
    QPointer<QNetworkReply> directReply;
    bool triedDirect = false;
    QString directSource;
    int succeededCount = 0, failedCount = 0;
    void directDownload();
    void api(const QString &method, const QUrlQuery &form,
             std::function<void(QJsonObject, QString)> callback);
    void next();
    void launch();
    void readOutput();
    void handleAuthPrompt(const QString &text, bool partial = false);
    void handleLine(const QString &line);
    void finished(int exitCode, QProcess::ExitStatus status);
    void fail(const QString &error, bool retry = false);
    void finishRun();
    void commit();
};
