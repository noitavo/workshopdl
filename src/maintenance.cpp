#include "window.h"
#include "history_model.h"
#include "ui_mainwindow.h"
#include "ui_dependenciesdialog.h"

namespace {
qint64 treeBytes(const QString &path) {
    if (linkedAncestor(path))
        return 0;
    qint64 bytes = 0;
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (!it.fileInfo().isSymLink() && !linkedAncestor(it.filePath()))
            bytes += it.fileInfo().size();
    }
    return bytes;
}
QString scrub(QString text, const QStringList &secrets) {
    for (const auto &secret : secrets)
        if (!secret.isEmpty())
            text.replace(secret, QStringLiteral("[redacted]"));
    text.replace(QRegularExpression(QStringLiteral("([?&](?:key|token|auth|password)=)[^&\\s]+"),
                                    QRegularExpression::CaseInsensitiveOption),
                 QStringLiteral("\\1[redacted]"));
    return text;
}
}

void Engine::sampleTransfer() {
    if (!busy || active.isEmpty() || !attemptClock.isValid())
        return;
    const auto *item = queue->get(active);
    if (!item)
        return;
    const qint64 elapsed = attemptClock.elapsed();
    const qint64 seconds = elapsed / 1000;
    const QString stage = phase == QStringLiteral("commit")  ? QStringLiteral("Saving")
                          : phase == QStringLiteral("login") ? QStringLiteral("Signing in")
                                                             : QStringLiteral("Downloading");
    QString text =
        QStringLiteral("%1 · %2:%3").arg(stage).arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    if (runTotal > 1)
        text.prepend(QStringLiteral("Item %1 of %2 · ")
                         .arg(qMin(succeededCount + failedCount + int(sessionDone.size()) + 1, runTotal))
                         .arg(runTotal));
    if (transferBytes >= 0 && phase != QStringLiteral("commit") && phase != QStringLiteral("login")) {
        text += QStringLiteral(" · %1").arg(sizeText(transferBytes));
        if (transferBytes != lastBytes) {
            if (lastSample > 0 && elapsed > lastSample && transferBytes > lastBytes)
                transferSpeed =
                    sizeText((transferBytes - lastBytes) * 1000 / (elapsed - lastSample)) + QStringLiteral("/s");
            lastBytes = transferBytes;
            lastSample = elapsed;
        } else if (elapsed - lastSample > 15000)
            transferSpeed.clear();
        if (!transferSpeed.isEmpty())
            text += QStringLiteral(" · ") + transferSpeed;
    }
    if ((phase == QStringLiteral("download") || phase == QStringLiteral("http")) &&
        elapsed - lastOutput > 180000)
        text += QStringLiteral(" · No recent activity - retry or cancel");
    emit transferStatus(text);
    if ((phase != QStringLiteral("download") && phase != QStringLiteral("login")) || sampling ||
        process.state() != QProcess::Running || (lastFolderSample > 0 && elapsed - lastFolderSample < 5000))
        return;
    lastFolderSample = elapsed;
    sampling = true;
    const QString path = QFileInfo(steamExe).absolutePath() + QStringLiteral("/steamapps/workshop/");
    const QString app = item->appId, id = active;
    const quint64 generation = attemptGeneration;
    auto *watcher = new QFutureWatcher<qint64>(this);
    connect(watcher, &QFutureWatcher<qint64>::finished, this, [this, watcher, generation] {
        sampling = false;
        const qint64 bytes = watcher->result();
        watcher->deleteLater();
        if (busy && generation == attemptGeneration &&
            (phase == QStringLiteral("download") || phase == QStringLiteral("login"))) {
            if (transferBaseline < 0)
                transferBaseline = bytes;
            if (bytes != transferBytes)
                lastOutput = attemptClock.elapsed();
            transferBytes = bytes;
            if (phase == QStringLiteral("login") && bytes > transferBaseline) {
                phase = QStringLiteral("download");
                timeout.stop();
            }
            auto *current = queue->get(active);
            if (current && transferBaseline == 0 && current->size > 0 && phase == QStringLiteral("download")) {
                const int estimate = int(qMin(qint64(99), bytes * 100 / current->size));
                if (estimate > current->progress) {
                    current->progress = estimate;
                    queue->changed(active);
                }
            }
        }
    });
    watcher->setFuture(QtConcurrent::run([path, app, id] {
        return treeBytes(path + QStringLiteral("content/") + app + QLatin1Char('/') + id) +
               treeBytes(path + QStringLiteral("downloads/") + app + QLatin1Char('/') + id);
    }));
}

