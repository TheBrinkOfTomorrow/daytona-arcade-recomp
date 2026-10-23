// license:BSD-3-Clause
// copyright-holders:ElSemi, R. Belmont
// thanks-to: kingshriek
//
// SCSP, from MAME's src/devices/sound/scsp.cpp and scspdsp.cpp at
// dddd73680656e355bb2b5beecab1167c9f07bf81 (BSD-3-Clause; notice above
// kept; THIRD_PARTY.md). Framework (streams, timers, save states, the
// serial interface, device_rom_interface) replaced by plain members and the
// board's callbacks; the arithmetic is unchanged. Not wired on Model 2 and
// left out: the main CPU's interrupt output, EXTS (CD audio) input. The
// noise source is a fixed pseudo-random sequence (MAME uses its machine's).
#include "runtime/scsp.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace snd {

namespace {

constexpr int SHIFT = 12;
constexpr int LFO_SHIFT = 8;
constexpr int EG_SHIFT = 16;
inline uint32_t FIX(float v) { return uint32_t(float(1 << SHIFT) * v); }

// slot parameters
#define KEYONEX(slot) ((slot->data[0x0] >> 0x0) & 0x1000)
#define KEYONB(slot) ((slot->data[0x0] >> 0x0) & 0x0800)
#define SBCTL(slot) ((slot->data[0x0] >> 0x9) & 0x0003)
#define SSCTL(slot) ((slot->data[0x0] >> 0x7) & 0x0003)
#define LPCTL(slot) ((slot->data[0x0] >> 0x5) & 0x0003)
#define PCM8B(slot) ((slot->data[0x0] >> 0x0) & 0x0010)
#define SA(slot) (((slot->data[0x0] & 0xF) << 16) | (slot->data[0x1]))
#define LSA(slot) (slot->data[0x2])
#define LEA(slot) (slot->data[0x3])
#define D2R(slot) ((slot->data[0x4] >> 0xB) & 0x001F)
#define D1R(slot) ((slot->data[0x4] >> 0x6) & 0x001F)
#define EGHOLD(slot) ((slot->data[0x4] >> 0x0) & 0x0020)
#define AR(slot) ((slot->data[0x4] >> 0x0) & 0x001F)
#define LPSLNK(slot) ((slot->data[0x5] >> 0x0) & 0x4000)
#define KRS(slot) ((slot->data[0x5] >> 0xA) & 0x000F)
#define DL(slot) ((slot->data[0x5] >> 0x5) & 0x001F)
#define RR(slot) ((slot->data[0x5] >> 0x0) & 0x001F)
#define STWINH(slot) ((slot->data[0x6] >> 0x0) & 0x0200)
#define SDIR(slot) ((slot->data[0x6] >> 0x0) & 0x0100)
#define TL(slot) ((slot->data[0x6] >> 0x0) & 0x00FF)
#define MDL(slot) ((slot->data[0x7] >> 0xC) & 0x000F)
#define MDXSL(slot) ((slot->data[0x7] >> 0x6) & 0x003F)
#define MDYSL(slot) ((slot->data[0x7] >> 0x0) & 0x003F)
#define OCT(slot) ((slot->data[0x8] >> 0xB) & 0x000F)
#define FNS(slot) ((slot->data[0x8] >> 0x0) & 0x03FF)
#define LFORE(slot) ((slot->data[0x9] >> 0x0) & 0x8000)
#define LFOF(slot) ((slot->data[0x9] >> 0xA) & 0x001F)
#define PLFOWS(slot) ((slot->data[0x9] >> 0x8) & 0x0003)
#define PLFOS(slot) ((slot->data[0x9] >> 0x5) & 0x0007)
#define ALFOWS(slot) ((slot->data[0x9] >> 0x3) & 0x0003)
#define ALFOS(slot) ((slot->data[0x9] >> 0x0) & 0x0007)
#define ISEL(slot) ((slot->data[0xA] >> 0x3) & 0x000F)
#define IMXL(slot) ((slot->data[0xA] >> 0x0) & 0x0007)
#define DISDL(slot) ((slot->data[0xB] >> 0xD) & 0x0007)
#define DIPAN(slot) ((slot->data[0xB] >> 0x8) & 0x001F)
#define EFSDL(slot) ((slot->data[0xB] >> 0x5) & 0x0007)
#define EFPAN(slot) ((slot->data[0xB] >> 0x0) & 0x001F)

// envelope times in ms
const double ARTimes[64] = {100000 /*infinity*/, 100000 /*infinity*/, 8100.0, 6900.0, 6000.0, 4800.0, 4000.0, 3400.0, 3000.0, 2400.0,
                            2000.0, 1700.0, 1500.0, 1200.0, 1000.0, 860.0, 760.0, 600.0, 500.0, 430.0, 380.0, 300.0, 250.0, 220.0,
                            190.0, 150.0, 130.0, 110.0, 95.0, 76.0, 63.0, 55.0, 47.0, 38.0, 31.0, 27.0, 24.0, 19.0, 15.0, 13.0,
                            12.0, 9.4, 7.9, 6.8, 6.0, 4.7, 3.8, 3.4, 3.0, 2.4, 2.0, 1.8, 1.6, 1.3, 1.1, 0.93, 0.85, 0.65, 0.53,
                            0.44, 0.40, 0.35, 0.0, 0.0};
const double DRTimes[64] = {100000 /*infinity*/, 100000 /*infinity*/, 118200.0, 101300.0, 88600.0, 70900.0, 59100.0, 50700.0, 44300.0,
                            35500.0, 29600.0, 25300.0, 22200.0, 17700.0, 14800.0, 12700.0, 11100.0, 8900.0, 7400.0, 6300.0, 5500.0,
                            4400.0, 3700.0, 3200.0, 2800.0, 2200.0, 1800.0, 1600.0, 1400.0, 1100.0, 920.0, 790.0, 690.0, 550.0,
                            460.0, 390.0, 340.0, 270.0, 230.0, 200.0, 170.0, 140.0, 110.0, 98.0, 85.0, 68.0, 57.0, 49.0, 43.0,
                            34.0, 28.0, 25.0, 22.0, 18.0, 14.0, 12.0, 11.0, 8.5, 7.1, 6.1, 5.4, 4.3, 3.6, 3.1};

#define MEM4B() ((m_udata[0] >> 0x0) & 0x0200)
#define DAC18B() ((m_udata[0] >> 0x0) & 0x0100)
#define MVOL() ((m_udata[0] >> 0x0) & 0x000F)
#define RBL() ((m_udata[1] >> 0x7) & 0x0003)
#define RBP() ((m_udata[1] >> 0x0) & 0x003F)

#define SCILV0() ((m_udata[0x24 / 2] >> 0x0) & 0xff)
#define SCILV1() ((m_udata[0x26 / 2] >> 0x0) & 0xff)
#define SCILV2() ((m_udata[0x28 / 2] >> 0x0) & 0xff)

constexpr uint8_t SCIMID = 3, SCIDMA = 4, SCIIRQ = 5, SCITMA = 6, SCITMB = 7;

const float SDLT[8] = {-1000000.0f, -36.0f, -30.0f, -24.0f, -18.0f, -12.0f, -6.0f, 0.0f};

inline int32_t sext24(int32_t v) { return int32_t(uint32_t(v) << 8) >> 8; }
inline int32_t sext13(int32_t v) { return int32_t(uint32_t(v) << 19) >> 19; }

uint16_t PACK(int32_t val) {
    const int sign = (val >> 23) & 1;
    uint32_t temp = (val ^ (val << 1)) & 0xFFFFFF;
    int exponent = 0;
    for (int k = 0; k < 12; k++) {
        if (temp & 0x800000) break;
        temp <<= 1;
        exponent += 1;
    }
    if (exponent < 12) val = (val << exponent) & 0x3FFFFF;
    else val <<= 11;
    val >>= 11;
    val &= 0x7FF;
    val |= sign << 15;
    val |= exponent << 11;
    return uint16_t(val);
}

int32_t UNPACK(uint16_t val) {
    const int sign = (val >> 15) & 1;
    int exponent = (val >> 11) & 0xF;
    const int mantissa = val & 0x7FF;
    int32_t uval = mantissa << 11;
    if (exponent > 11) {
        exponent = 11;
        uval |= sign << 22;
    } else {
        uval |= (sign ^ 1) << 22;
    }
    uval |= sign << 23;
    uval <<= 8;
    uval >>= 8;
    uval >>= exponent;
    return uval;
}

} // namespace

