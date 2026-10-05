#include "FprimeDictionary.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace FPrimeBridge {
namespace {

using DARTWIC::Value;
using Json = nlohmann::json;

struct Cursor {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t offset = 0;

    void require(const std::size_t count) const {
        if (count > size - offset) {
            throw std::runtime_error("serialized value ended early");
        }
    }

    std::uint64_t unsignedInteger(const std::size_t bytes) {
        if (bytes == 0 || bytes > sizeof(std::uint64_t)) {
            throw std::runtime_error("unsupported integer width");
        }
        require(bytes);
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < bytes; ++i) {
            value = (value << 8U) | data[offset++];
        }
        return value;
    }

    std::int64_t signedInteger(const std::size_t bytes) {
        const std::uint64_t raw = unsignedInteger(bytes);
        if (bytes == sizeof(std::uint64_t)) {
            return static_cast<std::int64_t>(raw);
        }
        const std::uint64_t sign = std::uint64_t{1} << (bytes * 8U - 1U);
        const std::uint64_t extended = (raw & sign) ? (raw | (~std::uint64_t{0} << (bytes * 8U))) : raw;
        return static_cast<std::int64_t>(extended);
    }

    double floatingPoint(const std::size_t bytes) {
        if (bytes == sizeof(float)) {
            const std::uint32_t bits = static_cast<std::uint32_t>(unsignedInteger(bytes));
            float value = 0.0F;
            std::memcpy(&value, &bits, sizeof(value));
            return static_cast<double>(value);
        }
        if (bytes == sizeof(double)) {
            const std::uint64_t bits = unsignedInteger(bytes);
            double value = 0.0;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }
        throw std::runtime_error("unsupported floating-point width");
    }
};

std::size_t bitSize(const Json& type) {
    const auto bits = type.value("size", 0);
    if (bits <= 0 || bits % 8 != 0) {
        throw std::runtime_error("type has an invalid serialized size");
    }
    return static_cast<std::size_t>(bits / 8);
}

bool isPrimitive(const std::string& name) {
    return name == "I8" || name == "I16" || name == "I32" || name == "I64" || name == "U8" ||
           name == "U16" || name == "U32" || name == "U64" || name == "F32" || name == "F64" ||
           name == "bool";
}

}  // namespace

struct Dictionary::Impl {
    struct ChannelEntry {
        std::string name;
        Json type;
    };
    struct EventEntry {
        std::string name;
        std::string format;
        std::string severity;
        Json parameters;
    };

    Json root;
    std::unordered_map<std::string, Json> definitions;
    std::unordered_map<std::uint64_t, ChannelEntry> channels;
    std::unordered_map<std::uint64_t, EventEntry> events;
    std::unordered_map<std::string, Json> commands;
    Json sizeType = {{"name", "U16"}, {"kind", "integer"}, {"size", 16}, {"signed", false}};
    // Standard F Prime defaults for dictionaries without these constants.
    std::uint8_t serializedTrue = 0xFF;
    std::uint8_t serializedFalse = 0x00;

    Json resolve(const Json& descriptor) const {
        const std::string name = descriptor.value("name", std::string{});
        const std::string kind = descriptor.value("kind", std::string{});
        if (kind == "alias" || kind == "enum" || kind == "array" || kind == "struct") {
            return descriptor;
        }
        if (isPrimitive(name) || name == "string" || kind == "integer" || kind == "float" || kind == "boolean") {
            return descriptor;
        }
        const auto found = definitions.find(name);
        if (found == definitions.end()) {
            throw std::runtime_error("dictionary type '" + name + "' is not defined");
        }
        return found->second;
    }

