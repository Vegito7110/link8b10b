// Known-answer and property tests for the golden model (no RTL involved).
// If the model is wrong, the RTL testbench would happily check against the
// wrong answers - so the model is pinned to published values first.
#include <cstdio>
#include <random>
#include <vector>

#include "model.hpp"

using namespace model;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failed;                                                   \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static void test_known_codes() {
    // Values from the 8b/10b tables (bit 'a' first / most significant).
    CHECK(encode(0xBC, true, false).code == 0b0011111010);  // K28.5 RD-
    CHECK(encode(0xBC, true, true).code == 0b1100000101);   // K28.5 RD+
    CHECK(encode(0x3C, true, false).code == 0b0011111001);  // K28.1 RD-
    CHECK(encode(0xFC, true, false).code == 0b0011111000);  // K28.7 RD-
    CHECK(encode(0xFB, true, false).code == 0b1101101000);  // K27.7 RD-
    CHECK(encode(0xB5, false, false).code == 0b1010101010); // D21.5 (both RDs)
    CHECK(encode(0xB5, false, true).code == 0b1010101010);
    CHECK(encode(0x4A, false, false).code == 0b0101010101); // D10.2
    CHECK(encode(0x00, false, false).code == 0b1001110100); // D0.0 RD-
    // Alternate 7 encoding: D17.7 with RD- uses 0111 instead of 1110.
    CHECK((encode(0xF1, false, false).code & 0xF) == 0b0111);
    CHECK((encode(0xF1, false, true).code & 0xF) == 0b0001);
    // RD flips after an unbalanced symbol, stays after a balanced one.
    CHECK(encode(0xBC, true, false).rd_out == true);
    CHECK(encode(0xB5, false, false).rd_out == false);
}

static void test_tables_are_consistent() {
    int legal = 0;
    for (int rd = 0; rd < 2; ++rd)
        for (int k = 0; k < 2; ++k)
            for (int b = 0; b < 256; ++b) {
                if (k && !is_valid_k(static_cast<uint8_t>(b))) continue;
                ++legal;
                Encoded e = encode(static_cast<uint8_t>(b), k, rd);
                const int ones = __builtin_popcount(e.code);
                CHECK(ones == 4 || ones == 5 || ones == 6);           // disparity 0 or +-2
                CHECK(ones == 5 || e.rd_out == (ones == 6));          // unbalanced -> RD flips to its sign
                CHECK(rd_after(e.code, rd) == e.rd_out);
                Decoded d = decode(e.code, rd);                       // round trip
                CHECK(!d.code_err && !d.disp_err && d.sym.data == b && d.sym.k == bool(k));
            }
    CHECK(legal == 2 * (256 + 12));  // 256 data + 12 control symbols, in each RD
}

// Over a long random stream: never more than 5 equal bits in a row, and the
// running disparity at every symbol boundary is always -1 or +1.
static void test_stream_properties() {
    std::mt19937 rng(1);
    Encoder enc;
    int run = 0, last = -1, max_run = 0, disparity = -1;
    bool disparity_ok = true;
    for (int i = 0; i < 200000; ++i) {
        Symbol s{static_cast<uint8_t>(rng()), false};
        if (rng() % 16 == 0) { s.k = true; s.data = (rng() % 2) ? COM : STP; }
        uint16_t c = enc.push(s);
        for (int b = 9; b >= 0; --b) {
            int bit = (c >> b) & 1;
            run = bit == last ? run + 1 : 1;
            last = bit;
            max_run = std::max(max_run, run);
            disparity += bit ? 1 : -1;
        }
        disparity_ok = disparity_ok && (disparity == -1 || disparity == 1);
    }
    CHECK(max_run <= 5);
    CHECK(disparity_ok);
}

