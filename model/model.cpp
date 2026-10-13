#include "model.hpp"

#include <bit>
#include <cstdio>
#include <stdexcept>

namespace model {

namespace {

// 5b/6b table, abcdei with 'a' as bit 5. {RD- column, RD+ column}.
constexpr uint8_t T6[32][2] = {
    {0b100111, 0b011000}, {0b011101, 0b100010}, {0b101101, 0b010010}, {0b110001, 0b110001},
    {0b110101, 0b001010}, {0b101001, 0b101001}, {0b011001, 0b011001}, {0b111000, 0b000111},
    {0b111001, 0b000110}, {0b100101, 0b100101}, {0b010101, 0b010101}, {0b110100, 0b110100},
    {0b001101, 0b001101}, {0b101100, 0b101100}, {0b011100, 0b011100}, {0b010111, 0b101000},
    {0b011011, 0b100100}, {0b100011, 0b100011}, {0b010011, 0b010011}, {0b110010, 0b110010},
    {0b001011, 0b001011}, {0b101010, 0b101010}, {0b011010, 0b011010}, {0b111010, 0b000101},
    {0b110011, 0b001100}, {0b100110, 0b100110}, {0b010110, 0b010110}, {0b110110, 0b001001},
    {0b001110, 0b001110}, {0b101110, 0b010001}, {0b011110, 0b100001}, {0b101011, 0b010100},
};
constexpr uint8_t K28_6[2] = {0b001111, 0b110000};

// 3b/4b tables, fghj with 'f' as bit 3. {RD-, RD+}.
constexpr uint8_t T4D[8][2] = {
    {0b1011, 0b0100}, {0b1001, 0b1001}, {0b0101, 0b0101}, {0b1100, 0b0011},
    {0b1101, 0b0010}, {0b1010, 0b1010}, {0b0110, 0b0110}, {0b1110, 0b0001},  // P7
};
constexpr uint8_t A7[2] = {0b0111, 0b1000};
constexpr uint8_t T4K[8][2] = {
    {0b1011, 0b0100}, {0b0110, 0b1001}, {0b1010, 0b0101}, {0b1100, 0b0011},
    {0b1101, 0b0010}, {0b0101, 0b1010}, {0b1001, 0b0110}, {0b0111, 0b1000},
};

bool rd_after_bits(unsigned code, int bits, bool rd) {
    const int ones = std::popcount(code), half = bits / 2;
    if (ones > half) return true;
    if (ones < half) return false;
    // Balanced sub-blocks that are chosen by RD (000111/111000, 0011/1100)
    // still define the RD after them.
    if (bits == 6) {
        if (code == 0b000111) return true;
        if (code == 0b111000) return false;
    } else {
        if (code == 0b0011) return true;
        if (code == 0b1100) return false;
    }
    return rd;
}

// Reverse table: code -> (k << 8 | byte) for each starting RD, -1 if illegal.
struct ReverseTable {
    std::array<std::array<int, 1024>, 2> t;
    ReverseTable() {
        for (auto& row : t) row.fill(-1);
        for (int rd = 0; rd < 2; ++rd)
            for (int k = 0; k < 2; ++k)
                for (int b = 0; b < 256; ++b) {
                    if (k && !is_valid_k(static_cast<uint8_t>(b))) continue;
                    Encoded e = encode(static_cast<uint8_t>(b), k, rd);
                    if (t[rd][e.code] != -1) throw std::logic_error("8b/10b table is not one-to-one");
                    t[rd][e.code] = (k << 8) | b;
                }
    }
};
const ReverseTable& reverse_table() {
    static const ReverseTable r;
    return r;
}

}  // namespace

std::string name(const Symbol& s) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%c%d.%d", s.k ? 'K' : 'D', s.data & 31, s.data >> 5);
    return buf;
}

bool is_valid_k(uint8_t byte) {
    const int x = byte & 31, y = byte >> 5;
    return x == 28 || (y == 7 && (x == 23 || x == 27 || x == 29 || x == 30));
}

