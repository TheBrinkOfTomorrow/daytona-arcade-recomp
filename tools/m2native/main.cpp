// m2native: the M1 lockstep harness for recompiled code. Same checks as
// m2replay (every device access, every sample's RAM hashes and registers,
// every interrupt against MAME), but the i960 code is the native C++ that
// m2recomp generated. There is no fallback: control reaching an address with
// no recompiled code is a hard error naming it.
//
//   m2native IMAGES_DIR TRACE.m2tr IRQ.log

#include "runtime/gen_support.h"
#include "runtime/lockstep.h"
#include "runtime/m2_replay_bus.h"
#include "runtime/m2_board.h"
#include "runtime/m2_tgp_board.h"
#include "geocheck.h"

#include <memory>

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
// Model 2A-CRX: the 315-5649 I/O chip and the EEPROM modelled natively
// (strict: every read must equal MAME's), with the inputs at rest, in
// front of the TGP board model.
class IoModel : public rt::DeviceModel {
public:
    explicit IoModel(rt::DeviceModel &next) : next_(next) {}
    void set_clock(const rt::Lockstep &ls) { io_.set_clock([&ls] { return rt::ticks_for_instructions(ls.count); }); }
    bool claims(uint32_t a) const override { return mine(a) || next_.claims(a); }
    Check read(uint32_t a, uint32_t mask, uint32_t &v) override {
        if (!mine(a)) return next_.read(a, mask, v);
        const uint32_t i = ((a & 0x1f) >> 2) * 2;
        v = ((mask & 0x000000ff) ? uint32_t(io_.read(i)) : 0) | ((mask & 0x00ff0000) ? uint32_t(io_.read(i + 1)) << 16 : 0);
        if (dbg_) std::fprintf(stderr, "R %08x %08x %08x\n", a, mask, v);
        // port B carries the EEPROM's DO: how long a write keeps it busy
        // depends on time, which we only estimate (measured; other bits strict)
        return a == 0x01c00000 && (mask & 0x00ff0000) ? Measured : Strict;
    }
    void write(uint32_t a, uint32_t data, uint32_t mask) override {
        if (!mine(a)) return next_.write(a, data, mask);
        const uint32_t i = ((a & 0x1f) >> 2) * 2;
        if (dbg_) std::fprintf(stderr, "W %08x %08x %08x\n", a, mask, data & mask);
        if (mask & 0x000000ff) io_.write(i, uint8_t(data));
        if (mask & 0x00ff0000) io_.write(i + 1, uint8_t(data >> 16));
    }
    void measured_mismatch(uint32_t a, uint32_t ours, uint32_t mame) override {
        if (!mine(a)) return next_.measured_mismatch(a, ours, mame);
        if ((ours ^ mame) & ~0x00200000u) {
            char b[96];
            std::snprintf(b, sizeof b, "I/O read %08x: ours %08x, MAME %08x", a, ours, mame);
            throw rt::Divergence(b);
        }
        ++do_mismatch;
    }
    uint64_t do_mismatch = 0; // EEPROM busy reads that differ
    const uint8_t *region(uint32_t base) const override { return next_.region(base); }

private:
    static bool mine(uint32_t a) { return a >= 0x01c00000 && a <= 0x01c0001f; }
    rt::DeviceModel &next_;
    bool dbg_ = std::getenv("M2_NATIVE_IO_LOG") != nullptr;
    rt::IoBoard board_;
    rt::Io5649 io_{board_};
};