// ---------------------------------------------------------------------------
// memory: the sound RAM on the SCSP's 20-bit bus (MAME's scsp_map: RAM at
// 0-0x7ffff, the rest unmapped and reading 0)

uint8_t Scsp::read_byte(uint32_t a) const {
    a &= 0xfffff;
    return a < ram_.size() ? ram_[a] : 0;
}
uint16_t Scsp::read_word(uint32_t a) const {
    a &= 0xffffe;
    return a < ram_.size() ? uint16_t(ram_[a] << 8 | ram_[a + 1]) : 0;
}
void Scsp::write_word(uint32_t a, uint16_t v) {
    a &= 0xffffe;
    if (a < ram_.size()) ram_[a] = uint8_t(v >> 8), ram_[a + 1] = uint8_t(v);
}
uint16_t Scsp::Dsp::read_word(uint32_t a) const {
    a &= 0xffffe;
    return a < ram->size() ? uint16_t((*ram)[a] << 8 | (*ram)[a + 1]) : 0;
}
void Scsp::Dsp::write_word(uint32_t a, uint16_t v) {
    a &= 0xffffe;
    if (a < ram->size()) (*ram)[a] = uint8_t(v >> 8), (*ram)[a + 1] = uint8_t(v);
}

uint32_t Scsp::rand() {
    rand_ = rand_ * 1664525u + 1013904223u;
    return rand_ >> 8;
}

void Scsp::irq(uint32_t line, bool assert) {
    if (line > 7) return;
    const uint32_t before = irq_lines_;
    if (assert) irq_lines_ |= 1u << line;
    else irq_lines_ &= ~(1u << line);
    if (irq_lines_ != before && on_irq) on_irq();
}

// ---------------------------------------------------------------------------

Scsp::Scsp(std::vector<uint8_t> &ram) : ram_(ram) {
    init();
}

uint8_t Scsp::DecodeSCI(uint8_t irq) {
    uint8_t SCI = 0;
    uint8_t v;
    v = (SCILV0() & (1 << irq)) ? 1 : 0;
    SCI |= v;
    v = (SCILV1() & (1 << irq)) ? 1 : 0;
    SCI |= v << 1;
    v = (SCILV2() & (1 << irq)) ? 1 : 0;
    SCI |= v << 2;
    return SCI;
}

void Scsp::CheckPendingIRQ() {
    uint32_t pend = m_udata[0x20 / 2];
    const uint32_t en = m_udata[0x1e / 2];
    if (m_MidiW != m_MidiR) {
        m_udata[0x20 / 2] |= 8;
        pend |= 8;
    }
    if (!pend) return;
    if (pend & en & 0x20) {
        irq(m_IrqCPU, true);
        return;
    }
    if (pend & 0x40)
        if (en & 0x40) {
            irq(m_IrqTimA, true);
            return;
        }
    if (pend & 0x80)
        if (en & 0x80) {
            irq(m_IrqTimBC, true);
            return;
        }
    if (pend & 0x100)
        if (en & 0x100) {
            irq(m_IrqTimBC, true);
            return;
        }
    if (pend & 8)
        if (en & 8) {
            irq(m_IrqMidi, true);
            return;
        }
    irq(0, false);
}

void Scsp::ResetInterrupts() {
    const uint32_t reset = m_udata[0x22 / 2];
    if (reset & 0x40) irq(m_IrqTimA, false);
    if (reset & 0x180) irq(m_IrqTimBC, false);
    if (reset & 0x20) {
        m_udata[0x20 / 2] &= ~0x20;
        irq(m_IrqCPU, false);
    }
    if (reset & 0x8) irq(m_IrqMidi, false);
    CheckPendingIRQ();
}

void Scsp::timer_expired(int n) { timer_cb(n); }

void Scsp::timer_cb(int n) {
    static const uint16_t kPend[3] = {0x40, 0x80, 0x100};
    m_TimCnt[n] = 0xFFFF;
    m_udata[0x20 / 2] |= kPend[n];
    m_udata[(0x18 + 2 * n) / 2] &= 0xff00;
    m_udata[(0x18 + 2 * n) / 2] |= m_TimCnt[n] >> 8;
    CheckPendingIRQ();
    if (n == 0) m_mcipd |= 0x40; // MainCheckPendingIRQ(0x40)
}

int Scsp::Get_AR(int base, int R) {
    const int Rate = base + (R << 1);
    return m_ARTABLE[std::clamp(Rate, 0, 63)];
}

