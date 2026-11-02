#include "modspage.h"
#include "icons.h"
#include "ui_activationdialog.h"
#include "ui_gameactivationdialog.h"
#include "ui_installdialog.h"
#include "ui_modspage.h"

namespace {
QString stripMarkup(QString text) {
    static const QRegularExpression tags(QStringLiteral("\\[/?[a-zA-Z0-9*]+(?:=[^\\]]*)?\\]"));
    return text.remove(tags).trimmed();
}
QString steamArtUrl(const QString &id) {
    return QStringLiteral("https://cdn.cloudflare.steamstatic.com/steam/apps/%1/header.jpg").arg(id);
}
}

ModsPage::ModsPage(Engine &downloadEngine, ItemModel &queueModel, ItemModel &libraryModel, QWidget *parent)
    : QWidget(parent), ui(std::make_unique<Ui::ModsPage>()), engine(downloadEngine), queue(queueModel),
      library(libraryModel), images(new ImageCache(downloadEngine.network, this)), model(new ModListModel(this)),
      filter(new ModFilter(this)) {
    ui->setupUi(this);
    placeholderIcon = uiIcon(QStringLiteral("browser"));
    allGamesIcon = uiIcon(QStringLiteral("folder"));
    filter->setSourceModel(model);
    filter->setSortRole(Qt::UserRole);
    filter->setSortCaseSensitivity(Qt::CaseInsensitive);
    ui->modView->setModel(filter);
    ui->modView->sortByColumn(0, Qt::AscendingOrder);
    auto *header = ui->modView->horizontalHeader();
    header->setHighlightSections(false);
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->modView->setColumnWidth(1, 150);
    ui->modView->setColumnWidth(2, 90);
    ui->modView->setColumnWidth(3, 80);
    ui->modView->setColumnWidth(4, 120);
    ui->emptyPanel->hide();
    ui->gameList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->gameList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    ui->gameSupport->setWordWrap(false);
    ui->gameFolders->setWordWrap(false);
    for (auto *label : {ui->gameSupport, ui->gameFolders, ui->gameNote})
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    ui->gameFolders->setMinimumHeight(ui->gameFolders->fontMetrics().lineSpacing() * 2);
    ui->gameFolders->installEventFilter(this);
    const int noteHeight = ui->gameNote->fontMetrics().lineSpacing() * 2;
    ui->gameNote->setFixedHeight(noteHeight);
    ui->gameNote->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    auto retain = ui->loadOrderButton->sizePolicy();
    retain.setRetainSizeWhenHidden(true);
    ui->loadOrderButton->setSizePolicy(retain);
    ui->detailText->setPlaceholderText(tr("No description available."));
    connect(filter, &QAbstractItemModel::modelReset, this, &ModsPage::updateEmptyState);
    connect(filter, &QAbstractItemModel::rowsInserted, this, &ModsPage::updateEmptyState);
    connect(filter, &QAbstractItemModel::rowsRemoved, this, &ModsPage::updateEmptyState);
    connect(filter, &QAbstractItemModel::layoutChanged, this, &ModsPage::updateEmptyState);
    connect(ui->emptyAction, &QPushButton::clicked, this, [this] {
        if (!model->entries.isEmpty()) {
            ui->modSearch->clear();
            ui->statusFilter->setCurrentIndex(0);
        } else if (currentGame.isEmpty())
            emit navigateRequested(QUrl(QStringLiteral("https://steamcommunity.com/workshop/")));
        else
            emit navigateRequested(QUrl(QStringLiteral("https://steamcommunity.com/app/%1/workshop/").arg(currentGame)));
    });
    ui->mainSplitter->setStretchFactor(0, 0);
    ui->mainSplitter->setStretchFactor(1, 1);
    ui->contentSplitter->setStretchFactor(0, 1);
    ui->contentSplitter->setStretchFactor(1, 0);
    ui->mainSplitter->setSizes({290, 1000});
    ui->contentSplitter->setSizes({700, 300});
    const auto mainState = engine.settings.value(QStringLiteral("modsMainSplitter")).toByteArray();
    const auto contentState = engine.settings.value(QStringLiteral("modsContentSplitter")).toByteArray();
    if (!mainState.isEmpty())
        ui->mainSplitter->restoreState(mainState);
    if (!contentState.isEmpty())
        ui->contentSplitter->restoreState(contentState);
    reloadTimer.setSingleShot(true);
    reloadTimer.setInterval(250);
    searchTimer.setSingleShot(true);
    searchTimer.setInterval(400);
    connect(&reloadTimer, &QTimer::timeout, this, &ModsPage::reload);
    for (auto *menuButton : {ui->foldersButton, ui->profilesButton, ui->moreButton})
        menuButton->setMenu(new QMenu(menuButton));
    connect(ui->foldersButton->menu(), &QMenu::aboutToShow, this, [this] { fillFolders(ui->foldersButton->menu()); });
    connect(ui->profilesButton->menu(), &QMenu::aboutToShow, this,
            [this] { fillProfiles(ui->profilesButton->menu()); });
    connect(ui->moreButton->menu(), &QMenu::aboutToShow, this, [this] { fillActions(ui->moreButton->menu()); });
    connect(&engine, &Engine::activity, this, &ModsPage::updateActions);
    connect(&library, &QAbstractItemModel::rowsInserted, this, &ModsPage::scheduleReload);
    connect(&library, &QAbstractItemModel::rowsRemoved, this, &ModsPage::scheduleReload);
    connect(&library, &QAbstractItemModel::dataChanged, this, &ModsPage::scheduleReload);
    connect(&library, &QAbstractItemModel::modelReset, this, &ModsPage::scheduleReload);
    connect(ui->gameList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (!item || !(item->flags() & Qt::ItemIsSelectable))
            return;
        const QString id = item->data(Qt::UserRole).toString();
        if (item->data(Qt::UserRole + 1).toBool()) {
            auto added = engine.settings.value(QStringLiteral("addedGames")).toStringList();
            if (!added.contains(id))
                added << id;
            engine.settings.setValue(QStringLiteral("addedGames"), added);
        }
        currentGame = id;
        engine.settings.setValue(QStringLiteral("modGame"), id);
        QTimer::singleShot(0, this, &ModsPage::reload);
    });
    connect(ui->gameSearch, &QLineEdit::textChanged, this, [this](const QString &text) {
        storeResults.clear();
        searching = text.trimmed().size() >= 3;
        if (searching)
            searchTimer.start();
        else
            searchTimer.stop();
        rebuildGames();
    });
    connect(&searchTimer, &QTimer::timeout, this, [this] {
        const QString term = ui->gameSearch->text().trimmed();
        engine.searchGames(term, this, [this, term](QList<StoreGame> games) {
            if (term != ui->gameSearch->text().trimmed())
                return;
            storeResults = games;
            searching = false;
            for (const auto &game : games)
                if (knownGameName(game.id).isEmpty() && !hasProfile(game.id))
                    rememberGame(game.id, game.name, game.image);
            rebuildGames();
        });
    });
    connect(ui->modSearch, &QLineEdit::textChanged, filter, &ModFilter::setSearch);
    connect(ui->statusFilter, &QComboBox::currentIndexChanged, filter, &ModFilter::setStatusGroup);
    connect(ui->modView->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        updateActions();
        updateDetails();
    });
    connect(ui->modView, &QTableView::doubleClicked, this, [this] {
        const auto entries = selectedEntries();
        if (!entries.isEmpty())
            openPath(entries.first().folder);
    });
    connect(ui->modView, &QTableView::customContextMenuRequested, this, [this](const QPoint &point) {
        const auto index = ui->modView->indexAt(point);
        if (index.isValid() && !ui->modView->selectionModel()->isRowSelected(index.row(), {}))
            ui->modView->selectRow(index.row());
        QMenu menu(this);
        const auto entries = selectedEntries();
        if (!entries.isEmpty()) {
            for (auto *button : {ui->installButton, ui->toggleButton, ui->uninstallButton, ui->openButton, ui->pageButton}) {
                auto *action = menu.addAction(button->text(), button, &QPushButton::click);
                action->setEnabled(button->isEnabled());
            }
            menu.addSeparator();
        }
        fillActions(&menu);
        menu.exec(ui->modView->viewport()->mapToGlobal(point));
    });
    connect(model, &ModListModel::enableRequested, this, [this](const QString &id, bool on) {
        for (const auto &entry : model->entries)
            if (entry.id == id) {
                setEnabled({entry}, on);
                return;
            }
    });
    connect(ui->installButton, &QPushButton::clicked, this, [this] {
        QStringList ids;
        for (const auto &entry : selectedEntries())
            if (entry.downloaded && !entry.unmanaged)
                ids << entry.id;
        install(ids, false);
    });
    connect(ui->toggleButton, &QPushButton::clicked, this, [this] {
        const auto entries = selectedEntries();
        const bool enable = std::any_of(entries.cbegin(), entries.cend(), [](const ModEntry &entry) {
            return entry.installed && !entry.enabled;
        });
        setEnabled(entries, enable);
    });
    connect(ui->uninstallButton, &QPushButton::clicked, this, [this] { uninstall(selectedEntries()); });
    connect(ui->openButton, &QPushButton::clicked, this, [this] {
        const auto entries = selectedEntries();
        if (!entries.isEmpty())
            openPath(entries.first().folder);
    });
    connect(ui->pageButton, &QPushButton::clicked, this, [this] {
        const auto entries = selectedEntries();
        if (!entries.isEmpty() && !entries.first().local && !entries.first().unmanaged)
            emit navigateRequested(QUrl(workshopUrl(entries.first().id)));
    });
    connect(ui->requiredButton, &QPushButton::clicked, this, [this] {
        QStringList ids;
        for (const auto &entry : selectedEntries())
            if (!entry.local && !entry.unmanaged)
                ids << entry.id;
        if (!ids.isEmpty())
            emit requiredRequested(ids);
    });
    connect(ui->checkUpdatesButton, &QPushButton::clicked, this, [this] {
        if (!blocked)
            engine.checkLibrary();
    });
    connect(ui->updateAllButton, &QPushButton::clicked, this, [this] {
        QList<ModEntry> outdated;
        for (const auto &entry : model->entries) {
            const auto *item = library.get(entry.id);
            if (entry.stale || (item && item->status == QStringLiteral("Update available")))
                outdated << entry;
        }
        updateMods(outdated);
    });
    connect(ui->playButton, &QPushButton::clicked, this, [this] {
        if (validId(currentGame))
            QDesktopServices::openUrl(QUrl(QStringLiteral("steam://rungameid/") + currentGame));
    });
    connect(ui->workshopButton, &QPushButton::clicked, this, [this] {
        if (validId(currentGame))
            emit navigateRequested(QUrl(QStringLiteral("https://steamcommunity.com/app/%1/workshop/").arg(currentGame)));
    });
    connect(ui->pinButton, &QPushButton::clicked, this, [this](bool pinned) {
        auto pins = engine.settings.value(QStringLiteral("pinnedGames")).toStringList();
        pins.removeAll(currentGame);
        if (pinned)
            pins << currentGame;
        engine.settings.setValue(QStringLiteral("pinnedGames"), pins);
        rebuildGames();
    });
    connect(ui->loadOrderButton, &QPushButton::clicked, this, [this] {
        if (profileFor(currentGame).activation == QStringLiteral("rimworld"))
            loadOrderDialog();
        else
            activeModsDialog();
    });
    currentGame = engine.settings.value(QStringLiteral("modGame")).toString();
    updateHeader();
    updateActions();
    updateDetails();
    QTimer::singleShot(1500, &engine, &Engine::backfillLibrary);
}

