#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <cassert>
#include <type_traits>
#include "orderbook/order_node.hpp"

namespace l3 {

template<typename T, size_t Alignment = 64>
struct AlignedDeleter {
    void operator()(T* ptr) const noexcept {
        if (ptr) {
#if defined(_MSC_VER)
            _aligned_free(ptr);
#else
            std::free(ptr);
#endif
        }
    }
};

template<typename T>
class Pool {
public:
    explicit Pool(size_t capacity, std::string name = "Pool")
        : capacity_(static_cast<uint32_t>(capacity)), name_(std::move(name)) {
        if (capacity == 0 || capacity > NULL_INDEX) {
            throw std::invalid_argument("Invalid pool capacity");
        }

        size_t total_bytes = capacity_ * sizeof(T);
        if (total_bytes % 64 != 0) {
            total_bytes = ((total_bytes / 64) + 1) * 64;
        }

        void* raw_ptr = nullptr;
#if defined(_MSC_VER)
        raw_ptr = _aligned_malloc(total_bytes, 64);
#else
        if (posix_memalign(&raw_ptr, 64, total_bytes) != 0) {
            raw_ptr = nullptr;
        }
#endif
        if (!raw_ptr) {
            throw std::bad_alloc();
        }

        storage_.reset(static_cast<T*>(raw_ptr));
        assert(reinterpret_cast<uintptr_t>(storage_.get()) % 64 == 0 && "Pool base address must be 64-byte aligned");

        for (uint32_t i = 0; i < capacity_; ++i) {
            new (&storage_[i]) T();
        }
        reset();
    }

    ~Pool() {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            if (storage_) {
                for (uint32_t i = 0; i < capacity_; ++i) {
                    storage_[i].~T();
                }
            }
        }
    }

    // Move-only semantics (no copying a large pool)
    Pool(Pool&&) noexcept = default;
    Pool& operator=(Pool&&) noexcept = default;
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    [[nodiscard]] bool is_64_byte_aligned() const noexcept {
        return reinterpret_cast<uintptr_t>(storage_.get()) % 64 == 0;
    }

    void reset() {
        allocated_count_ = 0;
        peak_allocated_ = 0;
        free_head_ = 0;

        // Initialize singly-linked free list using next_idx convention
        for (uint32_t i = 0; i < capacity_ - 1; ++i) {
            set_next_free(i, i + 1);
        }
        set_next_free(capacity_ - 1, NULL_INDEX);
    }

    [[nodiscard]] uint32_t allocate(size_t msg_idx = 0) {
        if (free_head_ == NULL_INDEX) [[unlikely]] {
            std::cerr << "\n[FATAL ERROR] " << name_ << " EXHAUSTED!\n"
                      << "  Capacity: " << capacity_ << "\n"
                      << "  Allocated: " << allocated_count_ << "\n"
                      << "  Message Index: " << msg_idx << "\n"
                      << "Aborting process to enforce zero-allocation rule.\n" << std::endl;
            std::abort();
        }

        uint32_t idx = free_head_;
        free_head_ = get_next_free(idx);
        allocated_count_++;
        if (allocated_count_ > peak_allocated_) {
            peak_allocated_ = allocated_count_;
        }

        // Initialize node to zero / default state
        storage_[idx] = T{};
        return idx;
    }

    void deallocate(uint32_t idx) noexcept {
        if (idx >= capacity_) return;
        set_next_free(idx, free_head_);
        free_head_ = idx;
        if (allocated_count_ > 0) {
            allocated_count_--;
        }
    }

    [[nodiscard]] T& operator[](uint32_t idx) noexcept {
        return storage_[idx];
    }

    [[nodiscard]] const T& operator[](uint32_t idx) const noexcept {
        return storage_[idx];
    }

    [[nodiscard]] uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] uint32_t allocated_count() const noexcept { return allocated_count_; }
    [[nodiscard]] uint32_t peak_allocated() const noexcept { return peak_allocated_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

private:
    void set_next_free(uint32_t idx, uint32_t next) noexcept {
        static_assert(sizeof(T) >= sizeof(uint32_t), "T must be at least 4 bytes to store free list pointer");
        std::memcpy(&storage_[idx], &next, sizeof(uint32_t));
    }

    [[nodiscard]] uint32_t get_next_free(uint32_t idx) const noexcept {
        static_assert(sizeof(T) >= sizeof(uint32_t), "T must be at least 4 bytes to store free list pointer");
        uint32_t next = NULL_INDEX;
        std::memcpy(&next, &storage_[idx], sizeof(uint32_t));
        return next;
    }

    uint32_t capacity_{0};
    uint32_t free_head_{NULL_INDEX};
    uint32_t allocated_count_{0};
    uint32_t peak_allocated_{0};
    std::string name_{"Pool"};
    std::unique_ptr<T[], AlignedDeleter<T, 64>> storage_{};
};

} // namespace l3
