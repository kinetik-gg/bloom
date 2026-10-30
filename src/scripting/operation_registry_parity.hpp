#pragma once

// The MCP parity operations: user edits the desktop UI makes through a menu, panel, drop or dialog
// that had no registry operation. Each builds the same command the UI issues, so an agent's edit
// has the UI's validation and undo, and each carries a full description. The families live in
// their own files (operation_registry_parity_*.cpp); this header is their shared schema and
// argument-reading vocabulary.

#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/scripting/operation_registry.hpp>

#include "operation_values.hpp"

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bloom::scripting::detail::parity {

// A required argument the shortest working call names.
[[nodiscard]] inline ArgumentSchema need(std::string name, const ValueKind kind, Value example,
                                         std::string summary) {
    return {.name = std::move(name),
            .kind = kind,
            .required = true,
            .inExample = true,
            .context = ContextSource::None,
            .example = std::move(example),
            .summary = std::move(summary)};
}

// An optional argument; `inExample` shows it in the example call when it is the ordinary one of a
// set of alternatives.
[[nodiscard]] inline ArgumentSchema maybe(std::string name, const ValueKind kind, Value example,
                                          std::string summary, const bool inExample = false) {
    return {.name = std::move(name),
            .kind = kind,
            .required = false,
            .inExample = inExample,
            .context = ContextSource::None,
            .example = std::move(example),
            .summary = std::move(summary)};
}

// A required argument the live session answers when it is omitted.
[[nodiscard]] inline ArgumentSchema fromContext(std::string name, const ValueKind kind,
                                                const ContextSource source, Value example,
                                                std::string summary) {
    return {.name = std::move(name),
            .kind = kind,
            .required = true,
            .inExample = false,
            .context = source,
            .example = std::move(example),
            .summary = std::move(summary)};
}

[[nodiscard]] inline ArgumentSchema composition() {
    return fromContext("composition", ValueKind::Id, ContextSource::Composition, StableId{1},
                       "Id. The composition to edit. Defaults to the current composition.");
}

[[nodiscard]] inline OperationCreateResult made(std::unique_ptr<commands::Operation> operation) {
    return OperationCreateResult(std::move(operation), std::nullopt);
}

inline void add(std::vector<OperationDescriptor>& descriptors, std::string id,
                std::string description, std::vector<ArgumentSchema> arguments,
                OperationFactory factory) {
    descriptors.push_back({.schema = {.typeId = std::move(id),
                                      .arguments = std::move(arguments),
                                      .description = std::move(description)},
                           .factory = std::move(factory),
                           .contextualFactory = {}});
}

// Reads arguments for one operation and keeps the first rejection, so a factory reads every
// argument and then checks `failed()` once.
class Reader final {
  public:
    Reader(const std::string& operation, const Arguments& arguments)
        : operation_(operation), arguments_(arguments) {}

    [[nodiscard]] bool failed() const noexcept { return static_cast<bool>(error_.diagnostic()); }
    [[nodiscard]] OperationCreateResult error() {
        return std::exchange(error_, OperationCreateResult(nullptr, std::nullopt));
    }
    [[nodiscard]] bool has(const std::string& name) const { return arguments_.contains(name); }

    void reject(const std::string& name, std::string message) {
        if (!failed())
            error_ = argumentFailure(operation_, name, std::move(message));
    }

    template <typename Id>
    [[nodiscard]] Id id(const std::string& name, const bool required = true) {
        const auto* value = find(name);
        if (value == nullptr) {
            if (required)
                reject(name, "is required");
            return Id{};
        }
        const auto raw = idOf(*value);
        if (!raw) {
            reject(name, "expects a positive stable id");
            return Id{};
        }
        return Id::fromRaw(*raw);
    }

    template <typename Id>
    [[nodiscard]] std::vector<Id> ids(const std::string& name, const bool required = true) {
        std::vector<Id> result;
        const auto* value = find(name);
        if (value == nullptr) {
            if (required)
                reject(name, "is required");
            return result;
        }
        const auto* array = std::get_if<ValueArray>(&value->storage);
        if (array == nullptr) {
            reject(name, "expects an array of positive stable ids");
            return result;
        }
        for (const auto& item : *array) {
            const auto raw = idOf(item);
            if (!raw) {
                reject(name, "expects an array of positive stable ids");
                return {};
            }
            result.push_back(Id::fromRaw(*raw));
        }
        return result;
    }

    [[nodiscard]] std::optional<std::string> text(const std::string& name) {
        const auto* value = find(name);
        if (value == nullptr)
            return std::nullopt;
        const auto* result = std::get_if<std::string>(&value->storage);
        if (result == nullptr)
            reject(name, "expects text");
        return result ? std::optional(*result) : std::nullopt;
    }

    [[nodiscard]] std::optional<bool> boolean(const std::string& name) {
        const auto* value = find(name);
        if (value == nullptr)
            return std::nullopt;
        const auto* result = std::get_if<bool>(&value->storage);
        if (result == nullptr)
            reject(name, "expects true or false");
        return result ? std::optional(*result) : std::nullopt;
    }

    [[nodiscard]] std::optional<std::int64_t> integer(const std::string& name) {
        const auto* value = find(name);
        if (value == nullptr)
            return std::nullopt;
        const auto result = integerOf(*value);
        if (!result)
            reject(name, "expects a whole number");
        return result;
    }

    [[nodiscard]] std::optional<double> number(const std::string& name) {
        const auto* value = find(name);
        if (value == nullptr)
            return std::nullopt;
        const auto result = numericOf(*value);
        if (!result || !std::isfinite(*result)) {
            reject(name, "expects a finite number");
            return std::nullopt;
        }
        return result;
    }

    [[nodiscard]] std::optional<core::RationalTime> time(const std::string& name) {
        const auto* value = find(name);
        if (value == nullptr)
            return std::nullopt;
        const auto result = rationalOf(*value);
        if (!result)
            reject(name, "expects a whole number or an exact [numerator, denominator] time");
        return result;
    }

    [[nodiscard]] std::optional<document::Vec2d> vec2(const std::string& name) {
        const auto* value = find(name);
        if (value == nullptr)
            return std::nullopt;
        if (const auto* point = std::get_if<document::Vec2d>(&value->storage);
            point != nullptr && std::isfinite(point->x) && std::isfinite(point->y))
            return *point;
        if (const auto* array = std::get_if<ValueArray>(&value->storage);
            array != nullptr && array->size() == 2) {
            const auto x = numericOf((*array)[0]);
            const auto y = numericOf((*array)[1]);
            if (x && y && std::isfinite(*x) && std::isfinite(*y))
                return document::Vec2d{*x, *y};
        }
        reject(name, "expects two finite numbers as [x, y]");
        return std::nullopt;
    }

    // A vec2, vec3 or colour: 2, 3 or 4 finite numbers.
    [[nodiscard]] std::optional<document::ParameterValue> vector(const std::string& name) {
        const auto* value = find(name);
        if (value == nullptr)
            return std::nullopt;
        if (const auto* held = std::get_if<document::Vec2d>(&value->storage))
            return *held;
        if (const auto* held = std::get_if<document::Vec3d>(&value->storage))
            return *held;
        if (const auto* held = std::get_if<core::Color4d>(&value->storage))
            return *held;
        const auto* array = std::get_if<ValueArray>(&value->storage);
        std::vector<double> numbers;
        if (array != nullptr)
            for (const auto& item : *array)
                if (const auto number = numericOf(item); number && std::isfinite(*number))
                    numbers.push_back(*number);
        if (array == nullptr || numbers.size() != array->size() || numbers.size() < 2 ||
            numbers.size() > 4) {
            reject(name, "expects 2, 3 or 4 finite numbers");
            return std::nullopt;
        }
        if (numbers.size() == 2)
            return document::Vec2d{numbers[0], numbers[1]};
        if (numbers.size() == 3)
            return document::Vec3d{numbers[0], numbers[1], numbers[2]};
        return core::Color4d{numbers[0], numbers[1], numbers[2], numbers[3]};
    }

  private:
    [[nodiscard]] const Value* find(const std::string& name) const {
        const auto found = arguments_.find(name);
        return found == arguments_.end() ? nullptr : &found->second;
    }

    const std::string& operation_;
    const Arguments& arguments_;
    OperationCreateResult error_{nullptr, std::nullopt};
};

// Registers bloom.layer.delete, duplicate and add-from-assets, and the Node canvas's compound
// adds (asset and composition sources, connected adds, reroutes).
void addLayerAndNodeOperations(std::vector<OperationDescriptor>& descriptors);
// Registers the Assets panel's delete and New Composition from Selection, and the Properties
// panel's reset, typed node-parameter values and video audio stream.
void addProjectOperations(std::vector<OperationDescriptor>& descriptors);

} // namespace bloom::scripting::detail::parity
