// The preset document as a format: what version 1 accepts, what it refuses,
// and the fact that a refusal is total. parsePresetDocument() is the only gate
// between a hand-edited or hostile JSON file and the composer, so every reject
// case here is the reason a compose case in config-composer never has to
// defend against that shape.
//
// Contract: config-r1, .refactor/P4_CONFIG_CONTRACT.md.
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "core/config/config_composer.h"

using core::config::Diagnostic;
using core::config::OperationKind;
using core::config::PresetDocument;

namespace {

QJsonObject op(const QString &kind, const QString &path,
               const QJsonValue &value = QJsonValue(QJsonValue::Undefined)) {
    QJsonObject encoded{{"op", kind}, {"path", path}};
    if (!value.isUndefined()) encoded.insert("value", value);
    return encoded;
}

QJsonObject preset(const QString &id, const QJsonArray &operations) {
    return QJsonObject{{"id", id}, {"name", id}, {"enabled", true}, {"operations", operations}};
}

}  // namespace

class PresetDocumentTest : public QObject {
    Q_OBJECT

    static bool accepts(const QJsonObject &document, PresetDocument *out = nullptr,
                        QVector<Diagnostic> *diagnostics = nullptr) {
        PresetDocument scratch;
        QVector<Diagnostic> ignored;
        return core::config::parsePresetDocument(document, out ? out : &scratch,
                                                 diagnostics ? diagnostics : &ignored);
    }

    static QString firstError(const QVector<Diagnostic> &diagnostics) {
        for (const Diagnostic &diagnostic : diagnostics)
            if (diagnostic.severity == QLatin1String("error")) return diagnostic.message;
        return {};
    }

private slots:

    void theEmptyDocumentIsValidAndIsWhatAFirstRunGets() {
        const QJsonObject empty = core::config::emptyPresetDocument();
        QCOMPARE(empty.value("version").toInt(), 1);
        PresetDocument parsed;
        QVERIFY(accepts(empty, &parsed));
        QVERIFY(parsed.global.isEmpty());
        QVERIFY(parsed.profiles.isEmpty());
    }

    void aWellFormedDocumentParsesIntoOrderedPresetsAndOperations() {
        const QJsonObject document{
            {"version", 1},
            {"global", QJsonArray{preset("first", QJsonArray{op("merge", "/dns",
                                                                QJsonObject{{"enable", true}})}),
                                  preset("second", QJsonArray{op("remove", "/port-forward")})}},
            {"profiles", QJsonObject{{"uid-a", QJsonArray{preset(
                                                   "third", QJsonArray{op("append", "/rules",
                                                                          QJsonArray{"MATCH,DIRECT"})})}}}}};
        PresetDocument parsed;
        QVERIFY(accepts(document, &parsed));
        QCOMPARE(parsed.global.size(), 2);
        QCOMPARE(parsed.global[0].id, QStringLiteral("first"));
        QCOMPARE(parsed.global[1].id, QStringLiteral("second"));
        QCOMPARE(parsed.global[0].operations.size(), 1);
        QCOMPARE(parsed.global[0].operations[0].kind, OperationKind::Merge);
        QCOMPARE(parsed.global[0].operations[0].tokens, QStringList{"dns"});
        QCOMPARE(parsed.global[1].operations[0].kind, OperationKind::Remove);
        QCOMPARE(parsed.profiles.value("uid-a").size(), 1);
        QCOMPARE(parsed.profiles.value("uid-a")[0].operations[0].kind, OperationKind::Append);
    }

    void defaultsAreAppliedForNameAndEnabled() {
        PresetDocument parsed;
        QVERIFY(accepts(QJsonObject{{"version", 1},
                                    {"global", QJsonArray{QJsonObject{{"id", "bare"}}}}},
                        &parsed));
        QCOMPARE(parsed.global.size(), 1);
        QCOMPARE(parsed.global[0].name, QStringLiteral("bare"));
        QVERIFY(parsed.global[0].enabled);
        QVERIFY(parsed.global[0].operations.isEmpty());
    }

