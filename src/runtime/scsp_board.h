// The Model 2A-CRX sound board as native runtime (design doc, sound): the
// recompiled 68000 program (tools/m2sndrecomp --model2a) on its native
// context, and the SCSP (MAME's, transplanted: scsp.h) receiving the i960's
// command bytes on its MIDI input. MAME's model2_snd map: sound RAM at 0
// (shared with the SCSP), the SCSP's registers at 0x100000, the program at
// 0x600000, the sample ROMs at 0x800000.
//
// No cycle clock, as on the Model 1 board: time is counted in completed
// 68000 instructions at the rate MAME's 68000 runs this program (kIps), and
// events land on that count: a command byte at the MIDI line rate, an SCSP
// timer expiring. The SCSP is rendered up to the current count before each
// register access, so every write takes effect on the right sample.
//
// The interface is the Model 1 board's (sound_board.h), so the game loop and
// the app take either: the SCSP's one stereo output comes out of take_fm(),
// take_pcm() is empty, both at 44.1 kHz.
#pragma once

#include "runtime/scsp.h"
#include "runtime/snd_gen_support.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace snd {

class ScspBoard : public Devices {
public:
    // 68000 instructions per second of board time: MAME's 68000 (11.2896
    // MHz) runs 46,585,675 instructions of this program in 60 s of attract.
    static constexpr uint64_t kIps = 776428;
    // The app opens its streams at kYmClock / 144 and kPcmClock / 224: both
    // the SCSP's rate here.
    static constexpr uint32_t kYmClock = Scsp::kRate * 144, kPcmClock = Scsp::kRate * 224;
    static constexpr uint32_t kBaud = 31250;  // MIDI-rate serial line from the i960
    static constexpr uint32_t kByteBits = 10; // start, 8 data, stop

    ScspBoard(const std::vector<uint8_t> &program, const std::vector<uint8_t> &samples);

    // Command bytes the i960 wrote to its UART; they arrive one line-time
    // apart from now on.
    void send(const uint8_t *bytes, size_t n);
    // Run the board for `seconds` of board time.
    void advance(double seconds);

    double fm_rate() const { return Scsp::kRate; }
    double pcm_rate() const { return Scsp::kRate; }
    std::vector<float> take_fm() { return std::exchange(out_, {}); } // the SCSP, interleaved stereo
    std::vector<float> take_pcm() { return {}; }

    uint64_t instructions() const { return sched_.count; }
    // The launcher's music and effects volumes: Daytona's only (the SCSP
    // mixes them itself); the app's overall volume still applies.
    void set_volumes(float, float) {}
    int voice_channel(unsigned, uint8_t) const { return -1; }
    const std::vector<uint8_t> &ram() const { return cpu_.ram; }
    size_t bytes_received() const { return received_; }

    // Devices (the 68000's view)
    uint16_t read16(uint32_t addr, uint16_t mask) override;
    void write16(uint32_t addr, uint16_t data, uint16_t mask) override;

private:
    void run_to(uint64_t count);
    void render_to(uint64_t count);

    std::vector<uint8_t> program_, samples_;
    Cpu68k cpu_;
    Sched sched_{cpu_};
    sndgen::Env env_{cpu_, sched_};
    Scsp scsp_{cpu_.ram};
    double carry_ = 0; // fractional instructions from advance()

    uint64_t line_free_ = 0; // count at which the serial line is free for the next byte
    size_t received_ = 0;
    uint64_t timer_gen_[3] = {};

    uint64_t rendered_ = 0; // samples
    std::vector<float> out_;
};

} // namespace snd
