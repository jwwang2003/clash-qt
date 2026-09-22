#include "core/mihomo/provider_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include "core/mihomo/mihomo_client.h"

namespace core {
namespace {

QString basePath(bool rules) { return rules ? "/providers/rules" : "/providers/proxies"; }

QDateTime parseDate(const QString &value) {
    auto date = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!date.isValid()) date = QDateTime::fromString(value, Qt::ISODate);
    return date;
}

quint64 positiveNumber(const QJsonValue &value) {
    const qint64 number = value.toInteger();
    return number > 0 ? static_cast<quint64>(number) : 0;
}

}  // namespace

ProviderClient::ProviderClient(MihomoClient *client, QObject *parent)
    : QObject(parent), client_(client), network_(new QNetworkAccessManager(this)) {
    if (client_ != nullptr) client_->addSessionParticipant(this);
}

ProviderClient::~ProviderClient() {
    // The client outlives this object in MihomoBackendImpl's declaration order,
    // so a participant that did not deregister would be retired after it died.
    if (client_ != nullptr) client_->removeSessionParticipant(this);
}

void ProviderClient::retireSession() {
    // The epoch moves BEFORE the abort, exactly as MihomoClient's own does:
    // finished() can run synchronously inside abort(), and a reply retired by a
    // bump that had not happened yet would settle as live data.
    ++epoch_;
    const auto replies = network_->findChildren<QNetworkReply *>();
    for (auto *reply : replies) reply->abort();
}

QNetworkReply *ProviderClient::request(const QString &path, bool put) {
    QNetworkRequest request{QUrl(client_->endpoint().httpBase() + path)};
    request.setTransferTimeout(30000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    if (!client_->endpoint().secret.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + client_->endpoint().secret.toUtf8());
    }
    return put ? network_->put(request, QByteArray()) : network_->get(request);
}

void ProviderClient::finish(const QString &key) {
    pending_.remove(key);
    emit busyChanged(!pending_.isEmpty());
}

void ProviderClient::settle(const QString &key, bool superseded, const QString &error) {
    emit requestSettled(key, superseded, error);
}

QString ProviderClient::fetch(bool rules) {
    const quint64 epoch = epoch_;
    const QString key = QString::number(epoch) + ':' + basePath(rules);
    // Coalesced, not rejected: the caller joins the outstanding operation and is
    // handed its key. Minting one id per submission would mint two and issue
    // both, which is exactly what this set exists to prevent. The epoch in the
    // key is what keeps that scoped to the CURRENT session.
    if (pending_.contains(key)) return key;
    pending_.insert(key);
    emit busyChanged(true);
    auto *reply = request(basePath(rules));
    connect(reply, &QNetworkReply::finished, this, [this, reply, rules, key, epoch] {
        reply->deleteLater();
        finish(key);
        // The epoch, and ONLY the epoch. Comparing the address instead - which
        // is what the removed sameEndpoint() did - accepted a reply owed by an
        // engine that had been replaced at the address it already held, because
        // a replacement preserves host, port and secret exactly.
        if (epoch != epoch_) { settle(key, true, {}); return; }
        KeyScope scope(this, key);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
            const QString message = tr("Could not load providers: %1").arg(reply->errorString());
            emit errorOccurred(message);
            settle(key, false, message);
            return;
        }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(reply->readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject() ||
            !document.object().value("providers").isObject()) {
            const QString message = tr("The controller returned an invalid provider list.");
            emit errorOccurred(message);
            settle(key, false, message);
            return;
        }
        QVector<Provider> providers;
        const auto entries = document.object().value("providers").toObject();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (!it.value().isObject()) continue;
            const auto value = it.value().toObject();
            Provider provider;
            provider.name = it.key();
            provider.type = value.value("type").toString();
            provider.vehicle = value.value("vehicleType").toString();
            if (!rules && provider.vehicle.compare("Compatible", Qt::CaseInsensitive) == 0)
                continue;
            provider.behavior = value.value("behavior").toString();
            provider.count = rules ? value.value("ruleCount").toInt()
                                   : value.value("proxies").toArray().size();
            provider.updated = parseDate(value.value("updatedAt").toString());
            const auto subscription = value.value("subscriptionInfo").toObject();
            provider.used = positiveNumber(subscription.value("Upload")) +
                            positiveNumber(subscription.value("Download"));
            provider.total = positiveNumber(subscription.value("Total"));
            const qint64 expires = subscription.value("Expire").toInteger();
            if (expires > 0) provider.expires = QDateTime::fromSecsSinceEpoch(expires);
            providers.append(provider);
        }
        emit providersReceived(rules, providers);
        settle(key, false, {});
    });
    return key;
}

QString ProviderClient::update(bool rules, const QString &name) { return operate(rules, name, false); }

QString ProviderClient::healthCheck(const QString &name) { return operate(false, name, true); }

QString ProviderClient::operate(bool rules, const QString &name, bool healthCheck) {
    if (name.isEmpty()) return {};
    const quint64 epoch = epoch_;
    const QString path = basePath(rules) + '/' + QString::fromLatin1(QUrl::toPercentEncoding(name)) +
                         (healthCheck ? "/healthcheck" : "");
    const QString key = QString::number(epoch) + ':' + path;
    if (pending_.contains(key)) return key;
    pending_.insert(key);
    emit busyChanged(true);
    auto *reply = request(path, !healthCheck);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, rules, name, healthCheck, key, epoch] {
        reply->deleteLater();
        finish(key);
        if (epoch != epoch_) { settle(key, true, {}); return; }
        KeyScope scope(this, key);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
            const QString message = tr("%1 failed for %2: %3")
                                        .arg(healthCheck ? tr("Health check") : tr("Update"), name,
                                             reply->errorString());
            emit errorOccurred(message);
            settle(key, false, message);
            return;
        }
        emit operationFinished(healthCheck ? tr("Health check completed for %1.").arg(name)
                                            : tr("Updated %1.").arg(name));
        settle(key, false, {});
        if (rules) client_->fetchRules();
        else client_->fetchProxies();
        fetch(rules);
    });
    return key;
}

}  // namespace core
