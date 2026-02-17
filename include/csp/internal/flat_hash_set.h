#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <new>
#include <utility>

namespace csp::detail {

template <typename Key, typename Hash = std::hash<Key>,
          typename Eq = std::equal_to<Key>>
class FlatHashSet {
public:
    explicit FlatHashSet(size_t initial_cap = 16, Hash hash = {}, Eq eq = {})
        : mask_(round_up_pow2(initial_cap) - 1)
        , hash_(hash)
        , eq_(eq)
        , ctrl_(alloc_ctrl(mask_ + 1))
        , slots_(alloc_slots(mask_ + 1))
    {
        for (size_t i = 0; i <= mask_; ++i) ctrl_[i] = 0;
    }

    ~FlatHashSet() {
        clear();
        dealloc_ctrl(ctrl_);
        dealloc_slots(slots_);
    }

    FlatHashSet(FlatHashSet const&) = delete;
    FlatHashSet& operator=(FlatHashSet const&) = delete;

    FlatHashSet(FlatHashSet&& o) noexcept
        : size_(o.size_)
        , mask_(o.mask_)
        , hash_(o.hash_)
        , eq_(o.eq_)
        , ctrl_(std::exchange(o.ctrl_, nullptr))
        , slots_(std::exchange(o.slots_, nullptr))
    {
        o.size_ = 0;
        o.mask_ = 0;
    }

    FlatHashSet& operator=(FlatHashSet&& o) noexcept {
        if (this != &o) {
            clear();
            dealloc_ctrl(ctrl_);
            dealloc_slots(slots_);
            size_ = o.size_;
            mask_ = o.mask_;
            hash_ = o.hash_;
            eq_ = o.eq_;
            ctrl_ = std::exchange(o.ctrl_, nullptr);
            slots_ = std::exchange(o.slots_, nullptr);
            o.size_ = 0;
            o.mask_ = 0;
        }
        return *this;
    }

    // Returns true if newly inserted.
    bool insert(const Key& k) {
        if (size_ * 4 >= (mask_ + 1) * 3) grow();
        size_t i = hash_(k) & mask_;
        while (ctrl_[i]) {
            if (eq_(slots_[i], k)) return false;
            i = (i + 1) & mask_;
        }
        new (&slots_[i]) Key(k);
        ctrl_[i] = 1;
        ++size_;
        return true;
    }

    // Erase with backward-shift deletion. Returns true if found.
    bool erase(const Key& k) {
        size_t i = hash_(k) & mask_;
        while (ctrl_[i]) {
            if (eq_(slots_[i], k)) {
                do_erase(i);
                return true;
            }
            i = (i + 1) & mask_;
        }
        return false;
    }

    size_t size() const { return size_; }

private:
    size_t size_ = 0;
    size_t mask_;
    Hash hash_;
    Eq eq_;
    uint8_t* ctrl_;
    Key* slots_;

    void clear() {
        if (!ctrl_) return;
        for (size_t i = 0; i <= mask_; ++i) {
            if (ctrl_[i]) {
                slots_[i].~Key();
                ctrl_[i] = 0;
            }
        }
        size_ = 0;
    }

    void do_erase(size_t i) {
        slots_[i].~Key();
        ctrl_[i] = 0;
        --size_;

        // Backward-shift: fill the gap by pulling displaced elements back.
        size_t j = (i + 1) & mask_;
        while (ctrl_[j]) {
            size_t ideal = hash_(slots_[j]) & mask_;
            // Shift j→i if i is in the probe range [ideal, j).
            // Circular distance: (i - ideal) & mask < (j - ideal) & mask
            if (((i - ideal) & mask_) < ((j - ideal) & mask_)) {
                new (&slots_[i]) Key(std::move(slots_[j]));
                ctrl_[i] = 1;
                slots_[j].~Key();
                ctrl_[j] = 0;
                i = j;
            }
            j = (j + 1) & mask_;
        }
    }

    void grow() {
        size_t old_cap = mask_ + 1;
        size_t new_cap = old_cap * 2;
        size_t new_mask = new_cap - 1;

        uint8_t* new_ctrl = alloc_ctrl(new_cap);
        Key* new_slots = alloc_slots(new_cap);
        for (size_t i = 0; i < new_cap; ++i) new_ctrl[i] = 0;

        for (size_t i = 0; i < old_cap; ++i) {
            if (ctrl_[i]) {
                size_t h = hash_(slots_[i]) & new_mask;
                while (new_ctrl[h]) h = (h + 1) & new_mask;
                new (&new_slots[h]) Key(std::move(slots_[i]));
                new_ctrl[h] = 1;
                slots_[i].~Key();
            }
        }

        dealloc_ctrl(ctrl_);
        dealloc_slots(slots_);
        ctrl_ = new_ctrl;
        slots_ = new_slots;
        mask_ = new_mask;
    }

    static size_t round_up_pow2(size_t n) {
        assert(n > 0);
        --n;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        if constexpr (sizeof(size_t) > 4)
            n |= n >> 32;
        return n + 1;
    }

    static uint8_t* alloc_ctrl(size_t n) {
        return static_cast<uint8_t*>(::operator new(n));
    }

    static void dealloc_ctrl(uint8_t* p) {
        if (p) ::operator delete(p);
    }

    static Key* alloc_slots(size_t n) {
        return static_cast<Key*>(
            ::operator new(n * sizeof(Key), std::align_val_t(alignof(Key))));
    }

    static void dealloc_slots(Key* p) {
        if (p) ::operator delete(p, std::align_val_t(alignof(Key)));
    }
};

}
