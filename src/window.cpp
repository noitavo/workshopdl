#include "window.h"
#include "ui_mainwindow.h"
#include "ui_statuspanel.h"
#include "ui_logdialog.h"
#include "modspage.h"
#include "steamsetupdialog.h"
#include "taskbar.h"

namespace {
class StatusDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const int percent = index.data(Qt::UserRole + 1).toInt();
        if (percent < 0) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionViewItem item = option;
        initStyleOption(&item, index);
        const QStyle *style = item.widget ? item.widget->style() : QApplication::style();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &item, painter, item.widget);
        QStyleOptionProgressBar bar;
        bar.rect = option.rect.adjusted(2, 3, -2, -3);
        bar.minimum = 0;
        bar.maximum = 100;
        bar.progress = percent;
        bar.text = QStringLiteral("%1%").arg(percent);
        bar.textVisible = true;
        bar.state = QStyle::State_Enabled | QStyle::State_Horizontal;
        bar.palette = option.palette;
        style->drawControl(QStyle::CE_ProgressBar, &bar, painter, item.widget);
    }
};
}

Window::Window(ItemModel &queueModel, ItemModel &libraryModel, Engine &downloadEngine)
    : ui(std::make_unique<Ui::MainWindow>()), queue(queueModel), library(libraryModel),
      engine(downloadEngine) {
    ui->setupUi(this);
    tabs = ui->tabs;
    queueView = ui->queueView;
    browserQueue = ui->browserQueue;
    browserItemPanel = ui->browserItemPanel;
    input = ui->input;
    address = ui->address;
    summary = ui->summary;
    details = ui->details;
    pageTitle = ui->pageTitle;
    pageStatus = ui->pageStatus;
    browserCount = ui->browserCount;
    startButton = ui->startButton;
    stopButton = ui->stopButton;
    cancelButton = ui->cancelButton;
    browserAdd = ui->browserAdd;
    browserDownload = ui->browserDownload;
    browserStart = ui->browserStart;
    browserCancel = ui->browserCancel;
    browserSelected = ui->browserSelected;
    browserRetry = ui->browserRetry;
    browserStop = ui->browserStop;
    browserRemove = ui->browserRemove;
    browserAdvanced = ui->browserAdvanced;
    queueToolsToggle = ui->queueToolsToggle;
    accountAction = ui->accountAction;
    backButton = ui->backButton;
    forwardButton = ui->forwardButton;
    reloadButton = ui->reloadButton;
    web = ui->web;
    browserSplitter = ui->browserSplitter;
    setWindowIcon(QIcon(QStringLiteral(":/branding/workshopdl.png")));
    auto *statusPanel = new QWidget(this);
    Ui::StatusPanel statusUi;
    statusUi.setupUi(statusPanel);
    activity = statusUi.activity;
    activity->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    progress = statusUi.progress;
    progress->hide();
    statusBar()->addWidget(statusPanel, 1);
    auto *activityButton = new QToolButton(statusBar());
    activityButton->setObjectName(QStringLiteral("activityButton"));
    activityButton->setText(QStringLiteral("Activity log"));
    activityButton->setAutoRaise(true);
    statusBar()->addPermanentWidget(activityButton);
    connect(activityButton, &QToolButton::clicked, this, &Window::showLog);
    auto *transferInfo = statusUi.transferInfo;
    transferInfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *retryTransfer = statusUi.retryTransfer;
    retryTransfer->hide();
    log = new QPlainTextEdit(this);
    log->hide();
    log->setReadOnly(true);
    log->setMaximumBlockCount(2000);
    log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    refreshTimer.setSingleShot(true);
    refreshTimer.setInterval(50);
    connect(&refreshTimer, &QTimer::timeout, this, &Window::refresh);
    setupDownloads();
    setupBrowser();
    modsPage = new ModsPage(engine, queue, library, this);
    tabs->addTab(modsPage, QStringLiteral("Mods"));
    tabs->setTabToolTip(2, QStringLiteral("Your downloaded mods, by game"));
    connect(modsPage, &ModsPage::busyChanged, this, [this](bool busy) {
        diskBusy = busy;
        refresh();
    });
    connect(modsPage, &ModsPage::navigateRequested, this, &Window::navigate);
    connect(modsPage, &ModsPage::status, this, [this](const QString &text) { activity->setText(text); });
    connect(modsPage, &ModsPage::log, this, &Window::appendLog);
    connect(modsPage, &ModsPage::downloadRequested, this, &Window::downloadNow);
    connect(modsPage, &ModsPage::queueRequested, this, [this](const QStringList &ids) {
        engine.resolve(ids);
        tabs->setCurrentIndex(1);
    });
    connect(modsPage, &ModsPage::requiredRequested, this, [this](const QStringList &ids) { addDependencies(ids); });
    connect(modsPage, &ModsPage::importRequested, this, &Window::importLocalMod);
    connect(modsPage, &ModsPage::recoverRequested, this, &Window::recoverLibrary);
    setupHistory();
    createMenus();
    connect(&engine, &Engine::activity, this, [this](const QString &text) {
        activity->setText(text);
        refresh();
        maybeStartDownloads();
    });
    connect(&engine, &Engine::message, this, &Window::appendLog);
    connect(&engine, &Engine::setupProgress, this, [this](const QString &text) {
        if (!steamSetupDialog) {
            steamSetupDialog = new SteamSetupDialog(this->engine, this);
            steamSetupDialog->setAttribute(Qt::WA_DeleteOnClose);
            steamSetupDialog->setStatus(text);
            steamSetupDialog->open();
        }
    });
    connect(&engine, &Engine::transferStatus, transferInfo, &QLabel::setText);
    connect(retryTransfer, &QPushButton::clicked, &engine, &Engine::retryCurrent);
    connect(&engine, &Engine::transferStatus, retryTransfer, [retryTransfer](const QString &text) {
        retryTransfer->setVisible(text.contains(QStringLiteral("No recent activity")));
    });
    connect(&engine, &Engine::busyChanged, this, [this](bool busy) {
        refresh();
        if (!busy && !isActiveWindow())
            QApplication::alert(this);
        if (!busy && closeRequested)
            return;
        if (!busy && engine.settings.value(QStringLiteral("openAfter"), false).toBool() &&
            !lastCompletedPath.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(lastCompletedPath));
        }
        if (!busy)
            lastCompletedPath.clear();
        if (!busy) {
            const auto ids = automaticInstalls;
            automaticInstalls.clear();
            modsPage->afterDownloads(ids, engine.settings.value(QStringLiteral("autoInstall")).toBool());
        }
        maybeStartDownloads();
    });
    connect(&engine, &Engine::itemSaved, this, [this](const QString &id) {
        if (const auto *item = library.get(id))
            lastCompletedPath = item->path;
        automaticInstalls << id;
    });
    connect(&engine, &Engine::authRequired, this, &Window::guardDialog, Qt::QueuedConnection);
    connect(&engine, &Engine::collectionReady, this, &Window::collectionDialog, Qt::QueuedConnection);
    connect(web, &BrowserView::acceleratorPressed, this, [this](const QKeySequence &keys) {
        QTimer::singleShot(0, this, [this, keys] {
            web->returnFocus();
            for (auto *action : findChildren<QAction *>())
                if (action->isEnabled() && action->shortcuts().contains(keys)) {
                    action->trigger();
                    return;
                }
            for (auto *shortcut : findChildren<QShortcut *>())
                if (shortcut->isEnabled() && shortcut->key() == keys) {
                    emit shortcut->activated();
                    return;
                }
        });
    });
    connect(web, &BrowserView::runtimeUnavailable, this, [this] {
        if (runtimeNotified)
            return;
        runtimeNotified = true;
        QTimer::singleShot(0, this, [this] {
            if (QMessageBox::question(this, QStringLiteral("WebView2 Runtime required"),
                                      QStringLiteral("The Browse tab requires the Microsoft Edge WebView2 "
                                                     "Runtime. Open the download page?")) == QMessageBox::Yes)
                QDesktopServices::openUrl(
                    QUrl(QStringLiteral("https://developer.microsoft.com/microsoft-edge/webview2/")));
        });
    });
    connect(&engine, &Engine::storageError, this, [this](const QString &text) {
        appendLog(text);
        activity->setText(text);
    });
    if (!engine.stateLoadError().isEmpty()) {
        appendLog(engine.stateLoadError());
        activity->setText(QStringLiteral("Some saved data could not be loaded. See Activity log."));
    }
    for (auto *model : {&queue, &library}) {
        connect(model, &QAbstractItemModel::rowsInserted, this, [this] { refreshTimer.start(); });
        connect(model, &QAbstractItemModel::rowsRemoved, this, [this] { refreshTimer.start(); });
        connect(model, &QAbstractItemModel::dataChanged, this, [this] { refreshTimer.start(); });
    }
    auto shortcut = [this](const QKeySequence &key, std::function<void()> action) {
        auto *s = new QShortcut(key, this);
        connect(s, &QShortcut::activated, this, action);
    };
    shortcut(QKeySequence(QStringLiteral("Ctrl+L")), [this] {
        if (tabs->currentIndex() == 0) {
            address->setFocus();
            address->selectAll();
        } else {
            tabs->setCurrentIndex(1);
            input->setFocus();
            input->selectAll();
        }
    });
    shortcut(QKeySequence::Refresh, [this] {
        if (tabs->currentIndex() == 0)
            web->reload();
    });
    shortcut(QKeySequence::Back, [this] {
        if (tabs->currentIndex() == 0)
            web->goBack();
    });
    shortcut(QKeySequence::Forward, [this] {
        if (tabs->currentIndex() == 0)
            web->goForward();
    });
    shortcut(QKeySequence(QStringLiteral("Ctrl+D")), [this] { editBookmark(); });
    shortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")), [this] { browseGame(); });
    const auto splitter = engine.settings.value(QStringLiteral("browserSplitter")).toByteArray();
    if (engine.settings.value(QStringLiteral("rememberLayout"), true).toBool() && !splitter.isEmpty())
        browserSplitter->restoreState(splitter);
    refresh();
    connect(tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (!activeLayoutKey.isEmpty() && activeLayoutKey != layoutSettingsKey(index)) {
            saveTabLayout();
            restoreTabLayout(index);
        }
        engine.settings.setValue(QStringLiteral("lastTab"), index);
    });
    if (engine.settings.value(QStringLiteral("rememberTab"), true).toBool())
        tabs->setCurrentIndex(qBound(0, engine.settings.value(QStringLiteral("lastTab"), 0).toInt(), 2));
    else
        tabs->setCurrentIndex(qBound(0, engine.settings.value(QStringLiteral("startupTab"), 0).toInt(), 2));
    restoreTabLayout(tabs->currentIndex());
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this, [this] {
        if (!engine.settings.value(QStringLiteral("clipboardQueue")).toBool() ||
            QApplication::clipboard()->ownsClipboard())
            return;
        const QString text = QApplication::clipboard()->text();
        if (text.contains(QStringLiteral("steamcommunity.com/"), Qt::CaseInsensitive))
            addText(text);
    });
    diskBusy = true;
    using StartupResult = QPair<QString, QHash<QString, QString>>;
    auto *recovery = new QFutureWatcher<StartupResult>(this);
    connect(recovery, &QFutureWatcher<StartupResult>::finished, this, [this, recovery] {
        const auto [error, folders] = recovery->result();
        discoveredGameFolders = folders;
        recovery->deleteLater();
        diskBusy = false;
        if (!error.isEmpty())
            appendLog(error);
        modsPage->setDiscoveredGames(folders);
        refresh();
        if (engine.settings.value(QStringLiteral("checkModsAtStartup"), false).toBool())
            engine.checkLibrary();
    });
    recovery->setFuture(
        QtConcurrent::run([] { return StartupResult{recoverInstallations(), discoverGames()}; }));
    refresh();
    if (engine.settings.value(QStringLiteral("checkUpdatesAtStartup"), true).toBool())
        QTimer::singleShot(2000, this, [this] { checkUpdates(true); });
    auto *modCheck = new QTimer(this);
    modCheck->setInterval(6 * 3600 * 1000);
    connect(modCheck, &QTimer::timeout, this, [this] {
        if (engine.settings.value(QStringLiteral("checkModsAtStartup"), false).toBool() && !engine.busy && !diskBusy &&
            !engine.checkingUpdates)
            engine.checkLibrary();
    });
    modCheck->start();
    if (!engine.settings.value(QStringLiteral("guideShown"), false).toBool()) {
        engine.settings.setValue(QStringLiteral("guideShown"), true);
        QTimer::singleShot(500, this, &Window::showGettingStarted);
    }
}