    // ------------------------------------------------------------- versions

    void theVersionIsRequiredAndOnlyVersionOneIsAccepted() {
        QVector<Diagnostic> diagnostics;
        QVERIFY(!accepts(QJsonObject{{"global", QJsonArray{}}}, nullptr, &diagnostics));
        QVERIFY(firstError(diagnostics).contains("version"));

        for (const QJsonValue &version : {QJsonValue(0), QJsonValue(2), QJsonValue("1"),
                                          QJsonValue(1.5), QJsonValue(QJsonValue::Null)}) {
            diagnostics.clear();
            QVERIFY2(!accepts(QJsonObject{{"version", version}}, nullptr, &diagnostics),
                     qPrintable(QJsonDocument(QJsonObject{{"v", version}}).toJson()));
            QVERIFY(!firstError(diagnostics).isEmpty());
        }
    }

    // ---------------------------------------------------------- identifiers

    void everyPresetNeedsAStableIdAndIdsAreUniqueAcrossTheDocument() {
        QVector<Diagnostic> diagnostics;
        QVERIFY(!accepts(QJsonObject{{"version", 1}, {"global", QJsonArray{QJsonObject{}}}},
                         nullptr, &diagnostics));
        QVERIFY(firstError(diagnostics).contains("id"));

        diagnostics.clear();
        QVERIFY(!accepts(
            QJsonObject{{"version", 1},
                        {"global", QJsonArray{QJsonObject{{"id", ""}}}}},
            nullptr, &diagnostics));

        // The same id twice inside one array.
        diagnostics.clear();
        QVERIFY(!accepts(QJsonObject{{"version", 1},
                                     {"global", QJsonArray{preset("dup", {}), preset("dup", {})}}},
                         nullptr, &diagnostics));
        QVERIFY(firstError(diagnostics).contains("dup"));

        // And across the global/profile boundary: an id names a preset, and
        // "global:x" and "profile:x" would be indistinguishable in provenance.
        diagnostics.clear();
        QVERIFY(!accepts(
            QJsonObject{{"version", 1},
                        {"global", QJsonArray{preset("shared", {})}},
                        {"profiles", QJsonObject{{"uid", QJsonArray{preset("shared", {})}}}}},
            nullptr, &diagnostics));
        QVERIFY(firstError(diagnostics).contains("shared"));
    }

    void profilesMustBeAnObjectKeyedByNonEmptyUid() {
        QVERIFY(!accepts(QJsonObject{{"version", 1}, {"profiles", QJsonArray{}}}));
        QVERIFY(!accepts(QJsonObject{{"version", 1}, {"profiles", QJsonObject{{"", QJsonArray{}}}}}));
        QVERIFY(!accepts(QJsonObject{{"version", 1},
                                     {"profiles", QJsonObject{{"uid", QJsonObject{}}}}}));
        QVERIFY(accepts(QJsonObject{{"version", 1},
                                    {"profiles", QJsonObject{{"uid", QJsonArray{}}}}}));
    }

    void globalMustBeAnArrayOfObjects() {
        QVERIFY(!accepts(QJsonObject{{"version", 1}, {"global", QJsonObject{}}}));
        QVERIFY(!accepts(QJsonObject{{"version", 1}, {"global", QJsonArray{"a string"}}}));
        QVERIFY(accepts(QJsonObject{{"version", 1}}));  // absent is empty, not invalid
    }

    void enabledAndNameAreTypeChecked() {
        QVERIFY(!accepts(QJsonObject{
            {"version", 1},
            {"global", QJsonArray{QJsonObject{{"id", "x"}, {"enabled", "yes"}}}}}));
        QVERIFY(!accepts(
            QJsonObject{{"version", 1},
                        {"global", QJsonArray{QJsonObject{{"id", "x"}, {"name", 7}}}}}));
        QVERIFY(!accepts(QJsonObject{
            {"version", 1},
            {"global", QJsonArray{QJsonObject{{"id", "x"}, {"operations", QJsonObject{}}}}}}));
    }

