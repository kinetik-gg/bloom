#pragma once

#include <yyjson.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::mcp {

struct InvalidInput final : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline constexpr std::size_t kRequestLimit = std::size_t{1024} * 1024U;

class Json final {
  public:
    Json();
    [[nodiscard]] yyjson_mut_val* object();
    [[nodiscard]] yyjson_mut_val* array();
    [[nodiscard]] yyjson_mut_val* text(std::string_view value);
    [[nodiscard]] yyjson_mut_val* number(std::uint64_t value);
    [[nodiscard]] yyjson_mut_val* signedNumber(std::int64_t value);
    [[nodiscard]] yyjson_mut_val* real(double value);
    [[nodiscard]] yyjson_mut_val* boolean(bool value);
    [[nodiscard]] yyjson_mut_val* null();
    [[nodiscard]] yyjson_mut_val* copy(yyjson_val* value);
    void set(yyjson_mut_val* object, std::string_view key, yyjson_mut_val* value);
    void append(yyjson_mut_val* array, yyjson_mut_val* value);
    [[nodiscard]] std::string encode(yyjson_mut_val* root);

  private:
    std::vector<std::max_align_t> arena_;
    yyjson_alc allocator_{};
    std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> document_;
};

class Parsed final {
  public:
    explicit Parsed(std::string_view source);
    [[nodiscard]] yyjson_val* root() const noexcept { return yyjson_doc_get_root(document_.get()); }

  private:
    std::vector<std::max_align_t> arena_;
    yyjson_alc allocator_{};
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> document_;
};

void members(yyjson_val* object, std::initializer_list<std::string_view> allowed,
             std::initializer_list<std::string_view> required = {});
[[nodiscard]] yyjson_val* member(yyjson_val* object, const char* name);
[[nodiscard]] std::string string(yyjson_val* value, std::size_t limit = 4096);
[[nodiscard]] std::uint64_t integer(yyjson_val* value);
[[nodiscard]] bool boolean(yyjson_val* value);

} // namespace bloom::mcp
