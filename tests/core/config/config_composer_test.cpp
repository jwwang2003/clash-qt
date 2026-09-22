// core::config::compose -- the precedence stack, the preset operations, the
// fields the application refuses to give up, and the promise that none of it
// touches a disk.
//
// Every case here runs without a ProfileStore, without an event loop and
// without a data directory, which is the whole reason the composition was
// pulled out of ProfileStore::buildRuntime(). Contract: config-r1.
#include <QtTest>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <yaml-cpp/yaml.h>

#include "core/config/config_composer.h"

using core::config::ComposeInput;
using core::config::ComposeResult;
using core::config::Diagnostic;
using core::config::PresetDocument;
using core::config::Provenance;

namespace {

QJsonObject op(const QString &kind, const QString &path,
               const QJsonValue &value = QJsonValue(QJsonValue::Undefined)) {
    QJsonObject encoded{{"op", kind}, {"path", path}};
    if (!value.isUndefined()) encoded.insert("value", value);
    return encoded;
}

QJsonObject preset(const QString &id, const QJsonArray &operations, bool enabled = true) {
    return QJsonObject{
        {"id", id}, {"name", id}, {"enabled", enabled}, {"operations", operations}};
}

QJsonObject document(const QJsonArray &global, const QJsonObject &profiles = {}) {
    return QJsonObject{{"version", 1}, {"global", global}, {"profiles", profiles}};
}

core::config::ControllerFields controller() {
    core::config::ControllerFields fields;
    fields.externalController = "127.0.0.1:29097";
    fields.secret = "the-real-secret";
    fields.mixedPort = 27890;
    fields.externalUi = "/data/ui";
    fields.externalUiUrl = "https://example.invalid/dist.zip";
    fields.storeSelected = true;
    return fields;
}

bool hasSeverity(const QVector<Diagnostic> &diagnostics, const QString &severity) {
    for (const Diagnostic &diagnostic : diagnostics)
        if (diagnostic.severity == severity) return true;
    return false;
}

QStringList messages(const QVector<Diagnostic> &diagnostics) {
    QStringList all;
    for (const Diagnostic &diagnostic : diagnostics)
        all.append(diagnostic.severity + '|' + diagnostic.source + '|' + diagnostic.path + '|' +
                   diagnostic.message);
    return all;
}

// An operation built WITHOUT going through parsePresetDocument().
//
// The parser refuses several shapes outright (see preset-document), so a
// fixture that goes through it cannot reach the composer's own defences at
// all -- it just yields an empty document, and a test written that way passes
// while asserting nothing. This is the second line of defence's only route in:
// the shape a document that arrived on disk some other way would have.
core::config::Operation raw(core::config::OperationKind kind, const QString &path,
                            const QJsonValue &value = QJsonValue(QJsonValue::Undefined)) {
    core::config::Operation operation;
    operation.kind = kind;
    operation.path = path;
    operation.value = value;
    QString error;
    core::config::decodePointer(path, &operation.tokens, &error);
    return operation;
}

QString provenanceOf(const QVector<Provenance> &provenance, const QString &path) {
    QString source;  // the LAST writer wins, which is what provenance means here
    for (const Provenance &entry : provenance)
        if (entry.path == path) source = entry.source;
    return source;
}

int provenanceCount(const QVector<Provenance> &provenance, const QString &path) {
    int seen = 0;
    for (const Provenance &entry : provenance)
        if (entry.path == path) ++seen;
    return seen;
}

// A `/nest` subtree `levels` maps deep, and the merge fragment that lines up
// with it key for key. The pair matters: a deep merge only recurses where BOTH
// sides are maps, so this is what it takes to reach the recursion guard at all.
QString nestedYaml(int levels) {
    QString yaml = QStringLiteral("nest:\n");
    QString indent = QStringLiteral("  ");
    for (int i = 0; i < levels; ++i) {
        yaml += indent + QStringLiteral("k:\n");
        indent += QStringLiteral("  ");
    }
    return yaml + indent + QStringLiteral("marker: shallow\n");
}

QJsonValue nestedFragment(int levels) {
    QJsonObject value{{"marker", "deep"}};
    for (int i = 0; i < levels; ++i) value = QJsonObject{{"k", value}};
    return value;
}

// The value at the bottom of `levels` nested "k" keys.
QString markerAt(const YAML::Node &root, int levels) {
    YAML::Node node = root["nest"];
    for (int i = 0; i < levels; ++i) {
        if (!node.IsMap()) return QStringLiteral("(missing at level %1)").arg(i);
        node = node["k"];
    }
    if (!node.IsMap() || !node["marker"].IsDefined()) return QStringLiteral("(no marker)");
    return QString::fromStdString(node["marker"].as<std::string>());
}

bool mentionsDepth(const QVector<Diagnostic> &diagnostics) {
    for (const Diagnostic &diagnostic : diagnostics)
        if (diagnostic.severity == QLatin1String("error") &&
            diagnostic.message.contains(QLatin1String("deep"), Qt::CaseInsensitive))
            return true;
    return false;
}

}  // namespace