int Scsp::Get_DR(int base, int R) {
    const int Rate = base + (R << 1);
    return m_DRTABLE[std::clamp(Rate, 0, 63)];
}

void Scsp::Compute_EG(Slot *slot) {
    const int octave = (OCT(slot) ^ 8) - 8;
    int rate;
    if (KRS(slot) != 0xf) rate = octave + 2 * KRS(slot) + ((FNS(slot) >> 9) & 1);
    else rate = 0;

    slot->EG.volume = 0x17F << EG_SHIFT;
    slot->EG.AR = Get_AR(rate, AR(slot));
    slot->EG.D1R = Get_DR(rate, D1R(slot));
    slot->EG.D2R = Get_DR(rate, D2R(slot));
    slot->EG.RR = Get_DR(rate, RR(slot));
    slot->EG.DL = 0x1f - DL(slot);
    slot->EG.EGHOLD = EGHOLD(slot);
}

int Scsp::EG_Update(Slot *slot) {
    switch (slot->EG.state) {
    case ATTACK:
        slot->EG.volume += slot->EG.AR;
        if (slot->EG.volume >= (0x3ff << EG_SHIFT)) {
            if (!LPSLNK(slot)) {
                slot->EG.state = DECAY1;
                if (slot->EG.D1R >= (1024 << EG_SHIFT)) slot->EG.state = DECAY2; // skip DECAY1
            }
            slot->EG.volume = 0x3ff << EG_SHIFT;
        }
        if (slot->EG.EGHOLD) return 0x3ff << (SHIFT - 10);
        break;
    case DECAY1:
        slot->EG.volume -= slot->EG.D1R;
        if (slot->EG.volume <= 0) slot->EG.volume = 0;
        if (slot->EG.volume >> (EG_SHIFT + 5) <= slot->EG.DL) slot->EG.state = DECAY2;
        break;
    case DECAY2:
        if (D2R(slot) == 0) return (slot->EG.volume >> EG_SHIFT) << (SHIFT - 10);
        slot->EG.volume -= slot->EG.D2R;
        if (slot->EG.volume <= 0) slot->EG.volume = 0;
        break;
    case RELEASE:
        slot->EG.volume -= slot->EG.RR;
        if (slot->EG.volume <= 0) {
            slot->EG.volume = 0;
            StopSlot(slot, 0);
        }
        break;
    default: return 1 << SHIFT;
    }
    return (slot->EG.volume >> EG_SHIFT) << (SHIFT - 10);
}

uint32_t Scsp::Step(Slot *slot) {
    const int octave = (OCT(slot) ^ 8) - 8 + SHIFT - 10;
    uint32_t Fn = FNS(slot) + (1 << 10);
    if (octave >= 0) Fn <<= octave;
    else Fn >>= -octave;
    return Fn;
}

void Scsp::Compute_LFO(Slot *slot) {
    if (PLFOS(slot) != 0) LFO_ComputeStep(&(slot->PLFO), LFOF(slot), PLFOWS(slot), PLFOS(slot), 0);
    if (ALFOS(slot) != 0) LFO_ComputeStep(&(slot->ALFO), LFOF(slot), ALFOWS(slot), ALFOS(slot), 1);
}

void Scsp::StartSlot(Slot *slot) {
    slot->active = 1;
    slot->cur_addr = 0;
    slot->nxt_addr = 1 << SHIFT;
    slot->step = Step(slot);
    Compute_EG(slot);
    slot->EG.state = ATTACK;
    slot->EG.volume = 0x17F << EG_SHIFT;
    slot->Prev = 0;
    slot->Backwards = 0;
    Compute_LFO(slot);
}

void Scsp::StopSlot(Slot *slot, int keyoff) {
    if (keyoff) slot->EG.state = RELEASE;
    else slot->active = 0;
    slot->data[0] &= ~0x800;
}

void Scsp::init() {
    m_DSP.Init();
    m_DSP.ram = &ram_;

    for (int i = 0; i < 0x400; ++i) {
        const float envDB = float(3 * (i - 0x3ff)) / 32.0f;
        const float scale = float(1 << SHIFT);
        m_EG_TABLE[i] = int32_t(powf(10.0f, envDB / 20.0f) * scale);
    }

    for (int i = 0; i < 0x10000; ++i) {
        const int iTL = (i >> 0x0) & 0xff;
        const int iPAN = (i >> 0x8) & 0x1f;
        const int iSDL = (i >> 0xD) & 0x07;
        float TL;
        float SegaDB = 0.0f;
        float fSDL;
        float PAN;
        float LPAN, RPAN;

        if (iTL & 0x01) SegaDB -= 0.4f;
        if (iTL & 0x02) SegaDB -= 0.8f;
        if (iTL & 0x04) SegaDB -= 1.5f;
        if (iTL & 0x08) SegaDB -= 3.0f;
        if (iTL & 0x10) SegaDB -= 6.0f;
        if (iTL & 0x20) SegaDB -= 12.0f;
        if (iTL & 0x40) SegaDB -= 24.0f;
        if (iTL & 0x80) SegaDB -= 48.0f;

        TL = powf(10.0f, SegaDB / 20.0f);

        SegaDB = 0;
        if (iPAN & 0x1) SegaDB -= 3.0f;
        if (iPAN & 0x2) SegaDB -= 6.0f;
        if (iPAN & 0x4) SegaDB -= 12.0f;
        if (iPAN & 0x8) SegaDB -= 24.0f;

        if ((iPAN & 0xf) == 0xf) PAN = 0.0;
        else PAN = powf(10.0f, SegaDB / 20.0f);

        if (iPAN < 0x10) {
            LPAN = PAN;
            RPAN = 1.0;
        } else {
            RPAN = PAN;
            LPAN = 1.0;
        }

        if (iSDL) fSDL = powf(10.0f, (SDLT[iSDL]) / 20.0f);
        else fSDL = 0.0;

        m_LPANTABLE[i] = int(FIX((4.0f * LPAN * TL * fSDL)));
        m_RPANTABLE[i] = int(FIX((4.0f * RPAN * TL * fSDL)));
    }

    m_ARTABLE[0] = m_DRTABLE[0] = 0; // infinite time
    m_ARTABLE[1] = m_DRTABLE[1] = 0;
    for (int i = 2; i < 64; ++i) {
        double step, scale;
        double t = ARTimes[i]; // in ms
        if (t != 0.0) {
            step = (1023 * 1000.0) / (44100.0 * t);
            scale = double(1 << EG_SHIFT);
            m_ARTABLE[i] = int(step * scale);
        } else {
            m_ARTABLE[i] = 1024 << EG_SHIFT;
        }
        t = DRTimes[i];
        step = (1023 * 1000.0) / (44100.0 * t);
        scale = double(1 << EG_SHIFT);
        m_DRTABLE[i] = int(step * scale);
    }

    for (int i = 0; i < 32; ++i) { // all slots off
        m_Slots[i].slot = i;
        m_Slots[i].active = 0;
        m_Slots[i].EG.state = RELEASE;
    }

    LFO_Init();
    m_udata[0x20 / 2] = 0; // no "pend"
    m_TimCnt[0] = 0xffff;
    m_TimCnt[1] = 0xffff;
    m_TimCnt[2] = 0xffff;
}

