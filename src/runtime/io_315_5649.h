// The Model 2A-CRX board's I/O (Sega Rally): the 315-5649 I/O chip on the
// i960 bus at 0x01c00000 and the 93C46 settings EEPROM it bit-bangs through
// port A. Inputs, the EEPROM's storage and the drive board's command queue
// are the IoBoard's (m2_board.h), so the host side (controls, saving the
// EEPROM, force feedback) is the same as Daytona's.
#pragma once

#include <array>
#include <cstdint>
#include <functional>

namespace rt {

class IoBoard;

// 93C46 in 16-bit mode (64 words): MAME's eeprom_serial_93cxx_device
// protocol, with MAME's write and erase times: DO reads busy (0) after CS
// rises until the operation completes, on the board's clock (set_clock: 25
// MHz ticks; without one, always ready).
class Eeprom93c46 {
public:
    // mem: 128 bytes, word n little-endian at 2n; dirty is set on each change.
    Eeprom93c46(std::array<uint8_t, 128> &mem, bool &dirty) : mem_(mem), dirty_(dirty) {}
    void cs_write(bool state);
    void clk_write(bool state);
    void di_write(bool state) { di_ = state; }
    bool do_read() const;
    void set_clock(std::function<uint64_t()> ticks) { clock_ = std::move(ticks); }

private:
    enum State { InReset, WaitStart, WaitCommand, Reading, WaitData, WaitCompletion };
    enum Command { Read, Write, Erase, Lock, Unlock, WriteAll, EraseAll };
    uint16_t word(uint32_t a) const { return uint16_t(mem_[2 * a] | mem_[2 * a + 1] << 8); }
    void set_word(uint32_t a, uint16_t v);
    void clk_rising();
    void execute_command();
    bool ready() const { return !clock_ || clock_() >= busy_until_; }
    void busy_for(uint64_t us) { if (clock_) busy_until_ = clock_() + us * 25; }

    std::array<uint8_t, 128> &mem_;
    bool &dirty_;
    State state_ = InReset;
    Command command_ = Read;
    bool cs_ = false, clk_ = false, di_ = false, locked_ = true;
    uint32_t accum_ = 0, bits_ = 0, address_ = 0, shift_ = 0;
    std::function<uint64_t()> clock_;
    uint64_t busy_until_ = 0;
};

// 315-5649: seven 8-bit ports (A-G), a direction register, an analogue
// multiplexer and two RS-422 channels (unused by Sega Rally). Offsets are
// MAME's (sega_315_5649_device::read/write), byte-wide.
class Io5649 {
public:
    explicit Io5649(IoBoard &io);
    void set_clock(std::function<uint64_t()> ticks) { eeprom_.set_clock(std::move(ticks)); }
    uint8_t read(uint32_t offset);
    void write(uint32_t offset, uint8_t data);

private:
    uint8_t port_in(uint32_t port);
    void port_out(uint32_t port, uint8_t data);

    IoBoard &io_;
    Eeprom93c46 eeprom_;
    std::array<uint8_t, 7> port_value_{};
    uint8_t port_config_ = 0xff, mode_ = 0, analog_channel_ = 0;
    bool ctrlmode_ = false; // port A bit 0: port B carries the EEPROM's DO
};

} // namespace rt
