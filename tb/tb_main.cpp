// tb_main.cpp - self-checking Verilator testbench for link_top.
//
// Structure (the same pieces a verification IP has):
//   stimulus   : symbols and packets pushed into a TX queue, one per cycle
//   reference  : the C++ golden model runs the same stream in lockstep
//   channel    : TX code words are fed back into RX, optionally with bit flips
//   scoreboards: TX code words, RX symbols + error flags, and packet results
//                are compared against the model's expected queues, in order
//   coverage   : counters for the input space and outcomes we must have seen
//
// Usage: Vlink_top [--packets N] [--seed S] [--flip-bits B]
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "Vlink_top.h"
#include "model.hpp"
#include "verilated.h"
#if VM_TRACE
#include "verilated_vcd_c.h"
#endif

using namespace model;

namespace {

// The 12 legal control symbols, for coverage and random idle traffic.
constexpr uint8_t kKSymbols[12] = {0x1C, 0x3C, 0x5C, 0x7C, 0x9C, 0xBC, 0xDC, 0xFC, 0xF7, 0xFB, 0xFD, 0xFE};

struct Tag {
    int pkt = -1;   // packet number, -1 for idle / ordered sets
    int idx = -1;   // position in the packet (STP = 0)
    Symbol orig;    // what the stimulus sent (for end-to-end checks)
};
struct TxItem { Symbol sym; uint16_t flip; Tag tag; };
struct ExpTx { uint16_t code; bool k_err; uint16_t flip; Tag tag; };
struct ExpRx { Symbol sym; bool code_err, disp_err; Tag tag; };
struct ExpPkt { PacketEvent ev; int owner; };

struct PacketInfo {
    bool corrupted = false;
    int flip_idx = -1;
    bool code_err_at = false, disp_err_at = false, flagged_later = false;
    int events = 0;
    bool abort = false, crc_ok = false;
};

struct Coverage {
    bool enc_data[2][256] = {};  // data byte at the encoder input x running disparity
    bool enc_k[2][12] = {};      // control symbol x running disparity
    std::map<std::string, uint64_t> hits;
    void hit(const std::string& bin) { ++hits[bin]; }
};

class Testbench {
public:
    explicit Testbench(VerilatedContext* ctx) : ctx_(ctx), dut_(new Vlink_top(ctx)) {
#if VM_TRACE
        Verilated::traceEverOn(true);
        tfp_ = std::make_unique<VerilatedVcdC>();
        dut_->trace(tfp_.get(), 99);
        tfp_->open("build/wave.vcd");
#endif
    }
    ~Testbench() {
#if VM_TRACE
        tfp_->close();
#endif
        dut_->final();
    }

    // ---- infrastructure ---------------------------------------------------
    void reset() {
        dut_->rst_n = 0;
        dut_->tx_valid = 0;
        dut_->rx_code_valid = 0;
        tick();
        tick();
        dut_->rst_n = 1;
        tick();
        mtx_scr_ = Scrambler{};
        mtx_enc_ = Encoder{};
        mrx_dec_ = Decoder{};
        mrx_scr_ = Scrambler{};
        mframer_ = Framer{};
        txq_.clear(); exp_tx_.clear(); exp_rx_.clear(); exp_pkt_.clear(); direct_.clear();
        channel_pending_ = false;
        open_pkt_ = -1;
    }

    void send(Symbol s, uint16_t flip = 0, Tag tag = {}) {
        tag.orig = s;
        txq_.push_back({s, flip, tag});
    }
    void send_direct(uint16_t code) { direct_.push_back(code); }

    void drain() {
        while (!txq_.empty() || !exp_tx_.empty() || !exp_rx_.empty() || !exp_pkt_.empty() || !direct_.empty() ||
               channel_pending_) {
            step();
            if (++guard_ > 50 && txq_.empty() && direct_.empty() && !channel_pending_ && idle_cycles_ > 20) {
                fail("pipeline drained but %zu TX / %zu RX / %zu packet results never appeared", exp_tx_.size(),
                     exp_rx_.size(), exp_pkt_.size());
                exp_tx_.clear(); exp_rx_.clear(); exp_pkt_.clear();
            }
        }
        guard_ = 0;
    }

