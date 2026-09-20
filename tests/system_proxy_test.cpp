#include <QtTest>
#include <QMap>
#include <QSet>

// This override exists only in this translation unit. Production builds
// always call the real process runner and expose no injection mechanism.
namespace {
QMap<QString, QString> reports;
QStringList mutations;
QSet<int> rejectedMutations;
bool rejectNextSnapshot = false;
int writes = 0;

bool proxyCommand(const QString &program, const QStringList &args, QString *output) {
    if (program == "/sbin/route") {
        if (output) *output = "route to: default\n interface: en9\n";
        return true;
    }
    if (args.first() == "-listnetworkserviceorder") {
        if (output) *output = "(1) Wi-Fi\n(Hardware Port: Wi-Fi, Device: en0)\n"
                              "(2) Ethernet\n(Hardware Port: Ethernet, Device: en9)";
        return true;
    }
    if (args.size() < 2 || args[1] != "Ethernet") return false;
    const QString command = args.first();
    if (command.startsWith("-get")) {
        if (rejectNextSnapshot && writes >= 6) {
            rejectNextSnapshot = false;
            return false;
        }
        if (!reports.contains(command)) return false;
        if (output) *output = reports.value(command);
        return true;
    }
    mutations.append(args.join('|'));
    ++writes;
    if (rejectedMutations.contains(writes)) return false;
    const QMap<QString, QString> proxySetters{
        {"-setwebproxy", "-getwebproxy"}, {"-setsecurewebproxy", "-getsecurewebproxy"},
        {"-setsocksfirewallproxy", "-getsocksfirewallproxy"}};
    const QMap<QString, QString> stateSetters{
        {"-setwebproxystate", "-getwebproxy"}, {"-setsecurewebproxystate", "-getsecurewebproxy"},
        {"-setsocksfirewallproxystate", "-getsocksfirewallproxy"}, {"-setautoproxystate", "-getautoproxyurl"}};
    if (proxySetters.contains(command)) {
        if (args[2].isEmpty() || args[3].toUShort() == 0) return false;
        reports[proxySetters.value(command)] = QString("Enabled: Yes\nServer: %1\nPort: %2\nAuthenticated Proxy Enabled: 0")
                                                   .arg(args[2], args[3]);
    } else if (stateSetters.contains(command)) {
        QString &report = reports[stateSetters.value(command)];
        report.replace("Enabled: Yes", "Enabled: No");
        if (args[2] == "on") report.replace("Enabled: No", "Enabled: Yes");
    } else if (command == "-setproxybypassdomains") {
        reports["-getproxybypassdomains"] = args[2] == "Empty"
            ? "There aren't any bypass domains set on Ethernet."
            : args.mid(2).join('\n');
    } else if (command == "-setproxyautodiscovery") {
        reports["-getproxyautodiscovery"] = args[2] == "on" ? "Auto Proxy Discovery: On" : "Auto Proxy Discovery: Off";
    } else return false;
    return true;
}
}
#define CLASH_QT_PROXY_TEST_RUNNER proxyCommand
#include "../src/platform/proxy/system_proxy.cpp"