ModsPage::~ModsPage() {
    engine.settings.setValue(QStringLiteral("modsMainSplitter"), ui->mainSplitter->saveState());
    engine.settings.setValue(QStringLiteral("modsContentSplitter"), ui->contentSplitter->saveState());
}

void ModsPage::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (dirty || ui->gameList->count() == 0)
        reload();
}

bool ModsPage::eventFilter(QObject *watched, QEvent *event) {
    if (watched == ui->gameFolders && event->type() == QEvent::Resize)
        updateFolderLabel();
    return QWidget::eventFilter(watched, event);
}

void ModsPage::setBlocked(bool value) {
    if (blocked == value)
        return;
    blocked = value;
    updateActions();
}

void ModsPage::setDiscoveredGames(const QHash<QString, QString> &games) {
    discovered = games;
    reload();
}

void ModsPage::showGame(const QString &id) {
    currentGame = id;
    ui->gameSearch->clear();
    reload();
}

void ModsPage::scheduleReload() {
    if (isVisible())
        reloadTimer.start();
    else
        dirty = true;
}

void ModsPage::reload() {
    dirty = false;
    rebuildGames();
    updateHeader();
    const quint64 current = ++generation;
    const QString game = currentGame;
    const auto items = library.items;
    const auto found = discovered;
    auto *watcher = new QFutureWatcher<QList<ModEntry>>(this);
    connect(watcher, &QFutureWatcher<QList<ModEntry>>::finished, this, [this, watcher, current] {
        const auto entries = watcher->result();
        watcher->deleteLater();
        if (current != generation)
            return;
        QSet<QString> keep;
        for (const auto &entry : selectedEntries())
            keep.insert(entry.id);
        const int scroll = shownGame == currentGame ? ui->modView->verticalScrollBar()->value() : 0;
        shownGame = currentGame;
        scanned = true;
        model->setEntries(entries);
        QItemSelection selection;
        for (int row = 0; row < model->rowCount(); ++row)
            if (keep.contains(model->entries[row].id)) {
                const auto index = filter->mapFromSource(model->index(row, 0));
                if (index.isValid())
                    selection.select(index, index);
            }
        ui->modView->selectionModel()->select(selection,
                                              QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        ui->modView->verticalScrollBar()->setValue(scroll);
        ui->modView->setColumnHidden(1, !currentGame.isEmpty());
        updateEmptyState();
        updateHeader();
        updateActions();
        updateDetails();
    });
    watcher->setFuture(QtConcurrent::run(
        [items, game, found] { return scanMods(items, game, found); }));
}

void ModsPage::updateEmptyState() {
    const bool loading = !scanned || shownGame != currentGame;
    const bool none = filter->rowCount() == 0;
    ui->modView->setVisible(!none);
    ui->emptyPanel->setVisible(none);
    ui->detailPanel->setVisible(!none);
    if (!none)
        return;
    ui->emptyAction->setVisible(!loading && currentGame != QStringLiteral("427520"));
    if (loading) {
        ui->emptyState->setText(tr("Loading mods…"));
    } else if (!model->entries.isEmpty()) {
        ui->emptyState->setText(tr("No mods match your search or filter."));
        ui->emptyAction->setText(tr("Clear filters"));
    } else if (currentGame.isEmpty()) {
        ui->emptyState->setText(tr("Nothing downloaded yet.\nFind mods on the Workshop, then download them."));
        ui->emptyAction->setText(tr("Browse the Workshop"));
    } else {
        ui->emptyState->setText(tr("No mods downloaded for %1 yet.").arg(gameName(currentGame)));
        ui->emptyAction->setText(tr("Browse its Workshop"));
    }
}

void ModsPage::addSection(const QString &title) {
    GameRow row;
    row.text = title;
    gameRows << row;
}

void ModsPage::addGameRow(const QString &id, const QString &name, int mods, int updates, bool store) {
    QString subtitle = store ? tr("Add game page") : mods > 0 ? tr("%n mod(s)", nullptr, mods) : tr("No mods yet");
    if (updates > 0)
        subtitle += QStringLiteral(" · ") + tr("%n update(s)", nullptr, updates);
    GameRow row;
    row.id = id;
    row.name = name;
    row.text = name + QLatin1Char('\n') + subtitle;
    row.selectable = true;
    row.store = store;
    gameRows << row;
}

void ModsPage::rebuildGames() {
    gameRows.clear();
    const QString term = ui->gameSearch->text().trimmed();
    const auto matches = [&term](const QString &name) { return name.contains(term, Qt::CaseInsensitive); };
    QHash<QString, int> counts, updates;
    int total = 0, totalUpdates = 0;
    QSet<QString> seen;
    for (const auto &item : library.items) {
        ++counts[item.appId];
        ++total;
        seen.insert(item.appId + QLatin1Char('/') + item.id);
        if (item.status == QStringLiteral("Update available")) {
            ++updates[item.appId];
            ++totalUpdates;
        }
    }
    const auto records = installedRecords();
    for (auto it = records.begin(); it != records.end(); ++it)
        if (!seen.contains(it.key())) {
            ++counts[it.value().toObject().value(QStringLiteral("app")).toString()];
            ++total;
        }
    if (term.isEmpty())
        addGameRow({}, tr("All downloads"), total, totalUpdates, false);
    const auto pins = engine.settings.value(QStringLiteral("pinnedGames")).toStringList();
    const auto added = engine.settings.value(QStringLiteral("addedGames")).toStringList();
    QSet<QString> listed;
    const auto section = [&](const QString &title, QStringList ids) {
        ids.removeIf([&](const QString &id) { return listed.contains(id) || !matches(gameName(id)); });
        std::sort(ids.begin(), ids.end(), [](const QString &a, const QString &b) {
            return QString::localeAwareCompare(gameName(a), gameName(b)) < 0;
        });
        if (ids.isEmpty())
            return;
        addSection(title);
        for (const auto &id : ids) {
            listed.insert(id);
            addGameRow(id, gameName(id), counts.value(id), updates.value(id), false);
        }
    };
    section(tr("Pinned"), pins);
    auto yours = counts.keys();
    yours.removeAll(QString());
    yours += added;
    if (!currentGame.isEmpty() && !discovered.contains(currentGame))
        yours << currentGame;
    yours.removeDuplicates();
    section(tr("Your games"), yours);
    section(tr("Installed games"), discovered.keys());
    if (!term.isEmpty()) {
        QList<StoreGame> remaining;
        for (const auto &game : storeResults)
            if (!listed.contains(game.id))
                remaining << game;
        if (!remaining.isEmpty())
            addSection(tr("On Steam"));
        for (const auto &game : remaining)
            addGameRow(game.id, game.name, 0, 0, true);
        if (searching)
            addSection(tr("Searching Steam…"));
        else if (listed.isEmpty() && remaining.isEmpty())
            addSection(term.size() < 3 ? tr("No matching games. Type more to search Steam.") : tr("No games found."));
    }
    QStringList signature;
    for (const auto &row : gameRows)
        signature << row.id + QLatin1Char('|') + row.text + QLatin1Char('|') + QString::number(row.store);
    const QSignalBlocker blocker(ui->gameList);
    if (signature != gameSignature) {
        gameSignature = signature;
        const int scroll = ui->gameList->verticalScrollBar()->value();
        ui->gameList->setUpdatesEnabled(false);
        ui->gameList->clear();
        for (const auto &row : gameRows) {
            auto *item = new QListWidgetItem(row.text, ui->gameList);
            if (!row.selectable) {
                item->setFlags(Qt::NoItemFlags);
                auto font = item->font();
                font.setBold(true);
                item->setFont(font);
                item->setSizeHint(QSize(0, 28));
                continue;
            }
            item->setData(Qt::UserRole, row.id);
            item->setData(Qt::UserRole + 1, row.store);
            item->setToolTip(row.name);
            item->setSizeHint(QSize(0, 50));
            const auto pixmap = art.value(row.id);
            item->setIcon(row.id.isEmpty() ? allGamesIcon : pixmap.isNull() ? placeholderIcon : QIcon(pixmap));
        }
        ui->gameList->doItemsLayout();
        ui->gameList->verticalScrollBar()->setValue(scroll);
        ui->gameList->setUpdatesEnabled(true);
    }
    for (int n = 0; n < ui->gameList->count(); ++n) {
        auto *item = ui->gameList->item(n);
        if ((item->flags() & Qt::ItemIsSelectable) && item->data(Qt::UserRole).toString() == currentGame) {
            if (ui->gameList->currentItem() != item) {
                const int scroll = ui->gameList->verticalScrollBar()->value();
                ui->gameList->setCurrentItem(item);
                ui->gameList->verticalScrollBar()->setValue(scroll);
            }
            break;
        }
    }
    for (const auto &row : gameRows) {
        if (!row.selectable || row.id.isEmpty())
            continue;
        requestArt(row.id);
        if (!hasProfile(row.id) && knownGameName(row.id).isEmpty() && !namesRequested.contains(row.id)) {
            namesRequested.insert(row.id);
            engine.lookupGame(row.id, this, [this](QString resolved, QString) {
                if (!resolved.isEmpty())
                    scheduleReload();
            });
        }
    }
}

void ModsPage::requestArt(const QString &id) {
    if (art.contains(id) || artPending.contains(id))
        return;
    artPending.insert(id);
    const QString known = knownGameImage(id);
    images->request(known.isEmpty() ? steamArtUrl(id) : known, this, [this, id, known](const QPixmap &pixmap) {
        if (!pixmap.isNull() || !known.isEmpty()) {
            storeArt(id, pixmap);
            return;
        }
        engine.lookupGame(id, this, [this, id](QString, QString image) {
            images->request(image, this, [this, id](const QPixmap &found) { storeArt(id, found); });
        });
    });
}

void ModsPage::storeArt(const QString &id, const QPixmap &pixmap) {
    artPending.remove(id);
    art.insert(id, pixmap);
    if (pixmap.isNull())
        return;
    for (int n = 0; n < ui->gameList->count(); ++n) {
        auto *item = ui->gameList->item(n);
        if (item->data(Qt::UserRole).toString() == id && (item->flags() & Qt::ItemIsSelectable))
            item->setIcon(QIcon(pixmap));
    }
    if (id == currentGame)
        updateHeader();
}

void ModsPage::updateFolderLabel() {
    const auto metrics = ui->gameFolders->fontMetrics();
    const int width = qMax(40, ui->gameFolders->width());
    ui->gameFolders->setText(metrics.elidedText(gameFolderText, Qt::ElideMiddle, width) + QLatin1Char('\n') +
                             metrics.elidedText(modFolderText, Qt::ElideMiddle, width));
    ui->gameFolders->setToolTip(gameFolderText + QLatin1Char('\n') + modFolderText);
}

void ModsPage::updateHeader() {
    const bool game = !currentGame.isEmpty();
    for (QWidget *widget : std::initializer_list<QWidget *>{ui->playButton, ui->workshopButton, ui->pinButton,
                                                            ui->foldersButton, ui->profilesButton})
        widget->setVisible(game);
    if (!game) {
        ui->loadOrderButton->hide();
        ui->gameArt->setPixmap(allGamesIcon.pixmap(48, 48));
        ui->gameTitle->setText(tr("All downloads"));
        ui->gameSupport->setText(tr("Everything WorkshopDL++ has downloaded, across all games."));
        ui->gameSupport->setToolTip(ui->gameSupport->text());
        gameFolderText = tr("Downloads: %1").arg(QDir::toNativeSeparators(engine.root));
        modFolderText = tr("Mods install into each game's own mod folder.");
        updateFolderLabel();
        ui->gameNote->setText(tr("Select a game to see its mod folders, install rules and profiles."));
        return;
    }
    const auto profile = profileFor(currentGame);
    ui->gameTitle->setText(profile.name);
    const QString level = supportLevel(profile);
    ui->gameSupport->setText(
        level == QStringLiteral("Full")
            ? tr("Full support: install, enable and order mods here.")
        : level == QStringLiteral("Install")
            ? tr("Install support: install mods here, then enable them in the game.")
            : tr("Download only: open the folder, or set a mod folder to install from here."));
    ui->gameSupport->setToolTip(ui->gameSupport->text());
    const QString gameFolder = gameFolderFor(currentGame, discovered);
    const QString modRoot = modRootFor(currentGame, discovered);
    gameFolderText = tr("Game: %1").arg(gameFolder.isEmpty() ? tr("not found") : QDir::toNativeSeparators(gameFolder));
    modFolderText = tr("Mods: %1").arg(modRoot.isEmpty() ? tr("not set") : QDir::toNativeSeparators(modRoot));
    updateFolderLabel();
    ui->gameNote->setText(profile.note);
    ui->pinButton->setChecked(engine.settings.value(QStringLiteral("pinnedGames")).toStringList().contains(currentGame));
    ui->workshopButton->setEnabled(currentGame != QStringLiteral("427520"));
    ui->loadOrderButton->setVisible(!profile.activation.isEmpty());
    ui->loadOrderButton->setText(profile.activation == QStringLiteral("rimworld") ? tr("Load order…") : tr("Active mods…"));
    const auto pixmap = art.value(currentGame);
    if (pixmap.isNull()) {
        ui->gameArt->setPixmap(placeholderIcon.pixmap(48, 48));
        requestArt(currentGame);
    } else {
        auto scaled = pixmap.scaled(ui->gameArt->size() * devicePixelRatioF(), Qt::KeepAspectRatio,
                                    Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(devicePixelRatioF());
        ui->gameArt->setPixmap(scaled);
    }
}

QList<ModEntry> ModsPage::selectedEntries() const {
    QList<ModEntry> entries;
    if (!ui->modView->selectionModel())
        return entries;
    for (const auto &index : ui->modView->selectionModel()->selectedRows()) {
        const auto source = filter->mapToSource(index);
        if (source.isValid() && source.row() < model->entries.size())
            entries << model->entries[source.row()];
    }
    return entries;
}

void ModsPage::updateActions() {
    const auto entries = selectedEntries();
    bool canInstall = false, reinstall = false, installed = false, anyDisabled = false, remote = false;
    for (const auto &entry : entries) {
        const bool installable = entry.downloaded && !entry.unmanaged;
        canInstall = canInstall || installable;
        reinstall = reinstall || (installable && entry.installed);
        installed = installed || entry.installed;
        anyDisabled = anyDisabled || (entry.installed && !entry.enabled);
        remote = remote || (!entry.local && !entry.unmanaged);
    }
    ui->installButton->setEnabled(!blocked && canInstall);
    ui->installButton->setText(reinstall && canInstall ? tr("Reinstall") : tr("Install"));
    ui->toggleButton->setEnabled(!blocked && installed);
    ui->toggleButton->setText(anyDisabled || !installed ? tr("Enable") : tr("Disable"));
    ui->uninstallButton->setEnabled(!blocked && installed);
    ui->openButton->setEnabled(!entries.isEmpty());
    ui->pageButton->setEnabled(!entries.isEmpty() && !entries.first().local && !entries.first().unmanaged);
    ui->requiredButton->setEnabled(!blocked && remote);
    ui->checkUpdatesButton->setEnabled(!blocked && !engine.checkingUpdates && !library.items.isEmpty());
    bool outdated = false;
    for (const auto &entry : model->entries) {
        const auto *item = library.get(entry.id);
        outdated = outdated || entry.stale || (item && item->status == QStringLiteral("Update available"));
    }
    ui->updateAllButton->setEnabled(!blocked && outdated);
    ui->loadOrderButton->setEnabled(!blocked);
    ui->profilesButton->setEnabled(!blocked);
}

void ModsPage::updateDetails() {
    const auto entries = selectedEntries();
    previewUrl.clear();
    if (entries.size() != 1) {
        ui->detailPreview->setText(entries.isEmpty() ? tr("No mod selected") : tr("Multiple mods selected"));
        ui->detailTitle->setText(entries.isEmpty() ? tr("Select a mod") : tr("%n mod(s) selected", nullptr, int(entries.size())));
        ui->detailMeta->setText(entries.isEmpty() ? tr("Its status, size and tags appear here.")
                                                  : tr("Actions apply to every selected mod."));
        ui->detailText->clear();
        ui->detailText->setPlaceholderText(entries.isEmpty() ? tr("Its description appears here.") : QString());
        return;
    }
    const auto &entry = entries.first();
    const auto *item = library.get(entry.id);
    ui->detailTitle->setText(entry.title);
    QStringList meta{tr("Status: %1").arg(entry.status)};
    if (entry.size > 0)
        meta << tr("Size: %1").arg(sizeText(entry.size));
    if (entry.updated > 0)
        meta << tr("Updated: %1").arg(QDateTime::fromSecsSinceEpoch(entry.updated).toString(QStringLiteral("yyyy-MM-dd")));
    if (!entry.local && !entry.unmanaged)
        meta << tr("Item ID: %1").arg(entry.id);
    if (item && !item->tags.isEmpty())
        meta << tr("Tags: %1").arg(item->tags.join(QStringLiteral(", ")));
    if (profileFor(entry.appId).activation == QStringLiteral("rimworld")) {
        const auto about = readRimMod((item ? item->path : entry.folder) + QStringLiteral("/About/About.xml"));
        if (!about.dependencies.isEmpty())
            meta << tr("Requires: %1").arg(about.dependencies.join(QStringLiteral(", ")));
    }
    ui->detailMeta->setText(meta.join(QLatin1Char('\n')));
    ui->detailText->setPlaceholderText(tr("No description available."));
    ui->detailText->setPlainText(item ? stripMarkup(item->description) : QString());
    const QString address = item ? item->previewUrl : QString();
    previewUrl = address;
    if (address.isEmpty()) {
        ui->detailPreview->setText(tr("No preview available"));
        return;
    }
    ui->detailPreview->setText(tr("Loading preview…"));
    images->request(address, this, [this, address](const QPixmap &pixmap) {
        if (address != previewUrl)
            return;
        if (pixmap.isNull()) {
            ui->detailPreview->setText(tr("No preview available"));
            return;
        }
        auto scaled = pixmap.scaled(ui->detailPreview->minimumSize() * devicePixelRatioF(), Qt::KeepAspectRatio,
                                    Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(devicePixelRatioF());
        ui->detailPreview->setPixmap(scaled);
    });
}

bool ModsPage::confirmNotRunning(const QStringList &apps) {
    QSet<QString> unique(apps.cbegin(), apps.cend());
    for (const auto &app : unique)
        if (gameRunning(gameFolderFor(app, discovered))) {
            QMessageBox::warning(this, tr("Close the game first"),
                                 tr("%1 is running. Close it before changing its mods.").arg(gameName(app)));
            return false;
        }
    return true;
}

void ModsPage::runOperation(const QString &label, std::function<QStringList()> work, bool quiet) {
    emit busyChanged(true);
    emit status(label);
    auto *watcher = new QFutureWatcher<QStringList>(this);
    connect(watcher, &QFutureWatcher<QStringList>::finished, this, [this, watcher, quiet] {
        const auto errors = watcher->result();
        watcher->deleteLater();
        emit busyChanged(false);
        reload();
        if (errors.isEmpty()) {
            emit status(tr("Done."));
            return;
        }
        const QString text = errors.join(QStringLiteral("\n\n"));
        emit log(text);
        emit status(tr("Some mod changes failed. See Activity log."));
        if (!quiet)
            QMessageBox::warning(this, tr("Some mods were not changed"), text);
    });
    watcher->setFuture(QtConcurrent::run(std::move(work)));
}

void ModsPage::install(const QStringList &ids, bool automatic, bool overwrite) {
    if (ids.isEmpty() || blocked)
        return;
    struct Request {
        Item item;
        QString target;
    };
    QDialog dialog(this);
    Ui::InstallDialog form;
    form.setupUi(&dialog);
    auto *table = form.table;
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table->setColumnWidth(1, 140);
    const auto records = installedRecords();
    bool missingTarget = false;
    QStringList apps;
    for (const auto &id : ids) {
        const auto *item = library.get(id);
        if (!item)
            continue;
        const auto profile = profileFor(item->appId);
        const QString target = records.contains(item->appId + QLatin1Char('/') + id)
                                   ? records.value(item->appId + QLatin1Char('/') + id)
                                         .toObject()
                                         .value(QStringLiteral("root"))
                                         .toString()
                                   : modRootFor(item->appId, discovered);
        if (profile.format == QStringLiteral("manual")) {
            if (!automatic)
                emit log(profile.name + QStringLiteral(": ") + profile.note);
            continue;
        }
        if (automatic && target.isEmpty())
            continue;
        if (automatic && gameRunning(gameFolderFor(item->appId, discovered))) {
            emit log(tr("%1 was not installed because %2 is running.").arg(item->title, profile.name));
            continue;
        }
        missingTarget = missingTarget || target.isEmpty();
        apps << item->appId;
        const int row = table->rowCount();
        table->insertRow(row);
        auto *title = new QTableWidgetItem(item->title);
        title->setData(Qt::UserRole, item->id);
        title->setCheckState(Qt::Checked);
        title->setFlags((title->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
        table->setItem(row, 0, title);
        auto *name = new QTableWidgetItem(profile.name);
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, 1, name);
        table->setItem(row, 2, new QTableWidgetItem(target));
    }
    if (!table->rowCount()) {
        if (!automatic)
            QMessageBox::information(this, tr("Installation"),
                                     tr("These items are download only. Set a mod folder for the game to install them."));
        return;
    }
    if (!automatic && !confirmNotRunning(apps))
        return;
    connect(form.browse, &QPushButton::clicked, &dialog, [&] {
        const int row = table->currentRow();
        if (row < 0)
            return;
        const auto path = QFileDialog::getExistingDirectory(&dialog, tr("Choose the game's mod folder"),
                                                            table->item(row, 2)->text());
        if (!path.isEmpty())
            table->item(row, 2)->setText(path);
    });
    form.buttons->button(QDialogButtonBox::Ok)->setText(tr("Install selected"));
    connect(form.buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(form.buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        for (int row = 0; row < table->rowCount(); ++row)
            if (table->item(row, 0)->checkState() == Qt::Checked && !QDir::isAbsolutePath(table->item(row, 2)->text())) {
                QMessageBox::warning(&dialog, tr("Mod folder required"),
                                     tr("Choose a mod folder for each selected item."));
                return;
            }
        dialog.accept();
    });
    if (missingTarget && dialog.exec() != QDialog::Accepted)
        return;
    QList<Request> requests;
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 0)->checkState() != Qt::Checked)
            continue;
        const auto *item = library.get(table->item(row, 0)->data(Qt::UserRole).toString());
        const QString target = table->item(row, 2)->text();
        if (!item || !QDir::isAbsolutePath(target))
            continue;
        engine.settings.setValue(QStringLiteral("destinations/") + item->appId, target);
        requests.append({*item, target});
    }
    if (requests.isEmpty())
        return;
    runOperation(
        tr("Installing %n mod(s)…", nullptr, int(requests.size())),
        [requests, overwrite] {
            QStringList errors;
            for (const auto &request : requests) {
                const QString error = installMod(request.item, request.target, overwrite);
                if (!error.isEmpty())
                    errors << request.item.title + QStringLiteral(": ") + error;
            }
            return errors;
        },
        automatic);
}

void ModsPage::setEnabled(const QList<ModEntry> &entries, bool enabled) {
    if (blocked)
        return;
    QList<ModEntry> targets;
    QStringList apps;
    for (const auto &entry : entries)
        if (entry.installed && entry.enabled != enabled) {
            targets << entry;
            apps << entry.appId;
        }
    if (targets.isEmpty() || !confirmNotRunning(apps)) {
        reload();
        return;
    }
    const auto found = discovered;
    runOperation(enabled ? tr("Enabling mods…") : tr("Disabling mods…"), [targets, enabled, found] {
        QStringList errors;
        for (const auto &entry : targets) {
            const QString error = setModEnabled(entry, enabled, found);
            if (!error.isEmpty())
                errors << entry.title + QStringLiteral(": ") + error;
        }
        return errors;
    });
}

void ModsPage::uninstall(const QList<ModEntry> &entries) {
    if (blocked)
        return;
    QList<ModEntry> targets;
    QStringList apps;
    for (const auto &entry : entries)
        if (entry.installed) {
            targets << entry;
            apps << entry.appId;
        }
    if (targets.isEmpty() || !confirmNotRunning(apps))
        return;
    if (QMessageBox::question(this, tr("Uninstall mods"),
                              tr("Remove the installed files for %n mod(s)? Downloaded copies are kept.", nullptr,
                                 int(targets.size()))) != QMessageBox::Yes)
        return;
    const auto found = discovered;
    runOperation(tr("Uninstalling mods…"), [targets, found] {
        QStringList errors;
        for (const auto &entry : targets) {
            const QString error = uninstallMod(entry, found);
            if (!error.isEmpty())
                errors << entry.title + QStringLiteral(": ") + error;
        }
        return errors;
    });
}

void ModsPage::updateMods(const QList<ModEntry> &entries) {
    if (blocked)
        return;
    QStringList download, reinstall, apps;
    bool modified = false;
    for (const auto &entry : entries) {
        const auto *item = library.get(entry.id);
        if (!item || item->local || entry.unmanaged)
            continue;
        modified = modified || entry.status == QStringLiteral("Modified");
        if (entry.installed)
            apps << entry.appId;
        if (item->status == QStringLiteral("Update available")) {
            download << entry.id;
            if (entry.installed)
                pendingUpdates.insert(entry.id);
        } else if (entry.stale)
            reinstall << entry.id;
    }
    if (download.isEmpty() && reinstall.isEmpty())
        return;
    if (!confirmNotRunning(apps))
        return;
    overwriteUpdates = false;
    if (modified) {
        const auto answer = QMessageBox::question(
            this, tr("Replace changed files?"),
            tr("Some installed mods have local changes. Replace them with the update?"),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel) {
            pendingUpdates.clear();
            return;
        }
        overwriteUpdates = answer == QMessageBox::Yes;
    }
    if (!reinstall.isEmpty())
        install(reinstall, false, overwriteUpdates);
    if (!download.isEmpty())
        emit downloadRequested(download);
}

void ModsPage::afterDownloads(const QStringList &ids, bool autoInstall) {
    const auto records = installedRecords();
    QStringList targets;
    for (const auto &id : ids) {
        const auto *item = library.get(id);
        if (!item)
            continue;
        const bool installed = records.contains(item->appId + QLatin1Char('/') + id);
        if (installed ? pendingUpdates.contains(id) : autoInstall)
            targets << id;
    }
    pendingUpdates.clear();
    const bool overwrite = overwriteUpdates;
    overwriteUpdates = false;
    install(targets, true, overwrite);
}

void ModsPage::removeRecords(const QList<ModEntry> &entries) {
    if (blocked)
        return;
    QStringList ids;
    for (const auto &entry : entries)
        if (entry.downloaded)
            ids << entry.id;
    library.removeIds(ids);
    engine.persist();
}

void ModsPage::deleteDownloads(const QList<ModEntry> &entries) {
    if (blocked)
        return;
    QList<Item> items;
    for (const auto &entry : entries)
        if (const auto *item = library.get(entry.id))
            items << *item;
    if (items.isEmpty())
        return;
    if (QMessageBox::warning(this, tr("Delete downloaded files"),
                             tr("Delete downloaded files for %n item(s) permanently? Installed mods are kept.",
                                nullptr, int(items.size())),
                             QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    emit busyChanged(true);
    auto *watcher = new QFutureWatcher<QStringList>(this);
    connect(watcher, &QFutureWatcher<QStringList>::finished, this, [this, watcher, items] {
        const auto failures = watcher->result();
        watcher->deleteLater();
        QStringList removed;
        for (const auto &item : items)
            if (!failures.contains(item.id))
                removed << item.id;
        library.removeIds(removed);
        queue.removeIds(removed);
        engine.persist();
        emit busyChanged(false);
        if (!failures.isEmpty())
            QMessageBox::warning(this, tr("Some files were kept"),
                                 tr("Could not safely delete item(s): %1").arg(failures.join(QStringLiteral(", "))));
    });
    watcher->setFuture(QtConcurrent::run([items] {
        QStringList failures;
        for (const auto &i : items) {
            if (i.path.isEmpty())
                continue;
            const QFileInfo info(i.path);
            const bool managedName = info.fileName() == i.id || info.fileName() == i.id + QStringLiteral(".partial") ||
                                     info.fileName() == i.id + QStringLiteral(".previous");
            if (!validId(i.id) || !validId(i.appId) || !managedName ||
                QFileInfo(info.absolutePath()).fileName() != i.appId || linkedAncestor(i.path)) {
                failures.append(i.id);
                continue;
            }
            if (info.exists() && (!safeTree(i.path, nullptr, true) || !QDir(i.path).removeRecursively())) {
                failures.append(i.id);
                continue;
            }
            QFile::remove(i.path + QStringLiteral(".workshopdl.json"));
            QDir().rmdir(info.absolutePath());
        }
        return failures;
    }));
}

void ModsPage::fillActions(QMenu *menu) {
    if (menu == ui->moreButton->menu())
        menu->clear();
    const auto entries = selectedEntries();
    bool remote = false, downloaded = false;
    for (const auto &entry : entries) {
        remote = remote || (!entry.local && !entry.unmanaged);
        downloaded = downloaded || entry.downloaded;
    }
    const auto remoteIds = [entries] {
        QStringList ids;
        for (const auto &entry : entries)
            if (!entry.local && !entry.unmanaged)
                ids << entry.id;
        return ids;
    }();
    auto *copy = menu->addAction(tr("Copy link"), this, [remoteIds] {
        QStringList links;
        for (const auto &id : remoteIds)
            links << workshopUrl(id);
        QApplication::clipboard()->setText(links.join(QLatin1Char('\n')));
    });
    copy->setEnabled(remote);
    auto *again = menu->addAction(tr("Download again"), this, [this, remoteIds] { emit queueRequested(remoteIds); });
    again->setEnabled(!blocked && remote);
    QString creator;
    if (entries.size() == 1)
        if (const auto *item = library.get(entries.first().id))
            creator = item->creator;
    auto *author = menu->addAction(tr("More from this author"), this, [this, creator] {
        emit navigateRequested(QUrl(QStringLiteral("https://steamcommunity.com/profiles/%1/myworkshopfiles/").arg(creator)));
    });
    author->setEnabled(validId(creator));
    menu->addSeparator();
    auto *folderImport = menu->addAction(tr("Import mod folder…"), this, [this] { emit importRequested(true); });
    auto *packageImport = menu->addAction(tr("Import mod package…"), this, [this] { emit importRequested(false); });
    auto *recover = menu->addAction(tr("Find downloaded files…"), this, [this] { emit recoverRequested(); });
    folderImport->setEnabled(!blocked);
    packageImport->setEnabled(!blocked);
    recover->setEnabled(!blocked);
    menu->addSeparator();
    auto *remove = menu->addAction(tr("Remove from list"), this, [this, entries] { removeRecords(entries); });
    remove->setEnabled(!blocked && downloaded);
    auto *erase = menu->addAction(tr("Delete download…"), this, [this, entries] { deleteDownloads(entries); });
    erase->setEnabled(!blocked && downloaded);
}

void ModsPage::openPath(const QString &path) {
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        emit status(tr("That folder does not exist."));
        return;
    }
    const QFileInfo info(path);
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.isDir() ? path : info.absolutePath()));
}

