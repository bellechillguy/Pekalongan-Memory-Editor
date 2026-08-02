#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class ValueType {
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float,
    Double,
};

struct TypedValue {
    ValueType type{ValueType::Int32};
    std::array<std::byte, 8> bytes{};

    [[nodiscard]] std::size_t size() const;
};

[[nodiscard]] std::string valueTypeName(ValueType type);
[[nodiscard]] std::optional<ValueType> parseValueType(std::string_view text);
[[nodiscard]] std::optional<TypedValue> parseValue(ValueType type,
                                                    std::string_view text,
                                                    std::string& error);
[[nodiscard]] std::string formatValue(const TypedValue& value);
[[nodiscard]] bool valuesEqual(const TypedValue& left,
                               const TypedValue& right);
[[nodiscard]] bool valueIncreased(const TypedValue& previous,
                                  const TypedValue& current);
[[nodiscard]] bool valueDecreased(const TypedValue& previous,
                                  const TypedValue& current);

