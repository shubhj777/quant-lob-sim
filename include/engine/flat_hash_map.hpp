#pragma once

#include "types.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace lob {

class FlatHashMap {
private:
    static constexpr OrderId EMPTY_KEY = std::numeric_limits<OrderId>::max();

    struct Entry {
        OrderId key = EMPTY_KEY;
        int32_t value = -1;
    };

    std::vector<Entry> table_;
    size_t mask_ = 0;
    size_t size_ = 0;

    static size_t hash_key(OrderId x) {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        return static_cast<size_t>(x);
    }

    static size_t next_power_of_two(size_t n) {
        size_t cap = 16;
        while (cap < n) cap <<= 1;
        return cap;
    }

public:
    explicit FlatHashMap(size_t max_elements) {
        size_t cap = next_power_of_two(max_elements * 2);
        table_.resize(cap);
        mask_ = cap - 1;
    }

    bool contains(OrderId key) const {
        return find(key) != -1;
    }

    int32_t find(OrderId key) const {
        size_t idx = hash_key(key) & mask_;
        while (table_[idx].key != EMPTY_KEY) {
            if (table_[idx].key == key) {
                return table_[idx].value;
            }
            idx = (idx + 1) & mask_;
        }
        return -1;
    }

    void insert(OrderId key, int32_t value) {
        assert(key != EMPTY_KEY);
        assert(size_ + 1 < table_.size());
        size_t idx = hash_key(key) & mask_;
        while (table_[idx].key != EMPTY_KEY) {
            if (table_[idx].key == key) {
                table_[idx].value = value;
                return;
            }
            idx = (idx + 1) & mask_;
        }
        table_[idx].key = key;
        table_[idx].value = value;
        ++size_;
    }

    bool erase(OrderId key) {
        size_t idx = hash_key(key) & mask_;
        while (table_[idx].key != EMPTY_KEY) {
            if (table_[idx].key == key) {
                --size_;
                size_t hole = idx;
                size_t next = (hole + 1) & mask_;
                while (table_[next].key != EMPTY_KEY) {
                    size_t natural = hash_key(table_[next].key) & mask_;
                    if (((next - natural) & mask_) >= ((next - hole) & mask_)) {
                        table_[hole] = table_[next];
                        hole = next;
                    }
                    next = (next + 1) & mask_;
                }
                table_[hole].key = EMPTY_KEY;
                table_[hole].value = -1;
                return true;
            }
            idx = (idx + 1) & mask_;
        }
        return false;
    }

    size_t size() const { return size_; }
};

} // namespace lob