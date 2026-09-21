#include "ui/pages/overview/home_page.h"

#include <algorithm>
#include <QComboBox>
#include "ui/widgets/combo_box.h"
#include <QFormLayout>
#include <QScrollArea>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/mihomo/mihomo_client.h"
#include "ui/theme/formatting.h"
#include "ui/theme/theme.h"
#include "ui/widgets/settings_section.h"
#include "ui/pages/overview/traffic_graph.h"

namespace ui {
namespace {

QLabel *valueLabel(QWidget *parent, const QString &text = QStringLiteral("—")) {
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}
} // namespace

HomePage::HomePage(core::MihomoClient *client, QWidget *parent) : QWidget(parent) {
    auto *body = new QWidget;
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, theme::kPageSpacing, 0);
    layout->setSpacing(theme::kPageSpacing);
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("settingsScroll");
    scroll->setWidget(body);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    theme::pageLayout(this)->addWidget(scroll);
    auto *heading = new QLabel(tr("Overview"), this);
    QFont headingFont = heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() + 4);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    layout->addWidget(heading);

    auto *overview = new SettingsSection(tr("Controller"), {}, body);
    auto *overviewLayout = new QFormLayout;
    overviewLayout->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    overviewLayout->setLabelAlignment(Qt::AlignLeft);
    overviewLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    overview->addLayout(overviewLayout);
    auto *status = valueLabel(overview, client->isConnected() ? tr("Connected") : tr("Disconnected"));
    auto *endpoint = valueLabel(overview, client->endpoint().httpBase());
    auto *version = valueLabel(overview);
    auto *mode = valueLabel(overview);
    auto *ports = valueLabel(overview);
    auto *memory = valueLabel(overview);
    overviewLayout->addRow(tr("Status"), status);
    overviewLayout->addRow(tr("Address"), endpoint);
    overviewLayout->addRow(tr("Core version"), version);
    overviewLayout->addRow(tr("Routing mode"), mode);
    overviewLayout->addRow(tr("Proxy ports"), ports);
    overviewLayout->addRow(tr("Memory"), memory);
    layout->addWidget(overview);

    auto *traffic = new SettingsSection(tr("Traffic"), {}, body);
    auto *trafficLayout = new QVBoxLayout;
    traffic->addLayout(trafficLayout);
    auto *rate = valueLabel(traffic, tr("Upload 0 B/s · Download 0 B/s"));
    auto *totals = valueLabel(traffic, tr("0 active connections · uploaded 0 B · downloaded 0 B"));
    rate->setObjectName("trafficRates");
    totals->setObjectName("trafficTotals");
    auto *chart = new TrafficGraph(traffic);
    chart->setObjectName("trafficChart");
    trafficLayout->addWidget(rate);
    trafficLayout->addWidget(chart, 1);
    trafficLayout->addWidget(totals);
    layout->addWidget(traffic);

    auto *diagnostics = new SettingsSection(tr("DNS diagnostics"), {}, body);
    auto *dnsLayout = new QVBoxLayout;
    diagnostics->addLayout(dnsLayout);
    auto *controls = new QHBoxLayout;
    auto *domain = new QLineEdit(diagnostics);
    domain->setPlaceholderText(tr("Domain to resolve, e.g. example.com"));
    domain->setClearButtonEnabled(true);
    auto *type = new ComboBox(diagnostics);
    type->addItems({"A", "AAAA", "CNAME", "MX", "TXT", "NS"});
    auto *query = new QPushButton(tr("Query"), diagnostics);
    query->setEnabled(client->isConnected());
    controls->addWidget(domain, 1);
    controls->addWidget(type);
    controls->addWidget(query);
    dnsLayout->addLayout(controls);
    auto *result = new QPlainTextEdit(diagnostics);
    result->setObjectName("dnsResult");
    result->setReadOnly(true);
    result->setPlaceholderText(tr("Queries use the connected core’s DNS configuration."));
    result->setMaximumBlockCount(1000);
    result->setFixedHeight(110);
    dnsLayout->addWidget(result, 1);
    auto *cacheControls = new QHBoxLayout;
    auto *cacheStatus = new QLabel(diagnostics);
    cacheStatus->setWordWrap(true);
    auto *flush = new QPushButton(tr("Clear DNS Cache"), diagnostics);
    auto *flushFake = new QPushButton(tr("Clear Fake-IP Cache"), diagnostics);
    flush->setObjectName("flushDnsButton");
    flushFake->setObjectName("flushFakeIpButton");
    flush->setEnabled(client->isConnected());
    flushFake->setEnabled(client->isConnected());
    cacheControls->addWidget(cacheStatus, 1);
    cacheControls->addWidget(flush);
    cacheControls->addWidget(flushFake);
    dnsLayout->addLayout(cacheControls);
    layout->addWidget(diagnostics);
    layout->addStretch(1);

    connect(client, &core::MihomoClient::versionReceived, this, [version](const QString &v) {
        version->setText(v);
    });
    connect(client, &core::MihomoClient::configReceived, this,
            [mode, ports](const core::BaseConfig &config) {
        mode->setText(config.mode);
        ports->setText(tr("Mixed %1 · HTTP %2 · SOCKS %3").arg(config.mixedPort).arg(config.httpPort).arg(config.socksPort));
    });
    connect(client, &core::MihomoClient::memorySample, this, [memory](quint64 inuse, quint64) {
        memory->setText(formatBytes(inuse));
    });
    connect(client, &core::MihomoClient::trafficSample, this, [rate, chart](quint64 up, quint64 down) {
        rate->setText(tr("Upload %1 · Download %2").arg(formatRate(up), formatRate(down)));
        chart->append(up, down);
    });
    connect(client, &core::MihomoClient::connectionsUpdated, this,
            [totals](const QVector<core::Connection> &connections, quint64 up, quint64 down) {
        totals->setText(tr("%1 active connections · uploaded %2 · downloaded %3")
                           .arg(connections.size()).arg(formatBytes(up), formatBytes(down)));
    });
    connect(client, &core::MihomoClient::connectedChanged, this,
            [=](bool connected) {
        status->setText(connected ? tr("Connected") : tr("Disconnected"));
        query->setEnabled(connected);
        flush->setEnabled(connected);
        flushFake->setEnabled(connected);
        if (!connected) { version->setText("—"); mode->setText("—"); ports->setText("—"); chart->clear(); }
    });
    connect(client, &core::MihomoClient::endpointChanged, this, [=] {
        endpoint->setText(client->endpoint().httpBase());
        result->clear();
        cacheStatus->clear();
        chart->clear();
    });
    const auto queryDns = [=] {
        if (!query->isEnabled() || domain->text().trimmed().isEmpty()) return;
        query->setEnabled(false);
        result->setPlainText(tr("Resolving %1…").arg(domain->text().trimmed()));
        client->queryDns(domain->text().trimmed(), type->currentText());
    };
    connect(query, &QPushButton::clicked, this, queryDns);
    connect(domain, &QLineEdit::returnPressed, this, queryDns);
    connect(client, &core::MihomoClient::dnsQueryFinished, this,
            [=](const QString &name, const QJsonObject &response, const QString &error) {
        query->setEnabled(client->isConnected());
        if (!error.isEmpty()) { result->setPlainText(error); return; }
        QStringList lines{tr("%1 · DNS status %2").arg(name).arg(response.value("Status").toInt())};
        for (const QString section : {QString("Answer"), QString("Authority"), QString("Additional")}) {
            const auto records = response.value(section).toArray();
            if (records.isEmpty()) continue;
            lines << section + ":";
            for (const auto &record : records) {
                const auto entry = record.toObject();
                lines << QString("%1  TTL %2  %3").arg(entry.value("name").toString())
                             .arg(entry.value("TTL").toInt()).arg(entry.value("data").toString());
            }
        }
        if (lines.size() == 1) lines << tr("No records returned.");
        result->setPlainText(lines.join('\n'));
    });
    connect(flush, &QPushButton::clicked, this, [=] {
        flush->setEnabled(false);
        cacheStatus->setText(tr("Clearing DNS cache…"));
        client->flushDnsCache();
    });
    connect(flushFake, &QPushButton::clicked, this, [=] {
        flushFake->setEnabled(false);
        cacheStatus->setText(tr("Clearing Fake-IP cache…"));
        client->flushDnsCache(true);
    });
    connect(client, &core::MihomoClient::dnsCacheFlushed, this, [=](bool fakeIp, const QString &error) {
        (fakeIp ? flushFake : flush)->setEnabled(client->isConnected());
        cacheStatus->setText(error.isEmpty() ? (fakeIp ? tr("Fake-IP cache cleared") : tr("DNS cache cleared")) : error);
    });
}

} // namespace ui