    CommandArgument commandArgument(const Json& parameter) const {
        CommandArgument result;
        result.name = parameter.at("name").get<std::string>();
        result.description = parameter.value("annotation", std::string{});
        Json type = parameter.at("type");
        for (unsigned depth = 0; depth < 32; ++depth) {
            type = resolve(type);
            if (type.value("kind", std::string{}) != "alias") break;
            type = type.at("underlyingType");
        }
        const auto kind = type.value("kind", std::string{});
        const auto name = type.value("name", std::string{});
        if (kind == "enum") {
            result.type = "string";
            for (const auto& constant : type.at("enumeratedConstants"))
                result.choices.emplace_back(constant.at("name").get<std::string>());
        } else if (kind == "boolean" || kind == "bool" || name == "bool") result.type = "boolean";
        else if (kind == "integer") result.type = "integer";
        else if (kind == "float") result.type = "number";
        else if (kind == "string" || name == "string") result.type = "string";
        else if (kind == "array") result.type = "array";
        else if (kind == "struct") result.type = "object";
        else result.type = "any";
        return result;
    }

    void encode(const Json& descriptor, const Value& value, std::vector<std::uint8_t>& out,
                std::size_t maximum, unsigned depth = 0) const {
        if (depth > 32) throw std::invalid_argument("command type nesting limit exceeded");
        auto integer = [&](std::uint64_t n, std::size_t bytes) {
            if (bytes == 0 || bytes > 8 || bytes > maximum - out.size())
                throw std::invalid_argument("command argument buffer limit exceeded");
            for (std::size_t i = bytes; i > 0; --i) out.push_back(static_cast<std::uint8_t>(n >> ((i-1)*8)));
        };
        const auto type = resolve(descriptor);
        const auto name = type.value("name", std::string{});
        const auto kind = type.value("kind", std::string{});
        if (kind == "alias") { encode(type.at("underlyingType"), value, out, maximum, depth+1); return; }
        if (kind == "enum") {
            Value representation = value;
            if (const auto* label = std::get_if<std::string>(&value.storage())) {
                bool found = false;
                for (const auto& c : type.at("enumeratedConstants")) if (c.at("name") == *label) {
                    representation = c.at("value").get<std::int64_t>(); found = true; break;
                }
                if (!found) throw std::invalid_argument("unknown enum label");
            }
            encode(type.at("representationType"), representation, out, maximum, depth+1); return;
        }
        if (kind == "boolean" || kind == "bool" || name == "bool") {
            integer(std::get<bool>(value.storage()) ? serializedTrue : serializedFalse, 1); return;
        }
        if (kind == "integer" || (!name.empty() && (name[0] == 'I' || name[0] == 'U') && isPrimitive(name))) {
            const auto bytes = bitSize(type);
            if (!bytes || bytes > 8) throw std::invalid_argument("invalid integer width");
            const bool signedValue = type.value("signed", name.starts_with("I"));
            std::uint64_t raw;
            if (const auto* n = std::get_if<std::int64_t>(&value.storage())) {
                if (!signedValue && *n < 0) throw std::invalid_argument("negative unsigned command argument");
                if (signedValue && bytes < 8 && (*n < -(std::int64_t{1} << (bytes*8-1)) || *n > (std::int64_t{1} << (bytes*8-1))-1))
                    throw std::invalid_argument("signed command argument out of range");
                raw = static_cast<std::uint64_t>(*n);
            } else if (const auto* n = std::get_if<std::uint64_t>(&value.storage())) {
                const auto limit = signedValue ? (bytes == 8 ? std::uint64_t{INT64_MAX} : (std::uint64_t{1} << (bytes*8-1))-1)
                    : (bytes == 8 ? UINT64_MAX : (std::uint64_t{1} << (bytes*8))-1);
                if (*n > limit) throw std::invalid_argument("command argument out of range");
                raw = *n;
            } else throw std::invalid_argument("command argument must be an integer");
            if (!signedValue && bytes < 8 && raw >= (std::uint64_t{1} << (bytes*8)))
                throw std::invalid_argument("unsigned command argument out of range");
            integer(raw, bytes); return;
        }
        if (kind == "float" || name == "F32" || name == "F64") {
            double n;
            if (auto v = std::get_if<double>(&value.storage())) n = *v;
            else if (auto v = std::get_if<std::int64_t>(&value.storage())) n = static_cast<double>(*v);
            else if (auto v = std::get_if<std::uint64_t>(&value.storage())) n = static_cast<double>(*v);
            else throw std::invalid_argument("command argument must be numeric");
            if (!std::isfinite(n)) throw std::invalid_argument("nonfinite command argument");
            if (bitSize(type) == 4) {
                const float v = static_cast<float>(n); std::uint32_t bits;
                if (!std::isfinite(v)) throw std::invalid_argument("F32 command argument out of range");
                std::memcpy(&bits, &v, 4); integer(bits, 4);
            } else if (bitSize(type) == 8) { std::uint64_t bits; std::memcpy(&bits, &n, 8); integer(bits, 8); }
            else throw std::invalid_argument("invalid float width");
            return;
        }
        if (kind == "string" || name == "string") {
            const auto& text = value.string();
            if (text.size() > type.value("size", std::size_t{0})) throw std::invalid_argument("command string too long");
            encode(sizeType, Value{static_cast<std::uint64_t>(text.size())}, out, maximum, depth+1);
            if (text.size() > maximum - out.size()) throw std::invalid_argument("command argument buffer limit exceeded");
            out.insert(out.end(), text.begin(), text.end()); return;
        }
        if (kind == "array") {
            const auto& array = value.array();
            if (array.size() != type.at("size").get<std::size_t>()) throw std::invalid_argument("command array has wrong length");
            for (const auto& item : array) encode(type.at("elementType"), item, out, maximum, depth+1);
            return;
        }
        if (kind == "struct") {
            const auto& fields = value.object();
            const auto& members = type.at("members");
            if (fields.size() != members.size()) throw std::invalid_argument("command struct has missing or extra fields");
            std::vector<std::pair<std::string, Json>> sorted;
            for (auto it=members.begin(); it!=members.end(); ++it) sorted.emplace_back(it.key(), it.value());
            std::sort(sorted.begin(), sorted.end(), [](const auto& a,const auto& b){return a.second.at("index") < b.second.at("index");});
            for (const auto& [key, member] : sorted) {
                const auto& v = fields.at(key);
                if (member.contains("size")) {
                    if (v.array().size() != member.at("size").get<std::size_t>()) throw std::invalid_argument("command member array has wrong length");
                    for (const auto& child : v.array()) encode(member.at("type"), child, out, maximum, depth+1);
                } else encode(member.at("type"), v, out, maximum, depth+1);
            }
            return;
        }
        throw std::invalid_argument("unsupported command type");
    }

