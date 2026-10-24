// The game core's C interface (include/m2core.h) over rt::GameLoop: what the
// desktop app does around the game, without SDL or a window, for front ends
// that bring their own (Attract Mode's SwiftUI and SDL apps).
#include "m2core.h"

#include "app/link_socket.h"
#include "runtime/game_loop.h"
#include "runtime/rom_import.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

struct m2core {
    std::unique_ptr<app::TcpLink> link; // declared first: outlives the game, which holds it
    std::unique_ptr<rt::GameLoop> game;
    std::string save_dir;
    std::deque<float> audio[2];
};

namespace {

void set_error(char *err, size_t size, const char *what) {
    if (err && size) std::snprintf(err, size, "%s", what);
}

std::string save_path(const m2core *c, const char *name) { return c->save_dir + "/" + name; }

template <typename C> void load_file(const std::string &path, C &into) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::vector<uint8_t> d{std::istreambuf_iterator<char>(f), {}};
    if (d.size() == into.size()) std::copy(d.begin(), d.end(), into.begin());
}
template <typename C> void save_file(const std::string &path, const C &from) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(from.data()), std::streamsize(from.size()));
}

// The app's mapping (app::Controls::sample) from controls to the board's ports.
rt::Inputs board_inputs(const m2core_input &c) {
    rt::Inputs in;
    const float lock = rt::kModel2a ? 0x7f : 0x60;
    const auto pedal = [](float v) {
        return uint8_t(std::lround(rt::kPedalMin + std::clamp(v, 0.f, 1.f) * (rt::kPedalMax - rt::kPedalMin)));
    };
    in.steer = uint8_t(std::lround(0x80 + std::clamp(c.steer, -1.f, 1.f) * lock));
    in.accel = pedal(c.accel);
    in.brake = pedal(c.brake);
    static const uint8_t gearvalue[5] = {0, 2, 1, 6, 5}; // MAME daytona_gearbox_r: neutral, 1-4
    in.in1 = uint8_t((in.in1 & ~0x70) | (gearvalue[std::clamp(c.gear, 0, 4)] << 4));
    const auto low = [&](uint8_t &port, uint8_t bit, uint32_t button) {
        if (c.buttons & button) port &= uint8_t(~bit);
    };
    low(in.in0, 0x01, M2CORE_COIN);
    low(in.in0, 0x04, M2CORE_TEST);
    low(in.in0, 0x08, M2CORE_SERVICE);
    low(in.in0, rt::kIn0Start, M2CORE_START);
    low(in.in0, 0x20, M2CORE_VIEW1);
    if (rt::kModel2a) { // IN2 is the hand brake (analogue, 0 = off)
        in.handbrake = uint8_t(std::lround(std::clamp(c.handbrake, 0.f, 1.f) * 0xff));
        return in;
    }
    low(in.in0, 0x40, M2CORE_VIEW2);
    low(in.in0, 0x80, M2CORE_VIEW3);
    low(in.in1, 0x01, M2CORE_VIEW4);
    return in;
}

void core_save(m2core *c) {
    if (!c || c->save_dir.empty()) return;
    save_file(save_path(c, "ioboard_eeprom.bin"), c->game->board().io().eeprom);
    save_file(save_path(c, "backup_ram.bin"), c->game->board().backup_ram());
}

m2core *core_create(const char *rom_path, const char *save_dir, char *err, size_t err_size) {
    try {
        auto c = std::make_unique<m2core>();
        const std::string rom = rom_path ? rom_path : "";
        std::error_code ec;
        c->game = std::filesystem::is_directory(rom, ec) ? std::make_unique<rt::GameLoop>(rom)
                                                         : std::make_unique<rt::GameLoop>(rt::import_rom_set(rom));
        if (save_dir && *save_dir) {
            c->save_dir = save_dir;
            load_file(save_path(c.get(), "ioboard_eeprom.bin"), c->game->board().io().eeprom);
            load_file(save_path(c.get(), "backup_ram.bin"), c->game->board().backup_ram());
        }
        return c.release();
    } catch (const std::exception &e) {
        set_error(err, err_size, e.what());
        return nullptr;
    }
}

