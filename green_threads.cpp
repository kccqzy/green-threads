#include <algorithm>
#include <deque>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STACK_SIZE (8192)
#define DEFAULT_THREADS 4

enum class ThreadState {
    Available,
    Ready,
    Running,
};

struct ThreadContext {
    uint64_t rsp, r15, r14, r13, r12, rbx, rbp;
    ThreadContext() : rsp(), r15(), r14(), r13(), r12(), rbx(), rbp() {}
    static __attribute__((noinline)) __attribute__((naked)) void
    switch_to(ThreadContext&, ThreadContext&) {
        __asm__ volatile(
          "movq %rsp, (%rdi)\n\t"
          "movq %r15, 0x8(%rdi)\n\t"
          "movq %r14, 0x10(%rdi)\n\t"
          "movq %r13, 0x18(%rdi)\n\t"
          "movq %r12, 0x20(%rdi)\n\t"
          "movq %rbx, 0x28(%rdi)\n\t"
          "movq %rbp, 0x30(%rdi)\n\t"

          "movq (%rsi), %rsp\n\t"
          "movq 0x8(%rsi), %r15\n\t"
          "movq 0x10(%rsi), %r14\n\t"
          "movq 0x18(%rsi), %r13\n\t"
          "movq 0x20(%rsi), %r12\n\t"
          "movq 0x28(%rsi), %rbx\n\t"
          "movq 0x30(%rsi), %rbp\n\t"

          "ret");
    }
};
static_assert(offsetof(ThreadContext, rsp) == 0, "unexpected offset");
static_assert(offsetof(ThreadContext, r15) == 0x8, "unexpected offset");
static_assert(offsetof(ThreadContext, r14) == 0x10, "unexpected offset");
static_assert(offsetof(ThreadContext, r13) == 0x18, "unexpected offset");
static_assert(offsetof(ThreadContext, r12) == 0x20, "unexpected offset");
static_assert(offsetof(ThreadContext, rbx) == 0x28, "unexpected offset");
static_assert(offsetof(ThreadContext, rbp) == 0x30, "unexpected offset");

struct Thread {
    void* stack;
    ThreadContext ctx;
    ThreadState state;
    bool is_system_thread;
    Thread() : stack(), state(ThreadState::Available), is_system_thread(false) {
        stack = malloc(STACK_SIZE);
        if (!stack) {
            fputs("Could not allocate memory for new thread.\n", stderr);
            exit(1);
        }
    }
    ~Thread() { free(stack); }
    Thread(Thread const&) = delete;
    Thread& operator=(Thread const&) = delete;
    Thread(Thread&& th) : stack(th.stack), ctx(th.ctx), state(th.state) {
        th.stack = nullptr;
    }
    Thread& operator=(Thread&& th) {
        free(stack);
        stack = th.stack;
        ctx = th.ctx;
        state = th.state;
        th.stack = nullptr;
        return *this;
    }
};

class Runtime {
private:
    // A round-robin queue of threads to be scheduled. There is a system thread
    // used for running the scheduler itself. INVARIANT: The current thread is
    // always the last one in the queue.
    std::deque<Thread> threads;
    Thread* current_thread;
    void ret() {
        assert(!current_thread->is_system_thread);
        // The system thread is running the loop. It cannot return.
        current_thread->state = ThreadState::Available;
        do_yield();
    }
    static void finish() { Runtime::instance().ret(); }
    Runtime() : threads() {
        // Create initial system thread
        threads.resize(1);
        threads.front().state = ThreadState::Running;
        threads.front().is_system_thread = true;
        current_thread = &threads.front();
    }
    Runtime(Runtime const&) = delete;
    Runtime& operator=(Runtime const&) = delete;

    static Runtime& instance() {
        static Runtime rt;
        return rt;
    }
    bool do_yield() {
        // If the current thread is not dead, set it to be ready.
        if (current_thread->state != ThreadState::Available)
            current_thread->state = ThreadState::Ready;

        auto old_current_thread = std::move(threads.back());
        threads.pop_back();

        // Find the first thread that is ready to run.
        while (!threads.empty()) {
            if (threads.front().state == ThreadState::Available) {
                threads.pop_front();
            } else if (threads.front().state == ThreadState::Running) {
                assert(false); // This should not happen. No thread is running.
            } else if (threads.front().state == ThreadState::Ready) {
                break;
            }
        }

        if (threads.empty()) {
            // No other thread to yield to. Resume running the current thread.
            threads.emplace_back(std::move(old_current_thread));
            threads.back().state = ThreadState::Running;
            return false;
        } else {
            // Yield to a new and different thread.
            threads.emplace_back(std::move(old_current_thread));
            ThreadContext& old_ctx = threads.back().ctx;
            auto next_thread = std::move(threads.front());
            threads.pop_front();
            next_thread.state = ThreadState::Running;
            threads.emplace_back(std::move(next_thread));
            current_thread = &threads.back();
            ThreadContext& new_ctx = threads.back().ctx;
            ThreadContext::switch_to(old_ctx, new_ctx);
            return true;
        }
    }
    void do_spawn(void (*func)()) {
        Thread new_thread;
        void (*guard)() = Runtime::finish;
        memcpy((unsigned char*) new_thread.stack + STACK_SIZE - 8, &guard, 8);
        memcpy((unsigned char*) new_thread.stack + STACK_SIZE - 16, &func, 8);
        new_thread.ctx.rsp =
          (uint64_t)((unsigned char*) new_thread.stack + STACK_SIZE - 16);
        new_thread.state = ThreadState::Ready;
        threads.emplace_front(std::move(new_thread));
    }
    [[noreturn]] void do_run() {
        while (do_yield())
            ;
        exit(0);
    }

public:
    static bool yield() { return instance().do_yield(); }
    static void spawn(void (*func)()) { return instance().do_spawn(func); }
    static void run_forever() { return instance().do_run(); }
};

static void thread1() {
    puts("thread 1 started");
    for (int i = 0; i < 10; ++i) {
        printf("thread 1 counting %d\n", i);
        Runtime::yield();
    }
}

static void thread2() {
    puts("thread 2 started");
    for (int i = 0; i < 10; ++i) {
        printf("thread 2 counting %d\n", i);
        Runtime::yield();
    }
    puts("thread 2 finished counting; calling thread1 again");
    Runtime::spawn(thread1);
}

int main() {
    Runtime::spawn(thread1);
    Runtime::spawn(thread2);
    Runtime::run_forever();
}