void Engine::recordAttempt(const QString &result, const QString &error) {
    const auto *item = queue->get(active);
    if (!item || !attemptClock.isValid())
        return;
    QStringList hidden = secrets;
    hidden << password << webApiKey;
    QJsonObject row{{QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                    {QStringLiteral("id"), item->id},
                    {QStringLiteral("app"), item->appId},
                    {QStringLiteral("title"), item->title},
                    {QStringLiteral("result"), result},
                    {QStringLiteral("method"), attemptMethod},
                    {QStringLiteral("seconds"), attemptSeconds >= 0 ? attemptSeconds : attemptClock.elapsed() / 1000},
                    {QStringLiteral("path"), QDir(root).filePath(item->appId + QLatin1Char('/') + item->id)},
                    {QStringLiteral("error"), scrub(error, hidden)}};
    const QString path = dataRoot() + QStringLiteral("/history.jsonl");
    QFile file(path);
    if (file.size() > 10 * 1024 * 1024) {
        QFile::remove(path + QStringLiteral(".previous"));
        if (!file.rename(path + QStringLiteral(".previous"))) {
            emit message(QStringLiteral("Could not rotate download history."));
            return;
        }
        file.setFileName(path);
    }
    const auto bytes = QJsonDocument(row).toJson(QJsonDocument::Compact) + '\n';
    if (!file.open(QIODevice::Append) || file.write(bytes) != bytes.size())
        emit storageError(QStringLiteral("Could not save download history."));
    file.close();
    emit historyChanged();
}

QString Engine::diagnostics() const {
    QString text = QStringLiteral("WorkshopDL++ %1\nQt %2\n%3\n\n")
                       .arg(QCoreApplication::applicationVersion(), QString::fromLatin1(qVersion()),
                            QSysInfo::prettyProductName());
    for (const auto &name : {QStringLiteral("activity.previous.log"), QStringLiteral("activity.log"),
                             QStringLiteral("history.jsonl.previous"), QStringLiteral("history.jsonl")}) {
        QFile file(dataRoot() + QLatin1Char('/') + name);
        if (file.open(QIODevice::ReadOnly))
            text += QString::fromUtf8(file.readAll()) + QLatin1Char('\n');
    }
    QStringList hidden = secrets;
    hidden << password << webApiKey << username;
    text.replace(QDir::homePath(), QStringLiteral("%USERPROFILE%"), Qt::CaseInsensitive);
    text.replace(QDir::toNativeSeparators(QDir::homePath()), QStringLiteral("%USERPROFILE%"),
                 Qt::CaseInsensitive);
    return scrub(text, hidden);
}

QString Engine::downloadError() const {
    return failureText(outputBuffer.toLower());
}

QString Engine::failureText(const QString &out) const {
    if (out.contains(QStringLiteral("rate limit")) || out.contains(QStringLiteral("too many login")))
        return QStringLiteral("Too many sign-in attempts. Wait before retrying.");
    if (out.contains(QStringLiteral("no subscription")) || out.contains(QStringLiteral("access denied")))
        return username.isEmpty()
                   ? QStringLiteral("Anonymous download denied. Sign in with an account that owns the game.")
                   : QStringLiteral("Access denied. This account must own the game.");
    if (out.contains(QStringLiteral("invalid password")) || out.contains(QStringLiteral("invalid login")))
        return QStringLiteral("Sign-in failed. Check Account settings.");
    if (out.contains(QStringLiteral("file not found")))
        return QStringLiteral("Item unavailable. Check its Workshop page and App ID.");
    if (out.contains(QStringLiteral("disk")) || out.contains(QStringLiteral("write failure")))
        return QStringLiteral("Disk error. Check free space and folder permissions.");
    if (out.contains(QStringLiteral("connection")) || out.contains(QStringLiteral("timeout")))
        return QStringLiteral("Connection failed. Retry the item.");
    if (username.isEmpty())
        return QStringLiteral("SteamCMD could not download this item anonymously. Many games require "
                              "signing in with an account that owns the game.");
    return QStringLiteral("SteamCMD failed. Check that this account owns the game, then see the Activity log.");
}

void Window::setupHistory() {
    historyModel = new HistoryModel(this);
    fullHistory = new HistoryFilter(this);
    fullHistory->setSourceModel(historyModel);
    fullHistory->setSortRole(Qt::UserRole);
    panelHistory = new HistoryFilter(this);
    panelHistory->setSourceModel(historyModel);
    panelHistory->setLatestOnly(true);
    auto *table = ui->historyTable;
    table->setModel(fullHistory);
    table->hideColumn(6);
    table->horizontalHeader()->setHighlightSections(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    table->setSortingEnabled(true);
    table->sortByColumn(0, Qt::DescendingOrder);
    auto *panel = ui->browserHistory;
    panel->setModel(panelHistory);
    for (const int column : {0, 3, 4, 5})
        panel->hideColumn(column);
    panel->horizontalHeader()->setHighlightSections(false);
    panel->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    panel->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->browserHistoryEmpty->hide();
    const auto bind = [this](QTableView *view, HistoryFilter *filter) {
        connect(view, &QTableView::activated, this, [this, view, filter] { openHistory(view, filter); });
        connect(view, &QTableView::customContextMenuRequested, this,
                [this, view, filter](const QPoint &point) { historyMenu(view, filter, point); });
        connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &Window::syncHistoryActions);
    };
    bind(table, fullHistory);
    bind(panel, panelHistory);
    connect(ui->historyOpen, &QPushButton::clicked, this, [this] { openHistory(ui->historyTable, fullHistory); });
    connect(ui->browserHistoryOpen, &QPushButton::clicked, this,
            [this] { openHistory(ui->browserHistory, panelHistory); });
    connect(ui->historyAgain, &QPushButton::clicked, this, [this] { downloadAgain(ui->historyTable, fullHistory); });
    connect(ui->browserHistoryAgain, &QPushButton::clicked, this,
            [this] { downloadAgain(ui->browserHistory, panelHistory); });
    connect(ui->historyWorkshop, &QPushButton::clicked, this, [this] {
        const auto rows = selectedHistory(ui->historyTable, fullHistory);
        if (!rows.isEmpty())
            navigate(QUrl(workshopUrl(rows.first().id)));
    });
    connect(ui->exportLogButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export log"),
                                                          QStringLiteral("WorkshopDL-log.txt"));
        if (path.isEmpty())
            return;
        QSaveFile file(path);
        const auto bytes = engine.diagnostics().toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            QMessageBox::warning(this, QStringLiteral("Export failed"), file.errorString());
    });
    connect(ui->historyClear, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, QStringLiteral("Clear history"),
                                  QStringLiteral("Clear the download history? Downloaded files are kept.")) !=
            QMessageBox::Yes)
            return;
        QFile::remove(dataRoot() + QStringLiteral("/history.jsonl"));
        QFile::remove(dataRoot() + QStringLiteral("/history.jsonl.previous"));
        historyModel->reload();
    });
    connect(ui->historySearch, &QLineEdit::textChanged, fullHistory, &HistoryFilter::setSearch);
    connect(ui->historyResult, &QComboBox::currentIndexChanged, this, [this](int index) {
        fullHistory->setResult(index == 0 ? QString() : ui->historyResult->itemText(index));
    });
    const auto sync = [this] {
        const int visible = fullHistory->rowCount();
        ui->historySummary->setText(historyModel->rowCount() == 0
                                        ? QStringLiteral("No downloads recorded yet.")
                                        : QStringLiteral("%1 of %2 entries").arg(visible).arg(historyModel->rowCount()));
        const bool empty = panelHistory->rowCount() == 0;
        ui->browserHistory->setVisible(!empty);
        ui->browserHistoryEmpty->setVisible(empty);
        syncHistoryActions();
    };
    for (auto *filter : {fullHistory, panelHistory}) {
        connect(filter, &QAbstractItemModel::modelReset, this, sync);
        connect(filter, &QAbstractItemModel::rowsInserted, this, sync);
        connect(filter, &QAbstractItemModel::rowsRemoved, this, sync);
        connect(filter, &QAbstractItemModel::layoutChanged, this, sync);
    }
    historyTimer.setSingleShot(true);
    historyTimer.setInterval(300);
    connect(&historyTimer, &QTimer::timeout, historyModel, &HistoryModel::reload);
    connect(&engine, &Engine::historyChanged, &historyTimer, [this] { historyTimer.start(); });
    ui->browserDownloads->setCurrentIndex(qBound(0, engine.settings.value(QStringLiteral("browserPanelTab"), 0).toInt(), 1));
    connect(ui->browserDownloads, &QTabWidget::currentChanged, this,
            [this](int index) { engine.settings.setValue(QStringLiteral("browserPanelTab"), index); });
    historyModel->reload();
    sync();
}