    Value decode(const Json& descriptor, Cursor& cursor, bool& numeric) const {
        const Json type = resolve(descriptor);
        const std::string name = type.value("name", std::string{});
        const std::string kind = type.value("kind", std::string{});

        if (name == "bool" || kind == "boolean" || kind == "bool") {
            const auto raw = cursor.unsignedInteger(1);
            if (raw != serializedTrue && raw != serializedFalse)
                throw std::runtime_error("invalid serialized F Prime boolean");
            numeric = true;
            return Value{raw == serializedTrue};
        }
        if (kind == "integer" || name == "I8" || name == "I16" || name == "I32" || name == "I64" ||
            name == "U8" || name == "U16" || name == "U32" || name == "U64") {
            numeric = true;
            const bool signedValue = type.value("signed", !name.empty() && name.front() == 'I');
            const std::size_t bytes = bitSize(type);
            return signedValue ? Value{cursor.signedInteger(bytes)} : Value{cursor.unsignedInteger(bytes)};
        }
        if (kind == "float" || name == "F32" || name == "F64") {
            numeric = true;
            return Value{cursor.floatingPoint(bitSize(type))};
        }
        if (name == "string" || kind == "string") {
            numeric = false;
            bool lengthNumeric = false;
            const Value lengthValue = decode(sizeType, cursor, lengthNumeric);
            std::uint64_t length = 0;
            if (const auto* value = std::get_if<std::uint64_t>(&lengthValue.storage())) {
                length = *value;
            } else if (const auto* value = std::get_if<std::int64_t>(&lengthValue.storage())) {
                length = static_cast<std::uint64_t>(*value);
            } else {
                throw std::runtime_error("string length type is not integral");
            }
            const auto maximum = static_cast<std::uint64_t>(type.value("size", std::numeric_limits<int>::max()));
            if (length > maximum) {
                throw std::runtime_error("serialized string exceeds its dictionary maximum");
            }
            cursor.require(static_cast<std::size_t>(length));
            std::string result(reinterpret_cast<const char*>(cursor.data + cursor.offset), static_cast<std::size_t>(length));
            cursor.offset += static_cast<std::size_t>(length);
            return Value{std::move(result)};
        }

        if (kind == "alias") {
            return decode(type.at("underlyingType"), cursor, numeric);
        }
        if (kind == "enum") {
            bool representationNumeric = false;
            Value raw = decode(type.at("representationType"), cursor, representationNumeric);
            // RAPID channel values are numeric. Keep the wire representation
            // numeric rather than replacing it with the dictionary label.
            numeric = true;
            return raw;
        }
        if (kind == "array") {
            numeric = false;
            Value::Array values;
            const auto count = type.at("size").get<std::size_t>();
            values.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                bool childNumeric = false;
                values.push_back(decode(type.at("elementType"), cursor, childNumeric));
            }
            return Value{std::move(values)};
        }
        if (kind == "struct") {
            numeric = false;
            struct Member {
                std::size_t index;
                std::string name;
                Json descriptor;
                std::size_t count;
            };
            std::vector<Member> members;
            for (auto it = type.at("members").begin(); it != type.at("members").end(); ++it) {
                members.push_back({it.value().at("index").get<std::size_t>(), it.key(), it.value().at("type"),
                                   it.value().value("size", std::size_t{1})});
            }
            std::sort(members.begin(), members.end(), [](const Member& left, const Member& right) {
                return left.index < right.index;
            });
            Value::Object values;
            for (const auto& member : members) {
                if (member.count == 1) {
                    bool childNumeric = false;
                    values.emplace(member.name, decode(member.descriptor, cursor, childNumeric));
                } else {
                    Value::Array array;
                    array.reserve(member.count);
                    for (std::size_t i = 0; i < member.count; ++i) {
                        bool childNumeric = false;
                        array.push_back(decode(member.descriptor, cursor, childNumeric));
                    }
                    values.emplace(member.name, Value{std::move(array)});
                }
            }
            return Value{std::move(values)};
        }
        throw std::runtime_error("unsupported dictionary type kind '" + kind + "'");
    }
};

