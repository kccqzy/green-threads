#include <algorithm>
#include <array>
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
    size_t id;
    void* stack;
    ThreadContext ctx;
    ThreadState state;
    Thread() : id(0), stack(), state(ThreadState::Available) {
        stack = malloc(STACK_SIZE);
        if (!stack) {
            fputs("Could not allocate memory for new thread.\n", stderr);
            exit(1);
        }
    }
    ~Thread() { free(stack); }
    Thread(Thread const&) = delete;
    Thread& operator=(Thread const&) = delete;
    Thread(Thread&& th)
      : id(th.id), stack(th.stack), ctx(th.ctx), state(th.state) {
        th.stack = nullptr;
    }
    Thread& operator=(Thread&& th) {
        free(stack);
        id = th.id;
        stack = th.stack;
        ctx = th.ctx;
        state = th.state;
        th.stack = nullptr;
        return *this;
    }
};

class Runtime {
private:
    size_t current;
    std::array<Thread, 1 + DEFAULT_THREADS> threads;
    void ret() {
        if (current) {
            threads[current].state = ThreadState::Available;
            do_yield();
        }
    }
    static void finish() { Runtime::instance().ret(); }
    Runtime() : current(0), threads() {
        threads.front().state = ThreadState::Running;
        for (size_t i = 1; i <= DEFAULT_THREADS; ++i) { threads[i].id = i; }
    }
    Runtime(Runtime const&) = delete;
    Runtime& operator=(Runtime const&) = delete;

    static Runtime& instance() {
        static Runtime rt;
        return rt;
    }
    bool do_yield() {
        auto next_ready = std::find_if(
          threads.begin() + current, threads.end(),
          [](Thread const& th) { return th.state == ThreadState::Ready; });
        if (next_ready == threads.end()) {
            next_ready = std::find_if(
              threads.begin(), threads.begin() + current,
              [](Thread const& th) { return th.state == ThreadState::Ready; });
        }
        if (next_ready == threads.begin() + current) { return false; }

        if (threads[current].state != ThreadState::Available)
            threads[current].state = ThreadState::Ready;

        next_ready->state = ThreadState::Running;
        size_t old = current;
        current = next_ready - threads.begin();

        ThreadContext::switch_to(threads[old].ctx, next_ready->ctx);
        return true;
    }
    void do_spawn(void (*func)()) {
        auto avail =
          std::find_if(threads.begin(), threads.end(), [](Thread const& th) {
              return th.state == ThreadState::Available;
          });
        if (avail == threads.end()) {
            fputs("Cannot spawn task. No available threads.\n", stderr);
            exit(2);
        }
        void (*guard)() = Runtime::finish;
        memcpy((unsigned char*) avail->stack + STACK_SIZE - 8, &guard, 8);
        memcpy((unsigned char*) avail->stack + STACK_SIZE - 16, &func, 8);
        avail->ctx.rsp =
          (uint64_t)((unsigned char*) avail->stack + STACK_SIZE - 16);
        avail->state = ThreadState::Ready;
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
    puts("thread 2 finished counting; calling thread1");
    Runtime::spawn(thread1);
}

int main() {
    Runtime::spawn(thread1);
    Runtime::spawn(thread2);
    Runtime::run_forever();
}
