// bloom.mcp.http-parser (ADR 0034, section 10): Streamable HTTP framing and admission as pure
// functions over byte strings, against a hostile corpus: split reads, pipelining, chunked edge
// cases, both length headers, duplicate fields, oversized headers, header injection, NUL bytes and
// non-1.1 versions. Also the bearer token's constant-time comparison and masking.
#include <bloom/mcp/protocol.hpp>
#include <bloom/mcp/streamable_http.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace bloom::mcp;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

constexpr std::string_view kToken = "bloom_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ";
static_assert(kToken.size() == kMcpTokenLength);

McpHttpRequestParser parser(const std::size_t body = 1024) {
    return McpHttpRequestParser(McpHttpLimits{}, body);
}

McpHttpParseStatus parseAll(const std::string_view bytes, McpHttpRequest* out = nullptr,
                            const std::size_t body = 1024) {
    auto p = parser(body);
    const auto status = p.feed(bytes);
    if (status == McpHttpParseStatus::Complete && out != nullptr)
        *out = p.take();
    return status;
}

std::string post(const std::string& body, const std::string& extra = {}) {
    return "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:6767\r\nContent-Type: application/json\r\n"
           "Accept: application/json, text/event-stream\r\nAuthorization: Bearer " +
           std::string(kToken) + "\r\n" + extra + "Content-Length: " + std::to_string(body.size()) +
           "\r\n\r\n" + body;
}

void framing() {
    McpHttpRequest request;
    expect(parseAll(post("{}"), &request) == McpHttpParseStatus::Complete, "a request parses");
    expect(request.method == McpHttpMethod::Post && request.path == "/mcp" &&
               request.body == "{}" && request.header("host") == "127.0.0.1:6767",
           "method, path, lower-cased fields and body");

    // Split reads, one byte at a time.
    const auto whole = post("{\"a\":1}");
    auto p = parser();
    McpHttpParseStatus status = McpHttpParseStatus::NeedMore;
    for (std::size_t index = 0; index < whole.size(); ++index) {
        status = p.feed(std::string_view(whole).substr(index, 1));
        if (index + 1 < whole.size())
            expect(status == McpHttpParseStatus::NeedMore, "partial input needs more");
    }
    expect(status == McpHttpParseStatus::Complete && p.take().body == "{\"a\":1}",
           "byte-at-a-time input completes");
    expect(p.idle(), "the parser is idle after take");

    // Pipelining: two requests in one read.
    auto pipe = parser();
    expect(pipe.feed(post("{}") + post("[]")) == McpHttpParseStatus::Complete, "first pipelined");
    expect(pipe.take().body == "{}", "first body");
    expect(pipe.feed({}) == McpHttpParseStatus::Complete && pipe.take().body == "[]",
           "the second pipelined request is kept and parsed");

    // Leading empty lines are ignored.
    expect(parseAll("\r\n" + post("{}")) == McpHttpParseStatus::Complete,
           "a leading CRLF is ignored");

    // Versions.
    expect(parseAll("POST /mcp HTTP/1.0\r\nHost: x\r\n\r\n") ==
               McpHttpParseStatus::UnsupportedVersion,
           "HTTP/1.0 is 505");
    expect(parseAll("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n") == McpHttpParseStatus::UnsupportedVersion,
           "HTTP/2 prior knowledge is 505");
    expect(parseAll("POST /mcp HTTX/1.1\r\n\r\n") == McpHttpParseStatus::Malformed,
           "a garbage version is malformed");
    expect(parseAll("POST  /mcp HTTP/1.1\r\n\r\n") == McpHttpParseStatus::Malformed,
           "a double space is malformed");
    expect(parseAll("POST http://127.0.0.1/mcp HTTP/1.1\r\n\r\n") == McpHttpParseStatus::Malformed,
           "absolute-form targets are refused");

    // Length headers.
    expect(parseAll("POST /mcp HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\nTransfer-Encoding: "
                    "chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n") == McpHttpParseStatus::AmbiguousLength,
           "both length headers are refused");
    expect(parseAll("POST /mcp HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\nContent-Length: "
                    "3\r\n\r\n{}") == McpHttpParseStatus::AmbiguousLength,
           "conflicting Content-Length values are refused");
    expect(parseAll("POST /mcp HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\nContent-Length: "
                    "2\r\n\r\n{}") == McpHttpParseStatus::Complete,
           "identical Content-Length values agree");
    expect(parseAll("POST /mcp HTTP/1.1\r\nHost: a\r\nContent-Length: +2\r\n\r\n{}") ==
               McpHttpParseStatus::Malformed,
           "a signed length is malformed");
    expect(parseAll("POST /mcp HTTP/1.1\r\nHost: a\r\nContent-Length: 2, 2\r\n\r\n{}") ==
               McpHttpParseStatus::Malformed,
           "a length list is malformed");
    expect(parseAll("POST /mcp HTTP/1.1\r\nHost: a\r\nContent-Length: "
                    "99999999999999999999999\r\n\r\n") == McpHttpParseStatus::BodyTooLarge,
           "an overflowing length is too large");
    expect(parseAll("POST /mcp HTTP/1.1\r\nHost: a\r\n\r\n") == McpHttpParseStatus::LengthRequired,
           "a POST without a length is 411");
    expect(parseAll("POST /mcp HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip, chunked\r\n\r\n") ==
               McpHttpParseStatus::UnsupportedTransferEncoding,
           "codings other than chunked are refused");
    expect(parseAll("POST /mcp HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n"
                    "Transfer-Encoding: chunked\r\n\r\n") ==
               McpHttpParseStatus::UnsupportedTransferEncoding,
           "a repeated Transfer-Encoding is refused");

    // Body bound: 1 MiB + 1.
    const auto limit = kMcpMessageLimits.requestBytes;
    expect(parseAll("POST /mcp HTTP/1.1\r\nHost: a\r\nContent-Length: " +
                        std::to_string(limit + 1) + "\r\n\r\n",
                    nullptr, limit) == McpHttpParseStatus::BodyTooLarge,
           "a 1 MiB + 1 body is 413 before it is read");
    expect(parseAll("POST /mcp HTTP/1.1\r\nHost: a\r\nContent-Length: " + std::to_string(limit) +
                        "\r\n\r\n" + std::string(limit, ' '),
                    nullptr, limit) == McpHttpParseStatus::Complete,
           "a body of exactly 1 MiB is accepted");
}