Window::~Window() = default;

void Window::configureTable(QTableView *view, ItemModel *model) {
    view->setModel(model);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->horizontalHeader()->setHighlightSections(false);
    view->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    view->setColumnWidth(1, 150);
    view->setColumnWidth(2, 75);
    view->setColumnWidth(3, 120);
    view->setColumnWidth(4, 105);
    view->setItemDelegateForColumn(3, new StatusDelegate(view));
}

void Window::setupDownloadMethods() {
    auto *menu = ui->methodMenu;
    menu->setToolTipsVisible(true);
    auto *choices = new QActionGroup(menu);
    choices->setExclusive(true);
    const QList<QPair<QAction *, QString>> methods{{ui->actionAutomatic, QStringLiteral("auto")},
                                                   {ui->actionSteamcmd, QStringLiteral("steamcmd")},
                                                   {ui->actionSteamApi, QStringLiteral("http")}};
    for (const auto &method : methods) {
        auto *action = method.first;
        action->setData(method.second);
        if (method.second == QStringLiteral("http"))
            action->setToolTip(QStringLiteral("Direct download from Steam. Available only for some items."));
        else if (method.second == QStringLiteral("auto"))
            action->setToolTip(QStringLiteral("SteamCMD first, then SteamAPI."));
        choices->addAction(action);
    }
    connect(menu, &QMenu::aboutToShow, this, [this, choices] {
        const QString method =
            engine.settings.value(QStringLiteral("method"), QStringLiteral("auto")).toString();
        for (auto *action : choices->actions()) {
            action->setChecked(action->data().toString() == method);
            action->setEnabled(!engine.busy && !diskBusy);
        }
    });
    connect(choices, &QActionGroup::triggered, this, [this](QAction *action) {
        if (engine.busy || diskBusy)
            return;
        engine.settings.setValue(QStringLiteral("method"), action->data());
        refresh();
    });
}

