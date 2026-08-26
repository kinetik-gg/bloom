#include <bloom/project/strict_json_dom.hpp>

#include "strict_json_preflight.hpp"

#include <yyjson.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace bloom::project {

JsonValue::JsonValue(std::pmr::memory_resource* const resource) noexcept
    : text_(resource), elements_(resource), members_(resource) {}

std::optional<bool> JsonValue::asBoolean() const noexcept {
    if (kind_ != JsonValueKind::Boolean) {
        return std::nullopt;
    }
    return boolean_;
}

std::optional<std::string_view> JsonValue::asNumberToken() const noexcept {
    if (kind_ != JsonValueKind::Number) {
        return std::nullopt;
    }
    return std::string_view{text_};
}

std::optional<std::string_view> JsonValue::asString() const noexcept {
    if (kind_ != JsonValueKind::String) {
        return std::nullopt;
    }
    return std::string_view{text_};
}

std::span<const JsonValue> JsonValue::arrayElements() const noexcept {
    if (kind_ != JsonValueKind::Array) {
        return {};
    }
    return {elements_.data(), elements_.size()};
}

std::span<const JsonMember> JsonValue::objectMembers() const noexcept {
    if (kind_ != JsonValueKind::Object) {
        return {};
    }
    return {members_.data(), members_.size()};
}

const JsonValue* JsonValue::findMember(const std::string_view key) const noexcept {
    if (kind_ != JsonValueKind::Object) {
        return nullptr;
    }
    for (const auto& member : members_) {
        if (member.key() == key) {
            return &member.value();
        }
    }
    return nullptr;
}

JsonMember::JsonMember(std::pmr::memory_resource* const resource, const std::string_view key)
    : key_(key.data(), key.size(), resource), value_(resource) {}

StrictJsonDomDocument::StrictJsonDomDocument(std::unique_ptr<ProjectIoMemoryResource> resource,
                                             JsonValue root) noexcept
    : resource_(std::move(resource)), root_(std::move(root)) {}

} // namespace bloom::project

namespace bloom::project::detail {

namespace {

static_assert(kStrictJsonDomMaximumInputBytes == kStrictJsonDocumentMaximumInputBytes);
static_assert(kStrictJsonDomMaximumDepth == kStrictJsonMaximumDepth);
static_assert(kStrictJsonDomMaximumValues == kStrictJsonMaximumValues);
static_assert(kStrictJsonDomMaximumContainerEntries == kStrictJsonMaximumContainerEntries);
static_assert(kStrictJsonDomMaximumDecodedStringBytes == kStrictJsonMaximumDecodedStringBytes);

[[nodiscard]] StrictJsonDomError
translatePreflightError(const StrictJsonPreflightError error) noexcept {
    switch (error) {
    case StrictJsonPreflightError::None:
        return StrictJsonDomError::None;
    case StrictJsonPreflightError::InvalidLimits:
        return StrictJsonDomError::InvalidLimits;
    case StrictJsonPreflightError::InputTooLarge:
        return StrictJsonDomError::InputTooLarge;
    case StrictJsonPreflightError::BomForbidden:
        return StrictJsonDomError::BomForbidden;
    case StrictJsonPreflightError::EmptyInput:
        return StrictJsonDomError::EmptyInput;
    case StrictJsonPreflightError::InvalidUtf8:
        return StrictJsonDomError::InvalidUtf8;
    case StrictJsonPreflightError::InvalidSyntax:
        return StrictJsonDomError::InvalidSyntax;
    case StrictJsonPreflightError::InvalidEscape:
        return StrictJsonDomError::InvalidEscape;
    case StrictJsonPreflightError::InvalidUnicodeScalar:
        return StrictJsonDomError::InvalidUnicodeScalar;
    case StrictJsonPreflightError::InvalidNumber:
        return StrictJsonDomError::InvalidNumber;
    case StrictJsonPreflightError::TrailingData:
        return StrictJsonDomError::TrailingData;
    case StrictJsonPreflightError::DepthLimitExceeded:
        return StrictJsonDomError::DepthLimitExceeded;
    case StrictJsonPreflightError::ValueLimitExceeded:
        return StrictJsonDomError::ValueLimitExceeded;
    case StrictJsonPreflightError::ContainerEntryLimitExceeded:
        return StrictJsonDomError::ContainerEntryLimitExceeded;
    case StrictJsonPreflightError::DecodedStringLimitExceeded:
        return StrictJsonDomError::DecodedStringLimitExceeded;
    case StrictJsonPreflightError::SizeOverflow:
        return StrictJsonDomError::SizeOverflow;
    case StrictJsonPreflightError::Cancelled:
        return StrictJsonDomError::ParseFailed;
    }
    return StrictJsonDomError::ParseFailed;
}

struct PathSegment final {
    bool isIndex = false;
    std::string_view key;
    std::size_t index = 0;
};

// A fixed-capacity stack of path segments from the document root to the value currently being
// built. Sized to the fixed v1 depth ceiling, which every accepted `StrictJsonDomLimits` is
// checked against before this stack is used.
class PathStack final {
  public:
    void pushKey(const std::string_view key) noexcept {
        entries_[size_] = PathSegment{.isIndex = false, .key = key, .index = 0};
        ++size_;
    }
    void pushIndex(const std::size_t index) noexcept {
        entries_[size_] = PathSegment{.isIndex = true, .key = {}, .index = index};
        ++size_;
    }
    void pop() noexcept { --size_; }
    [[nodiscard]] std::span<const PathSegment> segments() const noexcept {
        return {entries_.data(), size_};
    }