class ConfigComposerTest : public QObject {
    Q_OBJECT

    // Composes `yaml` with the presets in `presets` selected for `uid`.
    static ComposeResult run(const QString &yaml, const QJsonObject &presets = {},
                             const QString &uid = QStringLiteral("p1"),
                             const QJsonObject &overrides = {}) {
        ComposeInput input;
        input.sourceYaml = yaml;
        input.profileUid = uid;
        input.profileName = QStringLiteral("fixture");
        input.overrides = overrides;
        input.controller = controller();
        if (!presets.isEmpty()) {
            QVector<Diagnostic> diagnostics;
            // Loud on purpose. A silently rejected fixture leaves an EMPTY
            // preset document behind, and every assertion below it then passes
            // for the wrong reason.
            if (!core::config::parsePresetDocument(presets, &input.presets, &diagnostics))
                qFatal("fixture preset document rejected: %s",
                       qPrintable(messages(diagnostics).join(QLatin1Char(';'))));
        }
        return core::config::compose(input);
    }

    // Composes against a preset document assembled by hand, with no validation
    // in between.
    static ComposeResult runRaw(const QString &yaml, const PresetDocument &presets,
                                const QString &uid = QStringLiteral("p1")) {
        ComposeInput input;
        input.sourceYaml = yaml;
        input.profileUid = uid;
        input.profileName = QStringLiteral("fixture");
        input.controller = controller();
        input.presets = presets;
        return core::config::compose(input);
    }

    static YAML::Node yamlOf(const ComposeResult &result) {
        return YAML::Load(result.yaml.toStdString());
    }

private slots:

    // ------------------------------------------------------------ precedence

    // The stack, end to end, on one key per layer plus one key every layer
    // writes. The ordering claim is only worth anything if a single key is
    // contested all the way up.
    void everyLayerOverridesTheOneBelowIt() {
        const QJsonObject presets = document(
            QJsonArray{preset("g1", QJsonArray{op("replace", "/mode", "global"),
                                               op("replace", "/contested", "from-global")}),
                       preset("g2", QJsonArray{op("replace", "/contested", "from-global-2")})},
            QJsonObject{{"p1", QJsonArray{preset(
                                   "pr1", QJsonArray{op("replace", "/contested", "from-profile"),
                                                     op("replace", "/only-profile", true)})}}});

        const ComposeResult result =
            run("proxies: []\nmode: rule\ncontested: from-source\nonly-source: 1\n", presets,
                "p1", QJsonObject{{"contested", "from-override"}, {"only-override", 7}});
        QVERIFY2(result.ok, qPrintable(messages(result.diagnostics).join('\n')));

        const YAML::Node config = yamlOf(result);
        QCOMPARE(config["only-source"].as<int>(), 1);
        QCOMPARE(config["mode"].as<std::string>(), std::string("global"));
        QVERIFY(config["only-profile"].as<bool>());
        QCOMPARE(config["only-override"].as<int>(), 7);
        // source < global[0] < global[1] < profile < override
        QCOMPARE(config["contested"].as<std::string>(), std::string("from-override"));
        QCOMPARE(provenanceOf(result.provenance, "/contested"), QStringLiteral("override"));
        QCOMPARE(provenanceOf(result.provenance, "/mode"), QStringLiteral("global:g1"));
        QCOMPARE(provenanceOf(result.provenance, "/only-profile"), QStringLiteral("profile:pr1"));
    }

    // Array order within one layer, isolated from the layer ordering above.
    void presetsApplyInArrayOrderNotIdOrder() {
        const QJsonObject presets =
            document(QJsonArray{preset("zz", QJsonArray{op("replace", "/mode", "first")}),
                                preset("aa", QJsonArray{op("replace", "/mode", "second")})});
        QCOMPARE(yamlOf(run("proxies: []\n", presets))["mode"].as<std::string>(),
                 std::string("second"));
    }

    void disabledPresetsDoNothingAtAll() {
        const QJsonObject presets = document(QJsonArray{
            preset("off", QJsonArray{op("replace", "/mode", "global")}, /*enabled=*/false)});
        const ComposeResult result = run("proxies: []\nmode: rule\n", presets);
        QCOMPARE(yamlOf(result)["mode"].as<std::string>(), std::string("rule"));
        QVERIFY(provenanceOf(result.provenance, "/mode").isEmpty());
    }