void ModsPage::fillFolders(QMenu *menu) {
    menu->clear();
    const QString gameFolder = gameFolderFor(currentGame, discovered);
    const QString modRoot = modRootFor(currentGame, discovered);
    menu->addAction(tr("Open game folder"), this, [this, gameFolder] { openPath(gameFolder); })
        ->setEnabled(!gameFolder.isEmpty());
    menu->addAction(tr("Open mod folder"), this, [this, modRoot] { openPath(modRoot); })->setEnabled(!modRoot.isEmpty());
    menu->addSeparator();
    menu->addAction(tr("Set game folder…"), this, [this, gameFolder] {
        const QString path = QFileDialog::getExistingDirectory(this, tr("Select the game's folder"), gameFolder);
        if (path.isEmpty())
            return;
        engine.settings.setValue(QStringLiteral("gameFolders/") + currentGame, path);
        reload();
    });
    menu->addAction(tr("Set mod folder…"), this, &ModsPage::chooseModFolder)->setEnabled(!blocked);
    const auto profile = profileFor(currentGame);
    if (!profile.activation.isEmpty())
        menu->addAction(tr("Set mod list file…"), this, [this] {
            const QString path = QFileDialog::getOpenFileName(this, tr("Select the game's mod list file"));
            if (!path.isEmpty()) {
                engine.settings.setValue(QStringLiteral("activation/") + currentGame, path);
                reload();
            }
        });
}

