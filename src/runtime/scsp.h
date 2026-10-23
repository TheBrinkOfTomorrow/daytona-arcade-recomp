// Yamaha YMF292-F SCSP (Saturn Custom Sound Processor), the Model 2A-CRX
// sound board's sound chip, as native C++. Transplanted from MAME's scsp.cpp
// and scspdsp.cpp (BSD-3-Clause, ElSemi, R. Belmont; THIRD_PARTY.md) with
// the device framework removed: the sound board calls read()/write() on the
// 68000's register accesses, render() to make samples at 44.1 kHz, and runs
// the chip's three timers on its own clock (timer_due / timer_expired). The
// slot, envelope, LFO and DSP arithmetic is MAME's, expression for
// expression.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace snd {

class Scsp {
public:
    static constexpr uint32_t kClock = 22579200; // 45.1584 MHz / 2 on the 2A-CRX
    static constexpr uint32_t kRate = kClock / 512; // 44.1 kHz

    // ram: the board's sound RAM (512 KB, big-endian 16-bit words), shared
    // with the 68000: the slots play from it, the DSP keeps its ring buffer
    // in it and the DMA copies to and from it.
    explicit Scsp(std::vector<uint8_t> &ram);

    // Register access at byte offset (0x000-0xfff), 16 bits with MAME's
    // write mask. The caller renders up to the access first.
    uint16_t read(uint32_t offset);
    void write(uint32_t offset, uint16_t data, uint16_t mem_mask);

    // n stereo samples (interleaved), MAME's put_int_clamp scaled to +-1.0
    // and the MVOL output gain applied.
    void render(float *out, int n);

    // A byte from the MIDI input (the i960's UART, 31.25 kbit/s).
    void midi_in(uint8_t byte);
    // Bytes the 68000 wrote to MIDI out (to the i960's UART).
    std::vector<uint8_t> take_midi_out() { std::vector<uint8_t> v; v.swap(midi_out_); return v; }

    // The interrupt lines asserted to the 68000, one bit per IPL level (bit 0
    // as MAME's line 0, which never interrupts).
    uint32_t irq_lines() const { return irq_lines_; }
    int irq_level() const {
        for (int l = 7; l > 0; --l)
            if (irq_lines_ & (1u << l)) return l;
        return 0;
    }
    // Called whenever irq_lines() may have changed.
    std::function<void()> on_irq;

    // Timers A-C: on_timer(n, seconds) arms timer n to expire `seconds` from
    // now (replacing an armed one); the board calls timer_expired(n) then.
    std::function<void(int, double)> on_timer;
    void timer_expired(int n);

private:
    enum EgState { ATTACK, DECAY1, DECAY2, RELEASE };
    struct Eg {
        int volume = 0;
        EgState state = RELEASE;
        int step = 0;
        int AR = 0, D1R = 0, D2R = 0, RR = 0;
        int DL = 0;
        uint8_t EGHOLD = 0, LPLINK = 0;
    };
    struct Lfo {
        uint16_t phase = 0;
        uint32_t phase_step = 0;
        int *table = nullptr;
        int *scale = nullptr;
    };
    struct Slot {
        uint16_t data[0x10] = {}; // only 0x1a bytes used
        uint8_t Backwards = 0, active = 0;
        uint32_t cur_addr = 0, nxt_addr = 0, step = 0;
        Eg EG;
        Lfo PLFO, ALFO;
        int slot = 0;
        int16_t Prev = 0;
    };
    struct Dsp {
        std::vector<uint8_t> *ram = nullptr;
        uint32_t RBP = 0, RBL = 0;
        int16_t COEF[64] = {};
        uint16_t MADRS[32] = {};
        uint16_t MPRO[128 * 4] = {};
        int32_t TEMP[128] = {};
        int32_t MEMS[32] = {};
        uint32_t DEC = 0;
        int32_t MIXS[16] = {};
        int16_t EXTS[2] = {};
        int16_t EFREG[16] = {};
        bool Stopped = true;
        int LastStep = 0;
        void Init();
        void SetSample(int32_t sample, int32_t SEL, int32_t MXL);
        void Step();
        void Start();
        uint16_t read_word(uint32_t a) const;
        void write_word(uint32_t a, uint16_t v);
    };

    uint8_t read_byte(uint32_t a) const;
    uint16_t read_word(uint32_t a) const;
    void write_word(uint32_t a, uint16_t v);
    uint32_t rand();
    void irq(uint32_t line, bool assert);

    void exec_dma();
    uint8_t DecodeSCI(uint8_t irq);
    void CheckPendingIRQ();
    void ResetInterrupts();
    void timer_cb(int n);
    int Get_AR(int base, int R);
    int Get_DR(int base, int R);
    void Compute_EG(Slot *slot);
    int EG_Update(Slot *slot);
    uint32_t Step(Slot *slot);
    void Compute_LFO(Slot *slot);
    void StartSlot(Slot *slot);
    void StopSlot(Slot *slot, int keyoff);
    void init();
    void UpdateSlotReg(int s, int r);
    void UpdateReg(int reg);
    void UpdateRegR(int reg);
    void w16(uint32_t addr, uint16_t val);
    uint16_t r16(uint32_t addr);
    int32_t UpdateSlot(Slot *slot);
    void LFO_Init();
    int32_t PLFO_Step(Lfo *LFO);
    int32_t ALFO_Step(Lfo *LFO);
    void LFO_ComputeStep(Lfo *LFO, uint32_t LFOF, uint32_t LFOWS, uint32_t LFOS, int ALFO);

    std::vector<uint8_t> &ram_;
    uint32_t rand_ = 0x12345678;
    uint32_t irq_lines_ = 0;
    std::vector<uint8_t> midi_out_;
    float gain_ = 1; // MVOL / 15 once written (MAME: the stream gain, 1 until then)

    uint16_t m_udata[0x30 / 2] = {};
    Slot m_Slots[32];
    int16_t m_RINGBUF[128] = {};
    uint8_t m_BUFPTR = 0;
    int16_t *m_RBUFDST = nullptr;

    uint32_t m_IrqTimA = 0, m_IrqTimBC = 0, m_IrqMidi = 0, m_IrqCPU = 0, m_IrqDMA = 0;
    uint8_t m_latched_MSLC = 0;
    uint16_t m_latched_MSLC_data = 0;
    uint8_t m_MidiStack[32] = {};
    uint8_t m_MidiW = 0, m_MidiR = 0;
    uint16_t m_mcieb = 0, m_mcipd = 0; // main CPU interrupts (not wired on Model 2)

    int32_t m_EG_TABLE[0x400] = {};
    std::vector<int> m_LPANTABLE = std::vector<int>(0x10000), m_RPANTABLE = std::vector<int>(0x10000);
    int m_TimPris[3] = {};
    int m_TimCnt[3] = {};

    struct {
        uint32_t dmea = 0;
        uint16_t drga = 0, dtlg = 0;
        uint8_t dgate = 0, ddir = 0;
    } m_dma;

    int m_ARTABLE[64] = {}, m_DRTABLE[64] = {};
    Dsp m_DSP;

    int m_PLFO_TRI[256] = {}, m_PLFO_SQR[256] = {}, m_PLFO_SAW[256] = {}, m_PLFO_NOI[256] = {};
    int m_ALFO_TRI[256] = {}, m_ALFO_SQR[256] = {}, m_ALFO_SAW[256] = {}, m_ALFO_NOI[256] = {};
    int m_PSCALES[8][256] = {};
    int m_ASCALES[8][256] = {};
};

} // namespace snd
