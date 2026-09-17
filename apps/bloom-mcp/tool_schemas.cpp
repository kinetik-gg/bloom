#include "server.hpp"

namespace bloom::mcp {

yyjson_mut_val* Server::tools(Json& out) {
    // Schemas describe transport arguments. Operation arguments themselves are discovered from
    // the host registry through query(kind="operations").
    constexpr std::string_view schemas = R"json({"tools":[
{"name":"query","description":"Read revisioned project projections or operation schemas.","inputSchema":{"type":"object","properties":{"kind":{"type":"string","enum":["project","compositions","nodes","parameters","assets","operations"]},"composition":{"type":"integer","minimum":1}},"required":["kind"],"additionalProperties":false},"annotations":{"readOnlyHint":true,"openWorldHint":false}},
{"name":"transact","description":"Execute an atomic operation list with an expected revision and one undo entry.","inputSchema":{"type":"object","properties":{"expectedRevision":{"type":"integer","minimum":0},"label":{"type":"string","maxLength":4096},"operations":{"type":"array","minItems":1,"maxItems":4096,"items":{"type":"object","properties":{"op":{"type":"string"},"args":{"type":"object"}},"required":["op","args"],"additionalProperties":false}}},"required":["expectedRevision","operations"],"additionalProperties":false},"annotations":{"readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}},
{"name":"render","description":"Publish a reference still frame or inclusive frame range; return paths and SHA-256 digests.","inputSchema":{"type":"object","properties":{"composition":{"type":"integer","minimum":1},"frame":{"type":"integer","minimum":0},"first":{"type":"integer","minimum":0},"last":{"type":"integer","minimum":0},"preset":{"type":"string","enum":["PngRgba8SrgbV1","FlatExrRgba32fLinRec709SceneV1","TiffRgba16SrgbV1"]},"destination":{"type":"string","minLength":1,"maxLength":4096}},"required":["composition","preset","destination"],"additionalProperties":false},"annotations":{"readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}},
{"name":"export","description":"Publish a composition using MEDIA-4 image, ProRes preview, DNxHR or PCM presets. Worker capabilities remain platform dependent.","inputSchema":{"type":"object","properties":{"composition":{"type":"integer","minimum":1},"frame":{"type":"integer","minimum":0},"first":{"type":"integer","minimum":0},"last":{"type":"integer","minimum":0},"preset":{"type":"string","enum":["PngRgba8SrgbV1","FlatExrRgba32fLinRec709SceneV1","TiffRgba16SrgbV1","ProResMovV1","DnxhrMxfV1","PcmWavV1"]},"destination":{"type":"string","minLength":1,"maxLength":4096},"profile":{"type":"string","maxLength":32},"audio":{"type":"boolean"},"sampleRate":{"type":"integer","minimum":8000,"maximum":192000}},"required":["composition","preset","destination"],"additionalProperties":false},"annotations":{"readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}},
{"name":"events","description":"Poll revision-inclusive command events; use afterSequence to avoid duplicates.","inputSchema":{"type":"object","properties":{"sinceRevision":{"type":"integer","minimum":0},"afterSequence":{"type":"integer","minimum":0}},"required":["sinceRevision"],"additionalProperties":false},"annotations":{"readOnlyHint":true,"openWorldHint":false}}
]})json";
    Parsed parsed(schemas);
    return out.copy(parsed.root());
}
} // namespace bloom::mcp
