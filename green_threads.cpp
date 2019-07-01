#include <assert.h>
#include <deque>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STACK_SIZE (1048576)
#define DEFAULT_THREADS 4

enum class ThreadState {
    Dead,
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
    // The system thread is special in that it does not use the stack from
    // malloc; it uses the system-provided stack. There is exactly one system
    // thread. It is distinguished by having a null pointer as its stack here.
    Thread() : Thread(false) {}
    explicit Thread(bool is_system_thread)
      : stack(nullptr), state(ThreadState::Dead) {
        if (!is_system_thread) {
            stack = malloc(STACK_SIZE);
            if (!stack) {
                fputs("Could not allocate memory for new thread.\n", stderr);
                exit(1);
            }
        }
    }
    ~Thread() { free(stack); }
    Thread(Thread const&) = delete;
    Thread& operator=(Thread const&) = delete;
    Thread(Thread&& th) : stack(th.stack), ctx(th.ctx), state(th.state) {
        th.stack = nullptr;
        th.state = ThreadState::Dead;
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
    // that basically only ever yields and exits at the end.
    std::deque<Thread> threads;

    // The currently running thread.
    Thread current_thread;

    // Return from a green thread. When a green thread finishes, this will be
    // called.
    void ret() {
        assert(current_thread.stack);
        // The system thread (without its special stack) is running the loop. It
        // cannot return.
        current_thread.state = ThreadState::Dead;
        do_yield();
    }

    Runtime() : threads(), current_thread(true) {
        // Create initial system thread
        current_thread.state = ThreadState::Running;
    }

    Runtime(Runtime const&) = delete;
    Runtime& operator=(Runtime const&) = delete;

    static Runtime& instance() {
        static Runtime rt;
        return rt;
    }

    // Yield from the current thread. If there is a different thread ready to be
    // run, switch to that thread; when the originally thread has been switched
    // back, return true. If there is not a different thread to switch to,
    // return false.
    bool do_yield() {
        assert(current_thread.state == ThreadState::Running ||
               current_thread.state == ThreadState::Dead);

        // Find the first thread that is ready to run.
        while (!threads.empty()) {
            if (threads.front().state == ThreadState::Dead) {
                threads.pop_front();
            } else if (threads.front().state == ThreadState::Running) {
                assert(false); // This should not happen. No thread in this
                               // queue is running.
            } else if (threads.front().state == ThreadState::Ready) {
                break;
            }
        }

        if (threads.empty()) {
            // No other thread to yield to. Resume running the current thread.
            return false;
        } else {
            // Yield to a new and different thread at the front of the queue.
            if (current_thread.state != ThreadState::Dead) {
                current_thread.state = ThreadState::Ready;
            }
            threads.emplace_back(std::move(current_thread));
            ThreadContext& old_ctx = threads.back().ctx;
            current_thread = std::move(threads.front());
            threads.pop_front();
            current_thread.state = ThreadState::Running;
            ThreadContext& new_ctx = current_thread.ctx;
            ThreadContext::switch_to(old_ctx, new_ctx);
            return true;
        }
    }
    void do_spawn(void (*func)()) {
        Thread new_thread;

        // Place the address of exit_thread on top of the stack, so that if the
        // green thread function returns, it returns into that function.
        void (*guard)() = Runtime::exit_thread;
        memcpy((unsigned char*) new_thread.stack + STACK_SIZE - 8, &guard, 8);
        memcpy((unsigned char*) new_thread.stack + STACK_SIZE - 16, &func, 8);

        // The rsp of a new thread points to the address of the function we want
        // to run, because the first real instruction after switching stack and
        // state is "ret".
        new_thread.ctx.rsp =
          (uint64_t)((unsigned char*) new_thread.stack + STACK_SIZE - 16);
        new_thread.state = ThreadState::Ready;
        threads.emplace_back(std::move(new_thread));
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
    static void exit_thread() { return instance().ret(); }
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
