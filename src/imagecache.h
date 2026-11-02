#pragma once
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <functional>

class ImageCache : public QObject {
    Q_OBJECT
  public:
    using Callback = std::function<void(const QPixmap &)>;
    explicit ImageCache(QNetworkAccessManager &network, QObject *parent = nullptr);
    void request(const QString &url, QObject *context, Callback callback);

  private:
    struct Waiter {
        QPointer<QObject> context;
        Callback callback;
    };
    QNetworkAccessManager &network;
    QHash<QString, QList<Waiter>> waiting;
    QSet<QString> failed;
    QString pathFor(const QString &url) const;
    void finish(const QString &url, const QPixmap &pixmap);
};