    // ----------------------------------------------------------- operations

    void onlyTheFiveNamedOperationsExist() {
        for (const char *name : {"merge", "replace", "remove", "prepend", "append"}) {
            const QJsonValue value = QString::fromLatin1(name) == QLatin1String("prepend") ||
                                             QString::fromLatin1(name) == QLatin1String("append")
                                         ? QJsonValue(QJsonArray{"R"})
                                         : QJsonValue(1);
            const QString path = QString::fromLatin1(name) == QLatin1String("prepend") ||
                                         QString::fromLatin1(name) == QLatin1String("append")
                                     ? QStringLiteral("/rules")
                                     : QStringLiteral("/x");
            QVERIFY2(accepts(QJsonObject{
                         {"version", 1},
                         {"global", QJsonArray{preset(name, QJsonArray{op(name, path, value)})}}}),
                     name);
        }
        for (const char *name : {"add", "copy", "move", "test", "MERGE", ""})
            QVERIFY2(!accepts(QJsonObject{
                         {"version", 1},
                         {"global", QJsonArray{preset("p", QJsonArray{op(name, "/x", 1)})}}}),
                     name);
    }

    void pathsMustBeValidMapKeyPointers() {
        for (const char *path : {"", "x", "/a//b", "/a~", "/a~9"}) {
            QVector<Diagnostic> diagnostics;
            QVERIFY2(!accepts(QJsonObject{{"version", 1},
                                          {"global", QJsonArray{preset(
                                                         "p", QJsonArray{op("replace", path, 1)})}}},
                              nullptr, &diagnostics),
                     path);
            QVERIFY2(!firstError(diagnostics).isEmpty(), path);
        }
        QVERIFY(!accepts(QJsonObject{
            {"version", 1},
            {"global", QJsonArray{preset("p", QJsonArray{QJsonObject{{"op", "replace"},
                                                                     {"path", 7},
                                                                     {"value", 1}}})}}}));
    }

    void mergeAndReplaceNeedAValueAndRemoveDoesNot() {
        QVERIFY(!accepts(QJsonObject{
            {"version", 1},
            {"global", QJsonArray{preset("p", QJsonArray{op("merge", "/x")})}}}));
        QVERIFY(!accepts(QJsonObject{
            {"version", 1},
            {"global", QJsonArray{preset("p", QJsonArray{op("replace", "/x")})}}}));
        QVERIFY(accepts(QJsonObject{
            {"version", 1},
            {"global", QJsonArray{preset("p", QJsonArray{op("remove", "/x")})}}}));
        // An explicit null IS a value.
        QVERIFY(accepts(QJsonObject{
            {"version", 1},
            {"global", QJsonArray{preset("p", QJsonArray{op("replace", "/x",
                                                            QJsonValue(QJsonValue::Null))})}}}));
    }

    void prependAndAppendAreRulesOnlyAndTakeStringArrays() {
        QVERIFY(accepts(QJsonObject{
            {"version", 1},
            {"global", QJsonArray{preset("p", QJsonArray{op("append", "/rules",
                                                            QJsonArray{"MATCH,DIRECT"})})}}}));
        for (const char *path : {"/proxies", "/rules/0", "/dns/nameserver"})
            QVERIFY2(!accepts(QJsonObject{
                         {"version", 1},
                         {"global", QJsonArray{preset("p", QJsonArray{op("append", path,
                                                                         QJsonArray{"R"})})}}}),
                     path);
        for (const QJsonValue &value :
             {QJsonValue("R"), QJsonValue(QJsonArray{1}), QJsonValue(QJsonArray{QJsonObject{}}),
              QJsonValue(QJsonObject{})})
            QVERIFY(!accepts(QJsonObject{
                {"version", 1},
                {"global", QJsonArray{preset("p", QJsonArray{op("prepend", "/rules", value)})}}}));
    }