void core_destroy(m2core *c) {
    core_save(c);
    delete c;
}

void core_run_frame(m2core *c, const m2core_input *input) {
    c->game->run_frame(input ? board_inputs(*input) : rt::Inputs{});
    if (snd::SoundBoard *sb = c->game->sound()) {
        const std::vector<float> fm = sb->take_fm(), pcm = sb->take_pcm();
        c->audio[0].insert(c->audio[0].end(), fm.begin(), fm.end());
        c->audio[1].insert(c->audio[1].end(), pcm.begin(), pcm.end());
        // Nobody reading (paused output): keep at most a second.
        for (int s = 0; s < 2; ++s) {
            const size_t cap = 2 * size_t(sb->fm_rate() > sb->pcm_rate() ? sb->fm_rate() : sb->pcm_rate());
            if (c->audio[s].size() > cap) c->audio[s].erase(c->audio[s].begin(), c->audio[s].end() - cap);
        }
    }
}

const uint32_t *core_screen(m2core *c, int *width, int *height) {
    if (width) *width = c->game->screen_width();
    if (height) *height = rt::GameLoop::kHeight;
    return c->game->screen().data();
}

double core_audio_rate(m2core *c, int stream) {
    snd::SoundBoard *sb = c->game->sound();
    if (!sb || stream < 0 || stream > 1) return 0;
    return stream == 0 ? sb->fm_rate() : sb->pcm_rate();
}

size_t core_audio_read(m2core *c, int stream, float *out, size_t max_frames) {
    if (stream < 0 || stream > 1 || !out) return 0;
    std::deque<float> &q = c->audio[stream];
    const size_t frames = std::min(max_frames, q.size() / 2);
    std::copy_n(q.begin(), 2 * frames, out);
    q.erase(q.begin(), q.begin() + std::ptrdiff_t(2 * frames));
    return frames;
}

int core_link_start(m2core *c, int listen_port, const char *next, int framesync, char *err, size_t err_size) {
    if (listen_port < 1 || listen_port > 65535) {
        set_error(err, err_size, "link: listen port must be 1-65535");
        return -1;
    }
    try {
        auto link = std::make_unique<app::TcpLink>(uint16_t(listen_port), next ? next : "");
        if (!link->error().empty()) {
            set_error(err, err_size, ("link: " + link->error()).c_str());
            return -1;
        }
        c->game->board().set_link(link.get(), framesync != 0);
        c->link = std::move(link);
        return 0;
    } catch (const std::exception &e) {
        set_error(err, err_size, e.what());
        return -1;
    }
}

int core_link_state(m2core *c, int *id, int *count) {
    const rt::CommBoard *board = c->link ? c->game->board().comm_board() : nullptr;
    if (!board) return M2CORE_LINK_OFF;
    if (id) *id = board->id();
    if (count) *count = board->count();
    switch (board->link()) {
    case rt::CommBoard::Link::Waiting: return M2CORE_LINK_WAITING;
    case rt::CommBoard::Link::Up: return M2CORE_LINK_UP;
    case rt::CommBoard::Link::Lost: return M2CORE_LINK_LOST;
    default: return M2CORE_LINK_OFF;
    }
}

const m2core_api kApi = {
    M2CORE_API_VERSION, M2_ROMSET, rt::kGameTitle, rt::GameLoop::kFrameHz,
    rt::GameLoop::kWidth, rt::GameLoop::kHeight,
    core_create, core_destroy, core_run_frame, core_screen,
    core_audio_rate, core_audio_read, core_save, core_link_start, core_link_state,
};

} // namespace

#define M2CORE_ENTRY_(set) m2core_api_##set
#define M2CORE_ENTRY(set) M2CORE_ENTRY_(set)
extern "C" M2CORE_EXPORT const m2core_api *M2CORE_ENTRY(M2_ROMSET_ID)(void) { return &kApi; }
