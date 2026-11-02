#pragma once
#include "engine.h"
#include "imagecache.h"
#include "mod_model.h"
#include <QtWidgets>
#include <memory>

namespace Ui {
class ModsPage;
}

class ModsPage : public QWidget {
    Q_OBJECT
  public:
    ModsPage(Engine &engine, ItemModel &queue, ItemModel &library, QWidget *parent = nullptr);
    ~ModsPage() override;
    void setBlocked(bool blocked);
    void setDiscoveredGames(const QHash<QString, QString> &games);
    void showGame(const QString &id);
    void reload();
    void afterDownloads(const QStringList &ids, bool autoInstall);

  signals:
    void busyChanged(bool busy);
    void navigateRequested(const QUrl &url);
    void status(const QString &text);
    void log(const QString &text);
    void downloadRequested(const QStringList &ids);
    void queueRequested(const QStringList &ids);
    void requiredRequested(const QStringList &ids);
    void importRequested(bool folder);
    void recoverRequested();

  protected:
    void showEvent(QShowEvent *) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    std::unique_ptr<Ui::ModsPage> ui;
    Engine &engine;
    ItemModel &queue, &library;
    ImageCache *images;
    ModListModel *model;
    ModFilter *filter;
    QTimer reloadTimer, searchTimer;
    QHash<QString, QString> discovered;
    QHash<QString, QPixmap> art;
    QIcon placeholderIcon, allGamesIcon;
    QSet<QString> artPending, namesRequested, pendingUpdates;
    QList<StoreGame> storeResults;
    QString currentGame, previewUrl, shownGame, gameFolderText, modFolderText;
    QStringList gameSignature;
    bool scanned = false, searching = false;
    struct GameRow {
        QString id, text, name;
        bool selectable = false, store = false;
    };
    QList<GameRow> gameRows;
    quint64 generation = 0;
    bool blocked = false, dirty = false, overwriteUpdates = false;

    void scheduleReload();
    void rebuildGames();
    void addGameRow(const QString &id, const QString &name, int mods, int updates, bool store);
    void addSection(const QString &title);
    void updateEmptyState();
    void updateFolderLabel();
    void requestArt(const QString &id);
    void storeArt(const QString &id, const QPixmap &pixmap);
    void updateHeader();
    void updateActions();
    void updateDetails();
    QList<ModEntry> selectedEntries() const;
    QStringList gameIdsWithMods() const;
    bool confirmNotRunning(const QStringList &apps);
    void runOperation(const QString &label, std::function<QStringList()> work, bool quiet = false);
    void install(const QStringList &ids, bool automatic, bool overwrite = false);
    void setEnabled(const QList<ModEntry> &entries, bool enabled);
    void uninstall(const QList<ModEntry> &entries);
    void updateMods(const QList<ModEntry> &entries);
    void removeRecords(const QList<ModEntry> &entries);
    void deleteDownloads(const QList<ModEntry> &entries);
    void fillActions(QMenu *menu);
    void fillFolders(QMenu *menu);
    void fillProfiles(QMenu *menu);
    void chooseModFolder();
    void openPath(const QString &path);
    void saveProfile();
    void applyProfile(const QString &name);
    void exportProfile(const QString &name);
    void importProfile();
    void loadOrderDialog();
    void activeModsDialog();
};