class SystemProxyTest : public QObject {
    Q_OBJECT
private slots:
    void init() {
#ifndef Q_OS_MACOS
        QSKIP("This suite simulates the macOS networksetup backend");
#endif
        reports = {
            {"-getwebproxy", "Enabled: No\nServer: old-http\nPort: 80\nAuthenticated Proxy Enabled: 0"},
            {"-getsecurewebproxy", "Enabled: Yes\nServer: old-https\nPort: 443\nAuthenticated Proxy Enabled: 0"},
            {"-getsocksfirewallproxy", "Enabled: No\nServer: old-socks\nPort: 1080\nAuthenticated Proxy Enabled: 0"},
            {"-getproxybypassdomains", "localhost\n*.example.org"},
            {"-getautoproxyurl", "URL: https://example.org/proxy.pac\nEnabled: Yes"},
            {"-getproxyautodiscovery", "Auto Proxy Discovery: On"}
        };
        writes = 0;
        mutations.clear();
        rejectedMutations.clear();
        rejectNextSnapshot = false;
        platform::originalSettings = {};
        platform::ownedSettings = {};
    }
    void restoresAllOriginalSettings() {
        const auto before = reports;
        QVERIFY(platform::SystemProxy::enable({"127.0.0.1", 7890, {}}));
        QVERIFY(platform::SystemProxy::ownsProxy());
        QVERIFY(reports["-getautoproxyurl"].endsWith("Enabled: No"));
        QVERIFY(mutations.contains("-setproxybypassdomains|Ethernet|Empty"));
        QVERIFY(platform::SystemProxy::restoreOwned());
        QCOMPARE(reports, before);
    }
    void restoresFreshMachineToDisabledProxies() {
        for (const QString &getter : {QString("-getwebproxy"), QString("-getsecurewebproxy"),
                                      QString("-getsocksfirewallproxy")}) {
            reports[getter] = "Enabled: No\nServer: \nPort: 0\nAuthenticated Proxy Enabled: 0";
        }
        QVERIFY(platform::SystemProxy::enable({"127.0.0.1", 7890, "localhost", 7890}));
        QVERIFY(platform::SystemProxy::restoreOwned());
        QVERIFY(reports["-getwebproxy"].startsWith("Enabled: No"));
        QVERIFY(reports["-getsecurewebproxy"].startsWith("Enabled: No"));
        QVERIFY(reports["-getsocksfirewallproxy"].startsWith("Enabled: No"));
        QVERIFY(!platform::SystemProxy::isEnabled());
    }
    void partialEnableRollsBack() {
        const auto before = reports;
        rejectedMutations = {2};
        QVERIFY(!platform::SystemProxy::enable({"127.0.0.1", 7890, "localhost"}));
        QCOMPARE(reports, before);
        QVERIFY(!platform::SystemProxy::ownsProxy());
    }
    void externalChangesAreNotOverwritten() {
        QVERIFY(platform::SystemProxy::enable({"127.0.0.1", 7890, "localhost"}));
        reports["-getsecurewebproxy"].replace("127.0.0.1", "another-app");
        const auto external = reports;
        const int priorWrites = writes;
        QVERIFY(platform::SystemProxy::restoreOwned());
        QCOMPARE(writes, priorWrites);
        QCOMPARE(reports, external);
    }
    void failedRestoreCanBeRetried() {
        const auto before = reports;
        QVERIFY(platform::SystemProxy::enable({"127.0.0.1", 7890, "localhost"}));
        rejectedMutations.insert(writes + 3);
        QVERIFY(!platform::SystemProxy::restoreOwned());
        QVERIFY(platform::SystemProxy::restoreOwned());
        QCOMPARE(reports, before);
    }
    void unreadablePostEnableStateRollsBack() {
        const auto before = reports;
        rejectNextSnapshot = true;
        QVERIFY(!platform::SystemProxy::enable({"127.0.0.1", 7890, "localhost"}));
        QCOMPARE(reports, before);
    }
    void reenableKeepsOriginalSnapshot() {
        const auto before = reports;
        QVERIFY(platform::SystemProxy::enable({"127.0.0.1", 7890, "localhost"}));
        QVERIFY(platform::SystemProxy::enable({"127.0.0.1", 7891, "localhost"}));
        QVERIFY(platform::SystemProxy::restoreOwned());
        QCOMPARE(reports, before);
    }
    void httpOnlyDoesNotPointSocksAtHttpPort() {
        QVERIFY(platform::SystemProxy::enable({"127.0.0.1", 7890, "localhost", 0}));
        QVERIFY(reports["-getsocksfirewallproxy"].startsWith("Enabled: No"));
        QVERIFY(platform::SystemProxy::enable({"127.0.0.1", 7890, "localhost", 10808}));
        QVERIFY(reports["-getsocksfirewallproxy"].contains("Port: 10808"));
        QVERIFY(reports["-getsocksfirewallproxy"].startsWith("Enabled: Yes"));
    }
    void authenticatedProxyIsNotModified() {
        reports["-getwebproxy"].replace("Authenticated Proxy Enabled: 0", "Authenticated Proxy Enabled: 1");
        const auto before = reports;
        QVERIFY(!platform::SystemProxy::enable({"127.0.0.1", 7890, "localhost"}));
        QCOMPARE(writes, 0);
        QCOMPARE(reports, before);
    }
};
QTEST_GUILESS_MAIN(SystemProxyTest)
#include "system_proxy_test.moc"