    void onlyTheSelectedProfilesPresetsApply() {
        const QJsonObject presets = document(
            {}, QJsonObject{{"mine", QJsonArray{preset("a", QJsonArray{op("replace", "/x", 1)})}},
                            {"theirs", QJsonArray{preset("b", QJsonArray{op("replace", "/y", 2)})}}});
        const YAML::Node config = yamlOf(run("proxies: []\n", presets, "mine"));
        QCOMPARE(config["x"].as<int>(), 1);
        QVERIFY(!config["y"]);
    }

    // ------------------------------------------------------------ operations

    void mergeIsDeepForMapsAndReplacingForSequences() {
        const QJsonObject presets = document(QJsonArray{preset(
            "m", QJsonArray{op("merge", "/dns",
                               QJsonObject{{"enable", true},
                                           {"nameserver", QJsonArray{"1.1.1.1"}},
                                           {"nested", QJsonObject{{"added", 1}}}})})});
        const YAML::Node config = yamlOf(run(
            "proxies: []\ndns:\n  enable: false\n  listen: '0.0.0.0:53'\n"
            "  nameserver: [8.8.8.8, 9.9.9.9]\n  nested:\n    kept: 2\n",
            presets));
        QVERIFY(config["dns"]["enable"].as<bool>());              // replaced
        QCOMPARE(config["dns"]["listen"].as<std::string>(), std::string("0.0.0.0:53"));  // kept
        QCOMPARE(config["dns"]["nameserver"].size(), size_t(1));  // sequences replace whole
        QCOMPARE(config["dns"]["nameserver"][0].as<std::string>(), std::string("1.1.1.1"));
        QCOMPARE(config["dns"]["nested"]["kept"].as<int>(), 2);   // deep, not shallow
        QCOMPARE(config["dns"]["nested"]["added"].as<int>(), 1);
    }

    void replaceReplacesTheWholeValue() {
        const QJsonObject presets = document(QJsonArray{
            preset("r", QJsonArray{op("replace", "/dns", QJsonObject{{"enable", true}})})});
        const YAML::Node config =
            yamlOf(run("proxies: []\ndns:\n  enable: false\n  listen: x\n", presets));
        QVERIFY(config["dns"]["enable"].as<bool>());
        QVERIFY(!config["dns"]["listen"]);
    }

    void removeDropsTheKeyAndIsIdempotentWhenItIsAbsent() {
        const QJsonObject presets = document(QJsonArray{
            preset("d", QJsonArray{op("remove", "/dns/listen"), op("remove", "/dns/never-there"),
                                   op("remove", "/absent-parent/child")})});
        const ComposeResult result = run("proxies: []\ndns:\n  enable: true\n  listen: x\n", presets);
        QVERIFY2(result.ok, qPrintable(messages(result.diagnostics).join('\n')));
        // Removing what is not there is not an error and is not a diagnostic.
        QVERIFY2(result.diagnostics.isEmpty(), qPrintable(messages(result.diagnostics).join('\n')));
        const YAML::Node config = yamlOf(result);
        QVERIFY(!config["dns"]["listen"]);
        QVERIFY(config["dns"]["enable"].as<bool>());
    }

    void prependAndAppendPutRulesAtTheRightEnd() {
        const QJsonObject presets = document(QJsonArray{
            preset("p", QJsonArray{op("prepend", "/rules", QJsonArray{"A", "B"}),
                                   op("append", "/rules", QJsonArray{"Y", "Z"})})});
        const ComposeResult result = run("proxies: []\nrules:\n  - M\n", presets);
        QVERIFY2(result.ok, qPrintable(messages(result.diagnostics).join('\n')));
        const YAML::Node rules = yamlOf(result)["rules"];
        QCOMPARE(rules.size(), size_t(5));
        const char *expected[] = {"A", "B", "M", "Y", "Z"};
        for (size_t i = 0; i < 5; ++i)
            QCOMPARE(rules[i].as<std::string>(), std::string(expected[i]));
    }

    void appendCreatesTheRuleListWhenTheProfileHasNone() {
        const QJsonObject presets = document(
            QJsonArray{preset("p", QJsonArray{op("append", "/rules", QJsonArray{"MATCH,DIRECT"})})});
        const YAML::Node rules = yamlOf(run("proxies: []\n", presets))["rules"];
        QCOMPARE(rules.size(), size_t(1));
        QCOMPARE(rules[0].as<std::string>(), std::string("MATCH,DIRECT"));
    }

    void missingIntermediateMapsAreCreatedForWrites() {
        const QJsonObject presets = document(
            QJsonArray{preset("n", QJsonArray{op("replace", "/sniffer/sniff/HTTP/ports",
                                                 QJsonArray{80, 8080})})});
        const ComposeResult result = run("proxies: []\n", presets);
        QVERIFY2(result.ok, qPrintable(messages(result.diagnostics).join('\n')));
        QCOMPARE(yamlOf(result)["sniffer"]["sniff"]["HTTP"]["ports"].size(), size_t(2));
    }

