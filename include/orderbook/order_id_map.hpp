#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>
#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include "orderbook/order_node.hpp"

namespace l3 {

class OrderIdMap {
public:
    struct Entry {
        uint64_t order_ref{0};
        uint32_t node_idx{NULL_INDEX};
    };

    explicit OrderIdMap(size_t capacity_power_of_two) {
        if (capacity_power_of_two < 16 || (capacity_power_of_two & (capacity_power_of_two - 1)) != 0) {
            throw std::invalid_argument("OrderIdMap capacity must be a power of two >= 16");
        }
        capacity_ = capacity_power_of_two;
        mask_ = capacity_ - 1;
        table_.resize(capacity_, Entry{0, NULL_INDEX});
    }

    void reset() {
        std::fill(table_.begin(), table_.end(), Entry{0, NULL_INDEX});
        size_ = 0;
        total_inserts_ = 0;
        max_probe_length_ = 0;
        total_probes_insert_ = 0;
    }

    // Hash mixer: SplitMix64 finalizer for high-entropy avalanche
    static inline size_t hash_key(uint64_t key) noexcept {
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdULL;
        key ^= key >> 33;
        key *= 0xc4ceb9fe1a85ec53ULL;
        key ^= key >> 33;
        return static_cast<size_t>(key);
    }

    bool insert(uint64_t order_ref, uint32_t node_idx, size_t msg_idx = 0) {
        if (order_ref == 0) [[unlikely]] return false;

        if (size_ >= (capacity_ * 9) / 10) [[unlikely]] {
            std::cerr << "\n[FATAL ERROR] OrderIdMap LOAD FACTOR CRITICAL (>90%)!\n"
                      << "  Capacity: " << capacity_ << "\n"
                      << "  Elements: " << size_ << "\n"
                      << "  Message Index: " << msg_idx << "\n"
                      << "Aborting process to maintain bounded latency.\n" << std::endl;
            std::abort();
        }

        size_t idx = hash_key(order_ref) & mask_;
        size_t probes = 1;

        while (table_[idx].order_ref != 0) {
            if (table_[idx].order_ref == order_ref) {
                // Key already exists: overwrite
                table_[idx].node_idx = node_idx;
                return false;
            }
            idx = (idx + 1) & mask_;
            probes++;
        }

        table_[idx] = Entry{order_ref, node_idx};
        size_++;
        total_inserts_++;
        total_probes_insert_ += probes;
        if (probes > max_probe_length_) {
            max_probe_length_ = probes;
        }
        return true;
    }

    [[nodiscard]] uint32_t find(uint64_t order_ref) const noexcept {
        if (order_ref == 0) [[unlikely]] return NULL_INDEX;

        size_t idx = hash_key(order_ref) & mask_;
        while (table_[idx].order_ref != 0) {
            if (table_[idx].order_ref == order_ref) {
                return table_[idx].node_idx;
            }
            idx = (idx + 1) & mask_;
        }
        return NULL_INDEX;
    }

    // Backward-shift deletion (zero tombstones)
    bool erase(uint64_t order_ref) noexcept {
        if (order_ref == 0) [[unlikely]] return false;

        size_t i = hash_key(order_ref) & mask_;
        while (table_[i].order_ref != 0) {
            if (table_[i].order_ref == order_ref) {
                break;
            }
            i = (i + 1) & mask_;
        }

        if (table_[i].order_ref == 0) {
            return false; // Not found
        }

        // Backward shift subsequent cluster members
        size_t j = i;
        while (true) {
            j = (j + 1) & mask_;
            if (table_[j].order_ref == 0) {
                break;
            }

            size_t k = hash_key(table_[j].order_ref) & mask_;
            // Determine if k is cyclically strictly between i and j
            bool between = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
            if (!between) {
                table_[i] = table_[j];
                i = j;
            }
        }

        table_[i] = Entry{0, NULL_INDEX};
        size_--;
        return true;
    }

    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] double load_factor() const noexcept { return static_cast<double>(size_) / capacity_; }
    [[nodiscard]] size_t max_probe_length() const noexcept { return max_probe_length_; }
    [[nodiscard]] double avg_probes_per_insert() const noexcept {
        return total_inserts_ > 0 ? static_cast<double>(total_probes_insert_) / total_inserts_ : 1.0;
    }

private:
    size_t capacity_{0};
    size_t mask_{0};
    size_t size_{0};
    size_t total_inserts_{0};
    size_t max_probe_length_{0};
    size_t total_probes_insert_{0};
    std::vector<Entry> table_{};
};

} // namespace l3
