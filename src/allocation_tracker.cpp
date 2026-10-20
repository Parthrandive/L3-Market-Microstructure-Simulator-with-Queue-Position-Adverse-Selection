#include "orderbook/allocation_tracker.hpp"
#include <cstdlib>
#include <new>

void* operator new(std::size_t size) {
    if (l3::AllocationTracker::enabled.load(std::memory_order_relaxed)) {
        l3::AllocationTracker::allocation_count.fetch_add(1, std::memory_order_relaxed);
        l3::AllocationTracker::bytes_allocated.fetch_add(size, std::memory_order_relaxed);
    }
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}

void operator delete(void* ptr) noexcept {
    if (l3::AllocationTracker::enabled.load(std::memory_order_relaxed)) {
        l3::AllocationTracker::deallocation_count.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(ptr);
}

void* operator new[](std::size_t size) {
    if (l3::AllocationTracker::enabled.load(std::memory_order_relaxed)) {
        l3::AllocationTracker::allocation_count.fetch_add(1, std::memory_order_relaxed);
        l3::AllocationTracker::bytes_allocated.fetch_add(size, std::memory_order_relaxed);
    }
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}

void operator delete[](void* ptr) noexcept {
    if (l3::AllocationTracker::enabled.load(std::memory_order_relaxed)) {
        l3::AllocationTracker::deallocation_count.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    if (l3::AllocationTracker::enabled.load(std::memory_order_relaxed)) {
        l3::AllocationTracker::deallocation_count.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    if (l3::AllocationTracker::enabled.load(std::memory_order_relaxed)) {
        l3::AllocationTracker::deallocation_count.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(ptr);
}