    void controllerOwnedPathsAreRefusedAtTheDoorNotOnlyAtComposeTime() {
        for (const char *path : {"/external-controller", "/secret", "/external-ui",
                                 "/external-ui-url", "/mixed-port", "/port", "/socks-port",
                                 "/profile/store-selected"}) {
            QVector<Diagnostic> diagnostics;
            QVERIFY2(!accepts(QJsonObject{{"version", 1},
                                          {"global", QJsonArray{preset(
                                                         "p", QJsonArray{op("replace", path, 1)})}}},
                              nullptr, &diagnostics),
                     path);
            QVERIFY2(firstError(diagnostics).contains(QLatin1String(path)), path);
            QVERIFY2(core::config::isControllerOwnedPath(
                         QString::fromLatin1(path).mid(1).split('/')),
                     path);
        }
        // /profile itself is not owned: a preset may add its own keys there.
        QVERIFY(!core::config::isControllerOwnedPath(QStringList{"profile"}));
        QVERIFY(accepts(QJsonObject{
            {"version", 1},
            {"global", QJsonArray{preset("p", QJsonArray{op("merge", "/profile",
                                                            QJsonObject{{"store-fake-ip", true}})})}}}));
    }

    // ------------------------------------------------------- all or nothing

    // The property setPresetDocument() depends on: one bad operation anywhere
    // rejects the document, so a caller never adopts a half-parsed one.
    void oneBadOperationRejectsTheWholeDocument() {
        PresetDocument parsed;
        parsed.global.append(core::config::Preset{"sentinel", "sentinel", true, {}});
        QVector<Diagnostic> diagnostics;
        QVERIFY(!accepts(
            QJsonObject{{"version", 1},
                        {"global", QJsonArray{preset("good", QJsonArray{op("replace", "/a", 1)}),
                                              preset("bad", QJsonArray{op("nope", "/b", 1)})}}},
            &parsed, &diagnostics));
        // Untouched: the caller's document is exactly as it was.
        QCOMPARE(parsed.global.size(), 1);
        QCOMPARE(parsed.global[0].id, QStringLiteral("sentinel"));
    }

    // ----------------------------------------------------------- round trip

    void encodingAndReparsingIsAFixedPoint() {
        const QJsonObject original{
            {"version", 1},
            {"global", QJsonArray{preset("g", QJsonArray{op("merge", "/dns",
                                                            QJsonObject{{"enable", true}}),
                                                         op("remove", "/port-forward"),
                                                         op("append", "/rules",
                                                            QJsonArray{"MATCH,DIRECT"})})}},
            {"profiles", QJsonObject{{"uid", QJsonArray{preset("p", QJsonArray{
                                                                       op("replace", "/mode",
                                                                          "global")})}}}}};
        PresetDocument parsed;
        QVERIFY(accepts(original, &parsed));
        const QJsonObject encoded = core::config::presetDocumentToJson(parsed);

        PresetDocument reparsed;
        QVERIFY(accepts(encoded, &reparsed));
        QCOMPARE(core::config::presetDocumentToJson(reparsed), encoded);
        QCOMPARE(encoded.value("version").toInt(), 1);
        QCOMPARE(encoded.value("global").toArray().size(), 1);
        // A remove carries no "value" through the round trip.
        const QJsonArray operations =
            encoded.value("global").toArray()[0].toObject().value("operations").toArray();
        QCOMPARE(operations.size(), 3);
        QVERIFY(!operations[1].toObject().contains("value"));
        QCOMPARE(operations[2].toObject().value("op").toString(), QStringLiteral("append"));
    }
};

QTEST_GUILESS_MAIN(PresetDocumentTest)
#include "preset_document_test.moc"