  private:
    std::array<PathSegment, kStrictJsonDomMaximumDepth> entries_{};
    std::size_t size_ = 0;
};

} // namespace

// All yyjson use is confined to this translation unit; no yyjson type crosses strict_json_dom.hpp.
class StrictJsonDomBuilder final {
  public:
    [[nodiscard]] static StrictJsonDomResult parse(std::span<const std::byte> input,
                                                   const StrictJsonDomLimits& limits,
                                                   ProjectIoOperationMemory operation) noexcept;

  private:
    // Not noexcept: the DOM walk copies decoded tokens/keys into PMR-owned containers using
    // ordinary std::pmr::string/vector/unordered_set operations, which allocate through
    // ProjectIoMemoryResource's throwing memory_resource surface (do_allocate). A budget
    // rejection here must propagate as std::bad_alloc to parse()'s try/catch, which reports
    // ResourceExhausted; the allocator adapters used only by yyjson's C callbacks are the
    // noexcept ones (checkedAllocate), not this walk.
    [[nodiscard]] static bool buildValue(yyjson_val* value, JsonValue& out, PathStack& path,
                                         std::pmr::memory_resource* resource,
                                         StrictJsonDomPathText& errorPath);
    [[nodiscard]] static StrictJsonDomPathText formatPath(const PathStack& stack,
                                                          std::string_view finalKey) noexcept;

    [[nodiscard]] static StrictJsonDomResult makeFailure(StrictJsonDomError error,
                                                         std::size_t byteOffset) noexcept;
    [[nodiscard]] static StrictJsonDomResult makeSuccess(StrictJsonDomDocument document) noexcept;
    [[nodiscard]] static StrictJsonDomResult
    makeDuplicateKeyFailure(StrictJsonDomPathText path) noexcept;

