#pragma once

#include <bloom/scripting/value.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bloom::scripting {

struct ScriptTransaction final {
    std::string operation;
    Arguments arguments;
};

struct ScriptDiagnostic final {
    std::string code;
    std::size_t byteOffset = 0;
    std::string message;
};

class [[nodiscard]] ScriptParseResult final {
  public:
    ScriptParseResult() = default;
    ScriptParseResult(ScriptParseResult&&) noexcept = default;
    ScriptParseResult& operator=(ScriptParseResult&&) noexcept = default;
    ScriptParseResult(const ScriptParseResult&) = delete;
    ScriptParseResult& operator=(const ScriptParseResult&) = delete;
    ~ScriptParseResult() = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return diagnostic_.has_value() == false;
    }
    [[nodiscard]] std::span<const ScriptTransaction> transactions() const& noexcept {
        return transactions_;
    }
    [[nodiscard]] std::span<const ScriptTransaction> transactions() const&& = delete;
    [[nodiscard]] const ScriptDiagnostic* diagnostic() const noexcept {
        return diagnostic_.has_value() ? &*diagnostic_ : nullptr;
    }

  private:
    friend ScriptParseResult parseScriptJson(std::string_view);
    std::vector<ScriptTransaction> transactions_;
    std::optional<ScriptDiagnostic> diagnostic_;
};

[[nodiscard]] ScriptParseResult parseScriptJson(std::string_view json);

} // namespace bloom::scripting
