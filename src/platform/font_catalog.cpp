#include <bloom/platform/font_catalog.hpp>

#include <algorithm>
#include <string_view>

namespace bloom::platform {
namespace detail {
[[nodiscard]] FontCatalogue enumerateSystemFonts(const FontCatalogueProvider::Cancellation&);
}

namespace {
[[nodiscard]] FontFace embeddedFace(const std::uint32_t faceIndex, const std::string_view family,
                                    const std::string_view style, const std::int32_t weight,
                                    const std::string_view digest) {
    const auto parsed = core::Sha256Digest::fromLowercaseHex(digest);
    return {std::string(family),
            std::string(style),
            weight,
            100,
            false,
            {},
            parsed.value_or(core::Sha256Digest{}),
            FontSource::Embedded,
            faceIndex};
}
} // namespace

const FontFace* FontCatalogue::findByDigest(const core::Sha256Digest& digest) const noexcept {
    const auto found = std::ranges::find(faces, digest, &FontFace::contentDigest);
    return found == faces.end() ? nullptr : &*found;
}

FontCatalogue FontCatalogueProvider::embeddedCatalogue() {
    FontCatalogue result;
    result.status = FontCatalogueStatus::Available;
    result.faces = {
        embeddedFace(0, "DejaVu Sans", "Book", 400,
                     "7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954"),
        embeddedFace(1, "Inter", "Regular", 400,
                     "40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82"),
        embeddedFace(2, "Inter", "Medium", 500,
                     "97ad806f526e41546d46365bb3a393145f75b7b1568913db74549ad8b8dba872"),
        embeddedFace(3, "Inter", "SemiBold", 600,
                     "78a843fade9d4612a5567302fb595b56976eb5fcebf4fea5a5912d638bafcde3"),
    };
    return result;
}

FontCatalogue FontCatalogueProvider::enumerate(const Cancellation& cancelled) const {
    auto result = embeddedCatalogue();
    if (cancelled && cancelled()) {
        result.faces.clear();
        return result;
    }
    auto system = detail::enumerateSystemFonts(cancelled);
    result.faces.insert(result.faces.end(), std::make_move_iterator(system.faces.begin()),
                        std::make_move_iterator(system.faces.end()));
    std::stable_sort(result.faces.begin(), result.faces.end(),
                     [](const FontFace& left, const FontFace& right) {
                         if (left.source != right.source)
                             return left.source == FontSource::Embedded;
                         if (left.family != right.family)
                             return left.family < right.family;
                         if (left.style != right.style)
                             return left.style < right.style;
                         if (left.weight != right.weight)
                             return left.weight < right.weight;
                         if (left.width != right.width)
                             return left.width < right.width;
                         if (left.italic != right.italic)
                             return left.italic < right.italic;
                         if (left.path != right.path)
                             return left.path < right.path;
                         return left.faceIndex < right.faceIndex;
                     });
    return result;
}

} // namespace bloom::platform