void ModsPage::chooseModFolder() {
    auto profile = profileFor(currentGame);
    const QString path = QFileDialog::getExistingDirectory(this, tr("Select the game's mod folder"),
                                                           modRootFor(currentGame, discovered));
    if (path.isEmpty())
        return;
    if (linkedAncestor(path)) {
        QMessageBox::warning(this, tr("Mod folder"), tr("Choose a folder without directory links."));
        return;
    }
    if (profile.format == QStringLiteral("manual")) {
        const QStringList styles{tr("Copy each mod as a folder"), tr("Copy files with a specific extension")};
        bool ok = false;
        const QString style = QInputDialog::getItem(this, tr("How does this game load mods?"), tr("Install style"),
                                                    styles, 0, false, &ok);
        if (!ok)
            return;
        profile.format = QStringLiteral("folder");
        if (style == styles[1]) {
            const QString extension = QInputDialog::getText(this, tr("File extension"), tr("Extension, such as .pak"),
                                                            QLineEdit::Normal, QStringLiteral(".pak"), &ok)
                                          .trimmed();
            if (!ok || !QRegularExpression(QStringLiteral("^\\.[A-Za-z0-9]{1,16}$")).match(extension).hasMatch())
                return;
            profile.format = QStringLiteral("files:") + extension;
        }
        QString error;
        if (!saveGameRule(profile, &error)) {
            QMessageBox::warning(this, tr("Mod folder"), error);
            return;
        }
    }
    engine.settings.setValue(QStringLiteral("destinations/") + currentGame, path);
    emit status(tr("Mod folder saved. New installs go here; installed mods stay where they are."));
    reload();
}

