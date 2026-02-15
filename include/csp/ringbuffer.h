#ifndef INCLUDED__csp__ringbuffer_h
#define INCLUDED__csp__ringbuffer_h

#include <cassert>
#include <cstddef>
#include <new>
#include <utility>

namespace csp {

    namespace detail {

        template <typename T>
        class RingBuffer {
        public:
            static constexpr size_t npos = size_t(-1);

            explicit RingBuffer(size_t capacity = npos)
                : capacity_(capacity)
                , size_(round_up_pow2(capacity == npos ? 4 : capacity))
                , mask_(size_ - 1)
                , data_(alloc(size_))
            { }

            ~RingBuffer() {
                clear();
                dealloc(data_);
            }

            RingBuffer(RingBuffer const &) = delete;
            RingBuffer & operator=(RingBuffer const &) = delete;

            RingBuffer(RingBuffer && o) noexcept
                : capacity_(o.capacity_)
                , size_(o.size_)
                , mask_(o.mask_)
                , front_(o.front_)
                , back_(o.back_)
                , count_(o.count_)
                , data_(std::exchange(o.data_, nullptr))
            {
                o.count_ = 0;
                o.front_ = o.back_ = 0;
            }

            RingBuffer & operator=(RingBuffer && o) noexcept {
                if (this != &o) {
                    clear();
                    dealloc(data_);
                    capacity_ = o.capacity_;
                    size_ = o.size_;
                    mask_ = o.mask_;
                    front_ = o.front_;
                    back_ = o.back_;
                    count_ = o.count_;
                    data_ = std::exchange(o.data_, nullptr);
                    o.count_ = 0;
                    o.front_ = o.back_ = 0;
                }
                return *this;
            }

            size_t count() const { return count_; }
            bool empty() const { return !count_; }
            bool full() const { return count_ == capacity_; }
            T & front() const { return data_[front_]; }

            void * next() {
                if (count_ == size_) {
                    assert(capacity_ == npos);
                    grow();
                }
                return &data_[back_];
            }

            void push() {
                back_ = (back_ + 1) & mask_;
                ++count_;
            }

            void push(T t) {
                new (next()) T{std::move(t)};
                push();
            }

            template <typename... Args>
            void emplace(Args &&... args) {
                new (next()) T{std::forward<Args>(args)...};
                push();
            }

            void pop() {
                assert(!empty());
                data_[front_].~T();
                front_ = (front_ + 1) & mask_;
                --count_;
            }

            bool remove(T t) {
                for (size_t i = 0; i < count_; ++i) {
                    if (data_[(front_ + i) & mask_] == t) {
                        if (i == 0) {
                            pop();
                        } else if (i == count_ - 1) {
                            back_ = (back_ - 1) & mask_;
                            data_[back_].~T();
                            --count_;
                        } else {
                            // Fill hole from back.
                            back_ = (back_ - 1) & mask_;
                            data_[(front_ + i) & mask_] = std::move(data_[back_]);
                            data_[back_].~T();
                            --count_;
                        }
                        return true;
                    }
                }
                return false;
            }

            void clear() {
                while (!empty()) pop();
            }

            class iterator {
            public:
                using value_type = T;
                using reference = T &;
                using pointer = T *;
                using difference_type = std::ptrdiff_t;
                using iterator_category = std::random_access_iterator_tag;

                iterator() = default;

                reference operator*() const { return buf_->data_[(buf_->front_ + idx_) & buf_->mask_]; }
                pointer operator->() const { return &**this; }
                reference operator[](difference_type n) const { return *(*this + n); }

                iterator & operator++() { ++idx_; return *this; }
                iterator operator++(int) { auto t = *this; ++*this; return t; }
                iterator & operator--() { --idx_; return *this; }
                iterator operator--(int) { auto t = *this; --*this; return t; }

                iterator & operator+=(difference_type n) { idx_ += n; return *this; }
                iterator & operator-=(difference_type n) { idx_ -= n; return *this; }
                iterator operator+(difference_type n) const { auto t = *this; return t += n; }
                iterator operator-(difference_type n) const { auto t = *this; return t -= n; }
                friend iterator operator+(difference_type n, iterator i) { return i += n; }
                difference_type operator-(iterator const & o) const {
                    return static_cast<difference_type>(idx_) - static_cast<difference_type>(o.idx_);
                }

                bool operator==(iterator const & o) const { return idx_ == o.idx_; }
                bool operator!=(iterator const & o) const { return idx_ != o.idx_; }
                bool operator<(iterator const & o) const { return idx_ < o.idx_; }
                bool operator>(iterator const & o) const { return idx_ > o.idx_; }
                bool operator<=(iterator const & o) const { return idx_ <= o.idx_; }
                bool operator>=(iterator const & o) const { return idx_ >= o.idx_; }

            private:
                friend class RingBuffer;
                RingBuffer const * buf_ = nullptr;
                size_t idx_ = 0;
                iterator(RingBuffer const * buf, size_t idx) : buf_(buf), idx_(idx) { }
            };

            iterator begin() const { return {this, 0}; }
            iterator end() const { return {this, count_}; }

        private:
            size_t capacity_;
            size_t size_;
            size_t mask_;
            size_t front_ = 0;
            size_t back_ = 0;
            size_t count_ = 0;
            T * data_;

            static T * alloc(size_t n) {
                return static_cast<T *>(::operator new(n * sizeof(T), std::align_val_t(alignof(T))));
            }

            static void dealloc(T * p) {
                if (p) ::operator delete(p, std::align_val_t(alignof(T)));
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

            void grow() {
                size_t new_size = size_ * 2;
                T * newdata = alloc(new_size);
                for (size_t i = 0; i < count_; ++i) {
                    new (&newdata[i]) T{std::move(data_[(front_ + i) & mask_])};
                    data_[(front_ + i) & mask_].~T();
                }
                dealloc(data_);
                data_ = newdata;
                size_ = new_size;
                mask_ = new_size - 1;
                front_ = 0;
                back_ = count_;
            }
        };

    }

}

#endif // INCLUDED__csp__ringbuffer_h
