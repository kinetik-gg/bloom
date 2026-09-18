#pragma once

#include <bloom/core/color.hpp>
#include <bloom/document/graph.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::scripting {

struct StableId final {
    std::uint64_t value = 0;

    friend bool operator==(const StableId&, const StableId&) = default;
};

struct Value;
using ValueArray = std::vector<Value>;

using ValueStorage = std::variant<bool, std::int64_t, double, std::string, document::Vec2d,
                                  document::Vec3d, core::Color4d, StableId, ValueArray>;

struct Value final {
    ValueStorage storage;

    Value() : storage(false) {}
    Value(bool value) : storage(value) {}
    Value(std::int64_t value) : storage(value) {}
    Value(int value) : storage(static_cast<std::int64_t>(value)) {}
    Value(double value) : storage(value) {}
    Value(std::string value) : storage(std::move(value)) {}
    Value(const char* value) : storage(std::string(value)) {}
    Value(document::Vec2d value) : storage(value) {}
    Value(document::Vec3d value) : storage(value) {}
    Value(core::Color4d value) : storage(value) {}
    Value(StableId value) : storage(value) {}
    Value(ValueArray value) : storage(std::move(value)) {}
};

using Arguments = std::map<std::string, Value, std::less<>>;

} // namespace bloom::scripting
