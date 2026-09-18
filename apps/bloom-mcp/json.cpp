#include "json.hpp"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace bloom::mcp {
namespace {
constexpr std::size_t kArenaElements = (std::size_t{32} * 1024U * 1024U) / sizeof(std::max_align_t);

yyjson_mut_val* checked(yyjson_mut_val* value) {
    if (!value) {
        throw std::bad_alloc();
    }
    return value;
}

void validate(yyjson_val* value, const std::size_t depth, std::size_t& count) {
    if (depth > 32 || ++count > 65536) {
        throw InvalidInput("JSON exceeds depth or value limits");
    }
    if (yyjson_is_arr(value)) {
        if (yyjson_arr_size(value) > 4096) {
            throw InvalidInput("Array exceeds 4096 items");
        }
        yyjson_arr_iter iterator = yyjson_arr_iter_with(value);
        while (auto* child = yyjson_arr_iter_next(&iterator)) {
            validate(child, depth + 1, count);
        }
    } else if (yyjson_is_obj(value)) {
        std::set<std::string_view> keys;
        yyjson_obj_iter iterator = yyjson_obj_iter_with(value);
        while (auto* key = yyjson_obj_iter_next(&iterator)) {
            const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
            if (!keys.insert(name).second) {
                throw InvalidInput("Duplicate decoded object key");
            }
            validate(yyjson_obj_iter_get_val(key), depth + 1, count);
        }
    }
}
} // namespace

Json::Json() : arena_(kArenaElements), document_(nullptr, yyjson_mut_doc_free) {
    if (!yyjson_alc_pool_init(&allocator_, arena_.data(),
                              arena_.size() * sizeof(std::max_align_t))) {
        throw std::bad_alloc();
    }
    document_.reset(yyjson_mut_doc_new(&allocator_));
    if (!document_) {
        throw std::bad_alloc();
    }
}
yyjson_mut_val* Json::object() { return checked(yyjson_mut_obj(document_.get())); }
yyjson_mut_val* Json::array() { return checked(yyjson_mut_arr(document_.get())); }
yyjson_mut_val* Json::text(const std::string_view value) {
    return checked(yyjson_mut_strncpy(document_.get(), value.data(), value.size()));
}
yyjson_mut_val* Json::number(const std::uint64_t value) {
    return checked(yyjson_mut_uint(document_.get(), value));
}
yyjson_mut_val* Json::signedNumber(const std::int64_t value) {
    return checked(yyjson_mut_sint(document_.get(), value));
}
yyjson_mut_val* Json::real(const double value) {
    return checked(yyjson_mut_real(document_.get(), value));
}
yyjson_mut_val* Json::boolean(const bool value) {
    return checked(yyjson_mut_bool(document_.get(), value));
}
yyjson_mut_val* Json::null() { return checked(yyjson_mut_null(document_.get())); }
yyjson_mut_val* Json::copy(yyjson_val* value) {
    return value ? checked(yyjson_val_mut_copy(document_.get(), value)) : null();
}
void Json::set(yyjson_mut_val* object, const std::string_view key, yyjson_mut_val* value) {
    if (!yyjson_mut_obj_add(object, text(key), checked(value))) {
        throw std::bad_alloc();
    }
}
void Json::append(yyjson_mut_val* array, yyjson_mut_val* value) {
    if (!yyjson_mut_arr_append(array, checked(value))) {
        throw std::bad_alloc();
    }
}
std::string Json::encode(yyjson_mut_val* root) {
    yyjson_mut_doc_set_root(document_.get(), root);
    std::size_t size = 0;
    char* encoded = yyjson_mut_write_opts(document_.get(), 0, &allocator_, &size, nullptr);
    if (!encoded || size > 4U * kRequestLimit) {
        throw InvalidInput("Response exceeds 4 MiB");
    }
    const std::string result(encoded, size);
    allocator_.free(allocator_.ctx, encoded);
    return result;
}

Parsed::Parsed(const std::string_view source)
    : arena_(kArenaElements), document_(nullptr, yyjson_doc_free) {
    if (source.empty() || source.size() > kRequestLimit) {
        throw InvalidInput("Request exceeds 1 MiB or is empty");
    }
    if (!yyjson_alc_pool_init(&allocator_, arena_.data(),
                              arena_.size() * sizeof(std::max_align_t))) {
        throw std::bad_alloc();
    }
    yyjson_read_err error{};
    std::string bytes(source);
    document_.reset(
        yyjson_read_opts(bytes.data(), bytes.size(), YYJSON_READ_NOFLAG, &allocator_, &error));
    if (!document_) {
        throw InvalidInput(error.msg ? error.msg : "Invalid JSON");
    }
    std::size_t count = 0;
    validate(root(), 0, count);
}

void members(yyjson_val* object, const std::initializer_list<std::string_view> allowed,
             const std::initializer_list<std::string_view> required) {
    if (!yyjson_is_obj(object)) {
        throw InvalidInput("Expected an object");
    }
    yyjson_obj_iter iterator = yyjson_obj_iter_with(object);
    while (auto* key = yyjson_obj_iter_next(&iterator)) {
        const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
        if (std::ranges::find(allowed, name) == allowed.end()) {
            throw InvalidInput("Unknown member: " + std::string(name));
        }
    }
    for (const auto name : required) {
        if (!yyjson_obj_getn(object, name.data(), name.size())) {
            throw InvalidInput("Missing member: " + std::string(name));
        }
    }
}
yyjson_val* member(yyjson_val* object, const char* name) { return yyjson_obj_get(object, name); }
std::string string(yyjson_val* value, const std::size_t limit) {
    if (!yyjson_is_str(value) || yyjson_get_len(value) > limit) {
        throw InvalidInput("Expected a bounded string");
    }
    std::string result(yyjson_get_str(value), yyjson_get_len(value));
    if (result.find('\0') != std::string::npos) {
        throw InvalidInput("NUL is not allowed in strings");
    }
    return result;
}
std::uint64_t integer(yyjson_val* value) {
    if (!yyjson_is_uint(value)) {
        throw InvalidInput("Expected a nonnegative integer");
    }
    return yyjson_get_uint(value);
}
bool boolean(yyjson_val* value) {
    if (!yyjson_is_bool(value)) {
        throw InvalidInput("Expected a boolean");
    }
    return yyjson_get_bool(value);
}
} // namespace bloom::mcp
