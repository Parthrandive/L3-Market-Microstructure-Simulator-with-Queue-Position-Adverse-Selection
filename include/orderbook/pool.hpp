#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include "orderbook/order_node.hpp"

namespace l3 {

template<typename T>
class Pool {
public:
    explicit Pool(size_t capacity, std::string name = "Pool")
        : capacity_(static_cast<uint32_t>(capacity)), name_(std::move(name)) {
        if (capacity == 0 || capacity > NULL_INDEX) {
            throw std::invalid_argument("Invalid pool capacity");
        }
        storage_.resize(capacity_);
        reset();
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
    std::vector<T> storage_{};
};

} // namespace l3
