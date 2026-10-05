#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace TEMPEST {

/** Application values independent of encoding: null, bool, exact signed/unsigned 64-bit integers, double, string, array, or object. */
class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;
    using Storage = std::variant<std::monostate, bool, int64_t, uint64_t, double,
                                 std::string, Array, Object>;
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool v) : value_(v) {}
    Value(int v) : value_(static_cast<int64_t>(v)) {}
    Value(int64_t v) : value_(v) {}
    Value(uint64_t v) : value_(v) {}
    Value(double v) : value_(v) {}
    Value(const char* v) : value_(std::string(v ? v : "")) {}
    Value(std::string v) : value_(std::move(v)) {}
    Value(Array v) : value_(std::move(v)) {}
    Value(Object v) : value_(std::move(v)) {}
    const Storage& storage() const noexcept { return value_; }
    Storage& storage() noexcept { return value_; }
    bool isNull() const noexcept { return std::holds_alternative<std::monostate>(value_); }
    const Object& object() const { return std::get<Object>(value_); }
    const Array& array() const { return std::get<Array>(value_); }
    const std::string& string() const { return std::get<std::string>(value_); }
    const Value& at(const std::string& key) const { return object().at(key); }
    friend bool operator==(const Value&, const Value&) = default;
private:
    Storage value_;
};

} // namespace TEMPEST