void ModsPage::fillProfiles(QMenu *menu) {
    menu->clear();
    const auto profiles = loadModProfiles(currentGame);
    menu->addAction(tr("Save current as profile…"), this, &ModsPage::saveProfile);
    menu->addAction(tr("Import profile…"), this, &ModsPage::importProfile);
    if (profiles.isEmpty())
        return;
    menu->addSeparator();
    auto *apply = menu->addMenu(tr("Apply"));
    auto *exportMenu = menu->addMenu(tr("Export"));
    auto *removeMenu = menu->addMenu(tr("Delete"));
    for (const auto &name : profiles.keys()) {
        QString label = name;
        label.replace(QLatin1Char('&'), QStringLiteral("&&"));
        apply->addAction(label, this, [this, name] { applyProfile(name); });
        exportMenu->addAction(label, this, [this, name] { exportProfile(name); });
        removeMenu->addAction(label, this, [this, name] {
            auto all = loadModProfiles(currentGame);
            all.remove(name);
            saveModProfiles(currentGame, all);
        });
    }
}

void ModsPage::saveProfile() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save profile"), tr("Profile name"), QLineEdit::Normal, {}, &ok)
                             .trimmed();
    if (!ok || name.isEmpty())
        return;
    QJsonArray mods;
    for (const auto &entry : model->entries)
        if (entry.installed && entry.enabled && !entry.unmanaged)
            mods.append(entry.id);
    QJsonObject profile{{QStringLiteral("mods"), mods}};
    if (profileFor(currentGame).activation == QStringLiteral("rimworld")) {
        const QString config = activationConfig(currentGame, modRootFor(currentGame, discovered));
        profile.insert(QStringLiteral("order"), QJsonArray::fromStringList(rimworldActive(config)));
    }
    auto all = loadModProfiles(currentGame);
    all.insert(name, profile);
    if (!saveModProfiles(currentGame, all))
        QMessageBox::warning(this, tr("Save profile"), tr("The profile could not be saved."));
    else
        emit status(tr("Profile saved."));
}