Dictionary::Dictionary() : m_impl(std::make_unique<Impl>()) {}
Dictionary::~Dictionary() = default;
Dictionary::Dictionary(Dictionary&&) noexcept = default;
Dictionary& Dictionary::operator=(Dictionary&&) noexcept = default;

bool Dictionary::load(const std::string& path, std::string& error) {
    try {
        std::ifstream stream(path);
        if (!stream) {
            throw std::runtime_error("could not open F Prime dictionary '" + path + "'");
        }
        Impl next;
        stream >> next.root;
        for (const auto& constant : next.root.value("constants", Json::array())) {
            const auto name = constant.value("qualifiedName", std::string{});
            if (name != "FW_SERIALIZE_TRUE_VALUE" && name != "FW_SERIALIZE_FALSE_VALUE") continue;
            const auto& value = constant.at("value");
            if (!value.is_number_integer() || value < 0 || value > 255)
                throw std::runtime_error("dictionary boolean constant must be a byte");
            auto& target = name == "FW_SERIALIZE_TRUE_VALUE" ? next.serializedTrue : next.serializedFalse;
            target = value.get<std::uint8_t>();
        }
        if (next.serializedTrue == next.serializedFalse)
            throw std::runtime_error("dictionary boolean constants must be distinct");
        for (const auto& definition : next.root.at("typeDefinitions")) {
            const std::string name = definition.at("qualifiedName").get<std::string>();
            next.definitions.emplace(name, definition);
        }
        if (const auto found = next.definitions.find("FwSizeStoreType"); found != next.definitions.end()) {
            next.sizeType = found->second;
        }
        for (const auto& channel : next.root.at("telemetryChannels")) {
            next.channels.emplace(channel.at("id").get<std::uint64_t>(),
                                  Impl::ChannelEntry{channel.at("name").get<std::string>(), channel.at("type")});
        }
        for (const auto& event : next.root.at("events")) {
            next.events.emplace(event.at("id").get<std::uint64_t>(),
                                Impl::EventEntry{event.at("name").get<std::string>(),
                                                 event.value("format", std::string{}),
                                                 event.value("severity", std::string{}),
                                                 event.value("formalParams", Json::array())});
        }
        for (const auto& command : next.root.value("commands", Json::array()))
            if (!next.commands.emplace(command.at("name").get<std::string>(), command).second)
                throw std::runtime_error("duplicate dictionary command name");
        *m_impl = std::move(next);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        clear();
        error = exception.what();
        return false;
    }
}

