#ifndef INCLUDED__csp__ringbuffer_h
#define INCLUDED__csp__ringbuffer_h

#include <boost/iterator/iterator_facade.hpp>

#include <cassert>
#include <cstdint>
#include <random>

namespace csp {

    namespace detail {

        void * ring_buffer_alloc(size_t n, size_t el_size, size_t el_align);

        template <typename T>
        class RingBuffer {
        public:
            RingBuffer(size_t capacity = size_t(-1)) : capacity_(capacity) { }
            ~RingBuffer() {
                clear();
            }

            size_t count() const { return count_; }
            bool empty() const { return !count(); }
            bool full() const { return count_ == capacity_; }
            T & front() const { return data_[front_]; }

            void * next() {
                if (count_ == size_) {
                    assert(capacity_ == size_t(-1));
                    // resize
                    size_t newsize = 2 * size_;
                    T * newdata = alloc(newsize);
                    std::move(data_ + front_, data_ + size_, newdata);
                    std::move(data_, data_ + back_, newdata + size_ - front_);
                    front_ = 0;
                    back_ = size_;
                    size_ = newsize;
                    free(data_);
                    data_ = newdata;
                }
                return &data_[back_];
            }

            void push() {
                advance(back_);
                ++count_;
            }

            void push(T t) {
                new (next()) T{std::move(t)};
                push();
            }

            template <typename... AA>
            void emplace(AA &&... a) {
                new (next()) T{std::forward<AA>(a)...};
                push();
            }

            void pop() {
                assert(!empty());
                data_[front_].~T();
                advance(front_);
                --count_;
            }

            bool remove(T t) {
                for (size_t i = 0; i < size_; ++i) {
                    // TODO: Avoid % for efficiency.
                    size_t j = (front_ + i) % size_;
                    if (data_[j] == t) {
                        if (!i) {
                            pop();
                        } else if (i == size_ - 1) {
                            regress(back_);
                            data_[back_].~T();
                            --count_;
                        } else {
                            // Efficient random bits.
                            RandFunc rnd;

                            // Randomly pull an entry from the back or
                            // front to fill the hole.
                            if (rnd()) {
                                data_[j] = std::move(data_[front_]);
                                pop();
                            } else {
                                regress(back_);
                                data_[j] = std::move(data_[back_]);
                                data_[back_].~T();
                                --count_;
                            }
                        }
                        return true;
                    }
                }
                return false;
            }

            void clear() {
                while (!empty()) {
                    pop();
                }
            }

            class iterator : public boost::iterator_facade<iterator, T, boost::random_access_traversal_tag> {
            public:
                iterator(RingBuffer const * buffer, size_t index) : buffer_{buffer}, index_{index} { }

            private:
                RingBuffer const * buffer_;
                size_t index_;

                void increment() { ++index_; }

                void advance(size_t n) { index_ += n; }

                size_t distance_to(iterator const & i) const {
                    assert(buffer_ == i.buffer_);
                    return i.index_ - index_;
                }

                bool equal(iterator const & i) const {
                    return buffer_ == i.buffer_ && index_ == i.index_;
                }

                T & dereference() const { return buffer_->data_[(buffer_->front_ + index_) % buffer_->size_]; }

                friend class boost::iterator_core_access;
            };

            iterator begin() const { return {this, 0}; }
            iterator end() const { return {this, count_}; }

        private:
            size_t capacity_;
            size_t front_ = 0;
            size_t back_ = 0;
            size_t count_ = 0;
            size_t size_ = capacity_ == size_t(-1) ? 4 : capacity_;
            T * data_ = alloc(size_);

            static T * alloc(size_t n) {
                return static_cast<T *>(ring_buffer_alloc(n, sizeof(T), alignof(T)));
            };

            void advance(size_t & i) {
                ++i;
                // branch-free wrap-around
                i *= i < size_;
            }

            void regress(size_t & i) {
                i += size_ * !i;
                --i;
            }

            struct RandFunc {
                std::random_device rd;
                std::default_random_engine rng{rd()};
                std::uniform_int_distribution<uint_fast32_t> ud{1, UINT_FAST32_MAX};
                uint_fast32_t i = 1;

                bool operator()() {
                    if (i == 1) {
                        constexpr int himask = (~uint_fast32_t(0) >> 1) + 1;
                        i = ud(rng) & himask;
                    } else {
                        i >>= 1;
                    }
                    return bool(i & 1);
                }
            };
        };

    }

}

#endif // INCLUDED__csp__ringbuffer_h
