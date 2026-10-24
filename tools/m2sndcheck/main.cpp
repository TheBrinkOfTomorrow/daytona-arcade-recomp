// m2sndcheck: lockstep check of the recompiled sound 68000 against MAME
// (design doc, sound). MAME's log (M2TRACE_SNDLOG, patches/mame/0003) gives
// every device access the sound CPU made, each interrupt it took and its
// registers every 4096 instructions and after each interrupt, all tagged
// with instruction counts. This runs the recompiled code from reset with the
// logged device reads fed back and the interrupts applied at the logged
// counts, and fails on the first device access, register or PC that differs.
//
//   m2sndcheck SOUND_PROGRAM.bin SNDLOG [SAMPLES.bin]
//
// On the Model 2A board (Sega Rally) the SCSP's register accesses are
// checked with their byte masks, and SAMPLES.bin is the sample ROM the
// 68000 can read at 0x800000.
//
// Test-only: the log comes from a MAME run on the user's ROMs (git-ignored).

#include "runtime/snd_gen_support.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Access {
    uint64_t idx;
    bool write;
    uint32_t addr, val;
    uint32_t mask = 0; // SCSP accesses: the byte mask (0: not logged)
};
struct Snap {
    uint64_t idx;
    uint32_t pc, sr, r[17]; // d0-d7, a0-a6, a7 (active), other sp
};

[[noreturn]] void fail(const std::string &m) {
    std::fprintf(stderr, "m2sndcheck: MISMATCH: %s\n", m.c_str());
    std::exit(1);
}

class Replay : public snd::Devices {
public:
    Replay(std::vector<Access> acc, const snd::Sched &s) : acc_(std::move(acc)), s_(s) {}
    uint8_t read(uint32_t addr) override { return uint8_t(take(false, addr, 0)); }
    void write(uint32_t addr, uint16_t data) override { take(true, addr, data); }
    uint16_t read16(uint32_t addr, uint16_t mask) override { return uint16_t(take(false, addr, 0, mask)); }
    void write16(uint32_t addr, uint16_t data, uint16_t mask) override { take(true, addr, data, mask); }
    size_t done() const { return next_; }
    size_t total() const { return acc_.size(); }

private:
    uint32_t take(bool w, uint32_t addr, uint32_t data, uint32_t mask = 0) {
        char b[256];
        if (next_ >= acc_.size()) {
            std::snprintf(b, sizeof b, "extra %s %06x at instruction %" PRIu64 " (log has %zu accesses)", w ? "write" : "read", addr,
                          s_.count, acc_.size());
            fail(b);
        }
        const Access &a = acc_[next_++];
        if (a.write != w || a.addr != addr || a.idx != s_.count || (w && a.val != data) || (a.mask && a.mask != mask)) {
            std::snprintf(b, sizeof b, "access %zu: native %s %06x=%x (mask %x) at instruction %" PRIu64 ", MAME %s %06x=%x (mask %x) at %" PRIu64,
                          next_ - 1, w ? "write" : "read", addr, data, mask, s_.count, a.write ? "write" : "read", a.addr, a.val, a.mask, a.idx);
            fail(b);
        }
        return a.val;
    }
    std::vector<Access> acc_;
    size_t next_ = 0;
    const snd::Sched &s_;
};

