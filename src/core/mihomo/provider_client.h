#pragma once

#include <utility>

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

    // Each call returns the COALESCING key of the operation it now shares.
    // pending_ is keyed by (epoch, operation identity), not by submission: an
    // identical operation already outstanding under the same epoch issues
    // nothing and mints nothing, and the caller is told which outstanding
    // operation it joined. Empty means nothing was submitted at all.
    QString fetch(bool rules);
    QString update(bool rules, const QString &name);
    QString healthCheck(const QString &name);

    /// The key of the reply handler that is running, or empty. A payload signal
    /// emitted outside one reports empty, which marks it unsolicited.
    QString currentKey() const { return currentKey_; }

signals:
    void providersReceived(bool rules, const QVector<core::Provider> &providers);
    void busyChanged(bool busy);
    void errorOccurred(const QString &message);
    void operationFinished(const QString &message);
    /// The single terminal event of a provider operation, keyed by the same
    /// coalescing key fetch()/update()/healthCheck() returned.
    void requestSettled(const QString &key, bool superseded, const QString &error);

private:
    QNetworkReply *request(const QString &path, bool put = false);
    bool sameEndpoint(const Endpoint &endpoint) const;
    void finish(const QString &key);
    void settle(const QString &key, bool superseded, const QString &error);
    QString operate(bool rules, const QString &name, bool healthCheck);

    class KeyScope {
      public:
        KeyScope(ProviderClient *self, QString key)
            : self_(self), previous_(self->currentKey_) { self_->currentKey_ = std::move(key); }
        ~KeyScope() { self_->currentKey_ = previous_; }
        KeyScope(const KeyScope &) = delete;
        KeyScope &operator=(const KeyScope &) = delete;
      private:
        ProviderClient *self_;
        QString previous_;
    };

    MihomoClient *client_;
    QNetworkAccessManager *network_;
    QSet<QString> pending_;
    QString currentKey_;
    quint64 epoch_ = 0;
};

}  // namespace core
