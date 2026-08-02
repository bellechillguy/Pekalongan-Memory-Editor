#include "value.hpp"

#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace {

template <typename T>
TypedValue pack(ValueType type, T input) {
    TypedValue value;
    value.type = type;
    std::memcpy(value.bytes.data(), &input, sizeof(T));
    return value;
}

template <typename T>
T unpack(const TypedValue& value) {
    T result{};
    std::memcpy(&result, value.bytes.data(), sizeof(T));
    return result;
}

template <typename T>
std::optional<T> parseInteger(std::string_view text, std::string& error) {
    try {
        std::size_t consumed = 0;
        const auto copy = std::string(text);
        if constexpr (std::is_signed_v<T>) {
            long long parsed = std::stoll(copy, &consumed, 0);
            if (consumed != copy.size()) {
                error = "nilai mengandung karakter yang tidak dikenal";
                return std::nullopt;
            }
            if (parsed < static_cast<long long>(std::numeric_limits<T>::min()) ||
                parsed > static_cast<long long>(std::numeric_limits<T>::max())) {
                error = "nilai berada di luar rentang tipe data";
                return std::nullopt;
            }
            return static_cast<T>(parsed);
        } else {
            unsigned long long parsed = std::stoull(copy, &consumed, 0);
            if (consumed != copy.size()) {
                error = "nilai mengandung karakter yang tidak dikenal";
                return std::nullopt;
            }
            if (parsed > static_cast<unsigned long long>(std::numeric_limits<T>::max())) {
                error = "nilai berada di luar rentang tipe data";
                return std::nullopt;
            }
            return static_cast<T>(parsed);
        }
    } catch (const std::exception&) {
        error = "nilai integer tidak valid";
        return std::nullopt;
    }
}

template <typename T>
std::optional<T> parseFloating(std::string_view text, std::string& error) {
    try {
        std::size_t consumed = 0;
        const auto copy = std::string(text);
        long double parsed = std::stold(copy, &consumed);
        if (consumed != copy.size() || !std::isfinite(parsed)) {
            error = "nilai floating point tidak valid";
            return std::nullopt;
        }
        if (parsed < -std::numeric_limits<T>::max() ||
            parsed > std::numeric_limits<T>::max()) {
            error = "nilai berada di luar rentang tipe data";
            return std::nullopt;
        }
        return static_cast<T>(parsed);
    } catch (const std::exception&) {
        error = "nilai floating point tidak valid";
        return std::nullopt;
    }
}

template <typename T>
std::string formatFloating(T number) {
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<T>::max_digits10) << number;
    return output.str();
}

}  // namespace

std::size_t TypedValue::size() const {
    switch (type) {
        case ValueType::Int8:
        case ValueType::UInt8:
            return 1;
        case ValueType::Int16:
        case ValueType::UInt16:
            return 2;
        case ValueType::Int32:
        case ValueType::UInt32:
        case ValueType::Float:
            return 4;
        case ValueType::Int64:
        case ValueType::UInt64:
        case ValueType::Double:
            return 8;
    }
    return 0;
}

std::string valueTypeName(ValueType type) {
    switch (type) {
        case ValueType::Int8:
            return "i8";
        case ValueType::UInt8:
            return "u8";
        case ValueType::Int16:
            return "i16";
        case ValueType::UInt16:
            return "u16";
        case ValueType::Int32:
            return "i32";
        case ValueType::UInt32:
            return "u32";
        case ValueType::Int64:
            return "i64";
        case ValueType::UInt64:
            return "u64";
        case ValueType::Float:
            return "f32";
        case ValueType::Double:
            return "f64";
    }
    return "unknown";
}

std::optional<ValueType> parseValueType(std::string_view text) {
    if (text == "i8" || text == "int8") {
        return ValueType::Int8;
    }
    if (text == "u8" || text == "uint8") {
        return ValueType::UInt8;
    }
    if (text == "i16" || text == "int16") {
        return ValueType::Int16;
    }
    if (text == "u16" || text == "uint16") {
        return ValueType::UInt16;
    }
    if (text == "i32" || text == "int32") {
        return ValueType::Int32;
    }
    if (text == "u32" || text == "uint32") {
        return ValueType::UInt32;
    }
    if (text == "i64" || text == "int64") {
        return ValueType::Int64;
    }
    if (text == "u64" || text == "uint64") {
        return ValueType::UInt64;
    }
    if (text == "f32" || text == "float") {
        return ValueType::Float;
    }
    if (text == "f64" || text == "double") {
        return ValueType::Double;
    }
    return std::nullopt;
}

