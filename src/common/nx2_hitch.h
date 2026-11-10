// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>

#include "common/common_types.h"

// Hitch attribution (2026-11-10, the owner's v5.6 playtest: 0.4-1 s frames with 0 pipelines built,
// the cause unclear from 10-second totals). Every place that can hold the game up adds its time
// here; the game's present (nx2:vi2) calls OnPresent with the gap since the previous present, and
// a gap over 50 ms logs one line with its exact time and what each cause took inside it.
// EDEN_NX2_HITCH_LOG=0 turns the line off (the counters stay, they are a few relaxed adds).
namespace Common::Nx2Hitch {

enum class Cause : u8 {
    FullDrain,    ///< fence releases: the front waited until the back and the host GPU were idle
    ReadBack,     ///< guest reads of GPU-written memory (download + wait), any thread
    SaveData,     ///< save data writes, flushes and commits (fsp)
    MemorySample, ///< working-set walks for the memory lines (GPU thread, memory probe)
    ZeroTrim,     ///< zero-page passes that ended in the gap (their own thread)
    FileThrottle, ///< IStorage::Read's sleep under 40 MB/s (game files, the one FS thread)
    FileRead,     ///< IStorage::Read's read itself
    HostMemory,   ///< host Map/Unmap/Protect of guest memory (placeholder lock included)
    Count,
};

/// Names in the hitch line, in Cause order.
inline constexpr std::array<const char*, 9> CauseNames{
    "full drains", "read-backs",  "save data",  "memory walks", "zero-page passes",
    "file read throttle", "file reads", "host map/protect", "?"};

struct Counters {
    std::array<std::atomic<u64>, static_cast<size_t>(Cause::Count)> ns{};
    std::array<std::atomic<u64>, static_cast<size_t>(Cause::Count)> count{};
    /// The longest single one since the last present (reset there)
    std::array<std::atomic<u64>, static_cast<size_t>(Cause::Count)> longest{};
    /// steady_clock ns when the running zero-page pass began, 0 while none runs
    std::atomic<u64> zero_trim_since{};
};

inline Counters g_counters;

[[nodiscard]] inline u64 NowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

inline void Add(Cause cause, u64 ns) {
    const auto i = static_cast<size_t>(cause);
    g_counters.ns[i].fetch_add(ns, std::memory_order_relaxed);
    g_counters.count[i].fetch_add(1, std::memory_order_relaxed);
    u64 longest = g_counters.longest[i].load(std::memory_order_relaxed);
    while (ns > longest &&
           !g_counters.longest[i].compare_exchange_weak(longest, ns, std::memory_order_relaxed)) {
    }
}

/// Times its scope as `cause`.
class Scope {
public:
    explicit Scope(Cause cause_) : cause{cause_}, begin{NowNs()} {}
    ~Scope() {
        Add(cause, NowNs() - begin);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Cause cause;
    u64 begin;
};

inline void ZeroTrimBegin() {
    g_counters.zero_trim_since.store(NowNs(), std::memory_order_relaxed);
}

inline void ZeroTrimEnd() {
    const u64 since = g_counters.zero_trim_since.exchange(0, std::memory_order_relaxed);
    if (since != 0) {
        Add(Cause::ZeroTrim, NowNs() - since);
    }
}

static_assert(CauseNames.size() == static_cast<size_t>(Cause::Count) + 1);

/// From the game's present, one thread: `gap_us` since its previous present.
void OnPresent(long long gap_us);

} // namespace Common::Nx2Hitch
