#include "core/packet.hpp"

#include "core/checksum.hpp"

namespace arq {

namespace {
void put16(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
    b[off] = std::uint8_t(v >> 8);
    b[off + 1] = std::uint8_t(v);
}
void put32(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
    b[off] = std::uint8_t(v >> 24);
    b[off + 1] = std::uint8_t(v >> 16);
    b[off + 2] = std::uint8_t(v >> 8);
    b[off + 3] = std::uint8_t(v);
}
std::uint16_t get16(const std::uint8_t* b) { return std::uint16_t((b[0] << 8) | b[1]); }
std::uint32_t get32(const std::uint8_t* b) {
    return (std::uint32_t(b[0]) << 24) | (std::uint32_t(b[1]) << 16) | (std::uint32_t(b[2]) << 8) |
           std::uint32_t(b[3]);
}
bool validType(std::uint8_t t) { return t >= 1 && t <= 5; }
}  // namespace

const char* toString(PacketType t) {
    switch (t) {
        case PacketType::Data: return "DATA";
        case PacketType::Ack: return "ACK";
        case PacketType::Nak: return "NAK";
        case PacketType::Fin: return "FIN";
        case PacketType::FinAck: return "FINACK";
    }
    return "?";
}

std::vector<std::uint8_t> encode(const Packet& p) {
    std::vector<std::uint8_t> b(kHeaderSize + p.payload.size());
    b[0] = std::uint8_t(p.type);
    b[1] = p.flags;
    put16(b, 2, p.session);
    put32(b, 4, p.seq);
    put16(b, 8, std::uint16_t(p.payload.size()));
    put32(b, 10, 0);
    for (std::size_t i = 0; i < p.payload.size(); ++i) b[kHeaderSize + i] = p.payload[i];
    put32(b, 10, crc32(b.data(), b.size()));
    return b;
}

std::optional<Packet> decode(const std::uint8_t* data, std::size_t len) {
    if (len < kHeaderSize) return std::nullopt;
    if (!validType(data[0])) return std::nullopt;
    std::uint16_t plen = get16(data + 8);
    if (kHeaderSize + plen != len) return std::nullopt;
    std::uint32_t expected = get32(data + 10);
    std::vector<std::uint8_t> copy(data, data + len);
    copy[10] = copy[11] = copy[12] = copy[13] = 0;
    if (crc32(copy.data(), copy.size()) != expected) return std::nullopt;
    Packet p;
    p.type = PacketType(data[0]);
    p.flags = data[1];
    p.session = get16(data + 2);
    p.seq = get32(data + 4);
    p.payload.assign(data + kHeaderSize, data + len);
    return p;
}

bool peekHeader(const std::uint8_t* data, std::size_t len, PacketType& type, std::uint32_t& seq) {
    if (len < kHeaderSize || !validType(data[0])) return false;
    type = PacketType(data[0]);
    seq = get32(data + 4);
    return true;
}

}  // namespace arq
