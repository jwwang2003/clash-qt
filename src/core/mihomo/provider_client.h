#pragma once

#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QVector>

#include "core/types.h"

class QNetworkAccessManager;
class QNetworkReply;

namespace core {

class MihomoClient;

struct Provider {
    QString name;
    QString type;
    QString vehicle;
    QString behavior;
    int count = 0;
    QDateTime updated;
    quint64 used = 0;
    quint64 total = 0;
    QDateTime expires;
};

class ProviderClient : public QObject {
    Q_OBJECT

public:
    explicit ProviderClient(MihomoClient *client, QObject *parent = nullptr);
    void fetch(bool rules);
    void update(bool rules, const QString &name);
    void healthCheck(const QString &name);

signals:
    void providersReceived(bool rules, const QVector<core::Provider> &providers);
    void busyChanged(bool busy);
    void errorOccurred(const QString &message);
    void operationFinished(const QString &message);

private:
    QNetworkReply *request(const QString &path, bool put = false);
    bool sameEndpoint(const Endpoint &endpoint) const;
    void finish(const QString &key);
    void operate(bool rules, const QString &name, bool healthCheck);

    MihomoClient *client_;
    QNetworkAccessManager *network_;
    QSet<QString> pending_;
    quint64 epoch_ = 0;
};

}  // namespace core