void ModsPage::applyProfile(const QString &name) {
    if (blocked)
        return;
    const auto profile = loadModProfiles(currentGame).value(name).toObject();
    QSet<QString> wanted;
    for (const auto &value : profile.value(QStringLiteral("mods")).toArray())
        wanted.insert(value.toString());
    QStringList missing;
    QList<QPair<Item, QString>> toInstall;
    QList<ModEntry> toChange;
    QSet<QString> known;
    const QString root = modRootFor(currentGame, discovered);
    for (const auto &entry : model->entries) {
        known.insert(entry.id);
        if (entry.unmanaged)
            continue;
        if (!wanted.contains(entry.id)) {
            if (entry.installed && entry.enabled)
                toChange << entry;
        } else if (!entry.installed) {
            if (const auto *item = library.get(entry.id); item && !root.isEmpty())
                toInstall.append({*item, root});
        } else if (!entry.enabled)
            toChange << entry;
    }
    for (const auto &id : wanted)
        if (!known.contains(id))
            missing << id;
    if (!confirmNotRunning({currentGame}))
        return;
    QStringList order;
    for (const auto &value : profile.value(QStringLiteral("order")).toArray())
        order << value.toString();
    const QString config = order.isEmpty() ? QString() : activationConfig(currentGame, root);
    const auto found = discovered;
    runOperation(tr("Applying profile…"), [toInstall, toChange, wanted, order, config, found] {
        QStringList errors;
        for (const auto &request : toInstall) {
            const QString error = installMod(request.first, request.second, false);
            if (!error.isEmpty())
                errors << request.first.title + QStringLiteral(": ") + error;
        }
        for (const auto &entry : toChange) {
            const QString error = setModEnabled(entry, wanted.contains(entry.id), found);
            if (!error.isEmpty())
                errors << entry.title + QStringLiteral(": ") + error;
        }
        if (!config.isEmpty() && QFileInfo::exists(config)) {
            const QString error = setRimworldOrder(config, order);
            if (!error.isEmpty())
                errors << error;
        }
        return errors;
    });
    if (!missing.isEmpty()) {
        emit log(tr("Profile %1: downloading %n missing mod(s). Apply it again when they finish.", nullptr,
                    int(missing.size()))
                     .arg(name));
        emit downloadRequested(missing);
    }
}

