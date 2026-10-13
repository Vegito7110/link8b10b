// model.hpp - C++ golden reference model for the link blocks.
//
// Written independently of the RTL: the 8b/10b encoder here uses the full
// two-column (RD-/RD+) tables from the standard, while the RTL stores only the
// RD- column and derives RD+ by a complement rule. The decoder here is a
// reverse lookup built by enumerating every legal symbol. Agreement between
// the two is what the testbench checks.
//
// Code word convention: 10-bit integer, bit 9 = 'a' (first bit on the wire),
// bits 9..4 = abcdei (6b sub-block), bits 3..0 = fghj (4b sub-block).
// Running disparity (RD): false = RD-, true = RD+.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace model {

// Special symbols (PCIe Gen1/2 names). Byte value = (y << 5) | x for K.x.y.
constexpr uint8_t COM = 0xBC;  // K28.5 comma: alignment, resets the scrambler
constexpr uint8_t SKP = 0x1C;  // K28.0 skip: scrambler does not advance
constexpr uint8_t STP = 0xFB;  // K27.7 start of packet
constexpr uint8_t END = 0xFD;  // K29.7 end of packet

struct Symbol {
    uint8_t data = 0;
    bool k = false;
    bool operator==(const Symbol& o) const { return data == o.data && k == o.k; }
};

std::string name(const Symbol& s);  // "D21.5", "K28.5"

// ---- 8b/10b -----------------------------------------------------------------
bool is_valid_k(uint8_t byte);  // K28.0-7, K23.7, K27.7, K29.7, K30.7

struct Encoded {
    uint16_t code;
    bool rd_out;
};
// Requires k == false or is_valid_k(byte).
Encoded encode(uint8_t byte, bool k, bool rd);

// RD after transmitting `code` starting from `rd` (works for any 10-bit value).
bool rd_after(uint16_t code, bool rd);

struct Decoded {
    Symbol sym;          // decoded symbol (D0.0 on a code error)
    bool code_err = false;  // not a legal code word under either RD
    bool disp_err = false;  // legal, but only under the other RD
    bool rd_out = false;
};
Decoded decode(uint16_t code, bool rd);

class Encoder {
public:
    // Invalid K requests are sent as data, and flagged (mirrors the RTL).
    uint16_t push(const Symbol& s, bool* k_err = nullptr);
    bool rd() const { return rd_; }
private:
    bool rd_ = false;
};

class Decoder {
public:
    Decoded push(uint16_t code);
    bool rd() const { return rd_; }
private:
    bool rd_ = false;
};

// ---- Scrambler (PCIe Gen1/2 style) ------------------------------------------
// 16-bit LFSR, G(x) = x^16 + x^5 + x^4 + x^3 + 1, seeded with 0xFFFF.
// D symbols are XORed with 8 LFSR output bits (LSB first); K symbols pass
// through. COM re-seeds the LFSR; SKP does not advance it; every other symbol
// advances it by 8 bits. Scrambling and descrambling are the same operation.
class Scrambler {
public:
    Symbol push(const Symbol& s);
    uint16_t state() const { return lfsr_; }
private:
    uint16_t lfsr_ = 0xFFFF;
};

// ---- CRC-32 (IEEE 802.3 polynomial 0x04C11DB7, reflected, as in Ethernet) --
constexpr uint32_t kCrcInit = 0xFFFFFFFFu;
constexpr uint32_t kCrcResidue = 0xDEBB20E3u;  // register after message + its CRC
uint32_t crc32_update(uint32_t reg, uint8_t byte);
uint32_t crc32(const uint8_t* data, size_t n);  // final value (inverted register)
std::array<uint8_t, 4> crc_bytes(uint32_t crc); // transmission order, LSB first

// ---- Receive-side packet framing (mirrors rtl/link_top.sv) ------------------
// STP starts a packet (and resets the CRC), D symbols update the CRC, END
// finishes it: crc_ok if the register equals the residue. Any other K symbol
// inside a packet aborts it. STP inside a packet aborts the old one and starts
// a new one.
struct PacketEvent {
    bool abort = false;   // false = END seen
    bool crc_ok = false;
    uint32_t crc = 0;     // ~register at END (the CRC of what was received)
    bool operator==(const PacketEvent& o) const {
        return abort == o.abort && crc_ok == o.crc_ok && (abort || crc == o.crc);
    }
};
class Framer {
public:
    std::optional<PacketEvent> push(const Symbol& s);
private:
    bool in_packet_ = false;
    uint32_t reg_ = kCrcInit;
};

// ---- Comma alignment ---------------------------------------------------------
// Given a raw bit stream (one bit per element, first-transmitted first) of
// unknown alignment, return the offset of the first comma (0011111 or 1100000)
// - i.e. where a 10-bit symbol boundary lies - or -1 if none.
int find_comma(const std::vector<uint8_t>& bits);

}  // namespace model