void Scsp::UpdateSlotReg(int s, int r) {
    Slot *slot = m_Slots + s;
    switch (r & 0x3f) {
    case 0:
    case 1:
        if (KEYONEX(slot)) {
            for (int sl = 0; sl < 32; ++sl) {
                Slot *s2 = m_Slots + sl;
                if (KEYONB(s2) && s2->EG.state == RELEASE) StartSlot(s2);
                if (!KEYONB(s2)) StopSlot(s2, 1);
            }
            slot->data[0] &= ~0x1000;
        }
        break;
    case 0x10:
    case 0x11: slot->step = Step(slot); break;
    case 0xA:
    case 0xB:
        slot->EG.RR = Get_DR(0, RR(slot));
        slot->EG.DL = 0x1f - DL(slot);
        break;
    case 0x12:
    case 0x13: Compute_LFO(slot); break;
    }
}

void Scsp::UpdateReg(int reg) {
    switch (reg & 0x3f) {
    case 0x0:
        gain_ = float(MVOL()) / 15.0f;
        break;
    case 0x2:
    case 0x3:
        m_DSP.RBL = (8 * 1024) << RBL(); // 8 / 16 / 32 / 64 kwords
        m_DSP.RBP = RBP();
        break;
    case 0x6:
    case 0x7: midi_out_.push_back(uint8_t(m_udata[0x6 / 2] & 0xff)); break;
    case 8:
    case 9: m_latched_MSLC = (m_udata[0x8 / 2] & 0xf800) >> 11; break;
    case 0x12:
    case 0x13: m_dma.dmea = (m_udata[0x12 / 2] & 0xfffe) | (m_dma.dmea & 0xf0000); break;
    case 0x14:
    case 0x15:
        m_dma.dmea = ((m_udata[0x14 / 2] & 0xf000) << 4) | (m_dma.dmea & 0xfffe);
        m_dma.drga = (m_udata[0x14 / 2] & 0x0ffe);
        break;
    case 0x16:
    case 0x17:
        m_dma.dtlg = (m_udata[0x16 / 2] & 0x0ffe);
        m_dma.ddir = (m_udata[0x16 / 2] & 0x2000) >> 13;
        m_dma.dgate = (m_udata[0x16 / 2] & 0x4000) >> 14;
        if (m_udata[0x16 / 2] & 0x1000) exec_dma(); // DEXE
        break;
    case 0x18:
    case 0x19:
    case 0x1a:
    case 0x1b:
    case 0x1c:
    case 0x1d: {
        const int n = ((reg & 0x3f) - 0x18) / 2;
        const uint16_t v = m_udata[(0x18 + 2 * n) / 2];
        m_TimPris[n] = 1 << ((v >> 8) & 0x7);
        m_TimCnt[n] = (v & 0xff) << 8;
        if ((v & 0xff) != 255) {
            const uint32_t time = (kClock / uint32_t(m_TimPris[n])) / (255 - (v & 0xff));
            if (time && on_timer) on_timer(n, 512.0 / double(time)); // attotime::from_ticks(512, time)
        }
        break;
    }
    case 0x1e: // SCIEB
    case 0x1f: CheckPendingIRQ(); break;
    case 0x20: // SCIPD
    case 0x21:
        if (m_udata[0x1e / 2] & m_udata[0x20 / 2] & 0x20) CheckPendingIRQ();
        break;
    case 0x22: // SCIRE
    case 0x23:
        m_udata[0x20 / 2] &= ~m_udata[0x22 / 2];
        ResetInterrupts();
        // as on hardware: a reset of an expired timer comes straight back in SCIPD
        if (m_TimCnt[0] == 0xffff) m_udata[0x20 / 2] |= 0x40;
        if (m_TimCnt[1] == 0xffff) m_udata[0x20 / 2] |= 0x80;
        if (m_TimCnt[2] == 0xffff) m_udata[0x20 / 2] |= 0x100;
        break;
    case 0x24:
    case 0x25:
    case 0x26:
    case 0x27:
    case 0x28:
    case 0x29:
        m_IrqTimA = DecodeSCI(SCITMA);
        m_IrqTimBC = DecodeSCI(SCITMB);
        m_IrqMidi = DecodeSCI(SCIMID);
        m_IrqCPU = DecodeSCI(SCIIRQ);
        m_IrqDMA = DecodeSCI(SCIDMA);
        break;
    case 0x2a:
    case 0x2b: m_mcieb = m_udata[0x2a / 2]; break;
    case 0x2c:
    case 0x2d:
        if (m_udata[0x2c / 2] & 0x20) m_mcipd |= 0x20;
        break;
    case 0x2e:
    case 0x2f: m_mcipd &= ~m_udata[0x2e / 2]; break;
    }
}

void Scsp::UpdateRegR(int reg) {
    switch (reg & 0x3f) {
    case 4:
    case 5: {
        uint16_t v = m_udata[0x4 / 2];
        v &= 0xff00;
        v |= m_MidiStack[m_MidiR];
        if (m_MidiR != m_MidiW) {
            ++m_MidiR;
            m_MidiR &= 31;
        }
        if (m_MidiR == m_MidiW) { // input FIFO empty: clear the IRQ
            irq(m_IrqMidi, false);
            m_udata[0x20 / 2] &= ~8;
        }
        m_udata[0x4 / 2] = v;
        break;
    }
    case 8:
    case 9: m_udata[0x8 / 2] = m_latched_MSLC_data; break;
    case 0x2a:
    case 0x2b: m_udata[0x2a / 2] = m_mcieb; break;
    case 0x2c:
    case 0x2d: m_udata[0x2c / 2] = m_mcipd; break;
    }
}