EncodedCommand Dictionary::encodeCommand(const std::string& name, const Value::Object& arguments,
                                         std::size_t maximumBytes) const {
    const auto found = m_impl->commands.find(name);
    if (found == m_impl->commands.end()) throw std::invalid_argument("unknown dictionary command: " + name);
    const auto& command = found->second;
    const auto& parameters = command.at("formalParams");
    if (arguments.size() != parameters.size()) throw std::invalid_argument("command has missing or extra arguments");
    EncodedCommand result;
    result.opcode = command.at("opcode").get<std::uint64_t>();
    for (const auto& parameter : parameters)
        m_impl->encode(parameter.at("type"), arguments.at(parameter.at("name").get<std::string>()), result.arguments, maximumBytes);
    return result;
}

void Dictionary::clear() {
    m_impl = std::make_unique<Impl>();
}

bool Dictionary::loaded() const noexcept {
    return !m_impl->channels.empty() || !m_impl->events.empty() || !m_impl->commands.empty();
}

std::size_t Dictionary::channelBytes(const std::uint64_t id) const {
    const auto found = m_impl->channels.find(id);
    if (found == m_impl->channels.end()) throw std::invalid_argument("unknown F Prime channel ID");
    return bitSize(m_impl->resolve(found->second.type));
}

std::string Dictionary::commandWithSuffix(const std::string& suffix) const {
    std::string result;
    for (const auto& [name, _] : m_impl->commands) {
        if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            if (!result.empty()) throw std::invalid_argument("ambiguous F Prime command suffix");
            result = name;
        }
    }
    if (result.empty()) throw std::invalid_argument("F Prime command not found: " + suffix);
    return result;
}