QList<HistoryEntry> Window::selectedHistory(QTableView *view, HistoryFilter *filter) const {
    QList<HistoryEntry> rows;
    for (const auto &index : view->selectionModel()->selectedRows()) {
        const auto source = filter->mapToSource(index);
        if (source.isValid() && source.row() < historyModel->entries.size())
            rows << historyModel->entries[source.row()];
    }
    return rows;
}

void Window::syncHistoryActions() {
    if (!historyModel || !startButton)
        return;
    const auto update = [this](QTableView *view, HistoryFilter *filter, QPushButton *open, QPushButton *again) {
        const auto rows = selectedHistory(view, filter);
        open->setEnabled(std::any_of(rows.cbegin(), rows.cend(),
                                     [](const HistoryEntry &row) { return QFileInfo(row.path).isDir(); }));
        again->setEnabled(!diskBusy && !rows.isEmpty());
    };
    update(ui->historyTable, fullHistory, ui->historyOpen, ui->historyAgain);
    update(ui->browserHistory, panelHistory, ui->browserHistoryOpen, ui->browserHistoryAgain);
    ui->historyWorkshop->setEnabled(ui->historyTable->selectionModel()->hasSelection());
}

void Window::openHistory(QTableView *view, HistoryFilter *filter) {
    const auto rows = selectedHistory(view, filter);
    if (rows.isEmpty())
        return;
    if (QFileInfo(rows.first().path).isDir())
        openFolder(rows.first().path);
    else
        navigate(QUrl(workshopUrl(rows.first().id)));
}