void chunked() {
    McpHttpRequest request;
    const std::string head = "POST /mcp HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: Chunked\r\n\r\n";
    expect(parseAll(head + "2\r\n{\"\r\n3;ext=1\r\na\":\r\n1\r\n1\r\n1\r\n}\r\n0\r\nX-T: "
                           "1\r\n\r\n",
                    &request) == McpHttpParseStatus::Complete &&
               request.body == "{\"a\":1}",
           "chunks, an extension and a trailer reassemble");
    expect(parseAll(head + "g\r\n") == McpHttpParseStatus::Malformed, "a non-hex size");
    expect(parseAll(head + "-1\r\n") == McpHttpParseStatus::Malformed, "a negative size");
    expect(parseAll(head + "11111111111111111\r\n") == McpHttpParseStatus::Malformed,
           "a 17-digit size");
    expect(parseAll(head + "2\r\n{}XX0\r\n\r\n") == McpHttpParseStatus::Malformed,
           "chunk data must end with CRLF");
    expect(parseAll(head + "401\r\n", nullptr, 1024) == McpHttpParseStatus::BodyTooLarge,
           "a chunk larger than the body bound");
    expect(parseAll(head + "200\r\n" + std::string(512, 'a') + "\r\n201\r\n", nullptr, 1024) ==
               McpHttpParseStatus::BodyTooLarge,
           "chunks that sum past the body bound");
    expect(parseAll(head + std::string(2000, '0')) == McpHttpParseStatus::Malformed,
           "an endless size line");
    expect(parseAll(head + "0\r\nbad trailer\r\n\r\n") == McpHttpParseStatus::Malformed,
           "a malformed trailer");
    auto split = parser();
    const auto whole = head + "4\r\n{\"b\"\r\n2\r\n:2\r\n1\r\n}\r\n0\r\n\r\n";
    McpHttpParseStatus status = McpHttpParseStatus::NeedMore;
    for (const char c : whole)
        status = split.feed(std::string_view(&c, 1));
    expect(status == McpHttpParseStatus::Complete && split.take().body == "{\"b\":2}",
           "chunked input split byte by byte");
}

