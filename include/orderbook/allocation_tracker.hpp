#pragma once

#include <cstddef>
#include <atomic>

namespace l3 {

struct AllocationTracker {
    static inline std::atomic<bool> enabled{false};
    static inline std::atomic<size_t> allocation_count{0};
    static inline std::atomic<size_t> deallocation_count{0};
    static inline std::atomic<size_t> bytes_allocated{0};

    static void start() noexcept {
        allocation_count.store(0, std::memory_order_seq_cst);
        deallocation_count.store(0, std::memory_order_seq_cst);
        bytes_allocated.store(0, std::memory_order_seq_cst);
        enabled.store(true, std::memory_order_seq_cst);
    }

    static void stop() noexcept {
        enabled.store(false, std::memory_order_seq_cst);
    }

    static void reset() noexcept {
        allocation_count.store(0, std::memory_order_seq_cst);
        deallocation_count.store(0, std::memory_order_seq_cst);
        bytes_allocated.store(0, std::memory_order_seq_cst);
    }

    static size_t count() noexcept {
        return allocation_count.load(std::memory_order_seq_cst);
    }

    static size_t bytes() noexcept {
        return bytes_allocated.load(std::memory_order_seq_cst);
    }
};

} // namespace l3