void Window::downloadAgain(QTableView *view, HistoryFilter *filter) {
    QStringList ids;
    for (const auto &row : selectedHistory(view, filter))
        if (validId(row.id) && !ids.contains(row.id))
            ids << row.id;
    if (ids.isEmpty() || diskBusy)
        return;
    downloadNow(ids);
    if (view == ui->browserHistory)
        ui->browserDownloads->setCurrentIndex(0);
    else
        ui->downloadTabs->setCurrentIndex(0);
}

void Window::downloadNow(const QStringList &ids) {
    downloadWhenReady.append(ids);
    downloadWhenReady.removeDuplicates();
    engine.resolve(ids);
    maybeStartDownloads();
    refresh();
}

void Window::historyMenu(QTableView *view, HistoryFilter *filter, const QPoint &point) {
    const auto index = view->indexAt(point);
    if (!index.isValid())
        return;
    if (!view->selectionModel()->isRowSelected(index.row(), {}))
        view->selectRow(index.row());
    const auto rows = selectedHistory(view, filter);
    if (rows.isEmpty())
        return;
    QMenu menu(this);
    auto *open = menu.addAction(QStringLiteral("Open folder"), this, [this, rows] { openFolder(rows.first().path); });
    open->setEnabled(QFileInfo(rows.first().path).isDir());
    menu.addAction(QStringLiteral("Open Workshop page"), this,
                   [this, rows] { navigate(QUrl(workshopUrl(rows.first().id))); });
    menu.addAction(QStringLiteral("Copy link"), this, [rows] {
        QStringList links;
        for (const auto &row : rows)
            links << workshopUrl(row.id);
        QApplication::clipboard()->setText(links.join(QLatin1Char('\n')));
    });
    menu.addSeparator();
    auto *again = menu.addAction(QStringLiteral("Download again"), this,
                                 [this, view, filter] { downloadAgain(view, filter); });
    again->setEnabled(!diskBusy);
    menu.exec(view->viewport()->mapToGlobal(point));
}