    // COM, STP, payload, [CRC], END. Optionally flips bits in one symbol's code word.
    int send_packet(const std::vector<uint8_t>& payload, bool append_crc = true, int flip_idx = -1,
                    uint16_t flip_mask = 0) {
        const int id = static_cast<int>(packets_.size());
        packets_.emplace_back();
        if (flip_idx >= 0) { packets_[id].corrupted = true; packets_[id].flip_idx = flip_idx; }
        std::vector<Symbol> syms{{STP, true}};
        for (uint8_t b : payload) syms.push_back({b, false});
        if (append_crc)
            for (uint8_t b : crc_bytes(crc32(payload.data(), payload.size()))) syms.push_back({b, false});
        syms.push_back({END, true});
        send({COM, true});  // ordered set: lets the receiver re-sync its descrambler
        for (int i = 0; i < static_cast<int>(syms.size()); ++i)
            send(syms[i], i == flip_idx ? flip_mask : 0, Tag{id, i, {}});
        return id;
    }

    void fail(const char* fmt, ...) {
        if (++errors_ <= 20) {
            va_list ap;
            va_start(ap, fmt);
            std::printf("  ERROR @cycle %llu: ", static_cast<unsigned long long>(cycle_));
            std::vprintf(fmt, ap);
            std::printf("\n");
            va_end(ap);
        }
    }

    // ---- accessors for the test phases -------------------------------------
    Vlink_top& dut() { return *dut_; }
    uint64_t errors() const { return errors_; }
    uint64_t cycles() const { return cycle_; }
    std::vector<PacketInfo>& packets() { return packets_; }
    Coverage& cov() { return cov_; }
    bool end_to_end = false;  // also require RX symbols == TX symbols
    uint64_t flags_in_clean_packets = 0, flags_outside_packets = 0;
    uint16_t last_tx_code = 0;
    bool last_tx_k_err = false;

private:
    void tick() {
        dut_->clk = 0;
        dut_->eval();
#if VM_TRACE
        if (cycle_ < 20000) tfp_->dump(ctx_->time());
#endif
        ctx_->timeInc(1);
        dut_->clk = 1;
        dut_->eval();
#if VM_TRACE
        if (cycle_ < 20000) tfp_->dump(ctx_->time());
#endif
        ctx_->timeInc(1);
        ++cycle_;
    }

    // Run the receive side of the model on one code word, in the same order as the DUT.
    void deliver(uint16_t code, const Tag& tag) {
        Decoded d = mrx_dec_.push(code);
        Symbol s = mrx_scr_.push(d.sym);
        exp_rx_.push_back({s, d.code_err, d.disp_err, tag});
        const int owner = open_pkt_;
        auto ev = mframer_.push(s);
        if (ev) {
            exp_pkt_.push_back({*ev, owner});
            open_pkt_ = -1;
        }
        // A real STP opens its packet. An STP created by a bit flip (D27.1 -> K27.7
        // is one bit away) opens a phantom packet that no stimulus packet owns.
        if (s.k && s.data == STP) open_pkt_ = tag.idx == 0 ? tag.pkt : -1;
        cov_.hit(d.code_err ? "decoder: code violation" : d.disp_err ? "decoder: disparity error" : "decoder: valid");
    }

