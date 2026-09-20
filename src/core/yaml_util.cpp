#include "core/yaml_util.h"

#include <yaml-cpp/yaml.h>

namespace core::yamlutil {
namespace {

void emitNode(YAML::Emitter &emitter, const YAML::Node &node, int depth = 0) {
    if (depth > 128) throw YAML::BadConversion(node.Mark());
    switch (node.Type()) {
        case YAML::NodeType::Map:
            emitter << YAML::BeginMap;
            for (const auto &entry : node) {
                emitter << YAML::Key;
                emitNode(emitter, entry.first, depth + 1);
                emitter << YAML::Value;
                emitNode(emitter, entry.second, depth + 1);
            }
            emitter << YAML::EndMap;
            return;
        case YAML::NodeType::Sequence:
            emitter << YAML::BeginSeq;
            for (const auto &item : node) emitNode(emitter, item, depth + 1);
            emitter << YAML::EndSeq;
            return;
        case YAML::NodeType::Scalar: {
            if (node.Tag() == "!" || node.Tag() == "tag:yaml.org,2002:str") {
                emitter << YAML::DoubleQuoted;
            }
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
    try {
        emitNode(emitter, node);
    } catch (const YAML::Exception &) {
        return {};
    }
    return emitter.good() ? std::string(emitter.c_str()) : std::string();
}

}  // namespace core::yamlutil