void Window::moveDownloads(const QString &from, const QString &to) {
    struct Move {
        QString id, source, target;
    };
    QList<Move> moves;
    const QString prefix = QDir::cleanPath(from) + QLatin1Char('/');
    for (const auto &item : library.items) {
        const QString path = QDir::cleanPath(item.path);
        if (path.startsWith(prefix, Qt::CaseInsensitive))
            moves.append({item.id, path, QDir::cleanPath(to) + QLatin1Char('/') + path.mid(prefix.size())});
    }
    if (moves.isEmpty())
        return;
    diskBusy = true;
    refresh();
    activity->setText(QStringLiteral("Moving %1 download(s)…").arg(moves.size()));
    using MoveResult = QPair<QHash<QString, QString>, QStringList>;
    auto *watcher = new QFutureWatcher<MoveResult>(this);
    connect(watcher, &QFutureWatcher<MoveResult>::finished, this, [this, watcher] {
        const auto [moved, errors] = watcher->result();
        watcher->deleteLater();
        for (auto it = moved.cbegin(); it != moved.cend(); ++it)
            if (auto *item = library.get(it.key())) {
                item->path = it.value();
                library.changed(it.key());
            }
        diskBusy = false;
        engine.persist();
        refresh();
        activity->setText(QStringLiteral("%1 download(s) moved.").arg(moved.size()));
        if (!errors.isEmpty())
            QMessageBox::warning(this, QStringLiteral("Some downloads were not moved"),
                                 errors.join(QLatin1Char('\n')));
    });
    watcher->setFuture(QtConcurrent::run([moves] {
        QHash<QString, QString> moved;
        QStringList errors;
        for (const auto &move : moves) {
            if (!QFileInfo::exists(move.source))
                continue;
            if (QFileInfo::exists(move.target) || linkedAncestor(move.source) ||
                !QDir().mkpath(QFileInfo(move.target).absolutePath())) {
                errors << QDir::toNativeSeparators(move.target) + QStringLiteral(" already exists or is not writable.");
                continue;
            }
            if (!renamePath(move.source, move.target)) {
                QString error;
                const QString stage = move.target + QStringLiteral(".partial");
                if (QDir(stage).exists())
                    QDir(stage).removeRecursively();
                if (!copyTree(move.source, stage, &error) || !renamePath(stage, move.target)) {
                    QDir(stage).removeRecursively();
                    errors << move.id + QStringLiteral(": ") + (error.isEmpty() ? QStringLiteral("Could not move the files.") : error);
                    continue;
                }
                QDir(move.source).removeRecursively();
            }
            const QString receipt = move.source + QStringLiteral(".workshopdl.json");
            if (QFileInfo::exists(receipt) && !renamePath(receipt, move.target + QStringLiteral(".workshopdl.json")))
                QFile::copy(receipt, move.target + QStringLiteral(".workshopdl.json"));
            QFile::remove(receipt);
            QDir().rmdir(QFileInfo(move.source).absolutePath());
            moved.insert(move.id, move.target);
        }
        return MoveResult{moved, errors};
    }));
}

