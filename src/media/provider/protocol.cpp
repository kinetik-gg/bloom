#include "wire.hpp"
#include <algorithm>
#include <bloom/media/provider/protocol.hpp>
#include <new>
#include <set>

namespace bloom::media::provider {
namespace {
using wire::Reader;
using wire::require;
using wire::Writer;
constexpr std::uint32_t magic = 0x314d4c42;
void colour(Writer& w, const ColourTags& c) {
    w.number(c.primaries);
    w.number(c.transfer);
    w.number(c.matrix);
    w.number(c.range);
}
ColourTags colour(Reader& r) {
    ColourTags c;
    c.primaries = r.number<std::int32_t>();
    c.transfer = r.number<std::int32_t>();
    c.matrix = r.number<std::int32_t>();
    c.range = r.number<std::int32_t>();
    return c;
}
void handshake(Writer& w, const Handshake& h) {
    wire::write(w, h.execution);
    require(!h.declarations.empty());
    w.count(h.declarations.size());
    for (const auto& d : h.declarations) {
        require(d.execution == h.execution, Error::IdentityMismatch);
        wire::write(w, d.capability);
        wire::write(w, d.evidence);
    }
    w.count(h.pipelines.size());
    for (const auto& p : h.pipelines)
        w.hash(p);
    require(!h.transports.empty());
    w.count(h.transports.size(), 2);
    std::set<Transport> unique;
    for (auto t : h.transports) {
        require(unique.insert(t).second);
        w.enumeration(t, 0, 1);
    }
    require(std::ranges::find(h.transports, h.execution.transport) != h.transports.end());
}
Handshake handshake(Reader& r) {
    Handshake h;
    h.execution = wire::execution(r);
    auto n = r.count();
    for (std::uint32_t i = 0; i < n; ++i) {
        ProviderDeclaration d;
        d.execution = h.execution;
        d.capability = wire::capability(r);
        d.evidence = wire::evidence(r);
        h.declarations.push_back(std::move(d));
    }
    n = r.count();
    for (std::uint32_t i = 0; i < n; ++i)
        h.pipelines.push_back(r.hash());
    n = r.count(2);
    for (std::uint32_t i = 0; i < n; ++i)
        h.transports.push_back(r.enumeration<Transport>(0, 1));
    Writer validation;
    handshake(validation, h);
    return h;
}
void call(Writer& w, const CallRequest& c) {
    wire::write(w, c.capability);
    require(!c.source.empty());
    w.text(c.source);
    require(c.width > 0 && c.height > 0 && c.width <= c.capability.maxWidth &&
            c.height <= c.capability.maxHeight &&
            static_cast<std::uint64_t>(c.width) * c.height <= Limits::pixels);
    w.number(c.width);
    w.number(c.height);
    w.number(c.frame);
}
CallRequest call(Reader& r) {
    CallRequest c;
    c.capability = wire::capability(r);
    c.source = r.text();
    c.width = r.number<std::uint32_t>();
    c.height = r.number<std::uint32_t>();
    c.frame = r.number<std::uint64_t>();
    Writer validation;
    call(validation, c);
    return c;
}
void probe(Writer& w, const ProbeResult& p) {
    require(valid(p));
    w.text(p.container);
    w.text(p.version);
    w.number(p.sourceBytes);
    w.hash(p.sourceDigest);
    w.count(p.streams.size());
    for (const auto& s : p.streams) {
        w.number(s.id);
        w.enumeration(s.kind, 1, 3);
        w.text(s.codec);
        w.text(s.profile);
        w.rational(s.timebase);
        w.rational(s.rate);
        w.number(s.width);
        w.number(s.height);
        w.enumeration(s.format, 1, 3);
        colour(w, s.colour);
        w.number(s.sampleRate);
        w.count(s.channelLayout.size(), Limits::channels);
        for (const auto& c : s.channelLayout)
            w.text(c);
    }
}
ProbeResult probe(Reader& r) {
    ProbeResult p;
    p.container = r.text();
    p.version = r.text();
    p.sourceBytes = r.number<std::uint64_t>();
    p.sourceDigest = r.hash();
    auto n = r.count();
    for (std::uint32_t i = 0; i < n; ++i) {
        StreamDescriptor s;
        s.id = r.number<std::uint32_t>();
        s.kind = r.enumeration<MediaKind>(1, 3);
        s.codec = r.text();
        s.profile = r.text();
        s.timebase = r.rational();
        s.rate = r.rational();
        s.width = r.number<std::uint32_t>();
        s.height = r.number<std::uint32_t>();
        s.format = r.enumeration<PixelFormat>(1, 3);
        s.colour = colour(r);
        s.sampleRate = r.number<std::uint32_t>();
        auto channels = r.count(Limits::channels);
        for (std::uint32_t j = 0; j < channels; ++j)
            s.channelLayout.push_back(r.text());
        p.streams.push_back(std::move(s));
    }
    require(valid(p));
    return p;
}
void frame(Writer& w, const FrameProduct& f) {
    require(valid(f));
    w.enumeration(f.format, 1, 3);
    w.rational(f.pts);
    colour(w, f.colour);
    w.count(f.planes.size(), Limits::planes);
    for (const auto& p : f.planes) {
        w.number(p.width);
        w.number(p.height);
        w.number(p.stride);
        w.count(p.bytes.size(), Limits::productBytes);
        w.bytes.insert(w.bytes.end(), p.bytes.begin(), p.bytes.end());
        w.hash(p.digest);
    }
}
FrameProduct frame(Reader& r) {
    FrameProduct f;
    f.format = r.enumeration<PixelFormat>(1, 3);
    f.pts = r.rational();
    f.colour = colour(r);
    const auto n = r.count(Limits::planes);
    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        CpuPlane p;
        p.width = r.number<std::uint32_t>();
        p.height = r.number<std::uint32_t>();
        p.stride = r.number<std::uint32_t>();
        const auto size = r.count(Limits::productBytes);
        total += size;
        require(total <= Limits::productBytes, Error::Oversized);
        require(size <= r.bytes.size(), Error::BadLength);
        require(p.width > 0 && p.height > 0 && p.width <= Limits::dimension &&
                p.height <= Limits::dimension &&
                static_cast<std::uint64_t>(p.width) * p.height <= Limits::pixels &&
                size == static_cast<std::uint64_t>(p.stride) * p.height);
        p.bytes.assign(r.bytes.begin(), r.bytes.begin() + size);
        r.bytes = r.bytes.subspan(size);
        p.digest = r.hash();
        require(p.digest == digestBytes(p.bytes), Error::DigestMismatch);
        f.planes.push_back(std::move(p));
    }
    require(valid(f));
    return f;
}
} // namespace
Result<Bytes> encodeMessage(const Message& m) {
    try {
        require(m.session != 0 && m.sequence != 0);
        Writer w;
        w.number<std::uint32_t>(0);
        w.number(magic);
        w.number(kProtocolVersion);
        w.number(kSchemaVersion);
        w.enumeration(m.kind, 1, 8);
        w.number(m.session);
        w.number(m.sequence);
        switch (m.kind) {
        case MessageKind::Handshake:
            handshake(w, std::get<Handshake>(m.payload));
            break;
        case MessageKind::Call:
            call(w, std::get<CallRequest>(m.payload));
            break;
        case MessageKind::Probe:
            probe(w, std::get<ProbeResult>(m.payload));
            break;
        case MessageKind::Frame:
            frame(w, std::get<FrameProduct>(m.payload));
            break;
        case MessageKind::Failure: {
            const auto& e = std::get<Unavailable>(m.payload);
            w.enumeration(e.reason, 1, static_cast<std::uint8_t>(Error::Shutdown));
            w.text(e.detail);
            break;
        }
        case MessageKind::Cancel:
        case MessageKind::Shutdown:
        case MessageKind::Ack:
            require(std::holds_alternative<std::monostate>(m.payload));
            break;
        }
        require(w.bytes.size() - 4 <= Limits::frameBytes, Error::Oversized);
        const auto size = static_cast<std::uint32_t>(w.bytes.size() - 4);
        for (unsigned i = 0; i < 4; ++i)
            w.bytes[i] = static_cast<std::byte>((size >> (i * 8U)) & 255U);
        return std::move(w.bytes);
    } catch (wire::Failure f) {
        return Unavailable{f.code, "Invalid outbound media frame"};
    } catch (const std::bad_variant_access&) {
        return Unavailable{Error::InvalidValue, "Message payload does not match kind"};
    } catch (const std::bad_alloc&) {
        return Unavailable{Error::Oversized, "Media frame allocation failed"};
    }
}
Result<std::uint32_t> messageLength(std::span<const std::byte> prefix) {
    try {
        Reader r(prefix);
        const auto n = r.number<std::uint32_t>();
        r.end();
        require(n >= kEnvelopeBytes, Error::BadLength);
        require(n <= Limits::frameBytes, Error::Oversized);
        return n;
    } catch (wire::Failure f) {
        return Unavailable{f.code, "Invalid media frame length"};
    }
}
Result<Message> decodeMessage(std::span<const std::byte> bytes) {
    try {
        require(bytes.size() >= 4, Error::Truncated);
        const auto length = messageLength(bytes.first(4));
        if (const auto* error = std::get_if<Unavailable>(&length))
            return *error;
        const auto n = std::get<std::uint32_t>(length);
        require(bytes.size() - 4 >= n, Error::Truncated);
        require(bytes.size() - 4 == n, Error::BadLength);
        Reader r(bytes.subspan(4));
        require(r.number<std::uint32_t>() == magic, Error::InvalidValue);
        const auto protocol = r.number<std::uint16_t>();
        const auto schema = r.number<std::uint16_t>();
        require(protocol == kProtocolVersion && schema == kSchemaVersion, Error::VersionMismatch);
        Message m;
        m.kind = r.enumeration<MessageKind>(1, 8);
        m.session = r.number<std::uint64_t>();
        m.sequence = r.number<std::uint64_t>();
        require(m.session != 0 && m.sequence != 0);
        switch (m.kind) {
        case MessageKind::Handshake:
            m.payload = handshake(r);
            break;
        case MessageKind::Call:
            m.payload = call(r);
            break;
        case MessageKind::Probe:
            m.payload = probe(r);
            break;
        case MessageKind::Frame:
            m.payload = frame(r);
            break;
        case MessageKind::Failure: {
            Unavailable e;
            e.reason = r.enumeration<Error>(1, static_cast<std::uint8_t>(Error::Shutdown));
            e.detail = r.text();
            m.payload = std::move(e);
            break;
        }
        case MessageKind::Cancel:
        case MessageKind::Shutdown:
        case MessageKind::Ack:
            break;
        }
        r.end();
        return m;
    } catch (wire::Failure f) {
        return Unavailable{f.code, "Rejected inbound media frame"};
    } catch (const std::bad_alloc&) {
        return Unavailable{Error::Oversized, "Inbound media allocation failed"};
    }
}
Result<Message> HostProtocol::receive(std::span<const std::byte> bytes, MessageKind expectedKind) {
    auto decoded = decodeMessage(bytes);
    auto* m = std::get_if<Message>(&decoded);
    if (!m)
        return std::get<Unavailable>(decoded);
    if (m->session != session_ || m->sequence != lastSequence_ + 1)
        return Unavailable{Error::Replay, "Stale session or out-of-order message"};
    if (m->kind != expectedKind && (m->kind != MessageKind::Failure || !handshaken_))
        return Unavailable{Error::UnexpectedMessage, "Unexpected media response"};
    if (!handshaken_) {
        if (m->kind != MessageKind::Handshake)
            return Unavailable{Error::UnexpectedMessage, "Handshake required"};
        if (std::get<Handshake>(m->payload) != expected_ ||
            expected_.execution.transport != Transport::PipeCopiesV0)
            return Unavailable{Error::IdentityMismatch,
                               "Worker identity, qualification or transport mismatch"};
    } else if (m->kind == MessageKind::Handshake)
        return Unavailable{Error::Replay, "Repeated handshake"};
    handshaken_ = true;
    lastSequence_ = m->sequence;
    return decoded;
}
} // namespace bloom::media::provider
