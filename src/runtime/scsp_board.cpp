#include "runtime/scsp_board.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace snd {

ScspBoard::ScspBoard(const std::vector<uint8_t> &program, const std::vector<uint8_t> &samples)
    : program_(program), samples_(samples) {
    if (program_.size() < 0x40000) throw rt::Fatal("sound program image missing or short (re-run the importer)");
    if (samples_.size() < 0x800000) throw rt::Fatal("sample ROM image missing or short (re-run the importer)");
    program_.resize(0x80000); // the program region (512 KB; the ROM fills half)
    samples_.resize(0x800000);
    cpu_.rom = program_.data();
    cpu_.samples = samples_.data();
    cpu_.dev = this;
    std::copy(program_.begin(), program_.begin() + 16, cpu_.ram.begin()); // MAME reset_model2_scsp: the reset vectors
    cpu_.reset();

    scsp_.on_irq = [this] {
        cpu_.irq_line = scsp_.irq_level();
        sched_.poke();
    };
    scsp_.on_timer = [this](int n, double seconds) {
        const uint64_t gen = ++timer_gen_[n];
        const uint64_t at = sched_.count + uint64_t(std::llround(seconds * double(kIps)));
        sched_.at(at, [this, n, gen] {
            if (gen == timer_gen_[n]) scsp_.timer_expired(n);
        });
    };
}

void ScspBoard::send(const uint8_t *bytes, size_t n) {
    constexpr uint64_t kByteTime = kIps * kByteBits / kBaud; // instructions per byte on the line
    for (size_t i = 0; i < n; ++i) {
        const uint64_t at = std::max(line_free_, sched_.count) + kByteTime;
        line_free_ = at;
        const uint8_t b = bytes[i];
        sched_.at(at, [this, b] {
            scsp_.midi_in(b);
            ++received_;
        });
    }
}

void ScspBoard::advance(double seconds) {
    const double want = seconds * double(kIps) + carry_;
    const uint64_t n = uint64_t(want);
    carry_ = want - double(n);
    run_to(sched_.count + n);
}

void ScspBoard::run_to(uint64_t count) {
    while (sched_.count < count) {
        sched_.end_count = count;
        if (!sndgen::has_code(cpu_.pc)) {
            char b[96];
            std::snprintf(b, sizeof b, "sound 68000: no recompiled code at %06x", cpu_.pc);
            throw rt::Fatal(b);
        }
        sndgen::run(env_);
    }
    render_to(count);
    scsp_.take_midi_out(); // the i960 never reads its UART (as in MAME)
}

void ScspBoard::render_to(uint64_t count) {
    const uint64_t due = count * Scsp::kRate / kIps;
    if (due <= rendered_) return;
    const size_t n = size_t(due - rendered_), at = out_.size();
    out_.resize(at + 2 * n);
    scsp_.render(out_.data() + at, int(n));
    rendered_ = due;
}

uint16_t ScspBoard::read16(uint32_t addr, uint16_t) {
    if (addr >= 0x100000 && addr <= 0x100fff) {
        render_to(sched_.count);
        return scsp_.read(addr - 0x100000);
    }
    return 0;
}

void ScspBoard::write16(uint32_t addr, uint16_t data, uint16_t mask) {
    if (addr >= 0x100000 && addr <= 0x100fff) {
        render_to(sched_.count);
        scsp_.write(addr - 0x100000, data, mask);
    }
    // 0x400000: the sample banks (MAME model2snd_ctrl): only sets with more
    // than 8 MB of samples bank them; Sega Rally's 8 MB are mapped flat
}

} // namespace snd
