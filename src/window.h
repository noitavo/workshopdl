#pragma once
#include "engine.h"
#include "games.h"
#include "webview.h"
#include <QtWidgets>
#include <QtXml/QDomDocument>
#include <memory>

namespace Ui {
class MainWindow;
}

class HistoryFilter;
class HistoryModel;
class ModsPage;
struct HistoryEntry;
class SteamSetupDialog;

class Window : public QMainWindow {
    Q_OBJECT
  public:
    Window(ItemModel &queueModel, ItemModel &libraryModel, Engine &downloadEngine);
    ~Window() override;
    void queueLinks(const QString &text);

  signals:
    void logLine(const QString &text);

  protected:
    void closeEvent(QCloseEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;

  private:
    std::unique_ptr<Ui::MainWindow> ui;
    ItemModel &queue, &library;
    Engine &engine;
    QTabWidget *tabs = nullptr;
    QTableView *queueView = nullptr, *browserQueue = nullptr;
    QWidget *browserItemPanel = nullptr;
    ModsPage *modsPage = nullptr;
    HistoryModel *historyModel = nullptr;
    HistoryFilter *fullHistory = nullptr, *panelHistory = nullptr;
    QLineEdit *input = nullptr, *address = nullptr;
    QLabel *summary = nullptr, *activity = nullptr, *details = nullptr;
    QPointer<SteamSetupDialog> steamSetupDialog;
    QLabel *pageTitle = nullptr, *pageStatus = nullptr, *browserCount = nullptr;
    QProgressBar *progress = nullptr;
    QPlainTextEdit *log = nullptr;
    QPushButton *startButton = nullptr, *stopButton = nullptr, *cancelButton = nullptr;
    QPushButton *browserAdd = nullptr, *browserDownload = nullptr, *browserStart = nullptr,
                *browserCancel = nullptr;
    QPushButton *browserSelected = nullptr, *browserRetry = nullptr, *browserStop = nullptr,
                *browserRemove = nullptr;
    QWidget *browserAdvanced = nullptr;
    QToolButton *queueToolsToggle = nullptr;
    QAction *accountAction = nullptr;
    QString activeLayoutKey;
    QHash<QString, QByteArray> sessionLayouts;
    QString layoutSettingsKey(int index) const;
    void createMenus();
    void saveTabLayout();
    void restoreTabLayout(int index);
    QString browserPreviewId, browserPreviewTitle;
    bool browserIsCollection = false;
    quint64 browserPreviewGeneration = 0;
    QTimer browserMetadataTimer;
    QToolButton *backButton = nullptr, *forwardButton = nullptr, *reloadButton = nullptr;
    BrowserView *web = nullptr;
    QSplitter *browserSplitter = nullptr;
    QTimer browserLoadTimer;
    QStringList downloadWhenReady;
    QString lastCompletedPath;
    QStringList automaticInstalls;
    bool autoStartScheduled = false;
    bool dependenciesReviewed = false;
    bool closeRequested = false, collectionOpen = false, runtimeNotified = false;
    struct PendingCollection {
        QString id, title;
        QStringList children;
    };
    QList<PendingCollection> pendingCollections;
    QTimer refreshTimer, historyTimer;
    QPointer<QDialog> logDialog;
    QString browserError;
    void showGettingStarted();
    QHash<QString, QSet<QString>> collectionHistory;
    bool diskBusy = false;
    QHash<QString, QString> discoveredGameFolders;
    QSet<QString> accountPrompted;
    void setupDownloads();
    void setupBrowser();
    void setupDownloadMethods();
    void configureTable(QTableView *view, ItemModel *model);
    QStringList selected(QTableView *view, ItemModel *model) const;
    void addText(const QString &text);
    void refresh();
    void settingsDialog();
    void accountDialog();
    void guardDialog(const QString &prompt);
    void collectionDialog(const QString &id, const QString &title, const QStringList &children);
    void queueMenu(const QPoint &pos, QTableView *view);
    void appendLog(const QString &text);
    void navigate(const QUrl &url);
    void syncBrowser();
    void downloadPage(bool now);
    void maybeStartDownloads();
    void importList();
    void exportList();
    void collectionImport();
    void removeQueue(QTableView *view);
    QString queueFolder(const QString &id) const;
    void openFolder(const QString &path);
    void startQueue(QTableView *selection = nullptr);
    void retryFailed();
    void moveQueueSelection(QTableView *view, int direction);
    void showLog();
    void showSupport();
    void gameTools();
    void checkUpdates(bool quiet = false);
    void pasteMultiple();
    void browseGame();
    void fillBookmarks(QMenu *menu);
    void editBookmark(const QString &id = {});
    void importLocalMod(bool folder);
    void recoverLibrary();
    void cleanCache();
    void setupHistory();
    QList<HistoryEntry> selectedHistory(QTableView *view, HistoryFilter *filter) const;
    void syncHistoryActions();
    void historyMenu(QTableView *view, HistoryFilter *filter, const QPoint &point);
    void openHistory(QTableView *view, HistoryFilter *filter);
    void downloadAgain(QTableView *view, HistoryFilter *filter);
    void downloadNow(const QStringList &ids);
    void moveDownloads(const QString &from, const QString &to);
    void addDependencies(const QStringList &ids, bool download = false);
};