void Scsp::w16(uint32_t addr, uint16_t val) {
    addr &= 0xffff;
    if (addr < 0x400) {
        const int slot = addr / 0x20;
        addr &= 0x1f;
        m_Slots[slot].data[addr / 2] = val;
        UpdateSlotReg(slot, addr & 0x1f);
    } else if (addr < 0x600) {
        if (addr < 0x430) {
            // SCIPD and MCIPD are read-only except for bit 5 (CPU interrupts)
            if (addr == 0x420 || addr == 0x42e) m_udata[(addr & 0x3f) / 2] |= val & 0x20;
            else m_udata[(addr & 0x3f) / 2] = val;
            UpdateReg(addr & 0x3f);
        }
    } else if (addr < 0x700) {
        m_RINGBUF[(addr - 0x600) / 2] = int16_t(val);
    } else { // DSP
        if (addr < 0x780) m_DSP.COEF[(addr - 0x700) / 2] = int16_t(val);
        else if (addr < 0x7c0) m_DSP.MADRS[(addr - 0x780) / 2] = val;
        else if (addr < 0x800) m_DSP.MADRS[(addr - 0x7c0) / 2] = val; // MADRS is mirrored twice
        else if (addr < 0xC00) {
            m_DSP.MPRO[(addr - 0x800) / 2] = val;
            if (addr == 0xBF0) m_DSP.Start();
        }
    }
}

uint16_t Scsp::r16(uint32_t addr) {
    uint16_t v = 0;
    addr &= 0xffff;
    if (addr < 0x400) {
        const int slot = addr / 0x20;
        addr &= 0x1f;
        v = m_Slots[slot].data[addr / 2];
    } else if (addr < 0x600) {
        if (addr < 0x430) {
            UpdateRegR(addr & 0x3f);
            v = m_udata[(addr & 0x3f) / 2];
        }
    } else if (addr < 0x700) {
        v = uint16_t(m_RINGBUF[(addr - 0x600) / 2]);
    } else { // DSP
        if (addr < 0x780) v = uint16_t(m_DSP.COEF[(addr - 0x700) / 2]);
        else if (addr < 0x7c0) v = m_DSP.MADRS[(addr - 0x780) / 2];
        else if (addr < 0x800) v = m_DSP.MADRS[(addr - 0x7c0) / 2];
        else if (addr < 0xC00) v = m_DSP.MPRO[(addr - 0x800) / 2];
        else if (addr < 0xE00) {
            if (addr & 2) v = uint16_t(m_DSP.TEMP[(addr >> 2) & 0x7f] & 0xffff);
            else v = uint16_t(m_DSP.TEMP[(addr >> 2) & 0x7f] >> 16);
        } else if (addr < 0xE80) {
            if (addr & 2) v = uint16_t(m_DSP.MEMS[(addr >> 2) & 0x1f] & 0xffff);
            else v = uint16_t(m_DSP.MEMS[(addr >> 2) & 0x1f] >> 16);
        } else if (addr < 0xEC0) {
            if (addr & 2) v = uint16_t(m_DSP.MIXS[(addr >> 2) & 0xf] & 0xffff);
            else v = uint16_t(m_DSP.MIXS[(addr >> 2) & 0xf] >> 16);
        } else if (addr < 0xEE0) {
            v = uint16_t(m_DSP.EFREG[(addr - 0xec0) / 2]);
        } else if (addr < 0xEE4) {
            v = uint16_t(m_DSP.EXTS[(addr - 0xee0) / 2]);
        }
    }
    return v;
}

int32_t Scsp::UpdateSlot(Slot *slot) {
    if (SSCTL(slot) == 3) return 0; // manual says cannot be used

    int32_t sample = 0;
    int step = int(slot->step);
    uint32_t addr1, addr2, addr_select;
    uint32_t *addr[2] = {&addr1, &addr2};
    uint32_t *slot_addr[2] = {&(slot->cur_addr), &(slot->nxt_addr)};

    if (PLFOS(slot) != 0) {
        step = step * PLFO_Step(&(slot->PLFO));
        step >>= SHIFT;
    }

    if (PCM8B(slot)) {
        addr1 = slot->cur_addr >> SHIFT;
        addr2 = slot->nxt_addr >> SHIFT;
    } else {
        addr1 = (slot->cur_addr >> (SHIFT - 1)) & ~1u;
        addr2 = (slot->nxt_addr >> (SHIFT - 1)) & ~1u;
    }

    if (MDL(slot) != 0 || MDXSL(slot) != 0 || MDYSL(slot) != 0) {
        int32_t smp = (m_RINGBUF[(m_BUFPTR + MDXSL(slot)) & 63] + m_RINGBUF[(m_BUFPTR + MDYSL(slot)) & 63]) / 2;
        smp <<= 0xA;
        smp >>= 0x1A - MDL(slot);
        if (!PCM8B(slot)) smp <<= 1;
        addr1 += smp;
        addr2 += smp;
    }

    if (SSCTL(slot) == 0) { // external DRAM data
        if (PCM8B(slot)) { // 8-bit signed
            const int8_t p1 = int8_t(read_byte(SA(slot) + addr1));
            const int8_t p2 = int8_t(read_byte(SA(slot) + addr2));
            const int32_t fpart = slot->cur_addr & ((1 << SHIFT) - 1);
            const int32_t s = int(p1 << 8) * ((1 << SHIFT) - fpart) + int(p2 << 8) * fpart;
            sample = (s >> SHIFT);
        } else { // 16-bit signed
            const int16_t p1 = int16_t(read_word(SA(slot) + addr1));
            const int16_t p2 = int16_t(read_word(SA(slot) + addr2));
            const int32_t fpart = slot->cur_addr & ((1 << SHIFT) - 1);
            const int32_t s = int(p1) * ((1 << SHIFT) - fpart) + int(p2) * fpart;
            sample = (s >> SHIFT);
        }
    } else if (SSCTL(slot) == 1) { // noise
        sample = int16_t(rand() & 0xffff);
    } else { // all 0
        sample = 0;
    }

    if (SBCTL(slot) & 0x1) sample ^= 0x7FFF;
    if (SBCTL(slot) & 0x2) sample = int16_t(sample ^ 0x8000);

    if (slot->Backwards) slot->cur_addr -= step;
    else slot->cur_addr += step;
    slot->nxt_addr = slot->cur_addr + (1 << SHIFT);

    addr1 = slot->cur_addr >> SHIFT;
    addr2 = slot->nxt_addr >> SHIFT;

    if (addr1 >= LSA(slot) && !(slot->Backwards))
        if (LPSLNK(slot) && slot->EG.state == ATTACK) slot->EG.state = DECAY1;

    for (addr_select = 0; addr_select < 2; addr_select++) {
        int32_t rem_addr;
        switch (LPCTL(slot)) {
        case 0: // no loop
            if (*addr[addr_select] >= LSA(slot) && *addr[addr_select] >= LEA(slot)) StopSlot(slot, 0);
            break;
        case 1: // normal loop
            if (*addr[addr_select] >= LEA(slot)) {
                rem_addr = *slot_addr[addr_select] - (LEA(slot) << SHIFT);
                *slot_addr[addr_select] = (LSA(slot) << SHIFT) + rem_addr;
            }
            break;
        case 2: // reverse loop
            if ((*addr[addr_select] >= LSA(slot)) && !(slot->Backwards)) {
                rem_addr = *slot_addr[addr_select] - (LSA(slot) << SHIFT);
                *slot_addr[addr_select] = (LEA(slot) << SHIFT) - rem_addr;
                slot->Backwards = 1;
            } else if ((*addr[addr_select] < LSA(slot) || (*slot_addr[addr_select] & 0x80000000)) && slot->Backwards) {
                rem_addr = (LSA(slot) << SHIFT) - *slot_addr[addr_select];
                *slot_addr[addr_select] = (LEA(slot) << SHIFT) - rem_addr;
            }
            break;
        case 3: // ping-pong
            if (*addr[addr_select] >= LEA(slot)) { // reached the end: reverse to the start
                rem_addr = *slot_addr[addr_select] - (LEA(slot) << SHIFT);
                *slot_addr[addr_select] = (LEA(slot) << SHIFT) - rem_addr;
                slot->Backwards = 1;
            } else if ((*addr[addr_select] < LSA(slot) || (*slot_addr[addr_select] & 0x80000000)) && slot->Backwards) {
                rem_addr = (LSA(slot) << SHIFT) - *slot_addr[addr_select];
                *slot_addr[addr_select] = (LSA(slot) << SHIFT) + rem_addr;
                slot->Backwards = 0;
            }
            break;
        }
    }

    if (!SDIR(slot)) {
        if (ALFOS(slot) != 0) {
            sample = sample * ALFO_Step(&(slot->ALFO));
            sample >>= SHIFT;
        }
        if (slot->EG.state == ATTACK) sample = (sample * EG_Update(slot)) >> SHIFT;
        else sample = (sample * m_EG_TABLE[EG_Update(slot) >> (SHIFT - 10)]) >> SHIFT;
    }

    if (!STWINH(slot)) {
        if (!SDIR(slot)) {
            const uint16_t Enc = ((TL(slot)) << 0x0) | (0x7 << 0xd);
            *m_RBUFDST = int16_t((sample * m_LPANTABLE[Enc]) >> (SHIFT + 1));
        } else {
            const uint16_t Enc = (0 << 0x0) | (0x7 << 0xd);
            *m_RBUFDST = int16_t((sample * m_LPANTABLE[Enc]) >> (SHIFT + 1));
        }
    }

    return sample;
}