// The comma pattern never appears in a stream of data symbols, and with a
// K28.5 inserted it reveals the true symbol boundary at any bit offset.
static void test_comma_alignment() {
    std::mt19937 rng(2);
    for (int trial = 0; trial < 200; ++trial) {
        Encoder enc;
        std::vector<uint8_t> bits;
        // Unknown alignment: 0-9 leading bits. They alternate (0101...) so the
        // junk itself can't combine with the first symbol into a comma.
        const int junk = static_cast<int>(rng() % 10);
        for (int i = 0; i < junk; ++i) bits.push_back(static_cast<uint8_t>(i & 1));
        const int comma_at = 20 + static_cast<int>(rng() % 50);
        for (int i = 0; i < 100; ++i) {
            Symbol s = i == comma_at ? Symbol{COM, true} : Symbol{static_cast<uint8_t>(rng()), false};
            uint16_t c = enc.push(s);
            for (int b = 9; b >= 0; --b) bits.push_back((c >> b) & 1);
        }
        CHECK(find_comma(bits) == junk + 10 * comma_at);
    }
}

static void test_crc() {
    const char* s = "123456789";
    CHECK(crc32(reinterpret_cast<const uint8_t*>(s), 9) == 0xCBF43926u);  // standard check value
    // Receiver trick: run the CRC over message + transmitted CRC -> constant residue.
    uint32_t reg = kCrcInit;
    for (int i = 0; i < 9; ++i) reg = crc32_update(reg, static_cast<uint8_t>(s[i]));
    for (uint8_t b : crc_bytes(~reg)) reg = crc32_update(reg, b);
    CHECK(reg == kCrcResidue);
}

static void test_scrambler() {
    // First 16 scrambling bytes after COM (data = 0x00 -> output = LFSR bytes).
    const uint8_t expected[16] = {0xFF, 0x17, 0xC0, 0x14, 0xB2, 0xE7, 0x02, 0x82,
                                  0x72, 0x6E, 0x28, 0xA6, 0xBE, 0x6D, 0xBF, 0x8D};
    Scrambler s;
    s.push({COM, true});
    for (uint8_t e : expected) CHECK(s.push({0x00, false}).data == e);

    // Descrambling is the same operation; K symbols pass through; SKP doesn't advance.
    std::mt19937 rng(3);
    Scrambler tx, rx;
    for (int i = 0; i < 10000; ++i) {
        Symbol in{static_cast<uint8_t>(rng()), false};
        int r = static_cast<int>(rng() % 20);
        if (r == 0) in = {COM, true};
        else if (r == 1) in = {SKP, true};
        else if (r == 2) in = {STP, true};
        Symbol mid = tx.push(in), out = rx.push(mid);
        CHECK(out == in);
        if (in.k) CHECK(mid == in);
    }
    Scrambler a, b;
    a.push({SKP, true});
    CHECK(a.state() == b.state());
}

static void test_framer() {
    Framer f;
    std::vector<uint8_t> payload = {1, 2, 3, 4, 5};
    uint32_t crc = crc32(payload.data(), payload.size());
    CHECK(!f.push({STP, true}));
    for (uint8_t b : payload) CHECK(!f.push({b, false}));
    for (uint8_t b : crc_bytes(crc)) CHECK(!f.push({b, false}));
    auto ev = f.push({END, true});
    CHECK(ev && !ev->abort && ev->crc_ok);
    // Corrupt one byte -> CRC fails.
    f.push({STP, true});
    f.push({9, false});
    for (uint8_t b : crc_bytes(crc32(payload.data(), 1))) f.push({b, false});
    ev = f.push({END, true});
    CHECK(ev && !ev->abort && !ev->crc_ok);
    // Unexpected K inside a packet aborts it.
    f.push({STP, true});
    ev = f.push({COM, true});
    CHECK(ev && ev->abort);
}

int main() {
    struct T { const char* name; void (*fn)(); };
    const T tests[] = {
        {"known 8b/10b code words", test_known_codes},
        {"tables: disparity, RD rule, round trip", test_tables_are_consistent},
        {"stream: run length <= 5, RD always +-1", test_stream_properties},
        {"comma alignment at any bit offset", test_comma_alignment},
        {"CRC-32 check value and residue", test_crc},
        {"scrambler sequence and self-inverse", test_scrambler},
        {"packet framer", test_framer},
    };
    for (const auto& t : tests) {
        int before = g_failed;
        t.fn();
        std::printf("%s %s\n", g_failed == before ? "[pass]" : "[FAIL]", t.name);
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
