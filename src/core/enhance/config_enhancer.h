#pragma once

#include <QObject>
#include <QString>
#include <QVector>

namespace core {

enum class ChainKind { Merge, Script };

/// One step of the enhancement chain: a YAML fragment merged into the config,
/// or a JS script that rewrites it.
struct ChainItem {
    QString uid;
    QString name;
    ChainKind kind = ChainKind::Merge;
    QString filePath;
    bool enabled = true;
};

struct EnhanceResult {
    QString yaml;        // the enhanced config; the input unchanged on failure
    QStringList logs;    // console output from scripts, "level: message"
    QString error;       // empty on success
};

/// Applies a chain of merges and scripts to a profile's YAML.
///
/// The chain is global rather than per-profile: it applies to whichever profile
/// is active. Contract with the ui module. Extend, do not reshape.
class ConfigEnhancer : public QObject {
    Q_OBJECT

public:
    explicit ConfigEnhancer(QObject *parent = nullptr);

    QString chainDir() const;

    void load();
    QVector<ChainItem> chain() const;

    void addMerge(const QString &name);
    void addScript(const QString &name);
    void importItem(const QString &path, ChainKind kind);
    void removeItem(const QString &uid);
    void setEnabled(const QString &uid, bool enabled);
    void moveItem(const QString &uid, int toIndex);
    void renameItem(const QString &uid, const QString &name);

    /// Runs the enabled chain over `baseYaml`. Never throws: a failing step is
    /// reported in the result and leaves the config as it was.
    EnhanceResult apply(const QString &baseYaml) const;

signals:
    void chainChanged(const QVector<ChainItem> &chain);
    void errorOccurred(const QString &message);

private:
    int indexOf(const QString &uid) const;
    void save();
    void addItem(const QString &name, ChainKind kind, const QByteArray &contents);

    QVector<ChainItem> chain_;
};

}  // namespace core