    void rfc6901EscapesAreDecodedNotTakenLiterally() {
        const QJsonObject presets = document(QJsonArray{
            preset("e", QJsonArray{op("replace", "/rule-providers/ads~1blocked/url", "u"),
                                   op("replace", "/weird~0key", 1)})});
        const ComposeResult result = run("proxies: []\n", presets);
        QVERIFY2(result.ok, qPrintable(messages(result.diagnostics).join('\n')));
        const YAML::Node config = yamlOf(result);
        QCOMPARE(config["rule-providers"]["ads/blocked"]["url"].as<std::string>(), std::string("u"));
        QCOMPARE(config["weird~key"].as<int>(), 1);
    }

    // -------------------------------------------- meaningful "empty" values

    // The bug this forbids: treating false, 0 and "" as "nothing was set" and
    // silently keeping the profile's value. Every one of these is a decision.
    void explicitFalseZeroAndEmptyAreValuesNotAbsences() {
        const QJsonObject presets = document(QJsonArray{preset(
            "z", QJsonArray{op("replace", "/ipv6", false), op("replace", "/count", 0),
                            op("replace", "/label", ""), op("replace", "/list", QJsonArray{}),
                            op("merge", "/dns", QJsonObject{{"enable", false}})})});
        const ComposeResult result =
            run("proxies: []\nipv6: true\ncount: 9\nlabel: something\nlist: [a, b]\n"
                "dns:\n  enable: true\n",
                presets);
        QVERIFY2(result.ok, qPrintable(messages(result.diagnostics).join('\n')));
        const YAML::Node config = yamlOf(result);
        QVERIFY(config["ipv6"].IsDefined());
        QCOMPARE(config["ipv6"].as<bool>(), false);
        QCOMPARE(config["count"].as<int>(), 0);
        QCOMPARE(config["label"].as<std::string>(), std::string(""));
        QVERIFY(config["list"].IsSequence());
        QCOMPARE(config["list"].size(), size_t(0));
        QCOMPARE(config["dns"]["enable"].as<bool>(), false);
        // And the empty dns.enable is honoured downstream: no hijack default.
        QVERIFY(!config["tun"]["dns-hijack"]);
    }

    // A string that looks like a number must stay a string, or mihomo refuses
    // to unmarshal the document. The preset path gets this from the same
    // mechanism the override path always had.
    void stringValuesThatLookNumericKeepTheirStringTag() {
        const QJsonObject presets = document(QJsonArray{
            preset("s", QJsonArray{op("replace", "/password", "0123"),
                                   op("replace", "/hex", "0xFF"), op("replace", "/yes", "true")})});
        const YAML::Node config = yamlOf(run("proxies: []\n", presets));
        QCOMPARE(config["password"].Tag(), std::string("!"));
        QCOMPARE(config["hex"].Tag(), std::string("!"));
        QCOMPARE(config["yes"].Tag(), std::string("!"));
        QCOMPARE(config["password"].as<std::string>(), std::string("0123"));
    }

    // -------------------------------------------------- the protected fields

    void noPresetCanSelectItsOwnControllerSecretOrUiDirectory() {
        using core::config::OperationKind;
        core::config::Preset evil;
        evil.id = evil.name = QStringLiteral("evil");
        evil.operations = {
            raw(OperationKind::Replace, "/external-controller", "0.0.0.0:9999"),
            raw(OperationKind::Replace, "/secret", "attacker"),
            raw(OperationKind::Replace, "/external-ui", "/tmp/evil"),
            raw(OperationKind::Replace, "/external-ui-url", "http://evil.invalid/x.zip"),
            raw(OperationKind::Replace, "/mixed-port", 1),
            raw(OperationKind::Replace, "/profile/store-selected", false),
            raw(OperationKind::Replace, "/port", 1080),
            raw(OperationKind::Replace, "/socks-port", 1081)};
        PresetDocument presets;
        presets.global = {evil};
        const ComposeResult result = runRaw("proxies: []\n", presets);
        QVERIFY2(result.ok, qPrintable(messages(result.diagnostics).join('\n')));

        const YAML::Node config = yamlOf(result);
        QCOMPARE(config["external-controller"].as<std::string>(), std::string("127.0.0.1:29097"));
        QCOMPARE(config["secret"].as<std::string>(), std::string("the-real-secret"));
        QCOMPARE(config["external-ui"].as<std::string>(), std::string("/data/ui"));
        QCOMPARE(config["external-ui-url"].as<std::string>(),
                 std::string("https://example.invalid/dist.zip"));
        QCOMPARE(config["mixed-port"].as<int>(), 27890);
        QVERIFY(config["profile"]["store-selected"].as<bool>());
        QVERIFY(!config["port"]);
        QVERIFY(!config["socks-port"]);

        // Refused out loud, once per attempt, not silently overwritten.
        QCOMPARE(result.diagnostics.size(), 8);
        for (const Diagnostic &diagnostic : result.diagnostics) {
            QCOMPARE(diagnostic.severity, QStringLiteral("error"));
            QCOMPARE(diagnostic.source, QStringLiteral("global:evil"));
        }
        for (const char *path : {"/external-controller", "/secret", "/mixed-port", "/external-ui",
                                 "/external-ui-url", "/profile/store-selected"})
            QCOMPARE(provenanceOf(result.provenance, QLatin1String(path)),
                     QStringLiteral("controller"));
    }