void Window::recoverLibrary() {
    if (engine.busy || diskBusy || engine.resolving)
        return;
    const QString root =
        QFileDialog::getExistingDirectory(this, QStringLiteral("Find downloaded files"), engine.root);
    if (root.isEmpty() || linkedAncestor(root))
        return;
    diskBusy = true;
    refresh();
    auto *watcher = new QFutureWatcher<QList<Item>>(this);
    connect(watcher, &QFutureWatcher<QList<Item>>::finished, this, [this, watcher] {
        int added = 0, incomplete = 0;
        for (const auto &item : watcher->result()) {
            if (item.status == QStringLiteral("Incomplete"))
                ++incomplete;
            if (auto *existing = library.get(item.id); existing && !QDir(existing->path).exists()) {
                *existing = item;
                library.changed(item.id);
                ++added;
            } else if (!existing && library.add(item))
                ++added;
        }
        watcher->deleteLater();
        diskBusy = false;
        engine.persist();
        refresh();
        activity->setText(QStringLiteral("%1 items found · %2 incomplete").arg(added).arg(incomplete));
        engine.backfillLibrary();
    });
    watcher->setFuture(QtConcurrent::run([root] {
        QList<Item> found;
        for (const auto &app : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (!validId(app) || linkedAncestor(root + QLatin1Char('/') + app))
                continue;
            QDir folder(root + QLatin1Char('/') + app);
            for (const auto &leaf : folder.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                QString id = leaf;
                for (const auto &suffix : {QStringLiteral(".previous"), QStringLiteral(".partial")})
                    if (id.endsWith(suffix))
                        id.chop(suffix.size());
                if (!validId(id))
                    continue;
                const QString target = folder.filePath(id), source = folder.filePath(leaf);
                if (linkedAncestor(source) || linkedAncestor(target))
                    continue;
                const bool restored = leaf.endsWith(QStringLiteral(".previous")) &&
                                      !QFileInfo::exists(target) && QDir().rename(source, target);
                if (leaf != id && QFileInfo::exists(target) && !restored)
                    continue;
                Item item;
                item.id = id;
                item.appId = app;
                item.game = gameName(app);
                item.path = QFileInfo::exists(target) ? target : source;
                const auto receipt = readObject(target + QStringLiteral(".workshopdl.json"));
                const bool known = receipt.value(QStringLiteral("id")).toString() == id &&
                                   receipt.value(QStringLiteral("app")).toString() == app;
                item.local = known && receipt.value(QStringLiteral("local")).toBool();
                item.title = known ? receipt.value(QStringLiteral("title")).toString()
                                   : QStringLiteral("Recovered folder ") + id;
                item.updated = known ? receipt.value(QStringLiteral("updated")).toString().toLongLong() : 0;
                item.status = item.path == target && safeTree(target) ? QStringLiteral("Completed")
                                                                      : QStringLiteral("Incomplete");
                if (item.status == QStringLiteral("Incomplete"))
                    item.error = QStringLiteral("Incomplete files. Download again or remove this folder.");
                item.size = treeBytes(item.path);
                found.append(item);
            }
        }
        return found;
    }));
}

