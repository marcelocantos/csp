#ifndef INCLUDED__csp__microthread_internal_h
#define INCLUDED__csp__microthread_internal_h

#include <csp/microthread.h>
#include <csp/fcontext.h>

#include <condition_variable>
#include <cstdlib>
#include <cstddef>
#include <mutex>
#include <thread>

using namespace csp;

namespace csp {

    namespace detail {

        enum class Status : intptr_t { run, sleep, detach, exit, spawn };

        struct Microthread;

        extern Microthread * g_self;
        extern Microthread * g_busy;

        void do_switch(Status status = Status::sleep);

        struct alignas(16) Microthread {
            struct alignas(16) StackSlot { char c[16]; };

            static constexpr size_t stack_size = 32 << 10;

            Microthread * prev_;
            Microthread * next_;
            fcontext_t ctx_;
            StackSlot * stk_;
            char status_[32];
            csp_chanop const * chanops_;
            int n_chanops_, signal_;

            size_t id_ = []{
                static size_t next_ = 0;
                return next_++;
            }();

            std::thread os_thread_;
            bool run_ = false;
            intptr_t data_;
            std::condition_variable switch_;
            std::mutex mutex_;

            Microthread(fcontext_t ctx, StackSlot * stk);
            Microthread();
            Microthread(Microthread const &) = delete;

            Microthread & operator=(Microthread const &) = delete;

            char const * getfullstatus_() const {
                return status_;
            }

            void schedule(bool make_current = false);
            void deschedule();

            void run(Status status = Status::sleep);

            void notify(intptr_t data) {
                std::lock_guard<std::mutex> lk(mutex_);
                run_ = true;
                data_ = data;
                switch_.notify_one();
            }

            intptr_t wait() {
                std::unique_lock<std::mutex> lk(mutex_);
                while (!run_) {
                    switch_.wait(lk);
                }
                run_ = false;
                return data_;
            }

            void unwait(csp_chanop const * signalled, bool ready);
        };

        inline
        char const * getfullstatus(Microthread const * mt) {
            return mt ? mt->getfullstatus_() : "Ø";
        }

        inline
        char const * getstatus(Microthread const * mt) {
            return getfullstatus(mt);
        }

    }

}

#endif // INCLUDED__csp__microthread_internal_h