    // A rejected operation must not abort the preset it is in: the other
    // operations are independent decisions the user also made.
    void arefusedOperationDoesNotStopTheOnesAroundIt() {
        using core::config::OperationKind;
        core::config::Preset mixed;
        mixed.id = mixed.name = QStringLiteral("mixed");
        mixed.operations = {raw(OperationKind::Replace, "/before", 1),
                            raw(OperationKind::Replace, "/secret", "attacker"),
                            raw(OperationKind::Replace, "/after", 2)};
        PresetDocument presets;
        presets.global = {mixed};
        const ComposeResult result = runRaw("proxies: []\n", presets);
        QVERIFY(result.ok);
        const YAML::Node config = yamlOf(result);
        QCOMPARE(config["before"].as<int>(), 1);
        QCOMPARE(config["after"].as<int>(), 2);
        QCOMPARE(result.diagnostics.size(), 1);
    }

    void anOverrideOnAControllerFieldIsInertAndSaysSo() {
        const ComposeResult result =
            run("proxies: []\n", {}, "p1",
                QJsonObject{{"secret", "attacker"}, {"mixed-port", 28888}, {"mode", "direct"}});
        QVERIFY(result.ok);
        const YAML::Node config = yamlOf(result);
        QCOMPARE(config["secret"].as<std::string>(), std::string("the-real-secret"));
        QCOMPARE(config["mode"].as<std::string>(), std::string("direct"));
        QCOMPARE(result.diagnostics.size(), 1);
        QCOMPARE(result.diagnostics.first().severity, QStringLiteral("warning"));
        QCOMPARE(result.diagnostics.first().source, QStringLiteral("override"));
        QCOMPARE(result.diagnostics.first().path, QStringLiteral("/secret"));
        // mixed-port is the one override that IS honoured, so it is not warned
        // about -- and, unlike every other field on the list, it is not inert.
        QVERIFY(!result.diagnostics.first().message.contains("mixed-port"));
        QCOMPARE(config["mixed-port"].as<int>(), 28888);
    }

    // ------------------------------------------------------------ mixed-port
    //
    // The local proxy port is the user's decision, and compose() is the
    // published function that answers "what will the port be?". It used to
    // answer with the application default no matter what the overrides said,
    // because ProfileStore resolved the override before calling -- so the answer
    // was right only for the one caller that knew to do that.

    void avalidMixedPortOverrideWinsAndProvenanceNamesTheOverride() {
        const ComposeResult result =
            run("proxies: []\n", {}, "p1", QJsonObject{{"mixed-port", 41234}});
        QVERIFY2(result.ok, qPrintable(messages(result.diagnostics).join('\n')));
        QCOMPARE(yamlOf(result)["mixed-port"].as<int>(), 41234);
        QCOMPARE(provenanceOf(result.provenance, "/mixed-port"), QStringLiteral("override"));
        // Written once, by one layer, so the answer to "who chose this?" is one
        // answer and not a list.
        QCOMPARE(provenanceCount(result.provenance, "/mixed-port"), 1);
        QVERIFY2(result.diagnostics.isEmpty(), qPrintable(messages(result.diagnostics).join('\n')));
    }

    void withNoOverrideTheApplicationDefaultStandsAndSaysSo() {
        const ComposeResult result = run("proxies: []\n");
        QCOMPARE(yamlOf(result)["mixed-port"].as<int>(), 27890);
        QCOMPARE(provenanceOf(result.provenance, "/mixed-port"), QStringLiteral("controller"));
    }

