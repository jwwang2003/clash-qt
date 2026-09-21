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
#include <QUrl>
#include <QVBoxLayout>

#include "core/backend/backend_bridge.h"
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

/// The Controller card's Address row.
///
/// backend::Endpoint is deliberately behaviour-free -- core/backend/types.h
/// puts URL construction on whichever side owns the transport -- so
/// core::Endpoint::httpBase() has no equivalent there. This is that
/// construction, on the consumer side, and it is identical to what
/// core::Endpoint::baseUrl("http") produced for every real endpoint, IPv6
/// brackets included.
///
/// It differs in one place, deliberately: BEFORE anything is attached.
/// core::Endpoint defaulted to 127.0.0.1:9090, so this row used to claim an
/// address the application was not talking to; backend::Endpoint defaults to no
/// host and no port, which would render as "http://:0". Neither is an address,
/// so an invalid endpoint reads as the same em dash every other not-yet-known
/// field in this form starts at.
QString addressText(const core::backend::Endpoint &endpoint) {
    if (!core::backend::isValid(endpoint)) return QStringLiteral("—");
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    QString address = endpoint.host.trimmed();
    if (address.startsWith('[') && address.endsWith(']'))
        address = address.mid(1, address.size() - 2);
    url.setHost(address);
    url.setPort(endpoint.port);
    return url.toString(QUrl::FullyEncoded);
}
} // namespace

HomePage::HomePage(core::backend::BackendBridge *bridge, QWidget *parent) : QWidget(parent) {
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
    auto *status = valueLabel(overview, bridge->isConnected() ? tr("Connected") : tr("Disconnected"));
    auto *endpoint = valueLabel(overview, addressText(bridge->endpoint()));
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
    query->setEnabled(bridge->isConnected());
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
    flush->setEnabled(bridge->isConnected());
    flushFake->setEnabled(bridge->isConnected());
    cacheControls->addWidget(cacheStatus, 1);
    cacheControls->addWidget(flush);
    cacheControls->addWidget(flushFake);
    dnsLayout->addLayout(cacheControls);
    layout->addWidget(diagnostics);
    layout->addStretch(1);

    connect(bridge, &core::backend::BackendBridge::versionReceived, this, [version](const QString &v) {
        version->setText(v);
    });
    connect(bridge, &core::backend::BackendBridge::configReceived, this,
            [mode, ports](const core::backend::BaseConfig &config) {
        mode->setText(config.mode);
        ports->setText(tr("Mixed %1 · HTTP %2 · SOCKS %3").arg(config.mixedPort).arg(config.httpPort).arg(config.socksPort));
    });
    connect(bridge, &core::backend::BackendBridge::memorySample, this, [memory](quint64 inuse, quint64) {
        memory->setText(formatBytes(inuse));
    });
    connect(bridge, &core::backend::BackendBridge::trafficSample, this, [rate, chart](quint64 up, quint64 down) {
        rate->setText(tr("Upload %1 · Download %2").arg(formatRate(up), formatRate(down)));
        chart->append(up, down);
    });
    connect(bridge, &core::backend::BackendBridge::connectionsUpdated, this,
            [totals](const QVector<core::backend::Connection> &connections, quint64 up, quint64 down) {
        totals->setText(tr("%1 active connections · uploaded %2 · downloaded %3")
                           .arg(connections.size()).arg(formatBytes(up), formatBytes(down)));
    });
    connect(bridge, &core::backend::BackendBridge::connectedChanged, this,
            [=](bool connected) {
        status->setText(connected ? tr("Connected") : tr("Disconnected"));
        query->setEnabled(connected);
        flush->setEnabled(connected);
        flushFake->setEnabled(connected);
        if (!connected) { version->setText("—"); mode->setText("—"); ports->setText("—"); chart->clear(); }
    });
    connect(bridge, &core::backend::BackendBridge::endpointChanged, this, [=] {
        endpoint->setText(addressText(bridge->endpoint()));
        result->clear();
        cacheStatus->clear();
        chart->clear();
    });
    const auto queryDns = [=] {
        if (!query->isEnabled() || domain->text().trimmed().isEmpty()) return;
        query->setEnabled(false);
        result->setPlainText(tr("Resolving %1…").arg(domain->text().trimmed()));
        bridge->queryDns(domain->text().trimmed(), type->currentText());
    };
    connect(query, &QPushButton::clicked, this, queryDns);
    connect(domain, &QLineEdit::returnPressed, this, queryDns);
    connect(bridge, &core::backend::BackendBridge::dnsQueryFinished, this,
            [=](const QString &name, const QJsonObject &response, const QString &error) {
        query->setEnabled(bridge->isConnected());
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
        bridge->flushDnsCache(false);
    });
    connect(flushFake, &QPushButton::clicked, this, [=] {
        flushFake->setEnabled(false);
        cacheStatus->setText(tr("Clearing Fake-IP cache…"));
        bridge->flushDnsCache(true);
    });
    connect(bridge, &core::backend::BackendBridge::dnsCacheFlushed, this, [=](bool fakeIp, const QString &error) {
        (fakeIp ? flushFake : flush)->setEnabled(bridge->isConnected());
        cacheStatus->setText(error.isEmpty() ? (fakeIp ? tr("Fake-IP cache cleared") : tr("DNS cache cleared")) : error);
    });
}

} // namespace ui
