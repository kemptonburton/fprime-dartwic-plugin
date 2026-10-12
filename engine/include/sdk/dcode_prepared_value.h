#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>

namespace DARTWIC::API {

// A call-scoped mutable view of a fixed-shape DCode value. Plugins must not
// retain these pointers or spans after their registered handler returns.
enum class DCodePreparedKind : uint8_t { Null, Boolean, Number, String, List, Dictionary };
struct DCodePreparedFieldView;
struct DCodePreparedMutableField;

struct DCodePreparedValueView {
    DCodePreparedKind kind{DCodePreparedKind::Null};
    bool boolean{false};
    double number{0.0};
    std::string_view text;
    std::span<DCodePreparedValueView> elements;
    std::span<DCodePreparedFieldView> fields;

    DCodePreparedValueView& field(std::string_view name);
    DCodePreparedValueView& at(size_t index) {
        if (kind != DCodePreparedKind::List || index >= elements.size())
            throw std::out_of_range("Prepared DCode list index is invalid.");
        return elements[index];
    }
    double asNumber() const {
        if (kind != DCodePreparedKind::Number)
            throw std::invalid_argument("Prepared DCode value is not a number.");
        return number;
    }
    void setNumber(double value) {
        if (kind != DCodePreparedKind::Number)
            throw std::invalid_argument("Prepared DCode argument is not a number.");
        number = value;
    }
    void setBoolean(bool value) {
        if (kind != DCodePreparedKind::Boolean)
            throw std::invalid_argument("Prepared DCode argument is not a boolean.");
        boolean = value;
    }
};

struct DCodePreparedFieldView {
    std::string_view name;
    DCodePreparedValueView* value{nullptr};
};

inline DCodePreparedValueView& DCodePreparedValueView::field(std::string_view name) {
    if (kind != DCodePreparedKind::Dictionary)
        throw std::invalid_argument("Prepared DCode value is not a dictionary.");
    for (const auto& entry : fields)
        if (entry.name == name && entry.value) return *entry.value;
    throw std::out_of_range("Prepared DCode dictionary field is missing.");
}

struct DCodePreparedMutableValue {
    DCodePreparedKind kind{DCodePreparedKind::Null};
    bool boolean{false};
    double number{0.0};
    std::span<DCodePreparedMutableValue> elements;
    std::span<DCodePreparedMutableField> fields;

    DCodePreparedMutableValue& field(std::string_view name);
    DCodePreparedMutableValue& at(size_t index) {
        if (kind != DCodePreparedKind::List || index >= elements.size())
            throw std::out_of_range("Prepared DCode result list index is invalid.");
        return elements[index];
    }
    void setNumber(double value) {
        if (kind != DCodePreparedKind::Number)
            throw std::invalid_argument("Prepared DCode result field is not a number.");
        number = value;
    }
    void setBoolean(bool value) {
        if (kind != DCodePreparedKind::Boolean)
            throw std::invalid_argument("Prepared DCode result field is not a boolean.");
        boolean = value;
    }
};

struct DCodePreparedMutableField {
    std::string_view name;
    DCodePreparedMutableValue* value{nullptr};
};

inline DCodePreparedMutableValue& DCodePreparedMutableValue::field(std::string_view name) {
    if (kind != DCodePreparedKind::Dictionary)
        throw std::invalid_argument("Prepared DCode result is not a dictionary.");
    for (const auto& entry : fields)
        if (entry.name == name && entry.value) return *entry.value;
    throw std::out_of_range("Prepared DCode result field is missing.");
}

} // namespace DARTWIC::API