    // Anything that cannot be a port is not one. The engine would refuse the
    // document; refusing the value and saying so leaves a configuration that
    // still launches.
    void amixedPortOverrideThatIsNotAPortIsRefusedWithAWarning() {
        const QVector<QJsonValue> notPorts{QJsonValue(QStringLiteral("8080")), QJsonValue(0),
                                           QJsonValue(70000),                 QJsonValue(-1),
                                           QJsonValue(8080.5),                QJsonValue(true),
                                           QJsonValue(QJsonValue::Null)};
        for (const QJsonValue &value : notPorts) {
            const ComposeResult result =
                run("proxies: []\n", {}, "p1", QJsonObject{{"mixed-port", value}});
            const QString what = QJsonDocument(QJsonObject{{"v", value}}).toJson();
            QVERIFY2(result.ok, qPrintable(what));
            QCOMPARE(yamlOf(result)["mixed-port"].as<int>(), 27890);
            QCOMPARE(provenanceOf(result.provenance, "/mixed-port"), QStringLiteral("controller"));
            QVERIFY2(hasSeverity(result.diagnostics, "warning"), qPrintable(what));
            QVERIFY2(!hasSeverity(result.diagnostics, "error"), qPrintable(what));
            QCOMPARE(result.diagnostics.constLast().path, QStringLiteral("/mixed-port"));
        }
    }

    // The distinction that makes the exception safe: TRUSTED overrides may pick
    // the port, presets may not, and an override does not lend a preset its
    // privilege.
    void apresetStillCannotPickThePortEvenWhileAnOverrideDoes() {
        using core::config::OperationKind;
        core::config::Preset greedy;
        greedy.id = greedy.name = QStringLiteral("greedy");
        greedy.operations = {raw(OperationKind::Replace, "/mixed-port", 1)};
        ComposeInput input;
        input.sourceYaml = QStringLiteral("proxies: []\n");
        input.profileUid = QStringLiteral("p1");
        input.profileName = QStringLiteral("fixture");
        input.controller = controller();
        input.presets.global = {greedy};
        input.overrides = QJsonObject{{"mixed-port", 41234}};

        const ComposeResult result = core::config::compose(input);
        QVERIFY(result.ok);
        QCOMPARE(yamlOf(result)["mixed-port"].as<int>(), 41234);
        QVERIFY(hasSeverity(result.diagnostics, "error"));
        QCOMPARE(result.diagnostics.first().source, QStringLiteral("global:greedy"));
    }

    // ------------------------------------------------------- the merge guard
    //
    // The recursion guard is not negotiable -- untrusted input driving unbounded
    // recursion is a stack overflow -- but stopping halfway through a fragment
    // and reporting success is worse than refusing it: the user gets a
    // configuration nobody described, with no way to tell.

    void amergeNestedPastTheGuardIsRefusedInsteadOfTruncated() {
        for (const int levels : {64, 70, 100}) {
            const QJsonObject presets = document(QJsonArray{
                preset("deep", QJsonArray{op("merge", "/nest", nestedFragment(levels))})});
            const ComposeResult result = run(nestedYaml(levels), presets);
            const QByteArray what = QByteArrayLiteral("levels=") + QByteArray::number(levels);
            QVERIFY2(!result.ok, what.constData());
            QVERIFY2(result.yaml.isEmpty(), what.constData());
            QVERIFY2(mentionsDepth(result.diagnostics),
                     qPrintable(messages(result.diagnostics).join('\n')));
            QCOMPARE(result.diagnostics.constLast().path, QStringLiteral("/nest"));
            QCOMPARE(result.diagnostics.constLast().source, QStringLiteral("global:deep"));
        }
    }

    // The boundary, from the other side: everything that fits still applies in
    // full, so the refusal above is about the guard and not about deep documents.
    void amergeThatFitsInsideTheGuardStillAppliesCompletely() {
        const QJsonObject presets = document(
            QJsonArray{preset("deep", QJsonArray{op("merge", "/nest", nestedFragment(63))})});
        const ComposeResult result = run(nestedYaml(63), presets);
        QVERIFY2(result.ok, qPrintable(messages(result.diagnostics).join('\n')));
        QCOMPARE(markerAt(yamlOf(result), 63), QStringLiteral("deep"));
    }

    // Depth is only reached where both sides are maps. A fragment that meets no
    // matching map is cloned whole, past the merge guard, and must not be
    // refused. (100 levels, not more: the renderer in core/config/yaml_util.cpp
    // refuses to emit past 128 nested nodes, which is a different limit with a
    // different error.)
    void adeepFragmentThatMeetsNoMatchingMapIsClonedWhole() {
        const QJsonObject presets = document(
            QJsonArray{preset("deep", QJsonArray{op("merge", "/nest", nestedFragment(100))})});
        const ComposeResult result = run("nest:\n  unrelated: 1\n", presets);
        QVERIFY2(result.ok, qPrintable(messages(result.diagnostics).join('\n')));
        QCOMPARE(markerAt(yamlOf(result), 100), QStringLiteral("deep"));
        QCOMPARE(yamlOf(result)["nest"]["unrelated"].as<int>(), 1);
    }

