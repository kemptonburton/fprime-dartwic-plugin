#pragma once
#include <tempest/Value.h>
#include <nlohmann/json.hpp>
#include <cmath>

namespace TEMPEST::Json {
/** Convert an application object to a JSON object. */
inline nlohmann::json fromObject(const Value::Object& object);
/** Reject nonfinite doubles anywhere in an application value. */
inline void validate(const Value& value) {
    std::visit([](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, double>) {
            if (!std::isfinite(item)) throw std::invalid_argument("Non-finite floating point cannot be represented by JSON.");
        } else if constexpr (std::is_same_v<T, Value::Object>) {
            for (const auto& [key, child] : item) validate(child);
        } else if constexpr (std::is_same_v<T, Value::Array>) {
            for (const auto& child : item) validate(child);
        }
    }, value.storage());
}
/** Validate all values in an application object. */
inline void validateObject(const Value::Object& object) {
    for (const auto& [key, value] : object) validate(value);
}
/** Convert application values to JSON at an optional adapter boundary. Reject nonfinite doubles. Messaging and EngineProtocol never depend on this helper. */
inline nlohmann::json fromValue(const Value& value) {
    return std::visit([](const auto& item)->nlohmann::json {
        using T=std::decay_t<decltype(item)>;
        if constexpr(std::is_same_v<T,std::monostate>) return nullptr;
        else if constexpr(std::is_same_v<T,Value::Object>) {
            auto out=nlohmann::json::object(); for(const auto& [key,v]:item)out[key]=fromValue(v);return out;
        } else if constexpr(std::is_same_v<T,Value::Array>) {
            auto out=nlohmann::json::array();for(const auto& v:item)out.push_back(fromValue(v));return out;
        } else if constexpr(std::is_same_v<T,double>) {
            if(!std::isfinite(item))throw std::invalid_argument("Non-finite floating point cannot be represented by JSON.");
            return item;
        } else return item;
    },value.storage());
}
/** Convert an application object to a JSON object. */
inline nlohmann::json fromObject(const Value::Object& object) {
    auto out = nlohmann::json::object();
    for (const auto& [key, value] : object) out[key] = fromValue(value);
    return out;
}
/** Convert JSON to typed application values, retaining signed and unsigned integer precision. */
inline Value toValue(const nlohmann::json& value) {
    if(value.is_null())return {};
    if(value.is_boolean())return value.get<bool>();
    if(value.is_number_unsigned())return value.get<uint64_t>();
    if(value.is_number_integer())return value.get<int64_t>();
    if(value.is_number_float())return value.get<double>();
    if(value.is_string())return value.get<std::string>();
    if(value.is_array()){Value::Array out;for(const auto& v:value)out.push_back(toValue(v));return out;}
    if(value.is_object()){Value::Object out;for(const auto& [k,v]:value.items())out[k]=toValue(v);return out;}
    throw std::invalid_argument("Unsupported JSON value.");
}
/** Convert a JSON object to an application object, rejecting other JSON types. */
inline Value::Object toObject(const nlohmann::json& value) {
    if (!value.is_object()) throw std::invalid_argument("Expected a JSON object.");
    Value::Object out;
    for (const auto& [key, child] : value.items()) out.emplace(key, toValue(child));
    return out;
}

/** Build one application tree directly, without an intermediate JSON DOM. */
inline Value parseValue(const std::string& wire) {
    struct Sax final : nlohmann::json_sax<nlohmann::json> {
        struct Frame { Value value; std::string key; };
        std::vector<Frame> frames;
        Value result;
        Sax() { frames.reserve(32); }
        bool add(Value value) {
            if (frames.empty()) result = std::move(value);
            else if (auto* object = std::get_if<Value::Object>(&frames.back().value.storage()))
                object->insert_or_assign(frames.back().key, std::move(value));
            else std::get<Value::Array>(frames.back().value.storage()).push_back(std::move(value));
            return true;
        }
        bool null() override { return add({}); }
        bool boolean(bool value) override { return add(value); }
        bool number_integer(number_integer_t value) override { return add(value); }
        bool number_unsigned(number_unsigned_t value) override { return add(value); }
        bool number_float(number_float_t value, const string_t&) override { return std::isfinite(value) && add(value); }
        // Keep the lexer's reusable buffer. Moving it into every short key or
        // value steals its grown capacity and forces a fresh heap allocation
        // for the next token; copying preserves SSO for short application text.
        bool string(string_t& value) override { return add(value); }
        bool binary(binary_t&) override { return false; }
        bool start_object(std::size_t) override { frames.push_back({Value::Object{}, {}}); return true; }
        bool key(string_t& value) override { frames.back().key = value; return true; }
        bool end_object() override { return end(); }
        bool start_array(std::size_t) override { frames.push_back({Value::Array{}, {}}); return true; }
        bool end_array() override { return end(); }
        bool end() {
            auto value = std::move(frames.back().value); frames.pop_back();
            return add(std::move(value));
        }
        bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }
    } sax;
    if (!nlohmann::json::sax_parse(wire.data(), wire.data() + wire.size(), &sax))
        throw std::invalid_argument("Invalid JSON application value.");
    return std::move(sax.result);
}
}
