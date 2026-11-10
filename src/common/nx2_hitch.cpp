// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <string>

#include <fmt/format.h>

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
    std::array<u64, Causes> longest{};
    u64 page_faults{};
    u64 user_100ns{};   ///< the process's CPU time in user mode, all threads
    u64 kernel_100ns{}; ///< and in kernel mode (page faults, VM calls, waits' bookkeeping)
};

Snapshot Take() {
    Snapshot snapshot;
    for (size_t i = 0; i < Causes; ++i) {
        snapshot.ns[i] = g_counters.ns[i].load(std::memory_order_relaxed);
        snapshot.count[i] = g_counters.count[i].load(std::memory_order_relaxed);
        snapshot.longest[i] = g_counters.longest[i].exchange(0, std::memory_order_relaxed);
    }
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        snapshot.page_faults = counters.PageFaultCount;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const auto value = [](const FILETIME& time) {
            return (u64{time.dwHighDateTime} << 32) | time.dwLowDateTime;
        };
        snapshot.user_100ns = value(user);
        snapshot.kernel_100ns = value(kernel);
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
            std::string causes;
            for (size_t i = 0; i < Causes; ++i) {
                const u64 ns = now.ns[i] - last.ns[i];
                const u64 count = now.count[i] - last.count[i];
                if (count == 0) {
                    continue;
                }
                causes += fmt::format("{}{} {:.1f} ms ({}, longest {:.1f})",
                                      causes.empty() ? "" : ", ", CauseNames[i],
                                      static_cast<double>(ns) / 1e6, count,
                                      static_cast<double>(now.longest[i]) / 1e6);
            }
            const u64 trim_since = g_counters.zero_trim_since.load(std::memory_order_relaxed);
            if (trim_since != 0) {
                causes += fmt::format("{}a zero-page pass running for {:.0f} ms",
                                      causes.empty() ? "" : ", ",
                                      static_cast<double>(now_ns - trim_since) / 1e6);
            }
            LOG_INFO(Render, // with the frame lines it lines up against
                     "Hitch: {:.1f} ms between presents; in it: {}; page faults {}, CPU time "
                     "{:.0f} ms user + {:.0f} ms kernel (all threads)",
                     static_cast<double>(gap_us) / 1e3, causes.empty() ? "nothing counted" : causes,
                     now.page_faults - last.page_faults,
                     static_cast<double>(now.user_100ns - last.user_100ns) / 1e4,
                     static_cast<double>(now.kernel_100ns - last.kernel_100ns) / 1e4);
        }
    }
    last = now;
}

} // namespace Common::Nx2Hitch