    void step() {
        // Drive TX.
        if (!txq_.empty()) {
            TxItem it = txq_.front();
            txq_.pop_front();
            dut_->tx_valid = 1;
            dut_->tx_data = it.sym.data;
            dut_->tx_k = it.sym.k;
            const bool rd = mtx_enc_.rd();
            const Symbol scr = mtx_scr_.push(it.sym);
            bool k_err = false;
            const uint16_t code = mtx_enc_.push(scr, &k_err);
            if (k_err) cov_.hit("encoder: illegal K flagged");
            else if (scr.k) { for (int i = 0; i < 12; ++i) if (kKSymbols[i] == scr.data) cov_.enc_k[rd][i] = true; }
            else cov_.enc_data[rd][scr.data] = true;
            if (it.sym.k && it.sym.data == COM) cov_.hit("scrambler: COM reset");
            if (it.sym.k && it.sym.data == SKP) cov_.hit("scrambler: SKP hold");
            exp_tx_.push_back({code, k_err, it.flip, it.tag});
        } else {
            dut_->tx_valid = 0;
        }
        // Drive RX: from the channel, or directly (decoder-only tests).
        if (channel_pending_) {
            deliver(channel_code_, channel_tag_);
            dut_->rx_code_valid = 1;
            dut_->rx_code = channel_code_;
            channel_pending_ = false;
        } else if (!direct_.empty()) {
            const uint16_t code = direct_.front();
            direct_.pop_front();
            deliver(code, Tag{});
            dut_->rx_code_valid = 1;
            dut_->rx_code = code;
        } else {
            dut_->rx_code_valid = 0;
        }

        tick();
        bool activity = false;

        // TX scoreboard.
        if (dut_->tx_code_valid) {
            activity = true;
            if (exp_tx_.empty()) {
                fail("unexpected TX code word 0x%03X", dut_->tx_code);
            } else {
                ExpTx e = exp_tx_.front();
                exp_tx_.pop_front();
                if (dut_->tx_code != e.code || bool(dut_->tx_k_err) != e.k_err)
                    fail("TX code 0x%03X k_err %d, model 0x%03X k_err %d (symbol %s)", dut_->tx_code,
                         dut_->tx_k_err, e.code, e.k_err, name(e.tag.orig).c_str());
                last_tx_code = dut_->tx_code;
                last_tx_k_err = dut_->tx_k_err;
                // Channel: deliver next cycle, with the scheduled bit flips.
                channel_pending_ = true;
                channel_code_ = static_cast<uint16_t>(dut_->tx_code ^ e.flip);
                channel_tag_ = e.tag;
            }
        }

        // RX scoreboard.
        if (dut_->rx_valid) {
            activity = true;
            if (exp_rx_.empty()) {
                fail("unexpected RX symbol");
            } else {
                ExpRx e = exp_rx_.front();
                exp_rx_.pop_front();
                const Symbol got{static_cast<uint8_t>(dut_->rx_data), bool(dut_->rx_k)};
                const bool ce = dut_->rx_code_err, de = dut_->rx_disp_err;
                if (!(got == e.sym) || ce != e.code_err || de != e.disp_err)
                    fail("RX %s code_err %d disp_err %d, model %s %d %d", name(got).c_str(), ce, de,
                         name(e.sym).c_str(), e.code_err, e.disp_err);
                if (end_to_end && !(got == e.tag.orig))
                    fail("end-to-end: sent %s, received %s", name(e.tag.orig).c_str(), name(got).c_str());
                if (ce || de) record_flag(e.tag, ce, de);
            }
        } else if (dut_->rx_code_err || dut_->rx_disp_err) {
            fail("error flag without rx_valid");
        }

        // Packet scoreboard.
        if (dut_->pkt_done || dut_->pkt_abort) {
            activity = true;
            if (exp_pkt_.empty()) {
                fail("unexpected packet result");
            } else {
                ExpPkt e = exp_pkt_.front();
                exp_pkt_.pop_front();
                PacketEvent got{bool(dut_->pkt_abort), bool(dut_->pkt_crc_ok), dut_->pkt_crc};
                if (dut_->pkt_done && dut_->pkt_abort) fail("pkt_done and pkt_abort together");
                if (!(got == e.ev))
                    fail("packet: abort %d crc_ok %d crc %08X, model abort %d crc_ok %d crc %08X", got.abort,
                         got.crc_ok, got.crc, e.ev.abort, e.ev.crc_ok, e.ev.crc);
                if (e.owner >= 0) {
                    PacketInfo& p = packets_[e.owner];
                    ++p.events;
                    p.abort = got.abort;
                    p.crc_ok = got.crc_ok;
                }
                cov_.hit(got.abort ? "packet: aborted" : got.crc_ok ? "packet: CRC ok" : "packet: CRC bad");
                last_pkt_crc = got.crc;
            }
        }
        idle_cycles_ = activity ? 0 : idle_cycles_ + 1;
    }

