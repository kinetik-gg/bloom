#ifndef BLOOM_PROJECT_CANONICAL_JSON_STRING_DETAIL_HPP
#define BLOOM_PROJECT_CANONICAL_JSON_STRING_DETAIL_HPP

#include <bloom/project/canonical_json_string.hpp>

#include <cstddef>
#include <limits>

namespace bloom::project::detail {

// Narrow arithmetic seam used by the public preflight and its size_t boundary tests. Counts are
// emitted one, two, and six bytes at a time respectively, plus two surrounding quotes.
[[nodiscard]] constexpr CanonicalJsonStringSizeResult
canonicalJsonStringTokenSizeFromCounts(const std::size_t directBytes,
                                       const std::size_t shortEscapeCount,
                                       const std::size_t unicodeEscapeCount) noexcept {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    std::size_t size = 2;
    if (directBytes > maximum - size) {
        return CanonicalJsonStringSizeResult::failure(CanonicalJsonStringError::SizeOverflow);
    }
    size += directBytes;
    if (shortEscapeCount > (maximum - size) / 2) {
        return CanonicalJsonStringSizeResult::failure(CanonicalJsonStringError::SizeOverflow);
    }
    size += shortEscapeCount * 2;
    if (unicodeEscapeCount > (maximum - size) / 6) {
        return CanonicalJsonStringSizeResult::failure(CanonicalJsonStringError::SizeOverflow);
    }
    size += unicodeEscapeCount * 6;
    return CanonicalJsonStringSizeResult::success(size);
}

} // namespace bloom::project::detail

#endif // BLOOM_PROJECT_CANONICAL_JSON_STRING_DETAIL_HPP