void headers() {
    expect(parseAll("POST /mcp HTTP/1.1\r\nHost: a\r\n" + std::string(17 * 1024, 'x')) ==
               McpHttpParseStatus::HeaderTooLarge,
           "an oversized header block is 431");
    std::string many = "GET /mcp HTTP/1.1\r\n";
    for (int index = 0; index < 65; ++index)
        many += "X-" + std::to_string(index) + ": 1\r\n";
    expect(parseAll(many + "\r\n") == McpHttpParseStatus::TooManyHeaders, "65 fields are 431");
    expect(parseAll("GET /mcp HTTP/1.1\r\nHost : a\r\n\r\n") == McpHttpParseStatus::Malformed,
           "whitespace before the colon");
    expect(parseAll("GET /mcp HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n") ==
               McpHttpParseStatus::Malformed,
           "obsolete line folding");
    expect(parseAll("GET /mcp HTTP/1.1\r\nHost: a\nInjected: 1\r\n\r\n") ==
               McpHttpParseStatus::Malformed,
           "a bare LF cannot inject a field");
    expect(parseAll("GET /mcp HTTP/1.1\r\nHost: a\rInjected: 1\r\n\r\n") ==
               McpHttpParseStatus::Malformed,
           "a bare CR cannot inject a field");
    expect(parseAll(std::string("GET /mcp HTTP/1.1\r\nHost: a") + '\0' + "b\r\n\r\n") ==
               McpHttpParseStatus::Malformed,
           "a NUL byte in a value");
    expect(parseAll(std::string("GET /m") + '\0' + "cp HTTP/1.1\r\n\r\n") ==
               McpHttpParseStatus::Malformed,
           "a NUL byte in the target");
    expect(parseAll("GET /mcp HTTP/1.1\r\nBad\x01Name: a\r\n\r\n") == McpHttpParseStatus::Malformed,
           "a control character in a name");
    auto failed = parser();
    expect(failed.feed("GET /mcp HTTX\r\n\r\n") == McpHttpParseStatus::Malformed &&
               failed.feed(post("{}")) == McpHttpParseStatus::Malformed,
           "a failure is terminal for the connection");
}

McpHttpRejection admit(const std::string& bytes, const std::uint16_t port = 6767) {
    McpHttpRequest request;
    auto p = parser(kMcpMessageLimits.requestBytes);
    if (p.feed(bytes) != McpHttpParseStatus::Complete)
        return McpHttpRejection::Busy;
    request = p.take();
    return admitRequest(request, {.port = port, .allowedOrigins = {}}, kToken);
}

std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    if (at != std::string::npos)
        text.replace(at, from.size(), to);
    return text;
}

void admission() {
    const auto good = post("{}");
    expect(admit(good) == McpHttpRejection::None, "a good request is admitted");
    expect(admit(replaced(good, "127.0.0.1:6767", "localhost:6767")) == McpHttpRejection::None,
           "localhost is allowed");
    expect(admit(replaced(good, "127.0.0.1:6767", "LOCALHOST:6767")) == McpHttpRejection::None,
           "the host compares case-insensitively");
    for (const std::string host : {"evil.example:6767", "127.0.0.1", "127.0.0.1:6768",
                                   "127.0.0.1:06767", "[::1]:6767", "127.0.0.2:6767", ""}) {
        expect(admit(replaced(good, "127.0.0.1:6767", host)) == McpHttpRejection::HostNotAllowed,
               "Host " + host + " is 403");
    }
    expect(admit(replaced(good, "Host: 127.0.0.1:6767\r\n",
                          "Host: 127.0.0.1:6767\r\nHost: evil.example\r\n")) ==
               McpHttpRejection::HostNotAllowed,
           "a duplicate Host is 403");
    expect(admit(replaced(good, "Content-Type", "Origin: http://evil.example\r\nContent-Type")) ==
               McpHttpRejection::OriginNotAllowed,
           "a browser Origin is 403");
    expect(admit(replaced(good, "Content-Type", "Origin: null\r\nContent-Type")) ==
               McpHttpRejection::OriginNotAllowed,
           "Origin: null is 403");
    // Admission order: Host before Origin before path before token.
    expect(admit(replaced(replaced(good, "127.0.0.1:6767", "evil.example"), "Content-Type",
                          "Origin: http://evil.example\r\nContent-Type")) ==
               McpHttpRejection::HostNotAllowed,
           "Host is checked before Origin");
    expect(admit(replaced(good, "POST /mcp", "POST /.well-known/oauth-authorization-server")) ==
               McpHttpRejection::NotFound,
           "OAuth discovery is 404");
    expect(admit(replaced(good, "POST /mcp", "POST /mcp?x=1")) == McpHttpRejection::NotFound,
           "a query string is 404");
    expect(admit(replaced(replaced(good, "POST /mcp", "POST /other"), std::string(kToken),
                          "wrong")) == McpHttpRejection::NotFound,
           "the path is checked before the token");
    expect(admit(replaced(good, std::string(kToken), "bloom_wrong")) ==
               McpHttpRejection::Unauthorized,
           "a wrong token is 401");
    expect(admit(replaced(good, "Authorization: Bearer " + std::string(kToken) + "\r\n", "")) ==
               McpHttpRejection::Unauthorized,
           "a missing token is 401");
    expect(admit(replaced(good, "Bearer ", "Basic ")) == McpHttpRejection::Unauthorized,
           "another scheme is 401");
    expect(admit(replaced(good, "Bearer ", "bearer ")) == McpHttpRejection::None,
           "the scheme compares case-insensitively");
    const std::string get = "GET /mcp HTTP/1.1\r\nHost: 127.0.0.1:6767\r\nAuthorization: Bearer " +
                            std::string(kToken) + "\r\n\r\n";
    expect(admit(get) == McpHttpRejection::MethodNotAllowed, "GET is 405 after the token");
    expect(admit(replaced(get, std::string(kToken), "x")) == McpHttpRejection::Unauthorized,
           "GET without a token is 401 first");
    expect(admit(replaced(get, "GET", "DELETE")) == McpHttpRejection::None, "DELETE is admitted");
    expect(admit(replaced(good, "application/json\r\nAccept", "text/plain\r\nAccept")) ==
               McpHttpRejection::UnsupportedMediaType,
           "a wrong Content-Type is 415");
    expect(admit(replaced(good, "application/json\r\nAccept",
                          "application/json; charset=utf-8\r\nAccept")) == McpHttpRejection::None,
           "a charset parameter is fine");
    expect(admit(replaced(good, "application/json, text/event-stream", "application/json")) ==
               McpHttpRejection::NotAcceptable,
           "Accept without text/event-stream is 406");
    expect(admit(replaced(good, "application/json, text/event-stream", "*/*")) ==
               McpHttpRejection::None,
           "*/* accepts both");
    expect(admit(replaced(good, "application/json, text/event-stream",
                          "application/json, text/event-stream;q=0")) ==
               McpHttpRejection::NotAcceptable,
           "q=0 excludes a type");
    expect(admit(post(" [{\"jsonrpc\":\"2.0\"}]")) == McpHttpRejection::BatchNotSupported,
           "a batch is 400");
    expect(httpStatus(McpHttpRejection::BatchNotSupported) == 400 &&
               httpStatus(McpHttpRejection::Unauthorized) == 401 &&
               httpStatus(McpHttpRejection::HostNotAllowed) == 403 &&
               httpStatus(McpHttpRejection::MethodNotAllowed) == 405,
           "statuses");
}