    void record_flag(const Tag& t, bool code_err, bool disp_err) {
        if (t.pkt < 0) { ++flags_outside_packets; return; }
        PacketInfo& p = packets_[t.pkt];
        if (!p.corrupted) { ++flags_in_clean_packets; return; }
        if (t.idx == p.flip_idx) { p.code_err_at |= code_err; p.disp_err_at |= disp_err; }
        else if (t.idx > p.flip_idx) p.flagged_later = true;
    }

public:
    uint32_t last_pkt_crc = 0;

private:
    VerilatedContext* ctx_;
    std::unique_ptr<Vlink_top> dut_;
#if VM_TRACE
    std::unique_ptr<VerilatedVcdC> tfp_;
#endif
    uint64_t cycle_ = 0, errors_ = 0;
    int guard_ = 0, idle_cycles_ = 0;

    Scrambler mtx_scr_, mrx_scr_;
    Encoder mtx_enc_;
    Decoder mrx_dec_;
    Framer mframer_;
    int open_pkt_ = -1;

    std::deque<TxItem> txq_;
    std::deque<uint16_t> direct_;
    std::deque<ExpTx> exp_tx_;
    std::deque<ExpRx> exp_rx_;
    std::deque<ExpPkt> exp_pkt_;
    bool channel_pending_ = false;
    uint16_t channel_code_ = 0;
    Tag channel_tag_;

