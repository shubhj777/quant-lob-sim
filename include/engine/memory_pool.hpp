#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob {

template <typename T>
class MemoryPool {
private:
    std::vector<T> storage_;
    std::vector<int32_t> free_stack_;
    int32_t next_fresh_ = 0;
    size_t free_count_ = 0;

public:
    explicit MemoryPool(size_t capacity)
        : storage_(capacity), free_stack_(capacity, -1) {}

    int32_t allocate() {
        if (free_count_ > 0) {
            return free_stack_[--free_count_];
        }
        assert(static_cast<size_t>(next_fresh_) < storage_.size());
        return next_fresh_++;
    }

    void deallocate(int32_t idx) {
        assert(idx >= 0 && static_cast<size_t>(idx) < storage_.size());
        free_stack_[free_count_++] = idx;
    }

    T& operator[](int32_t idx) { return storage_[static_cast<size_t>(idx)]; }
    const T& operator[](int32_t idx) const { return storage_[static_cast<size_t>(idx)]; }

    size_t capacity() const { return storage_.size(); }
};

} // namespace lob