    // A refusal that costs the whole document must not also be a refusal that
    // hides which preset caused it, and it must not leave a half-composed
    // candidate behind for a later layer to dress up as valid.
    void afatalMergeStopsTheLayersAfterItRatherThanComposingAnyway() {
        const QJsonObject presets =
            document(QJsonArray{preset("deep", QJsonArray{op("merge", "/nest",
                                                             nestedFragment(70))}),
                                preset("later", QJsonArray{op("replace", "/mode", "global")})},
                     QJsonObject{{"p1", QJsonArray{preset("profile-level",
                                                          QJsonArray{op("replace", "/x", 1)})}}});
        const ComposeResult result =
            run(nestedYaml(70), presets, "p1", QJsonObject{{"mode", "direct"}});
        QVERIFY(!result.ok);
        QVERIFY(result.yaml.isEmpty());
        // One error, from the one preset that caused it.
        QCOMPARE(result.diagnostics.size(), 1);
        QCOMPARE(result.diagnostics.first().source, QStringLiteral("global:deep"));
        // And no layer below it claims to have written anything.
        QVERIFY(provenanceOf(result.provenance, "/mode").isEmpty());
        QVERIFY(provenanceOf(result.provenance, "/x").isEmpty());
        QVERIFY(provenanceOf(result.provenance, "/mixed-port").isEmpty());
    }

    // -------------------------------------------------------- bad structure

    void aNonMapParentIsDiagnosedAndTheRestStillComposes() {
        const QJsonObject presets = document(QJsonArray{
            preset("b", QJsonArray{op("replace", "/dns/enable", true), op("replace", "/ok", 1)})});
        const ComposeResult result = run("proxies: []\ndns: not-a-map\n", presets);
        QVERIFY(result.ok);  // one bad operation does not fail the composition
        QCOMPARE(yamlOf(result)["ok"].as<int>(), 1);
        QCOMPARE(yamlOf(result)["dns"].as<std::string>(), std::string("not-a-map"));
        QCOMPARE(result.diagnostics.size(), 1);
        QCOMPARE(result.diagnostics.first().severity, QStringLiteral("error"));
        QCOMPARE(result.diagnostics.first().path, QStringLiteral("/dns"));
    }

    void prependOntoANonSequenceIsDiagnosedNotForced() {
        const QJsonObject presets = document(
            QJsonArray{preset("b", QJsonArray{op("prepend", "/rules", QJsonArray{"A"})})});
        const ComposeResult result = run("proxies: []\nrules: nonsense\n", presets);
        QVERIFY(result.ok);
        QCOMPARE(yamlOf(result)["rules"].as<std::string>(), std::string("nonsense"));
        QVERIFY(hasSeverity(result.diagnostics, "error"));
    }

    void aSourceThatIsNotAMappingFailsWithoutAYamlDocument() {
        const ComposeResult result = run("- just\n- a\n- list\n");
        QVERIFY(!result.ok);
        QVERIFY(result.yaml.isEmpty());
        QVERIFY(hasSeverity(result.diagnostics, "error"));
    }

    void unparseableSourceFailsInsteadOfThrowing() {
        const ComposeResult result = run("proxies: [\n  unterminated\n");
        QVERIFY(!result.ok);
        QVERIFY(result.yaml.isEmpty());
        QVERIFY(hasSeverity(result.diagnostics, "error"));
    }

    void aCyclicDocumentFailsInsteadOfRecursingForever() {
        const ComposeResult result = run("proxies: []\ncycle: &loop [*loop]\n");
        QVERIFY(!result.ok);
        QVERIFY(result.yaml.isEmpty());
        QVERIFY(hasSeverity(result.diagnostics, "error"));
    }

    // ------------------------------------------------------- the defaults

    void tunDefaultsAreFilledInButNeverOverrideAChoice() {
        const YAML::Node fresh = yamlOf(run("proxies: []\n"));
        QVERIFY(!fresh["tun"]["enable"].as<bool>());
        QCOMPARE(fresh["tun"]["stack"].as<std::string>(), std::string("mixed"));
        QVERIFY(fresh["tun"]["auto-route"].as<bool>());
        QVERIFY(fresh["tun"]["auto-detect-interface"].as<bool>());
        QVERIFY(!fresh["tun"]["dns-hijack"]);

        const YAML::Node chosen = yamlOf(run(
            "proxies: []\ntun:\n  enable: true\n  stack: gvisor\n  auto-route: false\n"
            "  auto-detect-interface: false\n  dns-hijack: []\n"
            "dns:\n  enable: true\n"));
        QVERIFY(chosen["tun"]["enable"].as<bool>());
        QCOMPARE(chosen["tun"]["stack"].as<std::string>(), std::string("gvisor"));
        QVERIFY(!chosen["tun"]["auto-route"].as<bool>());
        QVERIFY(!chosen["tun"]["auto-detect-interface"].as<bool>());
        QCOMPARE(chosen["tun"]["dns-hijack"].size(), size_t(0));  // explicitly empty, honoured
    }

