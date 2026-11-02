#include "window.h"
#include "modspage.h"
#include "gma.h"
#include "ui_supportdialog.h"
#include "ui_archivedialog.h"
#include "ui_archivecontentsdialog.h"

void Window::pasteMultiple() {
    QInputDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Add Workshop items"));
    dialog.setLabelText(QStringLiteral("Workshop links or IDs, one per line"));
    dialog.setOption(QInputDialog::UsePlainTextEditForTextInput);
    dialog.setInputMode(QInputDialog::TextInput);
    dialog.setOkButtonText(QStringLiteral("Add to queue"));
    dialog.resize(QSize(720, 360).boundedTo(screen()->availableGeometry().size() - QSize(40, 60)));
    if (dialog.exec() == QDialog::Accepted)
        addText(dialog.textValue());
}
void Window::importLocalMod(bool folder) {
    if (engine.busy || diskBusy)
        return;
    QStringList names;
    QList<GameProfile> choices;
    for (const auto &p : gameProfiles())
        if (p.format != QStringLiteral("manual")) {
            names << p.name;
            choices << p;
        }
    bool ok = false;
    const auto name = QInputDialog::getItem(this, QStringLiteral("Import local mod"), QStringLiteral("Game"),
                                            names, 0, false, &ok);
    if (!ok)
        return;
    const auto profile = choices[names.indexOf(name)];
    const QString source =
        folder ? QFileDialog::getExistingDirectory(this, QStringLiteral("Select local mod folder"))
               : QFileDialog::getOpenFileName(
                     this, QStringLiteral("Select local mod package"), {},
                     QStringLiteral(
                         "Mod packages (*.gma *.vpk *.pak *.zip *.scs *.tmod *.json);;All files (*)"));
    if (source.isEmpty())
        return;
    Item item;
    item.local = true;
    item.id = QString::number(qulonglong(QRandomGenerator::global()->generate64()));
    item.appId = profile.id;
    item.game = profile.name;
    item.title = QFileInfo(source).completeBaseName();
    item.status = QStringLiteral("Completed");
    item.path = QDir(engine.root).absolutePath() + QLatin1Char('/') + profile.id + QLatin1Char('/') + item.id;
    if (folder &&
        (item.path.startsWith(QDir(source).absolutePath() + QLatin1Char('/'), Qt::CaseInsensitive) ||
         linkedAncestor(source))) {
        QMessageBox::warning(
            this, QStringLiteral("Import folder"),
            QStringLiteral("Choose a source outside the download destination, without directory links."));
        return;
    }
    diskBusy = true;
    refresh();
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, item] {
        const auto error = watcher->result();
        watcher->deleteLater();
        diskBusy = false;
        if (error.isEmpty()) {
            Item added = item;
            added.size = directoryBytes(item.path);
            library.add(added);
            writeJson(item.path + QStringLiteral(".workshopdl.json"),
                      QJsonDocument(QJsonObject{{QStringLiteral("id"), item.id},
                                                {QStringLiteral("app"), item.appId},
                                                {QStringLiteral("title"), item.title},
                                                {QStringLiteral("local"), true}}));
            engine.persist();
            tabs->setCurrentIndex(2);
            modsPage->showGame(item.appId);
        } else
            QMessageBox::warning(this, QStringLiteral("Import failed"), error);
        refresh();
    });
    watcher->setFuture(QtConcurrent::run([source, item, folder] {
        QString error;
        const QString stage = item.path + QStringLiteral(".partial");
        if (linkedAncestor(item.path) || QFileInfo::exists(item.path) || !QDir().mkpath(stage))
            return QStringLiteral("Cannot create the import folder.");
        auto cleanup = qScopeGuard([stage] { QDir(stage).removeRecursively(); });
        if (folder) {
            if (!copyTree(source, stage, &error))
                return error;
        } else if (linkedAncestor(source) ||
                   !QFile::copy(source, stage + QLatin1Char('/') + QFileInfo(source).fileName()))
            return QStringLiteral("Cannot copy the mod package.");
        if (!QDir().rename(stage, item.path))
            return QStringLiteral("Cannot finish the import.");
        return QString();
    }));
}
void Window::showSupport() {
    QDialog dialog(this);
    Ui::SupportDialog form;
    form.setupUi(&dialog);
    auto *filter = form.filter;
    auto *table = form.table;
    auto *browse = form.browse;
    auto *workshop = form.workshop;
    auto *close = form.close;
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    auto fill = [this, table](const QHash<QString, QString> &found) {
        discoveredGameFolders = found;
        table->setRowCount(0);
        for (const auto &p : gameProfiles()) {
            const int row = table->rowCount();
            table->insertRow(row);
            auto *name = new QTableWidgetItem(p.name);
            name->setData(Qt::UserRole, p.id);
            table->setItem(row, 0, name);
            table->setItem(row, 1,
                           new QTableWidgetItem(supportLevel(p)));
            const auto folder =
                engine.settings.value(QStringLiteral("gameFolders/") + p.id, found.value(p.id)).toString();
            table->setItem(row, 2, new QTableWidgetItem(QDir::toNativeSeparators(folder)));
            auto *note = new QTableWidgetItem(p.note);
            note->setToolTip(p.note);
            table->setItem(row, 3, note);
            table->setItem(row, 4,
                           new QTableWidgetItem(p.id == QStringLiteral("427520")
                                                    ? QStringLiteral("Local files")
                                                    : QStringLiteral("Steam access required")));
            table->setItem(
                row, 5,
                new QTableWidgetItem(p.activation.isEmpty() ? QStringLiteral("In game / launcher")
                                                            : QStringLiteral("In Mods tab")));
        }
    };
    auto *watcher = new QFutureWatcher<QHash<QString, QString>>(&dialog);
    connect(watcher, &QFutureWatcher<QHash<QString, QString>>::finished, &dialog,
            [watcher, fill] { fill(watcher->result()); });
    watcher->setFuture(QtConcurrent::run(discoverGames));
    connect(filter, &QLineEdit::textChanged, &dialog, [table](const QString &text) {
        for (int n = 0; n < table->rowCount(); ++n)
            table->setRowHidden(n, !table->item(n, 0)->text().contains(text, Qt::CaseInsensitive));
    });
    connect(browse, &QPushButton::clicked, &dialog, [&] {
        const int row = table->currentRow();
        if (row < 0)
            return;
        const auto id = table->item(row, 0)->data(Qt::UserRole).toString();
        const auto folder = QFileDialog::getExistingDirectory(
            &dialog, QStringLiteral("Select game installation folder"), table->item(row, 2)->text());
        if (folder.isEmpty())
            return;
        engine.settings.setValue(QStringLiteral("gameFolders/") + id, folder);
        table->item(row, 2)->setText(QDir::toNativeSeparators(folder));
        const auto dest = gameDestination(id, folder);
        if (!dest.isEmpty())
            engine.settings.setValue(QStringLiteral("destinations/") + id, dest);
    });
    connect(workshop, &QPushButton::clicked, &dialog, [&] {
        const int row = table->currentRow();
        if (row < 0)
            return;
        const auto id = table->item(row, 0)->data(Qt::UserRole).toString();
        if (id == QStringLiteral("427520"))
            return;
        navigate(QUrl(QStringLiteral("https://steamcommunity.com/app/%1/workshop/").arg(id)));
        dialog.accept();
    });
    connect(table, &QTableWidget::currentCellChanged, &dialog, [table, note = form.note](int row) {
        if (row >= 0 && table->item(row, 3))
            note->setText(table->item(row, 3)->text());
    });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
    modsPage->reload();
}

