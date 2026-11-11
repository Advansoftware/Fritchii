// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <memory>
#include <mutex>

#include "common/common_types.h"
#include "common/range_sets.h"
#include "common/range_sets.inc"

namespace VideoCommon {

/// One bit per 4 KiB device page of the 36-bit device address space (2 MiB), for any thread.
/// Every read-modify-write and the loads that pair with them are sequentially consistent: the
/// lock-free read answer (EDEN_NX2_EXP47) relies on a store-then-load handshake between a guest
/// core and the buffer cache's owner (BufferCache::Nx2CleanForRead).
class AtomicPageBits {
public:
    static constexpr u64 PageBits = 12;
    static constexpr u64 NumWords = (u64{1} << (36 - PageBits)) / 64;

    AtomicPageBits() : words{std::make_unique<std::atomic<u64>[]>(NumWords)} {}

    /// Whether every page of [address, address + size) has a bit.
    [[nodiscard]] static bool Covers(u64 address, u64 size) {
        return size != 0 && address + size > address &&
               ((address + size - 1) >> PageBits) < NumWords * 64;
    }

    /// Sets the bits of the pages of [address, address + size) (pages past the map are dropped:
    /// readers treat them as set, see Any).
    void Set(u64 address, u64 size) {
        ForEachWord(address, size, [](std::atomic<u64>& word, u64 mask) {
            // Mostly set already (storage buffer binds mark megabytes each draw).
            if ((word.load(std::memory_order_seq_cst) & mask) != mask) {
                word.fetch_or(mask, std::memory_order_seq_cst);
            }
        });
    }

    void Clear(u64 address, u64 size) {
        ForEachWord(address, size, [](std::atomic<u64>& word, u64 mask) {
            if ((word.load(std::memory_order_seq_cst) & mask) != 0) {
                word.fetch_and(~mask, std::memory_order_seq_cst);
            }
        });
    }

    /// Whether any page of [address, address + size) has its bit set. Pages past the map count
    /// as set.
    [[nodiscard]] bool Any(u64 address, u64 size,
                           std::memory_order order = std::memory_order_seq_cst) const {
        if (!Covers(address, size)) {
            return true;
        }
        bool any = false;
        ForEachWord(address, size, [&](const std::atomic<u64>& word, u64 mask) {
            any = any || (word.load(order) & mask) != 0;
        });
        return any;
    }

    /// Clears the set bits of [address, address + size) and calls func(begin, end) for each of
    /// their pages (byte addresses).
    template <typename Func>
    void TakeEach(u64 address, u64 size, Func&& func) {
        ForEachWord(address, size, [&](std::atomic<u64>& word, u64 mask) {
            if ((word.load(std::memory_order_seq_cst) & mask) == 0) {
                return;
            }
            u64 taken = word.fetch_and(~mask, std::memory_order_seq_cst) & mask;
            const u64 base_page = static_cast<u64>(&word - words.get()) * 64;
            while (taken != 0) {
                const u64 bit = static_cast<u64>(std::countr_zero(taken));
                taken &= taken - 1;
                const u64 page = base_page + bit;
                func(page << PageBits, (page + 1) << PageBits);
            }
        });
    }

private:
    template <typename Word, typename Func>
    static void ForEachWordIn(Word* words, u64 address, u64 size, Func&& func) {
        if (size == 0) {
            return;
        }
        const u64 first = address >> PageBits;
        u64 last = (address + size - 1) >> PageBits;
        if (address + size <= address || first >= NumWords * 64) {
            return;
        }
        last = (std::min)(last, NumWords * 64 - 1);
        for (u64 page = first; page <= last;) {
            const u64 bit = page % 64;
            const u64 count = (std::min<u64>)(64 - bit, last - page + 1);
            const u64 mask = (count == 64 ? ~u64{0} : (u64{1} << count) - 1) << bit;
            func(words[page / 64], mask);
            page += count;
        }
    }

    template <typename Func>
    void ForEachWord(u64 address, u64 size, Func&& func) {
        ForEachWordIn(words.get(), address, size, func);
    }

    template <typename Func>
    void ForEachWord(u64 address, u64 size, Func&& func) const {
        ForEachWordIn(static_cast<const std::atomic<u64>*>(words.get()), address, size, func);
    }

    std::unique_ptr<std::atomic<u64>[]> words;
};

class ReadTrapTracker {
public:
    /// Returns whether the core must be notified to arm this page-aligned range.
    bool Arm(DAddr address, u64 size) {
        // EDEN_NX2_EXP47: traps the guest cores consumed with a lock-free read answer
        // (ConsumeLockFree) are forgotten first, as Subtract would have under the lock, so this
        // write arms them again. After the caller's dirty-page store: either this sees a
        // concurrent reader's mark, or that reader sees the dirty page and takes the locked path.
        if (lock_free_consumed_any.load(std::memory_order_seq_cst)) [[unlikely]] {
            lock_free_consumed->TakeEach(address, size, [&](u64 begin, u64 end) {
                ranges.Subtract(begin, end - begin);
            });
        }
        u64 armed_bytes = 0;
        ranges.ForEachInRange(address, size, [&](DAddr lower, DAddr upper) {
            armed_bytes += upper - lower;
        });
        if (armed_bytes == size) {
            return false;
        }
        ranges.Add(address, size);
        return true;
    }

    /// A CPU read consumes the core's trap; unmapping also forgets the range.
    void Subtract(DAddr address, u64 size) {
        ranges.Subtract(address, size);
    }

    /// EDEN_NX2_EXP47, any thread without the owner's lock: a guest read consumed the core's trap
    /// on these page-aligned bytes; the next Arm over them forgets it. False (nothing recorded)
    /// when the range is past the page map: the caller must take the locked path.
    [[nodiscard]] bool ConsumeLockFree(DAddr address, u64 size) {
        if (!AtomicPageBits::Covers(address, size)) {
            return false;
        }
        EnsureConsumedMap();
        lock_free_consumed->Set(address, size);
        if (!lock_free_consumed_any.load(std::memory_order_seq_cst)) {
            lock_free_consumed_any.store(true, std::memory_order_seq_cst);
        }
        return true;
    }

private:
    void EnsureConsumedMap() {
        // Allocated by the first lock-free read only (2 MiB), published before the flag.
        if (consumed_ready.load(std::memory_order_acquire)) {
            return;
        }
        std::call_once(consumed_once, [this] {
            lock_free_consumed = std::make_unique<AtomicPageBits>();
            consumed_ready.store(true, std::memory_order_release);
        });
    }

    Common::RangeSet<DAddr> ranges;
    std::unique_ptr<AtomicPageBits> lock_free_consumed;
    std::once_flag consumed_once;
    std::atomic<bool> consumed_ready{};
    std::atomic<bool> lock_free_consumed_any{};
};

} // namespace VideoCommon
