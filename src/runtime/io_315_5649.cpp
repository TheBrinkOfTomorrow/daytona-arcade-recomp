// license:BSD-3-Clause
// copyright-holders:Dirk Best, Aaron Giles, R. Belmont, Olivier Galibert, ElSemi, Angelo Salese
//
// Model 2A-CRX I/O (see io_315_5649.h). Semantics follow MAME at
// dddd73680656e355bb2b5beecab1167c9f07bf81 (BSD-3-Clause; notice above
// kept): src/mame/sega/315_5649.cpp (read/write), src/devices/machine/
// eepromser.cpp (the 93Cxx serial protocol), and model2.cpp's model2a
// wiring (eeprom_w, in0_r, srallyc's ports and analogue channels). See
// THIRD_PARTY.md.

#include "runtime/io_315_5649.h"

#include "runtime/m2_board.h"

namespace rt {

// ---------------------------------------------------------------------------
// 93C46, 16-bit: 6 address bits, 2 command bits after the start bit.

namespace {
constexpr uint32_t kAddressBits = 6, kDataBits = 16;
}

void Eeprom93c46::set_word(uint32_t a, uint16_t v) {
    a &= 63;
    if (word(a) == v) return;
    mem_[2 * a] = uint8_t(v);
    mem_[2 * a + 1] = uint8_t(v >> 8);
    dirty_ = true;
}

void Eeprom93c46::cs_write(bool state) {
    if (state == cs_) return;
    cs_ = state;
    if (cs_) {
        if (state_ == InReset) state_ = WaitStart;
    } else {
        state_ = InReset; // a falling CS ends every state
    }
}

void Eeprom93c46::clk_write(bool state) {
    if (state == clk_) return;
    clk_ = state;
    if (clk_) clk_rising();
}

void Eeprom93c46::clk_rising() {
    switch (state_) {
    case WaitStart:
        if (di_ && ready()) {
            accum_ = bits_ = 0;
            state_ = WaitCommand;
        }
        break;
    case WaitCommand:
        accum_ = (accum_ << 1) | (di_ ? 1 : 0);
        if (++bits_ == 2 + kAddressBits) execute_command();
        break;
    case Reading: {
        // a dummy 0, then the word, MSB first; then 1s (no streaming)
        const uint32_t bit = bits_++;
        if (bit == 0) shift_ = uint32_t(word(address_)) << (32 - kDataBits);
        else shift_ = (shift_ << 1) | 1;
        break;
    }
    case WaitData:
        shift_ = (shift_ << 1) | (di_ ? 1 : 0);
        if (++bits_ == kDataBits) {
            if (!locked_) {
                if (command_ == Write) set_word(address_, uint16_t(shift_)), busy_for(1750);
                else { // WriteAll: ANDed into the cells (ERASEALL comes first)
                    for (uint32_t a = 0; a < 64; a++) set_word(a, uint16_t(word(a) & shift_));
                    busy_for(8000);
                }
            }
            state_ = locked_ ? InReset : WaitCompletion;
        }
        break;
    default: break;
    }
}

void Eeprom93c46::execute_command() {
    address_ = accum_ & ((1u << kAddressBits) - 1);
    switch (accum_ >> kAddressBits) {
    case 0:
        switch (address_ >> (kAddressBits - 2)) {
        case 0: command_ = Lock; break;
        case 1: command_ = WriteAll; break;
        case 2: command_ = EraseAll; break;
        default: command_ = Unlock; break;
        }
        address_ = 0;
        break;
    case 1: command_ = Write; break;
    case 2: command_ = Read; break;
    default: command_ = Erase; break;
    }
    bits_ = 0;
    switch (command_) {
    case Read: shift_ = 0; state_ = Reading; break;
    case Write:
    case WriteAll: shift_ = 0; state_ = WaitData; break;
    case Erase:
        if (locked_) { state_ = InReset; break; }
        set_word(address_, 0xffff);
        busy_for(1000);
        state_ = WaitCompletion;
        break;
    case EraseAll:
        if (locked_) { state_ = InReset; break; }
        for (uint32_t a = 0; a < 64; a++) set_word(a, 0xffff);
        busy_for(8000);
        state_ = WaitCompletion;
        break;
    case Lock: locked_ = true; state_ = InReset; break;
    case Unlock: locked_ = false; state_ = InReset; break;
    }
}

bool Eeprom93c46::do_read() const {
    // MAME: READY/BUSY while waiting for a start bit; the shifter while
    // reading; otherwise the tristated line reads 1
    if (state_ == Reading) return (shift_ & 0x80000000u) != 0;
    if (state_ == WaitStart) return ready();
    return true;
}

// ---------------------------------------------------------------------------
// 315-5649

Io5649::Io5649(IoBoard &io) : io_(io), eeprom_(io.eeprom, io.eeprom_dirty) {}

uint8_t Io5649::port_in(uint32_t port) {
    const Inputs &in = io_.inputs;
    switch (port) {
    case 1: // model2 in0_r: with port A bit 0 set, bit 5 is the EEPROM's DO
        if (ctrlmode_) return uint8_t(0xc0 | (eeprom_.do_read() ? 0x20 : 0) | 0x10 | (in.in0 & 0x0f));
        return in.in0;
    case 2: return in.in1;       // IN1: gears in bits 4-6
    case 3: return in.handbrake; // IN2: srallyc's hand brake (analogue, 0 = off)
    case 6: return 0xff;         // SW: DIP switches, all off
    default: return 0xff;        // no input wired
    }
}

void Io5649::port_out(uint32_t port, uint8_t data) {
    switch (port) {
    case 0: // model2 eeprom_w
        ctrlmode_ = data & 1;
        eeprom_.di_write((data >> 5) & 1);
        eeprom_.clk_write((data >> 7) & 1);
        eeprom_.cs_write((data >> 6) & 1);
        break;
    case 4: // srallyc: the drive board's command (MAME drive_board_w)
        if (io_.drive_commands.size() < IoBoard::kMaxDrive) io_.drive_commands.push_back(data);
        break;
    default: break; // port F: lamps
    }
}

uint8_t Io5649::read(uint32_t offset) {
    offset &= 0x1f;
    switch (offset) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06:
        // (port G's counter mode, mode bit 7, is not used by Sega Rally)
        return (port_config_ >> offset) & 1 ? port_in(offset) : port_value_[offset];
    case 0x0b: case 0x0c: return 0xff; // RS-422 input: nothing connected
    case 0x0d: return 0x0c;            // RS-422 status (MAME's: receive buffers full, transmit empty)
    case 0x0f: {                       // analogue input, auto-increments
        const Inputs &in = io_.inputs;
        const uint8_t v = analog_channel_ == 0 ? in.steer : analog_channel_ == 1 ? in.accel
                        : analog_channel_ == 2 ? in.brake : 0xff;
        analog_channel_ = (analog_channel_ + 1) & 7;
        return v;
    }
    default: return 0xff;
    }
}

void Io5649::write(uint32_t offset, uint8_t data) {
    offset &= 0x1f;
    switch (offset) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06:
        port_value_[offset] = data;
        port_out(offset, data);
        break;
    case 0x08: port_config_ = data; break; // direction: 1 = input
    case 0x0e: mode_ = data; break;
    case 0x0f: analog_channel_ = data & 7; break;
    default: break; // RS-422 output: nothing connected
    }
}

} // namespace rt