std::vector<uint8_t> load(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "usage: m2native IMAGES_DIR TRACE.m2tr IRQ.log [GEO.log]\n");
        return 2;
    }
    const std::string dir = argv[1];
    uint64_t done = 0;
    try {
        rt::M2ReplayBus bus(load(dir + "/program.bin"), load(dir + "/main_data.bin"), argv[2]);
        rt::Cpu core(&bus);
        bus.attach(&core);
        // M2: the geometry ports, TGP and buffer RAM are native, not replayed
        // (Model 2A-CRX's are model2_tgp_mem's too).
        rt::TgpBoard board(load(dir + "/copro_tables.bin"), load(dir + "/copro_data.bin"));
        rt::TgpBoardModel geo(board);
        // M2_NATIVE_IO=1 (Model 2A): the I/O chip native too
        IoModel io(geo);
        bus.set_model(rt::kModel2a && std::getenv("M2_NATIVE_IO") ? static_cast<rt::DeviceModel *>(&io) : &geo);
        rt::Lockstep ls(core, argv[3]);
        io.set_clock(ls);
        core.reset();
        gen::Env env{core, ls};
        // M2: the geometrizer, checked at each vblank against MAME's GEO.log.
        std::unique_ptr<rt::Geo> geom;
        std::unique_ptr<GeoCheck> geocheck;
        if (argc == 5) {
            geom = std::make_unique<rt::Geo>(load(dir + "/polygons.bin"), load(dir + "/textures.bin"), board.buffer_data());
            geocheck = std::make_unique<GeoCheck>(argv[4], *geom, board, bus, ls);
        }

        const auto t0 = std::chrono::steady_clock::now();
        while (!ls.finished() && !bus.trace_done()) {
            if (!gen::has_code(core.m_IP)) {
                char b[128];
                std::snprintf(b, sizeof b, "no recompiled code at %08x (instruction %" PRIu64 "): add it to the seeds",
                              core.m_IP, ls.count);
                throw rt::Fatal(b);
            }
            gen::run(env);
            done = ls.count;
        }
        done = ls.count;
        if (!bus.all_events_consumed())
            throw rt::Divergence("MAME's run ended but the trace still has events we did not make; " + bus.where());
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("m2native: MATCH. %" PRIu64 " instructions, all native (%" PRIu64 " recompiled), %" PRIu64
                    " epochs, %" PRIu64 " device events, %d interrupts, identical to MAME; %.2f s (%.1f M instr/s)\n",
                    ls.count, gen::native_instructions(), bus.epochs_matched(), bus.events_matched(), ls.interrupts(), s,
                    double(ls.count) / s / 1e6);
        std::printf("  TGP (recompiled, run on demand): %" PRIu64 " instructions, %" PRIu64
                    " output words identical to MAME\n"
                    "  measured (depends on relative timing): buffer RAM reads %" PRIu64 " differ of %" PRIu64
                    "; FIFO-status polls %" PRIu64 " differ of %" PRIu64 "; buffer RAM hash differs at %" PRIu64 " of %" PRIu64 " samples\n",
                    board.tgp_instructions(), geo.fifo_words, geo.buffer_mismatch, geo.buffer_reads, geo.status_mismatch,
                    geo.status_reads, geo.region_mismatch, geo.region_samples);
        if (geocheck)
            std::printf("  geometrizer (native): %" PRIu64 " frames, %" PRIu64 " rasterizer words and %" PRIu64
                        " polygons identical to MAME\n",
                        geocheck->frames_checked, geocheck->words, geocheck->polys);
        if (geocheck) {
            std::printf("  3D layer (CPU reference rasterizer): %" PRIu64 " of %" PRIu64 " frames identical to MAME%s%s\n",
                        geocheck->fb_checked - geocheck->fb_mismatch, geocheck->fb_checked,
                        geocheck->fb_mismatch ? "; first difference " : "", geocheck->first_fb_mismatch.c_str());
            std::printf("  screen (tilemaps + 3D, composed): %" PRIu64 " of %" PRIu64 " frames identical to MAME%s%s\n",
                        geocheck->scr_checked - geocheck->scr_mismatch, geocheck->scr_checked,
                        geocheck->scr_mismatch ? "; first difference " : "", geocheck->first_scr_mismatch.c_str());
        }
        if (rt::kModel2a && std::getenv("M2_NATIVE_IO"))
            std::printf("  I/O chip and EEPROM (native): strict except EEPROM busy reads, %" PRIu64 " of which differ\n", io.do_mismatch);
        if (geo.buffer_mismatch) std::printf("  first buffer RAM difference: %s\n", geo.first_buffer_mismatch.c_str());
        if (geo.status_mismatch) std::printf("  first FIFO-status difference: %s\n", geo.first_status_mismatch.c_str());
        return 0;
    } catch (const rt::Divergence &d) {
        std::printf("m2native: DIVERGED after %" PRIu64 " instructions: %s\n", done, d.what());
        return 1;
    } catch (const rt::GeoFatal &f) {
        std::printf("m2native: GEOMETRIZER FATAL after %" PRIu64 " instructions: %s\n", done, f.what());
        return 1;
    } catch (const rt::TgpFatal &f) {
        std::printf("m2native: TGP FATAL after %" PRIu64 " instructions: %s\n", done, f.what());
        return 1;
    } catch (const rt::Fatal &f) {
        std::printf("m2native: FATAL after %" PRIu64 " instructions: %s\n", done, f.what());
        return 1;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "m2native: %s\n", e.what());
        return 2;
    }
}