    void dnsHijackIsAddedOnlyWhenTheResolverIsOn() {
        const YAML::Node on = yamlOf(run("proxies: []\ndns:\n  enable: true\n"));
        QCOMPARE(on["tun"]["dns-hijack"].size(), size_t(2));
        QCOMPARE(on["tun"]["dns-hijack"][0].as<std::string>(), std::string("any:53"));
        QCOMPARE(on["tun"]["dns-hijack"][1].as<std::string>(), std::string("tcp://any:53"));
        QVERIFY(!yamlOf(run("proxies: []\ndns:\n  enable: false\n"))["tun"]["dns-hijack"]);
    }

    // A preset that turns the resolver on must get the hijack default too: the
    // defaults run after the presets, not against the unmodified profile.
    void aPresetThatEnablesTheResolverGetsTheHijackDefault() {
        const QJsonObject presets = document(QJsonArray{
            preset("dns", QJsonArray{op("merge", "/dns", QJsonObject{{"enable", true}})})});
        QCOMPARE(yamlOf(run("proxies: []\n", presets))["tun"]["dns-hijack"].size(), size_t(2));
    }

    void aMalformedTunBlockFailsTheWholeComposition() {
        for (const char *source : {"proxies: []\ntun: false\n", "proxies: []\ntun: [1, 2]\n"}) {
            const ComposeResult result = run(QString::fromLatin1(source));
            QVERIFY2(!result.ok, source);
            QVERIFY2(result.yaml.isEmpty(), source);
            QVERIFY(hasSeverity(result.diagnostics, "error"));
            QVERIFY(result.diagnostics.constLast().message.contains("TUN"));
        }
    }

    // --------------------------------------------------------------- purity

    // The claim the whole extraction rests on. Composition runs with the
    // process working directory inside an empty tree; if anything is created,
    // read back or removed, this notices.
    void composingTouchesNoFile() {
        QTemporaryDir sandbox;
        QVERIFY(sandbox.isValid());
        const QString previous = QDir::currentPath();
        QVERIFY(QDir::setCurrent(sandbox.path()));

        const QJsonObject presets = document(QJsonArray{preset(
            "p", QJsonArray{op("replace", "/mode", "global"), op("append", "/rules",
                                                                 QJsonArray{"MATCH,DIRECT"})})});
        const ComposeResult result =
            run("proxies: []\n", presets, "p1", QJsonObject{{"mode", "direct"}});
        const QStringList entries =
            QDir(sandbox.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);

        QVERIFY(QDir::setCurrent(previous));
        QVERIFY(result.ok);
        QVERIFY2(entries.isEmpty(), qPrintable(entries.join(", ")));
    }

    void composingTheSameInputTwiceGivesTheSameBytes() {
        const QJsonObject presets = document(
            QJsonArray{preset("p", QJsonArray{op("merge", "/dns", QJsonObject{{"enable", true}}),
                                              op("append", "/rules", QJsonArray{"MATCH,DIRECT"})})});
        const QString source = "proxies: []\nmode: rule\nrules:\n  - A\n";
        const ComposeResult first = run(source, presets, "p1", QJsonObject{{"ipv6", true}});
        const ComposeResult second = run(source, presets, "p1", QJsonObject{{"ipv6", true}});
        QVERIFY(first.ok);
        QCOMPARE(first.yaml, second.yaml);
    }

    void logsArePassedThroughUntouched() {
        ComposeInput input;
        input.sourceYaml = "proxies: []\n";
        input.controller = controller();
        input.logs = QStringList{"log: one", "warn: two"};
        const ComposeResult result = core::config::compose(input);
        QVERIFY(result.ok);
        QCOMPARE(result.logs, input.logs);
    }

    // ----------------------------------------------------------- pointers

    void pointerDecodingAcceptsAndRejectsTheRightShapes() {
        QStringList tokens;
        QString error;
        QVERIFY(core::config::decodePointer("/a/b", &tokens, &error));
        QCOMPARE(tokens, (QStringList{"a", "b"}));
        QVERIFY(core::config::decodePointer("/a~1b/c~0d", &tokens, &error));
        QCOMPARE(tokens, (QStringList{"a/b", "c~d"}));
        for (const char *bad : {"", "a/b", "/a//b", "/a~", "/a~2b", "/"}) {
            QVERIFY2(!core::config::decodePointer(QLatin1String(bad), &tokens, &error), bad);
            QVERIFY2(!error.isEmpty(), bad);
        }
    }
};

QTEST_GUILESS_MAIN(ConfigComposerTest)
#include "config_composer_test.moc"
