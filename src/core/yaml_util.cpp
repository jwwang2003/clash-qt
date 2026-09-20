#include "core/yaml_util.h"

#include <yaml-cpp/yaml.h>

namespace core::yamlutil {
namespace {

void emitNode(YAML::Emitter &emitter, const YAML::Node &node) {
    switch (node.Type()) {
        case YAML::NodeType::Map:
            emitter << YAML::BeginMap;
            for (const auto &entry : node) {
                emitter << YAML::Key;
                emitNode(emitter, entry.first);
                emitter << YAML::Value;
                emitNode(emitter, entry.second);
            }
            emitter << YAML::EndMap;
            return;
        case YAML::NodeType::Sequence:
            emitter << YAML::BeginSeq;
            for (const auto &item : node) emitNode(emitter, item);
            emitter << YAML::EndSeq;
            return;
        case YAML::NodeType::Scalar: {
            const QString text = QString::fromStdString(node.Scalar());
            if (node.Tag() == "!" && !plainToJson(text).isString()) emitter << YAML::DoubleQuoted;
            emitter << node.Scalar();
            return;
        }
        default:
            emitter << YAML::Null;
            return;
    }
}

}  // namespace

QJsonValue plainToJson(const QString &text) {
    if (text.isEmpty() || text == "~" || text.compare("null", Qt::CaseInsensitive) == 0) {
        return QJsonValue::Null;
    }
    if (text.compare("true", Qt::CaseInsensitive) == 0) return true;
    if (text.compare("false", Qt::CaseInsensitive) == 0) return false;

    bool ok = false;
    const qint64 integer = text.toLongLong(&ok);
    if (ok) return integer;
    const double real = text.toDouble(&ok);
    if (ok) return real;
    return text;
}

std::string dump(const YAML::Node &node) {
    YAML::Emitter emitter;
    emitNode(emitter, node);
    return emitter.c_str();
}

}  // namespace core::yamlutil
