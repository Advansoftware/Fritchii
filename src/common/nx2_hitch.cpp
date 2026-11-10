// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>

#include "common/logging.h"
#include "common/nx2_hitch.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace Common::Nx2Hitch {

namespace {

constexpr long long ThresholdUs = 50'000;
constexpr size_t Causes = static_cast<size_t>(Cause::Count);

struct Snapshot {
    std::array<u64, Causes> ns{};
    std::array<u64, Causes> count{};
    u64 page_faults{};
};

Snapshot Take() {
    Snapshot snapshot;
    for (size_t i = 0; i < Causes; ++i) {
        snapshot.ns[i] = g_counters.ns[i].load(std::memory_order_relaxed);
        snapshot.count[i] = g_counters.count[i].load(std::memory_order_relaxed);
    }
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        snapshot.page_faults = counters.PageFaultCount;
    }
#endif
    return snapshot;
}

bool Enabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_HITCH_LOG");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

} // Anonymous namespace

void OnPresent(long long gap_us) {
    static Snapshot last = Take();
    static u64 window_start_ns = NowNs();
    static int lines_in_window = 0;
    if (!Enabled()) {
        return;
    }
    const Snapshot now = Take();
    if (gap_us >= ThresholdUs) {
        // At most 20 lines a minute (a loading screen can stall many presents in a row).
        const u64 now_ns = NowNs();
        if (now_ns - window_start_ns >= 60'000'000'000ULL) {
            window_start_ns = now_ns;
            lines_in_window = 0;
        }
        if (lines_in_window++ < 20) {
            const auto ms = [&](Cause cause) {
                const auto i = static_cast<size_t>(cause);
                return static_cast<double>(now.ns[i] - last.ns[i]) / 1e6;
            };
            const auto n = [&](Cause cause) {
                const auto i = static_cast<size_t>(cause);
                return now.count[i] - last.count[i];
            };
            const u64 trim_since = g_counters.zero_trim_since.load(std::memory_order_relaxed);
            const double trim_running_ms =
                trim_since != 0 ? static_cast<double>(now_ns - trim_since) / 1e6 : 0.0;
            LOG_INFO(Render, // with the frame lines it lines up against
                     "Hitch: {:.1f} ms between presents; in it: full drains {:.1f} ms ({}), "
                     "read-backs {:.1f} ms ({}), save data {:.1f} ms ({}), memory walks {:.1f} ms "
                     "({}), zero-page passes ended {:.1f} ms ({}), one running for {:.0f} ms, "
                     "page faults {}",
                     static_cast<double>(gap_us) / 1e3, ms(Cause::FullDrain),
                     n(Cause::FullDrain), ms(Cause::ReadBack), n(Cause::ReadBack),
                     ms(Cause::SaveData), n(Cause::SaveData), ms(Cause::MemorySample),
                     n(Cause::MemorySample), ms(Cause::ZeroTrim), n(Cause::ZeroTrim),
                     trim_running_ms, now.page_faults - last.page_faults);
        }
    }
    last = now;
}

} // namespace Common::Nx2Hitch