void Window::cleanCache() {
    if (engine.busy || diskBusy || engine.resolving)
        return;
    const QString cache =
        QFileInfo(engine.steamExe).absolutePath() + QStringLiteral("/steamapps/workshop/content");
    QStringList paths;
    QHash<QString, QString> savedCopies;
    for (const auto &item : library.items) {
        if (item.local || item.status != QStringLiteral("Completed") || item.path.isEmpty())
            continue;
        const QString path = cache + QLatin1Char('/') + item.appId + QLatin1Char('/') + item.id;
        if (!linkedAncestor(path) && !pathsOverlap(path, item.path) && QDir(path).exists()) {
            paths << path;
            savedCopies.insert(path, item.path);
        }
    }
    if (QFileInfo::exists(dataRoot() + QStringLiteral("/http")))
        paths << dataRoot() + QStringLiteral("/http");
    QStringList protectedPaths{engine.root};
    for (const auto &item : library.items)
        if (!item.path.isEmpty())
            protectedPaths << item.path;
    for (const auto &record : installedRecords())
        protectedPaths << record.toObject().value(QStringLiteral("root")).toString();
    paths.removeIf([protectedPaths](const QString &path) {
        for (const auto &protectedPath : protectedPaths) {
            if (protectedPath.isEmpty())
                continue;
            if (pathsOverlap(path, protectedPath))
                return true;
        }
        return false;
    });
    if (paths.isEmpty()) {
        activity->setText(QStringLiteral("The cache is already clean."));
        return;
    }
    if (QMessageBox::question(
            this, QStringLiteral("Clean cache"),
            QStringLiteral("Remove %1 cache folder(s)?\nSaved downloads are kept.").arg(paths.size())) !=
        QMessageBox::Yes)
        return;
    diskBusy = true;
    refresh();
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        const QString result = watcher->result();
        watcher->deleteLater();
        diskBusy = false;
        refresh();
        activity->setText(result);
    });
    watcher->setFuture(QtConcurrent::run([paths, savedCopies] {
        qint64 freed = 0;
        int failed = 0;
        for (const auto &path : paths) {
            if (!QFileInfo::exists(path))
                continue;
            if (savedCopies.contains(path) && !safeTree(savedCopies.value(path))) {
                ++failed;
                continue;
            }
            const qint64 bytes = treeBytes(path);
            if (!linkedAncestor(path) && safeTree(path, nullptr, true) && QDir(path).removeRecursively()) {
                freed += bytes;
                if (savedCopies.contains(path)) {
                    QDir app(QFileInfo(path).absolutePath());
                    const QString appId = app.dirName();
                    if (app.cdUp() && app.cdUp())
                        QFile::remove(app.filePath(QStringLiteral("appworkshop_%1.acf").arg(appId)));
                }
            } else
                ++failed;
        }
        return QStringLiteral("%1 freed · %2 folder(s) kept").arg(sizeText(freed)).arg(failed);
    }));
}

void Engine::reviewDependencies(const QStringList &ids, QObject *context,
                                std::function<void(QMap<QString, QString>, QString)> callback) {
    const QString key = webApiKey;
    if (key.isEmpty()) {
        callback({}, QStringLiteral("Set a Steam Web API key in Settings to check required items."));
        return;
    }
    struct State {
        QStringList todo;
        QSet<QString> visited, roots;
        QMap<QString, QString> found;
        QString error;
    };
    auto state = std::make_shared<State>();
    for (const auto &id : ids)
        if (validId(id) && !state->visited.contains(id)) {
            state->todo << id;
            state->visited.insert(id);
            state->roots.insert(id);
        }
    QPointer<QObject> guard(context);
    auto step = [this, state, key, guard, callback](auto self) -> void {
        if (!guard)
            return;
        if (state->todo.isEmpty()) {
            callback(state->found, state->error);
            return;
        }
        QJsonArray ids;
        const auto batch = state->todo.mid(0, 50);
        state->todo = state->todo.mid(batch.size());
        for (const auto &id : batch)
            ids.append(id);
        const QJsonObject input{{QStringLiteral("publishedfileids"), ids},
                                {QStringLiteral("includechildren"), true}};
        QUrl url(QStringLiteral("https://api.steampowered.com/IPublishedFileService/GetDetails/v1/"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("key"), key);
        query.addQueryItem(QStringLiteral("input_json"),
                           QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact)));
        url.setQuery(query);
        QNetworkRequest request(url);
        request.setTransferTimeout(30000);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        auto *reply = network.get(request);
        connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 bytes, qint64) {
            if (bytes > 8 * 1024 * 1024)
                reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [state, guard, callback, reply, self, batch] {
            reply->deleteLater();
            if (!guard)
                return;
            const auto bytes = reply->readAll();
            const auto details = QJsonDocument::fromJson(bytes)
                                     .object()
                                     .value(QStringLiteral("response"))
                                     .toObject()
                                     .value(QStringLiteral("publishedfiledetails"))
                                     .toArray();
            if (reply->error() != QNetworkReply::NoError || bytes.size() > 8 * 1024 * 1024 ||
                details.size() != batch.size()) {
                callback(
                    state->found,
                    QStringLiteral("Required items could not be checked. Check the API key and connection."));
                return;
            }
            for (const auto &value : details) {
                const auto item = value.toObject();
                const QString id = item.value(QStringLiteral("publishedfileid")).toVariant().toString();
                if (item.value(QStringLiteral("result")).toInt() != 1) {
                    state->error = QStringLiteral("Some required items are unavailable.");
                    continue;
                }
                const bool collection = item.value(QStringLiteral("file_type")).toInt() == 2 ||
                                        item.value(QStringLiteral("filetype")).toInt() == 2;
                if (!state->roots.contains(id)) {
                    if (collection)
                        state->error = QStringLiteral("A required collection must be imported separately.");
                    else
                        state->found.insert(id, item.value(QStringLiteral("title")).toString(id));
                }
                if (collection)
                    continue;
                for (const auto &child : item.value(QStringLiteral("children")).toArray()) {
                    const QString childId =
                        child.toObject().value(QStringLiteral("publishedfileid")).toVariant().toString();
                    if (!validId(childId) || state->visited.contains(childId))
                        continue;
                    if (state->visited.size() >= 5000) {
                        callback(state->found, QStringLiteral("Required-item limit reached (5,000)."));
                        return;
                    }
                    state->visited.insert(childId);
                    state->todo << childId;
                }
            }
            self(self);
        });
    };
    step(step);
}

