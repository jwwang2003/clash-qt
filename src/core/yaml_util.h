#pragma once

#include <QJsonValue>
#include <QString>

#include <string>

namespace YAML {
class Node;
}

namespace core::yamlutil {

/// Interprets a plain (unquoted) YAML scalar the way a YAML reader would.
QJsonValue plainToJson(const QString &text);

/// Serialises `node` to YAML.
///
/// yaml-cpp's own emitter writes every scalar plain, so a string like "0123"
/// comes back as a number to the next reader and mihomo then refuses to
/// unmarshal it. Scalars the parser tagged as strings are re-quoted here when
/// their text would otherwise read as something else.
std::string dump(const YAML::Node &node);

}  // namespace core::yamlutil