    std::vector<PacketInfo> packets_;
    Coverage cov_;
};

// ---------------------------------------------------------------------------
// Test phases

void phase(const char* name, Testbench& tb, uint64_t errors_before) {
    std::printf("[%s] %s\n", tb.errors() == errors_before ? "pass" : "FAIL", name);
}

void directed_tests(Testbench& tb) {
    uint64_t e0 = tb.errors();
    tb.reset();
    // Independent of the model: K28.5 on the wire is 0011111010 in RD-, then
    // 1100000101 once RD has flipped to RD+.
    tb.send({COM, true});
    tb.drain();
    if (tb.last_tx_code != 0b0011111010) tb.fail("first COM should be 0x0FA, got 0x%03X", tb.last_tx_code);
    tb.send({COM, true});
    tb.drain();
    if (tb.last_tx_code != 0b1100000101) tb.fail("second COM should be 0x305, got 0x%03X", tb.last_tx_code);

    // CRC-32 standard check value, computed by the RTL CRC over "123456789".
    const std::string s = "123456789";
    tb.send_packet(std::vector<uint8_t>(s.begin(), s.end()), /*append_crc=*/false);
    tb.drain();
    if (tb.last_pkt_crc != 0xCBF43926u) tb.fail("CRC of \"123456789\" = %08X, expected CBF43926", tb.last_pkt_crc);

    // Illegal control symbol request (K0.0) is flagged and sent as data.
    tb.send({0x00, true});
    tb.drain();
    if (!tb.last_tx_k_err) tb.fail("K0.0 should raise tx_k_err");

    // A control symbol in the middle of a packet aborts it.
    tb.send({COM, true});
    tb.send({STP, true});
    tb.send({0x11, false});
    tb.send({SKP, true});
    tb.drain();
    phase("directed: K28.5 on the wire, CRC check value, illegal K, abort", tb, e0);
}

// Every possible 10-bit input, in both running-disparity states, straight into
// the decoder. 2048 cases: valid / disparity error / code violation must match.
void exhaustive_decoder(Testbench& tb) {
    uint64_t e0 = tb.errors();
    for (int rd = 0; rd < 2; ++rd)
        for (int code = 0; code < 1024; ++code) {
            tb.reset();
            if (rd) tb.send_direct(0b0011111010);  // K28.5 RD-: legal, leaves RD+
            tb.send_direct(static_cast<uint16_t>(code));
            tb.drain();
        }
    phase("exhaustive decoder: 1024 code words x 2 disparities", tb, e0);
}

std::vector<uint8_t> random_payload(std::mt19937_64& rng) {
    std::vector<uint8_t> p(1 + rng() % 64);
    for (auto& b : p) b = static_cast<uint8_t>(rng());
    if (rng() % 8 == 0) std::fill(p.begin(), p.end(), 0);  // long runs: scrambler matters here
    return p;
}

void idle(Testbench& tb, std::mt19937_64& rng) {
    const int n = static_cast<int>(rng() % 4);
    for (int i = 0; i < n; ++i) {
        if (rng() % 6 == 0) {
            uint8_t k;
            do k = kKSymbols[rng() % 12]; while (k == STP);  // anything but a packet start
            tb.send({k, true});
        } else {
            tb.send({0x00, false});  // logical idle
        }
    }
}

void random_clean(Testbench& tb, std::mt19937_64& rng, int npackets) {
    uint64_t e0 = tb.errors();
    tb.reset();
    tb.end_to_end = true;
    const size_t first = tb.packets().size();
    for (int i = 0; i < npackets; ++i) {
        tb.send_packet(random_payload(rng));
        idle(tb, rng);
        if (i % 256 == 255) tb.drain();
    }
    tb.drain();
    tb.end_to_end = false;
    size_t bad = 0;
    for (size_t i = first; i < tb.packets().size(); ++i)
        if (tb.packets()[i].events != 1 || !tb.packets()[i].crc_ok) ++bad;
    if (bad) tb.fail("%zu clean packets not received with a good CRC", bad);
    char name[96];
    std::snprintf(name, sizeof name, "random traffic: %d packets, end-to-end data and CRC", npackets);
    phase(name, tb, e0);
}

// Flip `bits` distinct bits of one code word in half of the packets.
void error_injection(Testbench& tb, std::mt19937_64& rng, int npackets, int bits) {
    uint64_t e0 = tb.errors();
    tb.reset();
    tb.flags_in_clean_packets = 0;
    const size_t first = tb.packets().size();
    for (int i = 0; i < npackets; ++i) {
        auto payload = random_payload(rng);
        if (rng() % 2) {
            const int n_data = static_cast<int>(payload.size()) + 4;  // payload + CRC symbols
            const int idx = 1 + static_cast<int>(rng() % n_data);     // never STP (0) or END
            uint16_t mask = 0;
            while (__builtin_popcount(mask) < bits) mask = static_cast<uint16_t>(mask | (1u << (rng() % 10)));
            tb.send_packet(payload, true, idx, mask);
        } else {
            tb.send_packet(payload);
        }
        idle(tb, rng);
        if (i % 256 == 255) tb.drain();
    }
    tb.drain();

    size_t corrupted = 0, at_code = 0, at_disp = 0, later = 0, aborted = 0, crc_only = 0, undetected = 0;
    size_t crc_caught = 0, clean = 0, clean_bad = 0;
    for (size_t i = first; i < tb.packets().size(); ++i) {
        const PacketInfo& p = tb.packets()[i];
        if (p.events != 1) tb.fail("packet %zu produced %d results", i, p.events);
        if (!p.corrupted) {
            ++clean;
            if (!p.crc_ok) ++clean_bad;
            continue;
        }
        ++corrupted;
        if (!p.abort && !p.crc_ok) ++crc_caught;
        if (p.code_err_at) ++at_code;
        else if (p.disp_err_at) ++at_disp;
        else if (p.flagged_later) ++later;
        else if (p.abort) ++aborted;
        else if (!p.crc_ok) ++crc_only;
        else ++undetected;
    }
    if (undetected) tb.fail("%zu corrupted packets were accepted", undetected);
    if (clean_bad) tb.fail("%zu clean packets failed", clean_bad);
    char name[96];
    std::snprintf(name, sizeof name, "error injection: %d packets, %d-bit flips in one symbol", npackets, bits);
    phase(name, tb, e0);

    auto pct = [&](size_t n) { return corrupted ? 100.0 * static_cast<double>(n) / static_cast<double>(corrupted) : 0.0; };
    std::printf("      corrupted packets                         %6zu\n", corrupted);
    std::printf("      first caught by: code violation at symbol  %6zu  (%5.1f%%)\n", at_code, pct(at_code));
    std::printf("                       disparity error at symbol %6zu  (%5.1f%%)\n", at_disp, pct(at_disp));
    std::printf("                       disparity error later     %6zu  (%5.1f%%)\n", later, pct(later));
    std::printf("                       packet aborted (became K) %6zu  (%5.1f%%)\n", aborted, pct(aborted));
    std::printf("                       CRC only                  %6zu  (%5.1f%%)\n", crc_only, pct(crc_only));
    std::printf("                       NOT DETECTED              %6zu\n", undetected);
    std::printf("      CRC failed on                             %6zu  (%5.1f%%) of corrupted packets\n", crc_caught,
                pct(crc_caught));
    std::printf("      clean packets: %zu, all CRC ok: %s; decoder flags in clean packets (RD resync): %llu\n", clean,
                clean_bad ? "NO" : "yes", static_cast<unsigned long long>(tb.flags_in_clean_packets));
}

bool coverage_report(Testbench& tb) {
    Coverage& c = tb.cov();
    int data = 0, k = 0;
    for (int rd = 0; rd < 2; ++rd) {
        for (int b = 0; b < 256; ++b) data += c.enc_data[rd][b];
        for (int i = 0; i < 12; ++i) k += c.enc_k[rd][i];
    }
    const char* required[] = {"decoder: valid", "decoder: disparity error", "decoder: code violation",
                              "packet: CRC ok", "packet: CRC bad", "packet: aborted",
                              "scrambler: COM reset", "scrambler: SKP hold", "encoder: illegal K flagged"};
    int hit = 0;
    std::printf("coverage:\n");
    std::printf("  encoder data symbols x RD   %3d / 512\n", data);
    std::printf("  encoder control symbols x RD %3d / 24\n", k);
    for (const char* r : required) {
        const uint64_t n = c.hits.count(r) ? c.hits[r] : 0;
        hit += n > 0;
        std::printf("  %-28s %llu\n", r, static_cast<unsigned long long>(n));
    }
    const int nreq = static_cast<int>(sizeof required / sizeof *required);
    const bool full = data == 512 && k == 24 && hit == nreq;
    std::printf("  functional coverage: %s\n", full ? "100%" : "INCOMPLETE");
    return full;
}

}  // namespace

int main(int argc, char** argv) {
    int npackets = 20000, flip_bits = 1;
    uint64_t seed = 1;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--packets") && i + 1 < argc) npackets = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--flip-bits") && i + 1 < argc) flip_bits = std::atoi(argv[++i]);
    }
    auto ctx = std::make_unique<VerilatedContext>();
    ctx->commandArgs(argc, argv);
    Testbench tb(ctx.get());
    std::mt19937_64 rng(seed);
    std::printf("link_top testbench, seed %llu\n", static_cast<unsigned long long>(seed));

    directed_tests(tb);
    exhaustive_decoder(tb);
    random_clean(tb, rng, npackets);
    error_injection(tb, rng, npackets, std::clamp(flip_bits, 1, 10));
    const bool covered = coverage_report(tb);

    std::printf("%llu cycles simulated, %llu errors\n", static_cast<unsigned long long>(tb.cycles()),
                static_cast<unsigned long long>(tb.errors()));
    const bool ok = tb.errors() == 0 && covered;
    std::printf("%s\n", ok ? "TEST PASSED" : "TEST FAILED");
    return ok ? 0 : 1;
}