void Scsp::render(float *out, int n) {
    for (int s = 0; s < n; ++s) {
        int32_t smpl = 0, smpr = 0;

        for (int sl = 0; sl < 32; ++sl) {
            m_RBUFDST = m_RINGBUF + m_BUFPTR;
            if (m_Slots[sl].active) {
                Slot *slot = m_Slots + sl;
                uint16_t Enc;
                const int32_t sample = UpdateSlot(slot);
                // SDIR bypasses TL in both the DSP feed and the direct mix (MAME)
                const uint16_t eff_tl = SDIR(slot) ? 0 : TL(slot);
                Enc = uint16_t((eff_tl << 0x0) | (IMXL(slot) << 0xd));
                m_DSP.SetSample((sample * m_LPANTABLE[Enc]) >> (SHIFT - 2), ISEL(slot), IMXL(slot));
                const uint16_t dir_tl = SDIR(slot) ? 0 : TL(slot);
                Enc = uint16_t((dir_tl << 0x0) | (DIPAN(slot) << 0x8) | (DISDL(slot) << 0xd));
                smpl += (sample * m_LPANTABLE[Enc]) >> SHIFT;
                smpr += (sample * m_RPANTABLE[Enc]) >> SHIFT;
            }
            ++m_BUFPTR;
            m_BUFPTR &= 63;
        }

        m_DSP.Step();

        for (int i = 0; i < 16; ++i) {
            Slot *slot = m_Slots + i;
            if (EFSDL(slot)) {
                const uint16_t Enc = uint16_t((EFPAN(slot) << 0x8) | (EFSDL(slot) << 0xd));
                smpl += (m_DSP.EFREG[i] * m_LPANTABLE[Enc]) >> SHIFT;
                smpr += (m_DSP.EFREG[i] * m_RPANTABLE[Enc]) >> SHIFT;
            }
        }
        // EXTS0/1 (slots 16 and 17's EFSDL, EFPAN): nothing connected on Model 2
        m_DSP.EXTS[0] = m_DSP.EXTS[1] = 0;

        float l, r;
        if (DAC18B()) {
            l = float(std::clamp(smpl, -131072, 131071)) / 131072.0f;
            r = float(std::clamp(smpr, -131072, 131071)) / 131072.0f;
        } else {
            l = float(std::clamp(smpl >> 2, -32768, 32767)) / 32768.0f;
            r = float(std::clamp(smpr >> 2, -32768, 32767)) / 32768.0f;
        }
        out[2 * s] = l * gain_;
        out[2 * s + 1] = r * gain_;
    }

    // latch the slot monitor (MSLC: CA, SGC, EG), as MAME at each stream update
    const Slot *slot = m_Slots + m_latched_MSLC;
    const uint32_t SGC = (slot->EG.state) & 3;
    const uint32_t CA = (slot->cur_addr >> (SHIFT + 12)) & 0xf;
    const uint32_t EG = (0x1f - (slot->EG.volume >> (EG_SHIFT + 5))) & 0x1f;
    m_latched_MSLC_data = uint16_t((CA << 7) | (SGC << 5) | EG);
}