std::vector<CommandDescriptor> Dictionary::commands() const {
    std::vector<CommandDescriptor> result;
    result.reserve(m_impl->commands.size());
    for (const auto& [name, command] : m_impl->commands) {
        CommandDescriptor descriptor;
        descriptor.name = name;
        descriptor.description = command.value("annotation", std::string{});
        for (const auto& parameter : command.value("formalParams", Json::array())) {
            try { descriptor.arguments.push_back(m_impl->commandArgument(parameter)); }
            catch (const std::exception&) {
                descriptor.arguments.push_back({parameter.at("name").get<std::string>(), "any",
                    parameter.value("annotation", std::string{}), {}});
            }
        }
        result.push_back(std::move(descriptor));
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    return result;
}

bool Dictionary::hasCommand(const std::string& name) const {
    return m_impl->commands.contains(name);
}

DecodedChannel Dictionary::decodeChannel(const std::uint64_t id,
                                         const std::uint8_t* data,
                                         const std::size_t size) const {
    DecodedChannel result;
    const auto found = m_impl->channels.find(id);
    result.name = peerChannelName(found == m_impl->channels.end() ? fallbackChannelName(id) : found->second.name);
    if (found == m_impl->channels.end()) {
        result.value = hex(data, size);
        result.error = "channel ID is not present in the loaded dictionary";
        return result;
    }
    try {
        Cursor cursor{data, size, 0};
        result.value = m_impl->decode(found->second.type, cursor, result.numeric);
        if (cursor.offset != size) {
            throw std::runtime_error("serialized channel has trailing bytes");
        }
        result.decoded = true;
    } catch (const std::exception& exception) {
        result.value = hex(data, size);
        result.numeric = false;
        result.error = exception.what();
    }
    return result;
}

DecodedChannel Dictionary::decodeChannelPrefix(const std::uint64_t id, const std::uint8_t* data,
                                                const std::size_t available, std::size_t& consumed) const {
    consumed = 0;
    DecodedChannel result;
    const auto found = m_impl->channels.find(id);
    if (found == m_impl->channels.end()) return result;
    result.name = peerChannelName(found->second.name);
    try {
        Cursor cursor{data, available, 0};
        result.value = m_impl->decode(found->second.type, cursor, result.numeric);
        consumed = cursor.offset;
        result.decoded = true;
    } catch (const std::exception& error) { result.error = error.what(); }
    return result;
}

DecodedEvent Dictionary::decodeEvent(const std::uint64_t id,
                                     const std::uint8_t* data,
                                     const std::size_t size) const {
    DecodedEvent result;
    const auto found = m_impl->events.find(id);
    result.name = found == m_impl->events.end() ? fallbackEventName(id) : found->second.name;
    if (found == m_impl->events.end()) {
        result.arguments.emplace("raw", hex(data, size));
        result.error = "event ID is not present in the loaded dictionary";
        return result;
    }
    result.format = found->second.format;
    result.severity = found->second.severity;
    try {
        Cursor cursor{data, size, 0};
        for (const auto& parameter : found->second.parameters) {
            bool numeric = false;
            result.arguments.emplace(parameter.at("name").get<std::string>(),
                                     m_impl->decode(parameter.at("type"), cursor, numeric));
        }
        if (cursor.offset != size) {
            throw std::runtime_error("serialized event arguments have trailing bytes");
        }
        result.decoded = true;
    } catch (const std::exception& exception) {
        result.arguments.clear();
        result.arguments.emplace("raw", hex(data, size));
        result.error = exception.what();
    }
    return result;
}

std::string Dictionary::fallbackChannelName(const std::uint64_t id) {
    std::ostringstream stream;
    stream << "fprime.channel.0x" << std::hex << std::uppercase << id;
    return stream.str();
}

std::string Dictionary::fallbackEventName(const std::uint64_t id) {
    std::ostringstream stream;
    stream << "F Prime event 0x" << std::hex << std::uppercase << id;
    return stream.str();
}

std::string Dictionary::hex(const std::uint8_t* data, const std::size_t size) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i) {
        stream << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    return stream.str();
}

std::string Dictionary::peerChannelName(const std::string& fprimeName) {
    std::string result = fprimeName;
    for (char& character : result) {
        const bool valid = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                           (character >= '0' && character <= '9') || character == '_' || character == '-' ||
                           character == '.';
        if (!valid) {
            character = '_';
        }
    }
    return result.empty() ? "fprime.channel.unknown" : result;
}

}  // namespace FPrimeBridge