void Window::setupDownloads() {
    configureTable(queueView, &queue);
    connect(ui->pasteButton, &QPushButton::clicked, this,
            [this] { input->setText(QApplication::clipboard()->text()); });
    connect(ui->downloadNowButton, &QPushButton::clicked, this, [this] {
        const auto ids = parseIds(input->text());
        if (ids.isEmpty()) {
            addText(input->text());
            return;
        }
        downloadWhenReady.append(ids);
        downloadWhenReady.removeDuplicates();
        addText(input->text());
        maybeStartDownloads();
    });
    connect(ui->addToQueueButton, &QPushButton::clicked, this, [this] { addText(input->text()); });
    connect(input, &QLineEdit::returnPressed, this, [this] { addText(input->text()); });
    connect(ui->collectionButton, &QPushButton::clicked, this, &Window::collectionImport);
    connect(ui->accountButton, &QPushButton::clicked, this, &Window::accountDialog);
    connect(queueView, &QTableView::customContextMenuRequested, this,
            [this](const QPoint &pos) { queueMenu(pos, queueView); });
    connect(queueView, &QTableView::activated, this,
            [this](const QModelIndex &index) { openFolder(queueFolder(queue.items[index.row()].id)); });
    connect(queueView->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        const auto ids = selected(queueView, &queue);
        details->setText(QStringLiteral("Select an item for details."));
        if (!ids.isEmpty())
            if (const auto *i = queue.get(ids.first()))
                details->setText(i->error.isEmpty() ? i->title + QStringLiteral(" · ") + i->id : i->error);
    });
    auto *queueMenu = new QMenu(ui->manualQueueTools);
    ui->manualQueueTools->setMenu(queueMenu);
    auto *remove = queueMenu->addAction(QStringLiteral("Remove selected\tDelete"), this,
                                        [this] { removeQueue(queueView); });
    queueMenu->addSeparator();
    auto *retry = queueMenu->addAction(QStringLiteral("Retry failed"), this, &Window::retryFailed);
    connect(queueMenu, &QMenu::aboutToShow, this, [this, remove, retry] {
        const bool idle = !engine.busy && !diskBusy && engine.resolving == 0;
        const bool removable = !diskBusy && engine.resolving == 0;
        bool failed = false;
        for (const auto &item : queue.items) {
            failed |= item.status == QStringLiteral("Failed") || item.status == QStringLiteral("Unresolved");
        }
        remove->setEnabled(removable && !selected(queueView, &queue).isEmpty());
        retry->setEnabled(idle && failed);
    });
    auto *deleteKey = new QShortcut(QKeySequence::Delete, queueView);
    deleteKey->setContext(Qt::WidgetWithChildrenShortcut);
    connect(deleteKey, &QShortcut::activated, this, [this] { removeQueue(queueView); });
    connect(cancelButton, &QPushButton::clicked, this, [this] {
        downloadWhenReady.clear();
        if (engine.busy)
            engine.cancel();
        refresh();
    });
    connect(stopButton, &QPushButton::clicked, this, [this] {
        engine.stopAfter = !engine.stopAfter;
        refresh();
    });
    connect(startButton, &QPushButton::clicked, this, [this] { startQueue(); });
}