void Scsp::exec_dma() {
    uint16_t tmp_dma[3] = {};
    if (!(m_dma.ddir))
        for (int i = 0; i < 3; i++) tmp_dma[i] = m_udata[(0x12 + (i * 2)) / 2];

    if (m_dma.ddir) {
        if (m_dma.dgate) {
            for (int i = 0; i < m_dma.dtlg; i += 2) {
                write_word(m_dma.dmea, 0);
                m_dma.dmea += 2;
            }
        } else {
            for (int i = 0; i < m_dma.dtlg; i += 2) {
                const uint16_t tmp = r16(m_dma.drga);
                write_word(m_dma.dmea, tmp);
                m_dma.dmea += 2;
                m_dma.drga += 2;
            }
        }
    } else {
        if (m_dma.dgate) {
            for (int i = 0; i < m_dma.dtlg; i += 2) {
                w16(m_dma.drga, 0);
                m_dma.drga += 2;
            }
        } else {
            for (int i = 0; i < m_dma.dtlg; i += 2) {
                const uint16_t tmp = read_word(m_dma.dmea);
                w16(m_dma.drga, tmp);
                m_dma.dmea += 2;
                m_dma.drga += 2;
            }
        }
    }

    if (!(m_dma.ddir))
        for (int i = 0; i < 3; i++) m_udata[(0x12 + (i * 2)) / 2] = tmp_dma[i];

    m_udata[0x16 / 2] &= ~0x1000; // done
    // MAME holds the DMA-end interrupt for one acknowledge (HOLD_LINE); Sega
    // Rally never enables it
    if (m_udata[0x1e / 2] & 0x10) irq(m_IrqDMA, true);
}

uint16_t Scsp::read(uint32_t offset) { return r16(offset & 0xffe); }

void Scsp::write(uint32_t offset, uint16_t data, uint16_t mem_mask) {
    uint16_t tmp = r16(offset & 0xffe);
    tmp = uint16_t((tmp & ~mem_mask) | (data & mem_mask)); // COMBINE_DATA
    w16(offset & 0xffe, tmp);
}

void Scsp::midi_in(uint8_t byte) { // rcv_complete
    m_MidiStack[m_MidiW++] = byte;
    m_MidiW &= 31;
    CheckPendingIRQ();
}

// ---------------------------------------------------------------------------
// LFOs

namespace {
inline uint32_t LFIX(float v) { return uint32_t(float(1 << LFO_SHIFT) * v); }
inline uint32_t DB(float v) { return LFIX(powf(10.0f, v / 20.0f)); }      // dB to amplitude multiplier
inline uint32_t CENTS(float v) { return LFIX(powf(2.0f, v / 1200.0f)); } // cents to step increment

const float LFOFreq[32] = {0.17f, 0.19f, 0.23f, 0.27f, 0.34f, 0.39f, 0.45f, 0.55f, 0.68f, 0.78f, 0.92f, 1.10f, 1.39f, 1.60f, 1.87f, 2.27f,
                           2.87f, 3.31f, 3.92f, 4.79f, 6.15f, 7.18f, 8.60f, 10.8f, 14.4f, 17.2f, 21.5f, 28.7f, 43.1f, 57.4f, 86.1f, 172.3f};
const float ASCALE[8] = {0.0f, 0.4f, 0.8f, 1.5f, 3.0f, 6.0f, 12.0f, 24.0f};
const float PSCALE[8] = {0.0f, 7.0f, 13.5f, 27.0f, 55.0f, 112.0f, 230.0f, 494.0f};
} // namespace

void Scsp::LFO_Init() {
    for (int i = 0; i < 256; ++i) {
        int a, p;
        // saw
        a = 255 - i;
        p = i < 128 ? i : i - 256;
        m_ALFO_SAW[i] = a;
        m_PLFO_SAW[i] = p;
        // square
        if (i < 128) a = 255, p = 127;
        else a = 0, p = -128;
        m_ALFO_SQR[i] = a;
        m_PLFO_SQR[i] = p;
        // triangle
        a = i < 128 ? 255 - (i * 2) : (i * 2) - 256;
        if (i < 64) p = i * 2;
        else if (i < 128) p = 255 - i * 2;
        else if (i < 192) p = 256 - i * 2;
        else p = i * 2 - 511;
        m_ALFO_TRI[i] = a;
        m_PLFO_TRI[i] = p;
        // noise
        a = int(rand() & 0xff);
        p = 128 - a;
        m_ALFO_NOI[i] = a;
        m_PLFO_NOI[i] = p;
    }

    for (int s = 0; s < 8; ++s) {
        float limit = PSCALE[s];
        for (int i = -128; i < 128; ++i) m_PSCALES[s][i + 128] = int(CENTS(((limit * float(i)) / 128.0f)));
        limit = -ASCALE[s];
        for (int i = 0; i < 256; ++i) m_ASCALES[s][i] = int(DB(((limit * float(i)) / 256.0f)));
    }
}

int32_t Scsp::PLFO_Step(Lfo *LFO) {
    LFO->phase = uint16_t(LFO->phase + LFO->phase_step);
    int p = LFO->table[LFO->phase >> LFO_SHIFT];
    p = LFO->scale[p + 128];
    return p << (SHIFT - LFO_SHIFT);
}

int32_t Scsp::ALFO_Step(Lfo *LFO) {
    LFO->phase = uint16_t(LFO->phase + LFO->phase_step);
    int p = LFO->table[LFO->phase >> LFO_SHIFT];
    p = LFO->scale[p];
    return p << (SHIFT - LFO_SHIFT);
}

void Scsp::LFO_ComputeStep(Lfo *LFO, uint32_t LFOF, uint32_t LFOWS, uint32_t LFOS, int ALFO) {
    const float step = float(LFOFreq[LFOF]) * 256.0f / 44100.0f;
    LFO->phase_step = uint32_t(float(1 << LFO_SHIFT) * step);
    if (ALFO) {
        switch (LFOWS) {
        case 0: LFO->table = m_ALFO_SAW; break;
        case 1: LFO->table = m_ALFO_SQR; break;
        case 2: LFO->table = m_ALFO_TRI; break;
        case 3: LFO->table = m_ALFO_NOI; break;
        }
        LFO->scale = m_ASCALES[LFOS];
    } else {
        switch (LFOWS) {
        case 0: LFO->table = m_PLFO_SAW; break;
        case 1: LFO->table = m_PLFO_SQR; break;
        case 2: LFO->table = m_PLFO_TRI; break;
        case 3: LFO->table = m_PLFO_NOI; break;
        }
        LFO->scale = m_PSCALES[LFOS];
    }
}

// ---------------------------------------------------------------------------
// DSP

void Scsp::Dsp::Init() {
    std::vector<uint8_t> *r = ram;
    *this = Dsp{};
    ram = r;
    RBL = 8 * 1024; // initial RBL is 0
    Stopped = true;
}

