#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <utility>
#include <type_traits>

namespace l3 {

// 128-byte alignment safely isolates cache lines on both Apple Silicon (128B L2) and x86_64 (64B)
constexpr size_t RING_BUFFER_CACHE_LINE = 128;

template <typename T, size_t Capacity>
class SpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static_assert(Capacity >= 2, "Capacity must be at least 2");

    static constexpr size_t MASK = Capacity - 1;

    // Buffer storage
    alignas(RING_BUFFER_CACHE_LINE) std::array<T, Capacity> buffer_{};

    // Producer cache line: write index and local cached read index
    alignas(RING_BUFFER_CACHE_LINE) std::atomic<size_t> write_idx_{0};
    size_t cached_read_idx_{0};

    // Consumer cache line: read index and local cached write index
    alignas(RING_BUFFER_CACHE_LINE) std::atomic<size_t> read_idx_{0};
    size_t cached_write_idx_{0};

public:
    SpscRingBuffer() noexcept = default;
    ~SpscRingBuffer() = default;

    // Non-copyable, non-movable
    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
    SpscRingBuffer(SpscRingBuffer&&) = delete;
    SpscRingBuffer& operator=(SpscRingBuffer&&) = delete;

    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity;
    }

    // Producer: push by const reference
    [[nodiscard]] bool try_push(const T& item) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        const size_t current_write = write_idx_.load(std::memory_order_relaxed);

        if (current_write - cached_read_idx_ >= Capacity) {
            cached_read_idx_ = read_idx_.load(std::memory_order_acquire);
            if (current_write - cached_read_idx_ >= Capacity) {
                return false; // Buffer full
            }
        }

        buffer_[current_write & MASK] = item;
        write_idx_.store(current_write + 1, std::memory_order_release);
        return true;
    }

    // Producer: push by rvalue reference (move)
    [[nodiscard]] bool try_push(T&& item) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const size_t current_write = write_idx_.load(std::memory_order_relaxed);

        if (current_write - cached_read_idx_ >= Capacity) {
            cached_read_idx_ = read_idx_.load(std::memory_order_acquire);
            if (current_write - cached_read_idx_ >= Capacity) {
                return false; // Buffer full
            }
        }

        buffer_[current_write & MASK] = std::move(item);
        write_idx_.store(current_write + 1, std::memory_order_release);
        return true;
    }

    // Producer: in-place emplace
    template <typename... Args>
    [[nodiscard]] bool try_emplace(Args&&... args) noexcept {
        const size_t current_write = write_idx_.load(std::memory_order_relaxed);

        if (current_write - cached_read_idx_ >= Capacity) {
            cached_read_idx_ = read_idx_.load(std::memory_order_acquire);
            if (current_write - cached_read_idx_ >= Capacity) {
                return false; // Buffer full
            }
        }

        buffer_[current_write & MASK] = T{std::forward<Args>(args)...};
        write_idx_.store(current_write + 1, std::memory_order_release);
        return true;
    }

    // Consumer: try pop
    [[nodiscard]] bool try_pop(T& out_item) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const size_t current_read = read_idx_.load(std::memory_order_relaxed);

        if (current_read == cached_write_idx_) {
            cached_write_idx_ = write_idx_.load(std::memory_order_acquire);
            if (current_read == cached_write_idx_) {
                return false; // Buffer empty
            }
        }

        out_item = std::move(buffer_[current_read & MASK]);
        read_idx_.store(current_read + 1, std::memory_order_release);
        return true;
    }

    // Approximate size observation
    [[nodiscard]] size_t size_approx() const noexcept {
        const size_t w = write_idx_.load(std::memory_order_relaxed);
        const size_t r = read_idx_.load(std::memory_order_relaxed);
        return (w >= r) ? (w - r) : 0;
    }

    [[nodiscard]] bool empty_approx() const noexcept {
        return size_approx() == 0;
    }
};

} // namespace l3
