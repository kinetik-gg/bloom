#pragma once

#include <bloom/commands/operation.hpp>
#include <bloom/scripting/value.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::scripting {

enum class ValueKind : std::uint8_t {
    Boolean,
    Integer,
    Double,
    String,
    Vec2,
    Vec3,
    Color4,
    Id,
    Array,
};

struct ArgumentSchema final {
    std::string name;
    ValueKind kind = ValueKind::String;
    bool required = true;
};

struct OperationSchema final {
    std::string typeId;
    std::vector<ArgumentSchema> arguments;
};

struct OperationDiagnostic final {
    std::string code;
    std::string operationId;
    std::string argument;
    std::string message;
};

class [[nodiscard]] OperationCreateResult final {
  public:
    OperationCreateResult(std::unique_ptr<commands::Operation> operation,
                          std::optional<OperationDiagnostic> diagnostic) noexcept
        : operation_(std::move(operation)), diagnostic_(std::move(diagnostic)) {}
    OperationCreateResult(OperationCreateResult&&) noexcept = default;
    OperationCreateResult& operator=(OperationCreateResult&&) noexcept = default;
    OperationCreateResult(const OperationCreateResult&) = delete;
    OperationCreateResult& operator=(const OperationCreateResult&) = delete;
    ~OperationCreateResult() = default;

    [[nodiscard]] explicit operator bool() const noexcept { return operation_ != nullptr; }
    [[nodiscard]] const OperationDiagnostic* diagnostic() const noexcept {
        return diagnostic_.has_value() ? &*diagnostic_ : nullptr;
    }
    [[nodiscard]] std::unique_ptr<commands::Operation> takeOperation() && noexcept {
        return std::move(operation_);
    }

  private:
    std::unique_ptr<commands::Operation> operation_;
    std::optional<OperationDiagnostic> diagnostic_;
};

using OperationFactory = std::function<OperationCreateResult(const std::string&, const Arguments&)>;

struct OperationDescriptor final {
    OperationSchema schema;
    OperationFactory factory;
};

class OperationRegistry final {
  public:
    OperationRegistry() = default;
    OperationRegistry(const OperationRegistry&) = delete;
    OperationRegistry& operator=(const OperationRegistry&) = delete;
    OperationRegistry(OperationRegistry&&) noexcept = default;
    OperationRegistry& operator=(OperationRegistry&&) noexcept = default;
    ~OperationRegistry() = default;

    [[nodiscard]] static OperationRegistry builtIn();

    [[nodiscard]] const OperationDescriptor* find(std::string_view typeId) const noexcept;
    [[nodiscard]] std::span<const OperationDescriptor> descriptors() const noexcept {
        return descriptors_;
    }
    [[nodiscard]] OperationCreateResult create(std::string_view typeId,
                                               const Arguments& arguments) const;

  private:
    std::vector<OperationDescriptor> descriptors_;
};

} // namespace bloom::scripting