void Scsp::Dsp::Step() {
    if (Stopped) return;

    std::fill(std::begin(EFREG), std::end(EFREG), 0);

    int32_t ACC = 0;     // 26 bit
    int32_t MEMVAL = 0;
    int32_t FRC_REG = 0; // 13 bit
    int32_t Y_REG = 0;   // 24 bit
    uint32_t ADRS_REG = 0; // 13 bit

    for (int step = 0; step < LastStep; ++step) {
        const uint16_t *const IPtr = MPRO + (step * 4);

        const uint32_t TRA = (IPtr[0] >> 8) & 0x7f;
        const uint32_t TWT = (IPtr[0] >> 7) & 0x01;
        const uint32_t TWA = (IPtr[0] >> 0) & 0x7f;

        const uint32_t XSEL = (IPtr[1] >> 15) & 0x01;
        const uint32_t YSEL = (IPtr[1] >> 13) & 0x03;
        const uint32_t IRA = (IPtr[1] >> 6) & 0x3f;
        const uint32_t IWT = (IPtr[1] >> 5) & 0x01;
        const uint32_t IWA = (IPtr[1] >> 0) & 0x1f;

        const uint32_t TABLE = (IPtr[2] >> 15) & 0x01;
        const uint32_t MWT = (IPtr[2] >> 14) & 0x01;
        const uint32_t MRD = (IPtr[2] >> 13) & 0x01;
        const uint32_t EWT = (IPtr[2] >> 12) & 0x01;
        const uint32_t EWA = (IPtr[2] >> 8) & 0x0f;
        const uint32_t ADRL = (IPtr[2] >> 7) & 0x01;
        const uint32_t FRCL = (IPtr[2] >> 6) & 0x01;
        const uint32_t SHFT = (IPtr[2] >> 4) & 0x03;
        const uint32_t YRL = (IPtr[2] >> 3) & 0x01;
        const uint32_t NEGB = (IPtr[2] >> 2) & 0x01;
        const uint32_t ZERO = (IPtr[2] >> 1) & 0x01;
        const uint32_t BSEL = (IPtr[2] >> 0) & 0x01;

        const uint32_t NOFL = (IPtr[3] >> 15) & 0x01;
        const uint32_t COEFI = (IPtr[3] >> 9) & 0x3f;

        const uint32_t MASA = (IPtr[3] >> 2) & 0x1f;
        const uint32_t ADREB = (IPtr[3] >> 1) & 0x01;
        const uint32_t NXADR = (IPtr[3] >> 0) & 0x01;

        // operations are done at 24-bit precision
        int32_t INPUTS; // 24-bit
        if (IRA <= 0x1f) INPUTS = MEMS[IRA];
        else if (IRA <= 0x2F) INPUTS = MIXS[IRA - 0x20] << 4; // MIXS is 20 bit
        else if (IRA <= 0x31) INPUTS = EXTS[IRA - 0x30] << 8; // EXTS is 16 bit
        else return;

        INPUTS = sext24(INPUTS);

        if (IWT) {
            MEMS[IWA] = MEMVAL; // MEMVAL was selected in the previous MRD
            if (IRA == IWA) INPUTS = MEMVAL;
        }

        int32_t B; // 26-bit
        if (!ZERO) {
            if (BSEL) B = ACC;
            else B = sext24(TEMP[(TRA + DEC) & 0x7f]);
            if (NEGB) B = 0 - B;
        } else {
            B = 0;
        }

        int32_t X; // 24-bit
        if (XSEL) X = INPUTS;
        else X = sext24(TEMP[(TRA + DEC) & 0x7f]);

        int32_t Y = 0; // 13 bit
        if (YSEL == 0) Y = FRC_REG;
        else if (YSEL == 1) Y = this->COEF[COEFI] >> 3; // COEF is 16 bits
        else if (YSEL == 2) Y = (Y_REG >> 11) & 0x1fff;
        else if (YSEL == 3) Y = (Y_REG >> 4) & 0x0fff;

        if (YRL) Y_REG = INPUTS;

        int32_t SHIFTED = 0; // 24 bit
        if (SHFT == 0) SHIFTED = std::clamp<int32_t>(ACC, -0x00800000, 0x007fffff);
        else if (SHFT == 1) SHIFTED = std::clamp<int32_t>(ACC * 2, -0x00800000, 0x007fffff);
        else if (SHFT == 2) SHIFTED = sext24(ACC * 2);
        else if (SHFT == 3) SHIFTED = sext24(ACC);

        Y = sext13(Y);

        const int64_t v = (int64_t(X) * int64_t(Y)) >> 12;
        ACC = int(v + B);

        if (TWT) TEMP[(TWA + DEC) & 0x7f] = SHIFTED;

        if (FRCL) {
            if (SHFT == 3) FRC_REG = SHIFTED & 0x0fff;
            else FRC_REG = (SHIFTED >> 11) & 0x1fff;
        }

        if (MRD || MWT) {
            uint32_t ADDR = MADRS[MASA];
            if (!TABLE) ADDR += DEC;
            if (ADREB) ADDR += ADRS_REG & 0x0FFF;
            if (NXADR) ADDR++;
            if (!TABLE) ADDR &= RBL - 1;
            else ADDR &= 0xffff;
            ADDR += RBP << 12;
            ADDR <<= 1;
            if (MRD && (step & 1)) { // memory only on odd steps
                if (NOFL) MEMVAL = read_word(ADDR) << 8;
                else MEMVAL = UNPACK(read_word(ADDR));
            }
            if (MWT && (step & 1)) {
                if (NOFL) write_word(ADDR, uint16_t(SHIFTED >> 8));
                else write_word(ADDR, PACK(SHIFTED));
            }
        }

        if (ADRL) {
            if (SHFT == 3) ADRS_REG = (SHIFTED >> 12) & 0xfff;
            else ADRS_REG = uint32_t(INPUTS >> 16);
        }

        if (EWT) EFREG[EWA] = int16_t(EFREG[EWA] + (SHIFTED >> 8));
    }
    --DEC;
    std::fill(std::begin(MIXS), std::end(MIXS), 0);
}

void Scsp::Dsp::SetSample(int32_t sample, int32_t SEL, int32_t) { MIXS[SEL] += sample; }

void Scsp::Dsp::Start() {
    Stopped = false;
    int i;
    for (i = 127; i >= 0; --i) {
        const uint16_t *const IPtr = MPRO + (i * 4);
        if (IPtr[0] || IPtr[1] || IPtr[2] || IPtr[3]) break;
    }
    LastStep = i + 1;
}

} // namespace snd