Encoded encode(uint8_t byte, bool k, bool rd) {
    if (k && !is_valid_k(byte)) throw std::invalid_argument("not a valid K symbol");
    const int x = byte & 31, y = byte >> 5;
    const unsigned c6 = (k && x == 28) ? K28_6[rd] : T6[x][rd];
    const bool rd1 = rd_after_bits(c6, 6, rd);
    unsigned c4;
    if (k) {
        c4 = T4K[y][rd1];
    } else if (y == 7) {
        // Alternate 7 avoids a run of five equal bits across the e,i / f,g,h boundary.
        const bool alt = (!rd1 && (x == 17 || x == 18 || x == 20)) || (rd1 && (x == 11 || x == 13 || x == 14));
        c4 = alt ? A7[rd1] : T4D[7][rd1];
    } else {
        c4 = T4D[y][rd1];
    }
    const bool rd2 = rd_after_bits(c4, 4, rd1);
    return {static_cast<uint16_t>((c6 << 4) | c4), rd2};
}

bool rd_after(uint16_t code, bool rd) {
    return rd_after_bits(code & 0xF, 4, rd_after_bits((code >> 4) & 0x3F, 6, rd));
}

Decoded decode(uint16_t code, bool rd) {
    const auto& t = reverse_table().t;
    Decoded d;
    d.rd_out = rd_after(code, rd);
    int v = t[rd][code & 0x3FF];
    if (v < 0) {
        v = t[!rd][code & 0x3FF];
        if (v >= 0) d.disp_err = true;
        else d.code_err = true;
    }
    if (v >= 0) d.sym = {static_cast<uint8_t>(v & 0xFF), (v >> 8) != 0};
    return d;  // code error -> D0.0
}

uint16_t Encoder::push(const Symbol& s, bool* k_err) {
    const bool bad_k = s.k && !is_valid_k(s.data);
    if (k_err) *k_err = bad_k;
    Encoded e = encode(s.data, s.k && !bad_k, rd_);
    rd_ = e.rd_out;
    return e.code;
}

Decoded Decoder::push(uint16_t code) {
    Decoded d = decode(code, rd_);
    rd_ = d.rd_out;
    return d;
}

Symbol Scrambler::push(const Symbol& s) {
    if (s.k && s.data == COM) {
        lfsr_ = 0xFFFF;
        return s;
    }
    if (s.k && s.data == SKP) return s;
    uint8_t out = s.data;
    for (int i = 0; i < 8; ++i) {
        const unsigned bit = (lfsr_ >> 15) & 1u;  // LFSR output, one per data bit
        if (!s.k) out = static_cast<uint8_t>(out ^ (bit << i));
        lfsr_ = static_cast<uint16_t>((lfsr_ << 1) ^ (bit ? 0x0039u : 0u));  // taps x^5 x^4 x^3 x^0
    }
    return {out, s.k};
}

uint32_t crc32_update(uint32_t reg, uint8_t byte) {
    reg ^= byte;
    for (int i = 0; i < 8; ++i) reg = (reg >> 1) ^ (0xEDB88320u & (0u - (reg & 1u)));
    return reg;
}

uint32_t crc32(const uint8_t* data, size_t n) {
    uint32_t reg = kCrcInit;
    for (size_t i = 0; i < n; ++i) reg = crc32_update(reg, data[i]);
    return ~reg;
}

std::array<uint8_t, 4> crc_bytes(uint32_t crc) {
    return {static_cast<uint8_t>(crc), static_cast<uint8_t>(crc >> 8), static_cast<uint8_t>(crc >> 16),
            static_cast<uint8_t>(crc >> 24)};
}

std::optional<PacketEvent> Framer::push(const Symbol& s) {
    if (s.k && s.data == STP) {
        const bool was = in_packet_;
        in_packet_ = true;
        reg_ = kCrcInit;
        if (was) return PacketEvent{true, false, 0};
        return std::nullopt;
    }
    if (!in_packet_) return std::nullopt;
    if (!s.k) {
        reg_ = crc32_update(reg_, s.data);
        return std::nullopt;
    }
    in_packet_ = false;
    if (s.data == END) return PacketEvent{false, reg_ == kCrcResidue, ~reg_};
    return PacketEvent{true, false, 0};
}

int find_comma(const std::vector<uint8_t>& bits) {
    for (size_t i = 0; i + 7 <= bits.size(); ++i) {
        unsigned w = 0;
        for (size_t j = 0; j < 7; ++j) w = (w << 1) | bits[i + j];
        if (w == 0b0011111 || w == 0b1100000) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace model