std::optional<TypedValue> parseValue(ValueType type,
                                     std::string_view text,
                                     std::string& error) {
    switch (type) {
        case ValueType::Int8: {
            const auto parsed = parseInteger<std::int8_t>(text, error);
            return parsed ? std::optional(pack(type, *parsed)) : std::nullopt;
        }
        case ValueType::UInt8: {
            const auto parsed = parseInteger<std::uint8_t>(text, error);
            return parsed ? std::optional(pack(type, *parsed)) : std::nullopt;
        }
        case ValueType::Int16: {
            const auto parsed = parseInteger<std::int16_t>(text, error);
            return parsed ? std::optional(pack(type, *parsed)) : std::nullopt;
        }
        case ValueType::UInt16: {
            const auto parsed = parseInteger<std::uint16_t>(text, error);
            return parsed ? std::optional(pack(type, *parsed)) : std::nullopt;
        }
        case ValueType::Int32: {
            const auto parsed = parseInteger<std::int32_t>(text, error);
            return parsed ? std::optional(pack(type, *parsed)) : std::nullopt;
        }
        case ValueType::UInt32: {
            const auto parsed = parseInteger<std::uint32_t>(text, error);
            return parsed ? std::optional(pack(type, *parsed)) : std::nullopt;
        }
        case ValueType::Int64: {
            const auto parsed = parseInteger<std::int64_t>(text, error);
            return parsed ? std::optional(pack(type, *parsed)) : std::nullopt;
        }
        case ValueType::UInt64: {
            const auto parsed = parseInteger<std::uint64_t>(text, error);
            return parsed ? std::optional(pack(type, *parsed)) : std::nullopt;
        }
        case ValueType::Float: {
            const auto parsed = parseFloating<float>(text, error);
            return parsed ? std::optional(pack(type, *parsed)) : std::nullopt;
        }
        case ValueType::Double: {
            const auto parsed = parseFloating<double>(text, error);
            return parsed ? std::optional(pack(type, *parsed)) : std::nullopt;
        }
    }
    error = "tipe data tidak dikenal";
    return std::nullopt;
}

std::string formatValue(const TypedValue& value) {
    switch (value.type) {
        case ValueType::Int8:
            return std::to_string(unpack<std::int8_t>(value));
        case ValueType::UInt8:
            return std::to_string(unpack<std::uint8_t>(value));
        case ValueType::Int16:
            return std::to_string(unpack<std::int16_t>(value));
        case ValueType::UInt16:
            return std::to_string(unpack<std::uint16_t>(value));
        case ValueType::Int32:
            return std::to_string(unpack<std::int32_t>(value));
        case ValueType::UInt32:
            return std::to_string(unpack<std::uint32_t>(value));
        case ValueType::Int64:
            return std::to_string(unpack<std::int64_t>(value));
        case ValueType::UInt64:
            return std::to_string(unpack<std::uint64_t>(value));
        case ValueType::Float:
            return formatFloating(unpack<float>(value));
        case ValueType::Double:
            return formatFloating(unpack<double>(value));
    }
    return "?";
}

bool valuesEqual(const TypedValue& left, const TypedValue& right) {
    return left.type == right.type &&
           std::memcmp(left.bytes.data(), right.bytes.data(), left.size()) == 0;
}

bool valueIncreased(const TypedValue& previous, const TypedValue& current) {
    if (previous.type != current.type) {
        return false;
    }
    switch (previous.type) {
        case ValueType::Int8:
            return unpack<std::int8_t>(current) > unpack<std::int8_t>(previous);
        case ValueType::UInt8:
            return unpack<std::uint8_t>(current) > unpack<std::uint8_t>(previous);
        case ValueType::Int16:
            return unpack<std::int16_t>(current) > unpack<std::int16_t>(previous);
        case ValueType::UInt16:
            return unpack<std::uint16_t>(current) > unpack<std::uint16_t>(previous);
        case ValueType::Int32:
            return unpack<std::int32_t>(current) > unpack<std::int32_t>(previous);
        case ValueType::UInt32:
            return unpack<std::uint32_t>(current) > unpack<std::uint32_t>(previous);
        case ValueType::Int64:
            return unpack<std::int64_t>(current) > unpack<std::int64_t>(previous);
        case ValueType::UInt64:
            return unpack<std::uint64_t>(current) > unpack<std::uint64_t>(previous);
        case ValueType::Float:
            return unpack<float>(current) > unpack<float>(previous);
        case ValueType::Double:
            return unpack<double>(current) > unpack<double>(previous);
    }
    return false;
}

bool valueDecreased(const TypedValue& previous, const TypedValue& current) {
    if (previous.type != current.type) {
        return false;
    }
    switch (previous.type) {
        case ValueType::Int8:
            return unpack<std::int8_t>(current) < unpack<std::int8_t>(previous);
        case ValueType::UInt8:
            return unpack<std::uint8_t>(current) < unpack<std::uint8_t>(previous);
        case ValueType::Int16:
            return unpack<std::int16_t>(current) < unpack<std::int16_t>(previous);
        case ValueType::UInt16:
            return unpack<std::uint16_t>(current) < unpack<std::uint16_t>(previous);
        case ValueType::Int32:
            return unpack<std::int32_t>(current) < unpack<std::int32_t>(previous);
        case ValueType::UInt32:
            return unpack<std::uint32_t>(current) < unpack<std::uint32_t>(previous);
        case ValueType::Int64:
            return unpack<std::int64_t>(current) < unpack<std::int64_t>(previous);
        case ValueType::UInt64:
            return unpack<std::uint64_t>(current) < unpack<std::uint64_t>(previous);
        case ValueType::Float:
            return unpack<float>(current) < unpack<float>(previous);
        case ValueType::Double:
            return unpack<double>(current) < unpack<double>(previous);
    }
    return false;
}