void ModsPage::exportProfile(const QString &name) {
    const auto profile = loadModProfiles(currentGame).value(name).toObject();
    const QString path = QFileDialog::getSaveFileName(this, tr("Export profile"), name + QStringLiteral(".txt"),
                                                      tr("Text files (*.txt)"));
    if (path.isEmpty())
        return;
    QByteArray bytes;
    for (const auto &value : profile.value(QStringLiteral("mods")).toArray()) {
        const auto *item = library.get(value.toString());
        if (validId(value.toString()) && !(item && item->local))
            bytes += workshopUrl(value.toString()).toUtf8() + '\n';
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        QMessageBox::warning(this, tr("Export failed"), file.errorString());
}

void ModsPage::importProfile() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Import profile"), {}, tr("Text files (*.txt);;All files (*)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
        QMessageBox::warning(this, tr("Import failed"), tr("Choose a readable text file smaller than 4 MB."));
        return;
    }
    const auto ids = parseIds(QString::fromUtf8(file.readAll()));
    if (ids.isEmpty()) {
        QMessageBox::warning(this, tr("Import failed"), tr("That file has no Workshop links or item IDs."));
        return;
    }
    const QString name = QFileInfo(path).completeBaseName();
    auto all = loadModProfiles(currentGame);
    all.insert(name, QJsonObject{{QStringLiteral("mods"), QJsonArray::fromStringList(ids)}});
    saveModProfiles(currentGame, all);
    if (QMessageBox::question(this, tr("Profile imported"),
                              tr("Profile %1 has %n mod(s). Apply it now?", nullptr, int(ids.size())).arg(name)) ==
        QMessageBox::Yes)
        applyProfile(name);
}

