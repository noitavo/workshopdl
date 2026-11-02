#include "imagecache.h"
#include "storage.h"
#include <QNetworkReply>
#include <QtConcurrent>

namespace {
constexpr qint64 maximumImageBytes = 16 * 1024 * 1024;
constexpr qint64 maximumCacheBytes = 256 * 1024 * 1024;

QString cacheFolder() {
    return dataRoot() + QStringLiteral("/cache/images");
}
bool allowedHost(const QUrl &url) {
    if (url.scheme() != QStringLiteral("https"))
        return false;
    const QString host = url.host().toLower();
    for (const auto &domain : {QStringLiteral("steamusercontent.com"), QStringLiteral("steamstatic.com"),
                               QStringLiteral("akamaihd.net"), QStringLiteral("steampowered.com")})
        if (host == domain || host.endsWith(QLatin1Char('.') + domain))
            return true;
    return false;
}
void pruneCache() {
    QDir folder(cacheFolder());
    auto files = folder.entryInfoList(QDir::Files, QDir::Time);
    qint64 total = 0;
    for (const auto &file : files) {
        total += file.size();
        if (total > maximumCacheBytes)
            QFile::remove(file.absoluteFilePath());
    }
}
}

ImageCache::ImageCache(QNetworkAccessManager &manager, QObject *parent) : QObject(parent), network(manager) {
    QDir().mkpath(cacheFolder());
    QThreadPool::globalInstance()->start(pruneCache);
}

QString ImageCache::pathFor(const QString &url) const {
    const QByteArray stable = QUrl(url).toString(QUrl::RemoveQuery).toUtf8();
    return cacheFolder() + QLatin1Char('/') +
           QString::fromLatin1(QCryptographicHash::hash(stable, QCryptographicHash::Sha256).toHex());
}

void ImageCache::finish(const QString &url, const QPixmap &pixmap) {
    const auto waiters = waiting.take(url);
    for (const auto &waiter : waiters)
        if (waiter.context)
            waiter.callback(pixmap);
}

void ImageCache::request(const QString &address, QObject *context, Callback callback) {
    const QUrl url(address);
    if (address.isEmpty() || !allowedHost(url) || failed.contains(address)) {
        callback({});
        return;
    }
    const QString path = pathFor(address);
    QPixmap cached;
    if (QFileInfo::exists(path) && cached.load(path)) {
        callback(cached);
        return;
    }
    auto &waiters = waiting[address];
    waiters.append({QPointer<QObject>(context), std::move(callback)});
    if (waiters.size() > 1)
        return;
    QNetworkRequest request(url);
    request.setTransferTimeout(30000);
    request.setRawHeader("User-Agent", "WorkshopDL++/" APP_VERSION);
    auto *reply = network.get(request);
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64) {
        if (received > maximumImageBytes)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, address, path] {
        reply->deleteLater();
        const QByteArray bytes = reply->readAll();
        QPixmap pixmap;
        if (reply->error() != QNetworkReply::NoError || !pixmap.loadFromData(bytes)) {
            failed.insert(address);
            finish(address, {});
            return;
        }
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size())
            file.commit();
        finish(address, pixmap);
    });
}