    static void* allocatorMalloc(void* ctx, std::size_t size) noexcept;
    static void* allocatorRealloc(void* ctx, void* ptr, std::size_t oldSize,
                                  std::size_t size) noexcept;
    static void allocatorFree(void* ctx, void* ptr) noexcept;
};

StrictJsonDomResult StrictJsonDomBuilder::parse(const std::span<const std::byte> input,
                                                const StrictJsonDomLimits& limits,
                                                ProjectIoOperationMemory operation) noexcept {
    if (limits.maximumInputBytes > kStrictJsonDocumentMaximumInputBytes ||
        limits.maximumValues > kStrictJsonMaximumValues ||
        limits.maximumContainerEntries > kStrictJsonMaximumContainerEntries ||
        limits.maximumDepth > kStrictJsonMaximumDepth ||
        limits.maximumDecodedStringBytes > kStrictJsonMaximumDecodedStringBytes) {
        return makeFailure(StrictJsonDomError::InvalidLimits, 0);
    }

    const StrictJsonPreflightLimits preflightLimits{
        .maximumInputBytes = limits.maximumInputBytes,
        .maximumValues = limits.maximumValues,
        .maximumContainerEntries = limits.maximumContainerEntries,
        .maximumDepth = limits.maximumDepth,
        .maximumDecodedStringBytes = limits.maximumDecodedStringBytes,
    };
    const auto preflight = preflightStrictJson(input, preflightLimits);
    if (!preflight.succeeded()) {
        return makeFailure(translatePreflightError(preflight.error), preflight.errorOffset);
    }

    std::unique_ptr<ProjectIoMemoryResource> resource;
    try {
        resource = std::make_unique<ProjectIoMemoryResource>(std::move(operation));
    } catch (const std::bad_alloc&) {
        return makeFailure(StrictJsonDomError::ResourceExhausted, 0);
    }

    const yyjson_alc allocator{&allocatorMalloc, &allocatorRealloc, &allocatorFree, resource.get()};
    yyjson_read_err readError{};
    // NOFLAG plus NUMBER_AS_RAW: strict RFC 8259 grammar (matching the preceding preflight scan)
    // with every number preserved as its exact source token. `input` is not modified because
    // YYJSON_READ_INSITU is not set.
    yyjson_doc* const doc =
        yyjson_read_opts(const_cast<char*>(reinterpret_cast<const char*>(input.data())),
                         input.size(), YYJSON_READ_NUMBER_AS_RAW, &allocator, &readError);
    if (doc == nullptr) {
        const auto error = readError.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
                               ? StrictJsonDomError::ResourceExhausted
                               : StrictJsonDomError::ParseFailed;
        return makeFailure(error, readError.pos);
    }

    struct DocGuard final {
        yyjson_doc* doc;
        ~DocGuard() { yyjson_doc_free(doc); }
    } docGuard{doc};

    try {
        PathStack path;
        JsonValue root(resource.get());
        StrictJsonDomPathText errorPath;
        if (!buildValue(yyjson_doc_get_root(doc), root, path, resource.get(), errorPath)) {
            return makeDuplicateKeyFailure(errorPath);
        }
        StrictJsonDomDocument document(std::move(resource), std::move(root));
        return makeSuccess(std::move(document));
    } catch (const std::bad_alloc&) {
        return makeFailure(StrictJsonDomError::ResourceExhausted, 0);
    } catch (...) {
        return makeFailure(StrictJsonDomError::ParseFailed, 0);
    }
}

bool StrictJsonDomBuilder::buildValue(yyjson_val* const value, JsonValue& out, PathStack& path,
                                      std::pmr::memory_resource* const resource,
                                      StrictJsonDomPathText& errorPath) {
    switch (yyjson_get_type(value)) {
    case YYJSON_TYPE_NULL:
        out.kind_ = JsonValueKind::Null;
        return true;
    case YYJSON_TYPE_BOOL:
        out.kind_ = JsonValueKind::Boolean;
        out.boolean_ = yyjson_is_true(value);
        return true;
    case YYJSON_TYPE_RAW:
        out.kind_ = JsonValueKind::Number;
        out.text_.assign(yyjson_get_raw(value), yyjson_get_len(value));
        return true;
    case YYJSON_TYPE_STR:
        out.kind_ = JsonValueKind::String;
        out.text_.assign(yyjson_get_str(value), yyjson_get_len(value));
        return true;
    case YYJSON_TYPE_ARR: {
        out.kind_ = JsonValueKind::Array;
        out.elements_.reserve(yyjson_arr_size(value));
        yyjson_arr_iter iter{};
        yyjson_arr_iter_init(value, &iter);
        std::size_t index = 0;
        for (yyjson_val* element = yyjson_arr_iter_next(&iter); element != nullptr;
             element = yyjson_arr_iter_next(&iter)) {
            out.elements_.emplace_back(resource);
            path.pushIndex(index);
            const bool built = buildValue(element, out.elements_.back(), path, resource, errorPath);
            path.pop();
            if (!built) {
                return false;
            }
            ++index;
        }
        return true;
    }
    case YYJSON_TYPE_OBJ: {
        out.kind_ = JsonValueKind::Object;
        const auto count = yyjson_obj_size(value);
        out.members_.reserve(count);
        std::pmr::unordered_set<std::string_view> seenKeys(resource);
        seenKeys.reserve(count);
        yyjson_obj_iter iter{};
        yyjson_obj_iter_init(value, &iter);
        for (yyjson_val* key = yyjson_obj_iter_next(&iter); key != nullptr;
             key = yyjson_obj_iter_next(&iter)) {
            yyjson_val* const memberValue = yyjson_obj_iter_get_val(key);
            const std::string_view decodedKey(yyjson_get_str(key), yyjson_get_len(key));
            if (!seenKeys.insert(decodedKey).second) {
                errorPath = formatPath(path, decodedKey);
                return false;
            }
            out.members_.emplace_back(resource, decodedKey);
            path.pushKey(decodedKey);
            const bool built =
                buildValue(memberValue, out.members_.back().value_, path, resource, errorPath);
            path.pop();
            if (!built) {
                return false;
            }
        }
        return true;
    }
    default:
        // YYJSON_TYPE_NONE is unreachable: the preceding preflight scan already proved `input`
        // holds exactly one well-formed root value.
        return true;
    }
}

StrictJsonDomPathText StrictJsonDomBuilder::formatPath(const PathStack& stack,
                                                       const std::string_view finalKey) noexcept {
    StrictJsonDomPathText text;
    const auto appendSlice = [&text](const std::string_view piece) noexcept {
        if (text.truncated_) {
            return;
        }
        const auto available = text.chars_.size() - text.size_;
        const auto copyBytes = std::min(available, piece.size());
        std::memcpy(text.chars_.data() + text.size_, piece.data(), copyBytes);
        text.size_ = static_cast<std::uint16_t>(text.size_ + copyBytes);
        if (copyBytes < piece.size()) {
            text.truncated_ = true;
        }
    };

    for (const auto& segment : stack.segments()) {
        appendSlice("/");
        if (segment.isIndex) {
            std::array<char, 24> buffer{};
            const auto conversion =
                std::to_chars(buffer.data(), buffer.data() + buffer.size(), segment.index);
            appendSlice(std::string_view(buffer.data(),
                                         static_cast<std::size_t>(conversion.ptr - buffer.data())));
        } else {
            appendSlice(segment.key);
        }
    }
    appendSlice("/");
    appendSlice(finalKey);
    return text;
}

StrictJsonDomResult StrictJsonDomBuilder::makeFailure(const StrictJsonDomError error,
                                                      const std::size_t byteOffset) noexcept {
    StrictJsonDomResult result;
    result.error_ = error;
    result.byteOffset_ = byteOffset;
    return result;
}

StrictJsonDomResult StrictJsonDomBuilder::makeSuccess(StrictJsonDomDocument document) noexcept {
    StrictJsonDomResult result;
    result.error_ = StrictJsonDomError::None;
    result.document_.emplace(std::move(document));
    return result;
}

StrictJsonDomResult
StrictJsonDomBuilder::makeDuplicateKeyFailure(StrictJsonDomPathText path) noexcept {
    StrictJsonDomResult result;
    result.error_ = StrictJsonDomError::DuplicateObjectKey;
    result.path_ = path;
    return result;
}

void* StrictJsonDomBuilder::allocatorMalloc(void* const ctx, const std::size_t size) noexcept {
    auto* const resource = static_cast<ProjectIoMemoryResource*>(ctx);
    const auto result = resource->checkedAllocate(size, alignof(std::max_align_t));
    return result ? result.pointer() : nullptr;
}

void* StrictJsonDomBuilder::allocatorRealloc(void* const ctx, void* const ptr,
                                             const std::size_t oldSize,
                                             const std::size_t size) noexcept {
    auto* const resource = static_cast<ProjectIoMemoryResource*>(ctx);
    const auto result = resource->checkedAllocate(size, alignof(std::max_align_t));
    if (!result) {
        return nullptr;
    }
    if (ptr != nullptr && oldSize > 0) {
        std::memcpy(result.pointer(), ptr, std::min(oldSize, size));
    }
    if (ptr != nullptr) {
        resource->deallocate(ptr, oldSize, alignof(std::max_align_t));
    }
    return result.pointer();
}

void StrictJsonDomBuilder::allocatorFree(void* const ctx, void* const ptr) noexcept {
    if (ptr == nullptr) {
        return;
    }
    auto* const resource = static_cast<ProjectIoMemoryResource*>(ctx);
    resource->deallocate(ptr, 0, alignof(std::max_align_t));
}

} // namespace bloom::project::detail

namespace bloom::project {

StrictJsonDomResult parseStrictJsonDom(const std::span<const std::byte> input,
                                       const StrictJsonDomLimits& limits,
                                       ProjectIoOperationMemory operation) noexcept {
    return detail::StrictJsonDomBuilder::parse(input, limits, std::move(operation));
}

} // namespace bloom::project