QStringList Window::selected(QTableView *view, ItemModel *model) const {
    QStringList ids;
    for (const auto &index : view->selectionModel()->selectedRows()) {
        if (index.row() >= 0 && index.row() < model->items.size())
            ids.append(model->items[index.row()].id);
    }
    return ids;
}
void Window::queueLinks(const QString &text) {
    addText(text);
    if (isMinimized())
        showNormal();
    raise();
    activateWindow();
}
void Window::addText(const QString &text) {
    const auto ids = parseIds(text);
    if (ids.isEmpty()) {
        activity->setText(QStringLiteral("Enter a Workshop link or item ID."));
        return;
    }
    const auto newItems = std::count_if(ids.cbegin(), ids.cend(),
                                        [this](const QString &id) { return queue.get(id) == nullptr; });
    if (newItems + queue.items.size() > 10000) {
        QMessageBox::warning(this, QStringLiteral("Queue limit"),
                             QStringLiteral("The queue can hold up to 10,000 items."));
        return;
    }
    QStringList fresh;
    for (const auto &id : ids) {
        const auto *item = queue.get(id);
        if (!item)
            collectionHistory.remove(id);
        if (!item || (downloadWhenReady.contains(id) && item->status != QStringLiteral("Resolving") &&
                      item->status != QStringLiteral("Collection")))
            fresh.append(id);
    }
    if (!fresh.isEmpty())
        engine.resolve(fresh);
    else
        activity->setText(QStringLiteral("Already in queue."));
    engine.persist();
    input->clear();
    refresh();
}
void Window::refresh() {
    if (!startButton || !browserCount || !activity || !modsPage)
        return;
    int ready = 0, failed = 0;
    const Item *current = nullptr;
    for (const auto &i : queue.items) {
        if (i.status == QStringLiteral("Ready") || i.status == QStringLiteral("Cancelled"))
            ++ready;
        if (i.status == QStringLiteral("Failed"))
            ++failed;
        if (i.status == QStringLiteral("Downloading") || i.status == QStringLiteral("Verifying"))
            current = &i;
    }
    summary->setText(QStringLiteral("%1 in queue · %2 ready%3")
                         .arg(queue.items.size())
                         .arg(ready)
                         .arg(failed ? QStringLiteral(" · %1 failed").arg(failed) : QString()));
    browserCount->setText(failed ? QStringLiteral("%1 ready · %2 failed").arg(ready).arg(failed)
                                 : QStringLiteral("%1 ready").arg(ready));
    const bool canModifyQueue = !engine.busy && !diskBusy && engine.resolving == 0;
    startButton->setEnabled(canModifyQueue && ready > 0);
    browserStart->setEnabled(canModifyQueue && (ready > 0 || queue.items.isEmpty()));
    browserStart->setText(queue.items.isEmpty() ? QStringLiteral("Add links…")
                                                : QStringLiteral("Download queue"));
    const bool emptyQueue = queue.items.isEmpty();
    ui->emptyTitle->setVisible(emptyQueue);
    const bool pendingStart = !downloadWhenReady.isEmpty();
    cancelButton->setEnabled(engine.busy || pendingStart);
    browserCancel->setEnabled(engine.busy || pendingStart);
    browserCancel->setText(engine.busy ? QStringLiteral("Cancel downloads") : QStringLiteral("Cancel start"));
    stopButton->setEnabled(engine.busy);
    stopButton->setChecked(engine.stopAfter);
    const auto browserIds = selected(browserQueue, &queue);
    bool selectedReady = false;
    for (const auto &id : browserIds)
        if (const auto *item = queue.get(id))
            selectedReady |= validId(item->appId) && (item->status == QStringLiteral("Ready") ||
                                                      item->status == QStringLiteral("Cancelled"));
    browserSelected->setEnabled(canModifyQueue && selectedReady);
    browserSelected->setVisible(!browserIds.isEmpty());
    browserRemove->setVisible(!browserIds.isEmpty());
    browserSelected->setToolTip(selectedReady
                                    ? QStringLiteral("Download the ready items in your selection")
                                    : QStringLiteral("Select ready items, or choose Retry failed."));
    browserRemove->setEnabled(!diskBusy && engine.resolving == 0 && !browserIds.isEmpty());
    bool retryable = false;
    for (const auto &item : queue.items)
        retryable |= item.status == QStringLiteral("Failed") || item.status == QStringLiteral("Unresolved");
    browserRetry->setVisible(retryable);
    browserRetry->setEnabled(canModifyQueue);
    browserStop->setVisible(engine.busy);
    browserStop->setChecked(engine.stopAfter);
    browserCancel->setVisible(engine.busy || pendingStart);
    ui->moveUpButton->setEnabled(canModifyQueue && !browserIds.isEmpty());
    ui->moveDownButton->setEnabled(canModifyQueue && !browserIds.isEmpty());
    queueToolsToggle->setVisible(!queue.items.isEmpty());
    browserAdvanced->setVisible(!queue.items.isEmpty() && queueToolsToggle->isChecked());
    accountAction->setEnabled(!engine.busy && !diskBusy);
    const bool idle = !engine.busy && !diskBusy;
    modsPage->setBlocked(!idle);
    progress->setVisible(engine.busy || diskBusy || pendingStart);
    if (current && current->progress >= 0) {
        progress->setRange(0, 100);
        progress->setValue(current->progress);
    } else
        progress->setRange(0, 0);
    setTaskbarProgress(this, engine.busy || diskBusy || pendingStart,
                       current && current->progress >= 0 ? current->progress : -1);
    if (closeRequested && !engine.busy && !diskBusy)
        QTimer::singleShot(0, this, [this] { close(); });
    tabs->setTabText(2, QStringLiteral("Mods (%1)").arg(library.items.size()));
    tabs->setTabText(1, queue.items.isEmpty() ? QStringLiteral("Downloads")
                                              : QStringLiteral("Downloads (%1)").arg(queue.items.size()));
    ui->downloadTabs->setTabText(0, queue.items.isEmpty() ? QStringLiteral("Queue")
                                                          : QStringLiteral("Queue (%1)").arg(queue.items.size()));
    ui->browserDownloads->setTabText(0, ui->downloadTabs->tabText(0));
    syncHistoryActions();
    ui->accountStatus->setText(engine.username.isEmpty() ? QStringLiteral("Downloading anonymously")
                                                         : QStringLiteral("Signed in as %1").arg(engine.username));
    ui->accountButton->setEnabled(!engine.busy && !diskBusy);
    syncBrowser();
    maybeStartDownloads();
}
void Window::maybeStartDownloads() {
    if (downloadWhenReady.isEmpty() || autoStartScheduled || engine.busy || diskBusy || engine.resolving)
        return;
    for (const auto &id : downloadWhenReady)
        if (const auto *item = queue.get(id))
            if (item->status == QStringLiteral("Resolving") || item->status == QStringLiteral("Collection"))
                return;
    autoStartScheduled = true;
    QTimer::singleShot(0, this, [this] {
        autoStartScheduled = false;
        if (engine.busy || diskBusy || engine.resolving)
            return;
        const auto ids = downloadWhenReady;
        downloadWhenReady.clear();
        if (!ids.isEmpty()) {
            const bool reviewed = dependenciesReviewed;
            dependenciesReviewed = false;
            QStringList unprompted;
            for (const auto &app : engine.gamesNeedingAccount(ids))
                if (!accountPrompted.contains(app)) {
                    accountPrompted.insert(app);
                    unprompted << gameName(app);
                }
            if (!unprompted.isEmpty() &&
                QMessageBox::question(this, QStringLiteral("Sign in to Steam?"),
                                      QStringLiteral("Anonymous downloads failed before for %1. Sign in with an "
                                                     "account that owns the game?")
                                          .arg(unprompted.join(QStringLiteral(", ")))) == QMessageBox::Yes)
                accountDialog();
            if (engine.busy || diskBusy)
                return;
            if (!reviewed && engine.settings.value(QStringLiteral("checkDependencies"), false).toBool())
                addDependencies(ids, true);
            else
                engine.start(ids);
        }
    });
}
void Window::importList() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import Workshop links"), {},
                                                      QStringLiteral("Text files (*.txt);;All files (*)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
        QMessageBox::warning(this, QStringLiteral("Import failed"),
                             QStringLiteral("Choose a readable text file smaller than 4 MB."));
        return;
    }
    addText(QString::fromUtf8(file.readAll()));
}
void Window::exportList() {
    if (queue.items.isEmpty()) {
        activity->setText(QStringLiteral("The queue is empty."));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export queue"),
                                                      QStringLiteral("Workshop-links.txt"),
                                                      QStringLiteral("Text files (*.txt)"));
    if (path.isEmpty())
        return;
    QByteArray bytes;
    for (const auto &i : queue.items)
        bytes += workshopUrl(i.id).toUtf8() + '\n';
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        QMessageBox::warning(this, QStringLiteral("Export failed"), file.errorString());
}
void Window::collectionImport() {
    QInputDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Import collection"));
    dialog.setLabelText(QStringLiteral("Collection URL or ID"));
    dialog.setInputMode(QInputDialog::TextInput);
    dialog.setOkButtonText(QStringLiteral("Review collection"));
    if (browserIsCollection)
        dialog.setTextValue(web->url().toString());
    dialog.resize(dialog.sizeHint().expandedTo(QSize(480, 0)));
    if (dialog.exec() == QDialog::Accepted)
        addText(dialog.textValue());
}
void Window::appendLog(const QString &text) {
    QString clean = text;
    if (!engine.password.isEmpty())
        clean.replace(engine.password, QStringLiteral("[redacted]"));
    const QString line = QTime::currentTime().toString(QStringLiteral("HH:mm:ss  ")) + clean;
    log->appendPlainText(line);
    emit logLine(line);
    QFile file(dataRoot() + QStringLiteral("/activity.log"));
    if (file.size() > 2 * 1024 * 1024) {
        QFile::remove(dataRoot() + QStringLiteral("/activity.previous.log"));
        file.rename(dataRoot() + QStringLiteral("/activity.previous.log"));
        file.setFileName(dataRoot() + QStringLiteral("/activity.log"));
    }
    if (file.open(QIODevice::WriteOnly | QIODevice::Append))
        file.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QLatin1Char(' ') + clean +
                    QLatin1Char('\n'))
                       .toUtf8());
}
void Window::showLog() {
    if (logDialog) {
        logDialog->show();
        logDialog->raise();
        logDialog->activateWindow();
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    logDialog = dialog;
    Ui::LogDialog form;
    form.setupUi(dialog);
    auto *view = form.view;
    view->setFont(log->font());
    view->setMaximumBlockCount(log->maximumBlockCount());
    view->setPlainText(log->toPlainText());
    view->moveCursor(QTextCursor::End);
    connect(this, &Window::logLine, view, &QPlainTextEdit::appendPlainText);
    connect(form.copy, &QPushButton::clicked, dialog,
            [view] { QApplication::clipboard()->setText(view->toPlainText()); });
    connect(form.save, &QPushButton::clicked, dialog, [this, view] {
        const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Save activity log"),
                                                       QStringLiteral("WorkshopDL-activity.txt"),
                                                       QStringLiteral("Text files (*.txt)"));
        if (path.isEmpty())
            return;
        QSaveFile file(path);
        const auto bytes = view->toPlainText().toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            QMessageBox::warning(this, QStringLiteral("Save failed"), file.errorString());
    });
    connect(form.close, &QPushButton::clicked, dialog, &QDialog::accept);
    dialog->show();
}