void Window::gameTools() {
    if (diskBusy || engine.busy)
        return;
    QDialog dialog(this);
    Ui::ArchiveDialog form;
    form.setupUi(&dialog);
    auto *add = form.add;
    auto *downloaded = form.downloaded;
    auto *inspect = form.inspect;
    auto *table = form.table;
    auto *buttons = form.buttons;
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    QMap<QString, GmaInfo> archives;
    const auto addPaths = [&, table](QStringList paths, const QStringList &folders) {
        add->setEnabled(false);
        downloaded->setEnabled(false);
        inspect->setEnabled(false);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        auto *watcher = new QFutureWatcher<QMap<QString, GmaInfo>>(&dialog);
        connect(watcher, &QFutureWatcher<QMap<QString, GmaInfo>>::finished, &dialog, [&, watcher] {
            const auto results = watcher->result();
            watcher->deleteLater();
            for (auto it = results.cbegin(); it != results.cend(); ++it) {
                const auto &path = it.key();
                if (archives.contains(path))
                    continue;
                const auto &info = it.value();
                archives.insert(path, info);
                const int row = table->rowCount();
                table->insertRow(row);
                auto *item = new QTableWidgetItem(QFileInfo(path).fileName());
                item->setData(Qt::UserRole, path);
                item->setToolTip(path);
                table->setItem(row, 0, item);
                table->setItem(row, 1, new QTableWidgetItem(info.title));
                table->setItem(row, 2, new QTableWidgetItem(QString::number(info.files.size())));
                table->setItem(
                    row, 3,
                    new QTableWidgetItem(info.error.isEmpty() ? QStringLiteral("Ready") : info.error));
            }
            add->setEnabled(true);
            downloaded->setEnabled(true);
            inspect->setEnabled(table->currentRow() >= 0);
            buttons->button(QDialogButtonBox::Ok)->setEnabled(table->selectionModel()->hasSelection());
        });
        watcher->setFuture(QtConcurrent::run([paths = std::move(paths), folders]() mutable {
            for (const auto &folder : folders) {
                if (folder.isEmpty() || linkedAncestor(folder))
                    continue;
                QDirIterator it(folder, {QStringLiteral("*.gma"), QStringLiteral("*.bin")}, QDir::Files,
                                QDirIterator::Subdirectories);
                while (it.hasNext())
                    paths.append(it.next());
            }
            paths.removeDuplicates();
            QMap<QString, GmaInfo> results;
            for (const auto &path : paths)
                results.insert(path, inspectGma(path));
            return results;
        }));
    };
    inspect->setEnabled(false);
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(table->selectionModel(), &QItemSelectionModel::selectionChanged, &dialog, [&] {
        inspect->setEnabled(add->isEnabled() && table->currentRow() >= 0);
        buttons->button(QDialogButtonBox::Ok)
            ->setEnabled(add->isEnabled() && table->selectionModel()->hasSelection());
    });
    connect(add, &QPushButton::clicked, &dialog, [&, addPaths] {
        const auto paths = QFileDialog::getOpenFileNames(
            &dialog, QStringLiteral("Choose GMA archives"), {},
            QStringLiteral("Garry's Mod archives (*.gma *.bin);;All files (*)"));
        if (!paths.isEmpty())
            addPaths(paths, {});
    });
    connect(downloaded, &QPushButton::clicked, &dialog, [this, addPaths] {
        QStringList folders;
        for (const auto &item : library.items)
            if (item.appId == QStringLiteral("4000"))
                folders.append(item.path);
        addPaths({}, folders);
    });
    connect(inspect, &QPushButton::clicked, &dialog, [&] {
        if (table->currentRow() < 0)
            return;
        const auto info = archives.value(table->item(table->currentRow(), 0)->data(Qt::UserRole).toString());
        if (!info.error.isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("Archive"), info.error);
            return;
        }
        QDialog contents(&dialog);
        Ui::ArchiveContentsDialog contentsForm;
        contentsForm.setupUi(&contents);
        auto *files = contentsForm.files;
        auto *close = contentsForm.close;
        contents.setWindowTitle(info.title);
        contentsForm.author->setText(QStringLiteral("Author: ") + info.author);
        files->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        for (const auto &entry : info.files) {
            const int row = files->rowCount();
            files->insertRow(row);
            files->setItem(row, 0, new QTableWidgetItem(entry.name));
            files->setItem(row, 1, new QTableWidgetItem(sizeText(qint64(entry.size))));
        }
        connect(close, &QDialogButtonBox::rejected, &contents, &QDialog::reject);
        contents.exec();
    });
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Extract selected…"));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QStringList sources;
        for (const auto &row : table->selectionModel()->selectedRows())
            sources << table->item(row.row(), 0)->data(Qt::UserRole).toString();
        if (sources.isEmpty()) {
            QMessageBox::information(&dialog, QStringLiteral("Select archives"),
                                     QStringLiteral("Select archives first."));
            return;
        }
        const auto parent = QFileDialog::getExistingDirectory(&dialog, QStringLiteral("Extract into"));
        if (parent.isEmpty())
            return;
        diskBusy = true;
        refresh();
        dialog.accept();
        auto *watcher = new QFutureWatcher<QStringList>(this);
        connect(watcher, &QFutureWatcher<QStringList>::finished, this, [this, watcher, parent] {
            const auto errors = watcher->result();
            watcher->deleteLater();
            diskBusy = false;
            refresh();
            if (!errors.isEmpty())
                QMessageBox::warning(this, QStringLiteral("Extraction results"),
                                     errors.join(QStringLiteral("\n\n")));
            else
                QDesktopServices::openUrl(QUrl::fromLocalFile(parent));
        });
        watcher->setFuture(QtConcurrent::run([sources, parent] {
            QStringList errors;
            int n = 0;
            for (const auto &source : sources) {
                const QString target = parent + QLatin1Char('/') + QFileInfo(source).completeBaseName() +
                                       QStringLiteral("-%1").arg(++n);
                const auto error = extractGma(source, target);
                if (!error.isEmpty())
                    errors << QFileInfo(source).fileName() + QStringLiteral(": ") + error;
            }
            return errors;
        }));
    });
    dialog.exec();
}