void tokens() {
    // Constant time: equal-length near misses differing at the first and the last byte take the
    // same path (every byte is compared); timing is measured loosely as a sanity bound.
    std::string first(kToken);
    first[6] = first[6] == 'a' ? 'b' : 'a';
    std::string last(kToken);
    last.back() = last.back() == 'Q' ? 'R' : 'Q';
    const auto bearer = [](const std::string& token) { return "Bearer " + token; };
    expect(bearerMatches(bearer(std::string(kToken)), kToken), "the token matches");
    expect(!bearerMatches(bearer(first), kToken) && !bearerMatches(bearer(last), kToken),
           "near misses fail");
    expect(!bearerMatches(bearer(std::string(kToken) + "x"), kToken) &&
               !bearerMatches(bearer(std::string(kToken.substr(0, 10))), kToken) &&
               !bearerMatches(std::nullopt, kToken) && !bearerMatches(bearer(""), kToken),
           "length mismatches and absence fail");
    const auto time = [&](const std::string& value) {
        const auto header = bearer(value);
        const auto start = std::chrono::steady_clock::now();
        bool any = false;
        for (int index = 0; index < 200000; ++index)
            any = any != bearerMatches(header, kToken);
        static_cast<void>(any);
        return std::chrono::steady_clock::now() - start;
    };
    const auto early = time(first);
    const auto late = time(last);
    const auto ratio = static_cast<double>(early.count()) / static_cast<double>(late.count());
    expect(ratio > 0.5 && ratio < 2.0, "near misses at the first and last byte take similar time");

    expect(tokenWellFormed(kToken), "a well-formed token");
    expect(!tokenWellFormed("bloom_short") && !tokenWellFormed(std::string(kToken) + "A") &&
               !tokenWellFormed(replaced(std::string(kToken), "abc", "a+c")),
           "malformed tokens");
    const auto masked = maskToken(kToken);
    expect(masked.starts_with("bloom_") && masked.ends_with("NOPQ") &&
               masked.find("abcdef") == std::string::npos,
           "the mask shows the prefix and the last four characters only");
    expect(maskToken("garbage").find("garbage") == std::string::npos, "garbage is never shown");
    const std::array<std::byte, 3> bytes{std::byte{0xfb}, std::byte{0xff}, std::byte{0x01}};
    expect(base64Url(bytes) == "-_8B", "base64url alphabet");
}

void responses() {
    const std::vector<std::pair<std::string_view, std::string>> fields{
        {"Content-Type", "application/json"}};
    const auto response = httpResponse(200, fields, std::string_view("{}"));
    expect(response == "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                       "2\r\n\r\n{}",
           "a JSON response");
    expect(response.find("Access-Control") == std::string::npos, "no CORS headers");
    expect(sseMessage("{\"a\":1}") == "event: message\r\ndata: {\"a\":1}\r\n\r\n", "an SSE event");
}

} // namespace

int main() {
    framing();
    chunked();
    headers();
    admission();
    tokens();
    responses();
    if (failures == 0)
        std::cout << "http parser: framing, chunked, headers, admission, tokens and responses "
                     "passed\n";
    return failures == 0 ? 0 : 1;
}