void ModsPage::loadOrderDialog() {
    if (blocked || !confirmNotRunning({currentGame}))
        return;
    const QString root = modRootFor(currentGame, discovered);
    QString path = activationConfig(currentGame, root);
    if (!QFileInfo::exists(path))
        path = QFileDialog::getOpenFileName(this, tr("Select RimWorld ModsConfig.xml"), {},
                                            tr("RimWorld config (ModsConfig.xml)"));
    if (path.isEmpty() || linkedAncestor(path))
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
        QMessageBox::warning(this, tr("Cannot read configuration"), tr("Choose a readable ModsConfig.xml smaller than 4 MB."));
        return;
    }
    const QByteArray original = file.readAll();
    file.close();
    const auto mods = rimworldMods(gameFolderFor(currentGame, discovered), root);
    QDialog dialog(this);
    Ui::ActivationDialog form;
    form.setupUi(&dialog);
    auto *list = form.list;
    QSet<QString> listed;
    const auto addRow = [&](const QString &id, bool active) {
        const auto mod = mods.value(id.toCaseFolded());
        auto *row = new QListWidgetItem(mod.name.isEmpty() ? id : mod.name, list);
        row->setData(Qt::UserRole, id);
        row->setToolTip(id);
        row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
        row->setCheckState(active ? Qt::Checked : Qt::Unchecked);
        listed.insert(id.toCaseFolded());
    };
    for (const auto &id : rimworldActive(path))
        addRow(id, true);
    QStringList others;
    for (const auto &mod : mods)
        if (!listed.contains(mod.id.toCaseFolded()))
            others << mod.id;
    others.sort(Qt::CaseInsensitive);
    for (const auto &id : others)
        addRow(id, false);
    const auto activeOrder = [list] {
        QStringList order;
        for (int n = 0; n < list->count(); ++n)
            if (list->item(n)->checkState() == Qt::Checked)
                order << list->item(n)->data(Qt::UserRole).toString();
        return order;
    };
    const auto refreshWarnings = [&] {
        const auto warnings = rimworldWarnings(activeOrder(), mods);
        form.warnings->setPlainText(warnings.join(QLatin1Char('\n')));
        form.warnings->setVisible(!warnings.isEmpty());
    };
    refreshWarnings();
    connect(list, &QListWidget::itemChanged, &dialog, refreshWarnings);
    connect(list->model(), &QAbstractItemModel::rowsMoved, &dialog, refreshWarnings);
    connect(form.sort, &QPushButton::clicked, &dialog, [&] {
        QStringList warnings;
        const auto sorted = rimworldSort(activeOrder(), mods, &warnings);
        const QSignalBlocker blocker(list);
        QList<QListWidgetItem *> inactive;
        QHash<QString, QListWidgetItem *> active;
        while (list->count()) {
            auto *row = list->takeItem(0);
            if (row->checkState() == Qt::Checked)
                active.insert(row->data(Qt::UserRole).toString(), row);
            else
                inactive << row;
        }
        for (const auto &id : sorted)
            list->addItem(active.value(id));
        for (auto *row : inactive)
            list->addItem(row);
        refreshWarnings();
        if (!warnings.isEmpty())
            form.warnings->appendPlainText(warnings.join(QLatin1Char('\n')));
    });
    connect(form.buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(form.buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QFile current(path);
        if (!current.open(QIODevice::ReadOnly) || current.readAll() != original) {
            QMessageBox::warning(&dialog, tr("Configuration changed"), tr("The file changed. Close and reopen this editor."));
            return;
        }
        current.close();
        const QString error = setRimworldOrder(path, activeOrder());
        if (!error.isEmpty()) {
            QMessageBox::warning(&dialog, tr("Save failed"), error);
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() == QDialog::Accepted)
        reload();
}

void ModsPage::activeModsDialog() {
    if (blocked || !confirmNotRunning({currentGame}))
        return;
    const QString game = currentGame;
    const auto profile = profileFor(game);
    const QString root = modRootFor(game, discovered);
    QString path = activationConfig(game, root);
    if (!QFileInfo::exists(path))
        path = QFileDialog::getOpenFileName(this, tr("Select the game's mod list file"), root,
                                            profile.activation == QStringLiteral("factorio")
                                                ? tr("Factorio (mod-list.json)")
                                                : tr("tModLoader (enabled.json)"));
    if (path.isEmpty() || linkedAncestor(path))
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
        QMessageBox::warning(this, tr("Cannot read configuration"), tr("Choose a readable file smaller than 4 MB."));
        return;
    }
    const QByteArray original = file.readAll();
    file.close();
    engine.settings.setValue(QStringLiteral("activation/") + game, path);
    auto states = activationStates(game, path);
    QStringList names = states.keys();
    if (profile.activation == QStringLiteral("tmodloader")) {
        const QString knownKey =
            QStringLiteral("knownActivation/") +
            QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex());
        names += engine.settings.value(knownKey).toStringList();
        names.removeDuplicates();
        engine.settings.setValue(knownKey, names);
    }
    names.sort(Qt::CaseInsensitive);
    QDialog dialog(this);
    Ui::GameActivationDialog form;
    form.setupUi(&dialog);
    dialog.setWindowTitle(tr("%1 active mods").arg(profile.name));
    for (const auto &name : names) {
        auto *item = new QListWidgetItem(name, form.list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(states.value(name) ? Qt::Checked : Qt::Unchecked);
        if (profile.activation == QStringLiteral("factorio") && name == QStringLiteral("base"))
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
    }
    connect(form.buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(form.buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QFile current(path);
        if (!current.open(QIODevice::ReadOnly) || current.readAll() != original) {
            QMessageBox::warning(&dialog, tr("Configuration changed"), tr("The game changed this file. Close and reopen this editor."));
            return;
        }
        current.close();
        for (int n = 0; n < form.list->count(); ++n) {
            auto *item = form.list->item(n);
            const bool checked = item->checkState() == Qt::Checked;
            if (checked == states.value(item->text()))
                continue;
            const QString error = setModActive(game, item->text(), path, checked);
            if (!error.isEmpty()) {
                QMessageBox::warning(&dialog, tr("Save failed"), error);
                return;
            }
        }
        dialog.accept();
    });
    if (dialog.exec() == QDialog::Accepted)
        reload();
}
