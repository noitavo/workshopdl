#include "engine.h"
#include "games.h"

namespace {
QNetworkRequest storeRequest(const QUrl &url) {
    QNetworkRequest request(url);
    request.setTransferTimeout(20000);
    request.setRawHeader("User-Agent", "WorkshopDL++/" APP_VERSION);
    return request;
}
void limitReply(QNetworkReply *reply) {
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64) {
        if (received > 4 * 1024 * 1024)
            reply->abort();
    });
}
}

void applyWorkshopDetails(Item &item, const QJsonObject &details) {
    item.previewUrl = details.value(QStringLiteral("preview_url")).toString(item.previewUrl);
    item.description = details.value(QStringLiteral("description")).toString(item.description).left(8000);
    item.creator = details.value(QStringLiteral("creator")).toString(item.creator);
    const auto tags = details.value(QStringLiteral("tags")).toArray();
    if (!tags.isEmpty()) {
        item.tags.clear();
        for (const auto &tag : tags)
            item.tags.append(tag.toObject().value(QStringLiteral("tag")).toString());
    }
}

void Engine::searchGames(const QString &term, QObject *context,
                         std::function<void(QList<StoreGame>)> callback) {
    QUrl url(QStringLiteral("https://store.steampowered.com/api/storesearch/"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("term"), term);
    query.addQueryItem(QStringLiteral("l"), QStringLiteral("english"));
    query.addQueryItem(QStringLiteral("cc"), QStringLiteral("US"));
    url.setQuery(query);
    auto *reply = network.get(storeRequest(url));
    limitReply(reply);
    QPointer<QObject> guard(context);
    connect(reply, &QNetworkReply::finished, this, [reply, guard, callback] {
        reply->deleteLater();
        if (!guard)
            return;
        QList<StoreGame> games;
        if (reply->error() == QNetworkReply::NoError) {
            const auto rows = QJsonDocument::fromJson(reply->readAll())
                                  .object()
                                  .value(QStringLiteral("items"))
                                  .toArray();
            for (const auto &row : rows) {
                const auto object = row.toObject();
                const QString id = object.value(QStringLiteral("id")).toVariant().toString();
                if (validId(id) && object.value(QStringLiteral("type")).toString() == QStringLiteral("app"))
                    games.append({id, object.value(QStringLiteral("name")).toString(),
                                  object.value(QStringLiteral("tiny_image")).toString()});
            }
        }
        callback(games);
    });
}

void Engine::lookupGame(const QString &id, QObject *context,
                        std::function<void(QString, QString)> callback) {
    QUrl url(QStringLiteral("https://store.steampowered.com/api/appdetails"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("appids"), id);
    query.addQueryItem(QStringLiteral("filters"), QStringLiteral("basic"));
    url.setQuery(query);
    auto *reply = network.get(storeRequest(url));
    limitReply(reply);
    QPointer<QObject> guard(context);
    connect(reply, &QNetworkReply::finished, this, [reply, guard, callback, id] {
        reply->deleteLater();
        if (!guard)
            return;
        QString name, image;
        if (reply->error() == QNetworkReply::NoError) {
            const auto data = QJsonDocument::fromJson(reply->readAll())
                                  .object()
                                  .value(id)
                                  .toObject()
                                  .value(QStringLiteral("data"))
                                  .toObject();
            name = data.value(QStringLiteral("name")).toString();
            image = data.value(QStringLiteral("header_image")).toString();
        }
        if (!name.isEmpty())
            rememberGame(id, name, image);
        callback(name, image);
    });
}

void Engine::backfillLibrary() {
    QStringList ids;
    for (const auto &item : library->items)
        if (!item.local && item.previewUrl.isEmpty())
            ids << item.id;
    if (ids.isEmpty())
        return;
    preview(ids, this, [this](QHash<QString, QJsonObject> details, QString) {
        for (auto it = details.cbegin(); it != details.cend(); ++it) {
            auto *item = library->get(it.key());
            if (!item || it.value().value(QStringLiteral("result")).toInt() != 1)
                continue;
            applyWorkshopDetails(*item, it.value());
            if (item->title.startsWith(QStringLiteral("Recovered folder ")))
                item->title = it.value().value(QStringLiteral("title")).toString(item->title);
            library->changed(item->id);
        }
        persist();
    });
}