void Window::addDependencies(const QStringList &ids, bool download) {
    if (engine.busy || diskBusy || engine.resolving)
        return;
    diskBusy = true;
    refresh();
    activity->setText(QStringLiteral("Checking required items…"));
    engine.reviewDependencies(ids, this, [this, ids, download](QMap<QString, QString> found, QString error) {
        diskBusy = false;
        refresh();
        if (found.isEmpty()) {
            if (download) {
                if (error.isEmpty() ||
                    QMessageBox::question(this, QStringLiteral("Required items"),
                                          error + QStringLiteral("\nDownload anyway?")) == QMessageBox::Yes)
                    engine.start(ids);
                return;
            }
            QMessageBox::information(this, QStringLiteral("Required items"),
                                     error.isEmpty() ? QStringLiteral("No required items found.") : error);
            return;
        }
        QDialog dialog(this);
        Ui::DependenciesDialog form;
        form.setupUi(&dialog);
        auto *list = form.list;
        auto *buttons = form.buttons;
        form.note->setText(error);
        form.note->setVisible(!error.isEmpty());
        for (auto it = found.cbegin(); it != found.cend(); ++it) {
            auto *row = new QListWidgetItem(it.value(), list);
            row->setData(Qt::UserRole, it.key());
            row->setToolTip(workshopUrl(it.key()));
            row->setCheckState(queue.get(it.key()) || library.get(it.key()) ? Qt::Unchecked : Qt::Checked);
        }
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Add selected"));
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted)
            return;
        QStringList selectedIds;
        for (int n = 0; n < list->count(); ++n)
            if (list->item(n)->checkState() == Qt::Checked)
                selectedIds << list->item(n)->data(Qt::UserRole).toString();
        const auto newItems = std::count_if(selectedIds.cbegin(), selectedIds.cend(),
                                            [this](const QString &id) { return !queue.get(id); });
        if (queue.items.size() + newItems > 10000) {
            QMessageBox::warning(this, QStringLiteral("Queue limit"),
                                 QStringLiteral("The queue can hold up to 10,000 items."));
            return;
        }
        if (download) {
            dependenciesReviewed = true;
            downloadWhenReady = ids;
            downloadWhenReady.append(selectedIds);
            downloadWhenReady.removeDuplicates();
        }
        if (!selectedIds.isEmpty())
            engine.resolve(selectedIds);
        if (download)
            maybeStartDownloads();
    });
}
