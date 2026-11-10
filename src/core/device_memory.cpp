// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include <cstdlib>

#include "common/logging.h"
#include "common/memory_detect.h"
#include "common/thread.h"
#endif

#include "core/device_memory.h"
#include "common/nx2_hitch.h"
#include "hle/kernel/board/nintendo/nx/k_system_control.h"

namespace Core {

#ifdef HAS_NCE
constexpr size_t VirtualReserveSize = 1ULL << 38;
#else
constexpr size_t VirtualReserveSize = 1ULL << 39;
#endif

DeviceMemory::DeviceMemory()
    : buffer{Kernel::Board::Nintendo::Nx::KSystemControl::Init::GetIntendedMemorySize(),
             VirtualReserveSize} {
#ifdef _WIN32
    // EDEN_NX2_ZERO_TRIM=0: no zero-page passes (A/B: their trims make the game fault the pages
    // back in when it reuses them, ~180k faults in an area load, 2026-11-10).
    if (const char* value = std::getenv("EDEN_NX2_ZERO_TRIM"); value == nullptr || value[0] != '0') {
        zero_trim_thread = std::jthread{[this](std::stop_token stop) { TrimZeroPages(stop); }};
    }
    if (const char* value = std::getenv("EDEN_NX2_MEM_OWNERS"); value == nullptr || value[0] != '0') {
        memory_probe_thread = std::jthread{[this](std::stop_token stop) { ProbeMemory(stop); }};
    }
#endif
}

DeviceMemory::~DeviceMemory() = default;

#ifdef _WIN32
// Windows can't punch holes in the guest memory section, so pages holding only zeros (games clear
// their heaps while booting: Ocarina leaves ~7 GB) stay resident until they leave the working set.
// A pass comes every 5 s while they keep turning up, backing off to once a minute.
void DeviceMemory::TrimZeroPages(std::stop_token stop) {
    using namespace std::chrono_literals;
    Common::SetCurrentThreadName("ZeroTrim");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    constexpr size_t Busy = size_t{64} << 20;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::chrono::seconds interval = 5s;
    while (true) {
        {
            std::unique_lock lock{mutex};
            wake.wait_for(lock, stop, interval, [] { return false; });
        }
        if (stop.stop_requested()) {
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        Common::Nx2Hitch::ZeroTrimBegin();
        const auto [resident, trimmed] = buffer.TrimZeroPages();
        Common::Nx2Hitch::ZeroTrimEnd();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        if (trimmed < Busy) {
            // A slow pass is logged too: its working set query and page reads take ~1 s at
            // ~7 GB resident, and freezes need lining up against it (fable52_answer_1 M8c).
            if (ms >= 100) {
                const auto phases = Common::HostMemory::LastZeroTrimPhases();
                LOG_INFO(HW_Memory,
                         "Zero-page pass: {} MiB trimmed of {} MiB resident in {} ms (next in {} s); "
                         "working set query {} ms, page reads {} ms, {} unlock runs {} ms "
                         "(longest {:.1f})",
                         trimmed >> 20, resident >> 20, ms,
                         (std::min)(interval * 2, std::chrono::seconds{60}).count(),
                         phases.query_us / 1000, phases.scan_us / 1000, phases.unlock_runs,
                         phases.unlock_us / 1000, static_cast<double>(phases.longest_unlock_us) / 1e3);
            }
            interval = (std::min)(interval * 2, std::chrono::seconds{60});
            continue;
        }
        interval = 5s;
        const auto phases = Common::HostMemory::LastZeroTrimPhases();
        LOG_INFO(HW_Memory,
                 "Trimmed {} MiB of all-zero guest pages out of {} MiB resident in {} ms; the kernel "
                 "zero-filled {} MiB so far; working set query {} ms, page reads {} ms, {} unlock "
                 "runs {} ms (longest {:.1f})",
                 trimmed >> 20, resident >> 20, ms, Common::HostMemory::ZeroFilledBytes() >> 20,
                 phases.query_us / 1000, phases.scan_us / 1000, phases.unlock_runs,
                 phases.unlock_us / 1000, static_cast<double>(phases.longest_unlock_us) / 1e3);
    }
}

void DeviceMemory::ProbeMemory(std::stop_token stop) {
    Common::SetCurrentThreadName("MemoryProbe");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    std::mutex mutex;
    std::condition_variable_any wake;
    while (true) {
        {
            std::unique_lock lock{mutex};
            wake.wait_for(lock, stop, std::chrono::seconds{10}, [] { return false; });
        }
        if (stop.stop_requested()) {
            return;
        }
        // Published for the GPU thread's line (Vulkan::LogMemoryOwners), which reads the last ones.
        {
            const Common::Nx2Hitch::Scope timed{Common::Nx2Hitch::Cause::MemorySample};
            static_cast<void>(buffer.SampleResident());
        }
        // Walking the heaps locks each one for the whole walk: the C runtime heap (~2.5 GB) took
        // ~250 ms, and every game thread that allocated meanwhile froze, a 270 ms hitch every
        // 10 s (2026-11-09). Only for a memory hunt: EDEN_NX2_MEM_HEAPS=1.
        static const bool walk_heaps = [] {
            const char* value = std::getenv("EDEN_NX2_MEM_HEAPS");
            return value != nullptr && value[0] == '1';
        }();
        if (walk_heaps) {
            static_cast<void>(Common::SampleHeapUsage());
        }
    }
}
#endif

} // namespace Core