std::vector<uint8_t> load(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) fail("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        std::fprintf(stderr, "usage: m2sndcheck SOUND_PROGRAM.bin SNDLOG [SAMPLES.bin]\n");
        return 2;
    }
    std::vector<uint8_t> rom = load(argv[1]), samples;
    if (snd::kScspBoard) {
        if (argc != 4) fail("the Model 2A board needs SAMPLES.bin");
        samples = load(argv[3]);
        rom.resize(0x80000);      // the program region, 512 KB
        samples.resize(0x800000); // 0x800000-0xffffff
    }
    std::ifstream log(argv[2]);
    if (!log) fail(std::string("cannot open ") + argv[2]);

    std::vector<Access> acc;
    std::vector<Snap> snaps;
    std::vector<std::pair<uint64_t, int>> irqs;
    std::string line;
    while (std::getline(log, line)) {
        if (line.size() < 3) continue;
        std::istringstream s(line.substr(2));
        if (line[0] == 'r' || line[0] == 'w') {
            Access a{};
            a.write = line[0] == 'w';
            s >> std::dec >> a.idx >> std::hex >> a.addr >> a.val;
            if (!(s >> a.mask)) a.mask = 0;
            acc.push_back(a);
        } else if (line[0] == 'i') {
            uint64_t n;
            int lv;
            s >> std::dec >> n >> lv;
            irqs.emplace_back(n, lv);
        } else if (line[0] == 's') {
            Snap p{};
            s >> std::dec >> p.idx >> std::hex >> p.pc >> p.sr;
            for (uint32_t &r : p.r) s >> r;
            // The level MAME's 68000 took is the mask in the handler's first
            // snapshot: its log line has the level asserted when the
            // interrupt starts, which can have risen since it was sampled.
            if (!irqs.empty() && irqs.back().first == p.idx) irqs.back().second = int((p.sr >> 8) & 7);
            snaps.push_back(p);
        }
    }
    if (snaps.empty()) fail("no register snapshots in the log");

    snd::Cpu68k c;
    snd::Sched s(c);
    Replay dev(std::move(acc), s);
    c.rom = rom.data();
    c.samples = samples.data();
    c.dev = &dev;
    if (snd::kScspBoard) std::copy(rom.begin(), rom.begin() + 16, c.ram.begin()); // MAME's reset_model2_scsp: the vectors
    c.reset();
    s.auto_irq = false;

    // Interrupts before the snapshot at the same count (MAME logs the
    // interrupt, then the first instruction of the handler).
    size_t irq_i = 0;
    int taken = 0;
    for (const Snap &p : snaps) {
        for (; irq_i < irqs.size() && irqs[irq_i].first <= p.idx; ++irq_i) {
            const auto [n, lv] = irqs[irq_i];
            s.at(n, [&c, &taken, n = n, lv = lv] {
                if (lv <= c.mask()) {
                    char b[128];
                    std::snprintf(b, sizeof b, "MAME took a level %d interrupt at %" PRIu64 " but the mask is %d", lv, n, c.mask());
                    fail(b);
                }
                c.take_irq(lv);
                ++taken;
            });
        }
        s.at(p.idx, [&c, p] {
            uint32_t mine[17];
            std::memcpy(mine, c.d, 32);
            std::memcpy(mine + 8, c.a, 28);
            mine[15] = c.a[7];
            mine[16] = c.other_sp;
            static const char *names[17] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a0", "a1",
                                            "a2", "a3", "a4", "a5", "a6", "a7", "other sp"};
            char b[256];
            if (c.pc != p.pc || c.sr() != p.sr) {
                std::snprintf(b, sizeof b, "at instruction %" PRIu64 ": native pc %06x sr %04x, MAME pc %06x sr %04x", p.idx, c.pc, c.sr(),
                              p.pc, p.sr);
                fail(b);
            }
            for (int i = 0; i < 17; ++i)
                if (mine[i] != p.r[i]) {
                    std::snprintf(b, sizeof b, "at instruction %" PRIu64 " (pc %06x): %s native %08x, MAME %08x", p.idx, c.pc, names[i],
                                  mine[i], p.r[i]);
                    fail(b);
                }
        });
    }
    s.end_count = snaps.back().idx + 1; // through the last checkpoint

    sndgen::Env env{c, s};
    try {
        while (!s.finished()) {
            if (!sndgen::has_code(c.pc)) {
                char b[96];
                std::snprintf(b, sizeof b, "no recompiled code at %06x (instruction %" PRIu64 ")", c.pc, s.count);
                fail(b);
            }
            sndgen::run(env);
        }
    } catch (const rt::Fatal &e) {
        fail(e.what());
    }
    std::printf("m2sndcheck: MATCH: %" PRIu64 " instructions, %zu register checkpoints, %d interrupts, %zu of %zu device accesses\n",
                s.count, snaps.size(), taken, dev.done(), dev.total());
    return 0;
}
