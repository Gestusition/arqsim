#pragma once
// Wire format shared by sender, channel emulator and receiver.
//
//  0      1      2             4                     8            10                  14
//  +------+------+-------------+---------------------+------------+-------------------+---------
//  | type | flags| session(16) |      seq (32)       |  len (16)  |     crc32 (32)    | payload
//  +------+------+-------------+---------------------+------------+-------------------+---------
//
// All integers are big-endian (network byte order). The CRC-32 covers the
// header (with the crc field zeroed) plus the payload, so any bit error that
// the physical-layer model injects is detected by the receiver.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace arq {

enum class PacketType : std::uint8_t {
    Data = 1,
    Ack = 2,     // cumulative (SW/GBN: "next expected") or selective (SR: "this seq")
    Nak = 3,     // negative acknowledgement: "seq is missing, resend it now"
    Fin = 4,     // sender: all data acknowledged, tear down
    FinAck = 5,  // receiver: FIN received
};

const char* toString(PacketType t);

struct Packet {
    PacketType type = PacketType::Data;
    std::uint8_t flags = 0;
    std::uint16_t session = 0;
    std::uint32_t seq = 0;
    std::vector<std::uint8_t> payload;
};

constexpr std::size_t kHeaderSize = 14;
constexpr std::size_t kMaxDatagram = 65000;

/// Serialises a packet and fills in the CRC-32.
std::vector<std::uint8_t> encode(const Packet& p);

/// Parses a datagram. Returns std::nullopt if it is malformed or the CRC fails
/// (i.e. the packet was corrupted in the channel).
std::optional<Packet> decode(const std::uint8_t* data, std::size_t len);

/// Reads only the header fields without verifying the CRC (used by the
/// channel emulator, which must classify traffic but never "repairs" it).
bool peekHeader(const std::uint8_t* data, std::size_t len, PacketType& type, std::uint32_t& seq);

}  // namespace arq
