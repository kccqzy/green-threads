#include <assert.h>
#include <deque>
#include <functional>
#include <inttypes.h>
#include <memory>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// Naked functions are only supported since GCC 8. clang seems to support it
// since eons ago.
#ifdef __GNUC__
#ifndef __clang__
#if __GNUC__ < 8
#error "Please upgrade to GCC 8 or newer."
#endif
#endif
#else
#warning "Unsupported compiler. YMMV"
#endif

#define STACK_SIZE (8192)
#define STACK_MAX_SIZE (1024 * STACK_SIZE)
#define STACK_ALLOC_SIZE (2 * STACK_MAX_SIZE)
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

struct ThreadStack {
    void* stack;
    operator void*() const { return stack; }
    operator unsigned char*() const { return (unsigned char*) stack; }
    operator char*() const { return (char*) stack; }
    operator bool() const { return stack; }
    ThreadStack() : stack() {
        size_t guard_size = STACK_ALLOC_SIZE - STACK_SIZE;
        stack =
          mmap(0, STACK_ALLOC_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (stack == MAP_FAILED) {
            fputs(
              "Could not allocate virtual address space for new thread "
              "stack.\n",
              stderr);
            exit(1);
        }
        if (mprotect((unsigned char*) stack + guard_size,
                     STACK_ALLOC_SIZE - guard_size,
                     PROT_READ | PROT_WRITE) == -1) {
            fputs("Could not set protection on stack.\n", stderr);
            exit(1);
        }
    }
    explicit ThreadStack(void* stack) : stack(stack) {}
    ~ThreadStack() {
        if (stack) munmap(stack, 2 * STACK_MAX_SIZE);
    }
    ThreadStack(ThreadStack const&) = delete;
    ThreadStack& operator=(ThreadStack const&) = delete;
    ThreadStack(ThreadStack&& ts) : stack(ts.stack) { ts.stack = nullptr; }
    ThreadStack& operator=(ThreadStack&& th) {
        if (stack) munmap(stack, 2 * STACK_MAX_SIZE);
        stack = th.stack;
        th.stack = nullptr;
        return *this;
    }
};

struct Thread {
    ThreadStack stack;
    ThreadContext ctx;
    ThreadState state;
    std::unique_ptr<std::function<void()>> callable;
    // The system thread is special in that it does not use the stack from
    // malloc; it uses the system-provided stack. There is exactly one system
    // thread. It is distinguished by having a null pointer as its stack here.
    Thread() : Thread(false) {}
    explicit Thread(bool is_system_thread)
      : stack(nullptr), state(ThreadState::Dead) {
        if (!is_system_thread) { stack = ThreadStack(); }
    }
    Thread(Thread const&) = delete;
    Thread& operator=(Thread const&) = delete;
    Thread(Thread&& th) = default;
    Thread& operator=(Thread&&) = default;
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
            assert(current_thread.state != ThreadState::Dead);
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
        memcpy((unsigned char*) new_thread.stack + STACK_ALLOC_SIZE - 8, &guard,
               8);
        memcpy((unsigned char*) new_thread.stack + STACK_ALLOC_SIZE - 16, &func,
               8);

        // The rsp of a new thread points to the address of the function we want
        // to run, because the first real instruction after switching stack and
        // state is "ret".
        new_thread.ctx.rsp =
          (uint64_t)((unsigned char*) new_thread.stack + STACK_ALLOC_SIZE - 16);
        new_thread.state = ThreadState::Ready;
        threads.emplace_back(std::move(new_thread));
    }

    void do_spawn_callable(std::function<void()>&& func) {
        assert(func);
        do_spawn([]() {
            assert(instance().current_thread.callable);
            (*instance().current_thread.callable)();
        });
        threads.back().callable = std::make_unique<std::function<void()>>(
          std::forward<std::function<void()>>(func));
        assert(threads.back().callable);
        assert(*threads.back().callable);
    }

    [[noreturn]] void do_run() {
        setup_growable_stack();
        while (do_yield())
            ;
        exit(0);
    }

    ptrdiff_t do_get_current_stack_usage() const {
        unsigned char* rsp;
        __asm__ volatile("movq %%rsp, %0" : "=r"(rsp));
        if (current_thread.stack) {
            return (unsigned char*) current_thread.stack + STACK_ALLOC_SIZE -
                   rsp;
        } else {
            return -1;
        }
    }

    // Grow a green thread stack in response to a segfault.
    void grow_stack(void* fault_addr) const {
        // Round down to page
        fault_addr = (void*) ((uint64_t) fault_addr & ~0xfffull);

        if (fault_addr >=
              (unsigned char*) current_thread.stack + STACK_MAX_SIZE &&
            fault_addr <
              (unsigned char*) current_thread.stack + STACK_ALLOC_SIZE) {
            if (mprotect(fault_addr,
                         (unsigned char*) current_thread.stack +
                           STACK_ALLOC_SIZE - (unsigned char*) fault_addr,
                         PROT_READ | PROT_WRITE)) {
                const char msg[] = "No more memory for thread stacks.\n";
                write(2, msg, sizeof msg - 1);
                _exit(5);
            }
        } else {
            const char msg[] = "Unexpected segfault.\n";
            write(2, msg, sizeof msg - 1);
            _exit(2);
        }
    }

    void setup_growable_stack() {
        static char sigstack[81920];
        stack_t st = {};
        st.ss_sp = sigstack;
        st.ss_size = sizeof sigstack;
        if (sigaltstack(&st, NULL) == -1) {
            perror("sigaltstack");
            exit(1);
        }

        struct sigaction sa = {};
        sa.sa_flags = SA_ONSTACK | SA_SIGINFO;
        sa.sa_sigaction = [](int sig, siginfo_t* info, void* ucontext) {
            (void) sig;
            (void) ucontext;
            void* fault_addr = info->si_addr;
            instance().grow_stack(fault_addr);
        };

        if (sigaction(SIGSEGV, &sa, NULL) == -1 ||
            sigaction(SIGBUS, &sa, NULL) == -1) {
            perror("sigaction");
            exit(1);
        }
    }

public:
    static bool yield() { return instance().do_yield(); }
    static void spawn(void (*func)()) { return instance().do_spawn(func); }
    static void spawn_callable(std::function<void()>&& func) {
        return instance().do_spawn_callable(
          std::forward<std::function<void()>>(func));
    }
    static void run_forever() { return instance().do_run(); }
    static void exit_thread() { return instance().ret(); }
    static ptrdiff_t get_current_stack_usage() {
        return instance().do_get_current_stack_usage();
    }
};

static void func1() {
    puts("func 1 started");
    for (int i = 0; i < 10; ++i) {
        printf("func 1 counting %d\n", i);
        Runtime::yield();
    }
}

static void func2() {
    puts("func 2 started");
    for (int i = 0; i < 10; ++i) {
        printf("func 2 counting %d\n", i);
        Runtime::yield();
        printf("func 2 current stack usage: %ld\n",
               Runtime::get_current_stack_usage());
    }
    puts("func 2 finished counting; spawning func 1 again");
    Runtime::spawn(func1);
}

static uint64_t lots_of_stack(uint64_t m, uint64_t n) {
    printf("Current stack usage: %ld\n", Runtime::get_current_stack_usage());
    if (m == 0)
        return n + 1;
    else if (n == 0)
        return lots_of_stack(m - 1, 1);
    Runtime::yield();
    uint64_t rv = lots_of_stack(m - 1, lots_of_stack(m, n - 1));
    printf("ackermann(%" PRIu64 ", %" PRIu64 ") = %" PRIu64 "\n", m, n, rv);
    return rv;
}

int main() {
    volatile uint64_t ack35 = 0;
    Runtime::spawn(func1);
    Runtime::spawn(func2);
    Runtime::spawn(
      []() { printf("final result = %" PRIu64 "\n", lots_of_stack(3, 6)); });
    Runtime::spawn_callable([&ack35]() { ack35 = lots_of_stack(3, 5); });
    Runtime::spawn_callable([&ack35]() {
        while (!ack35) {
            puts("other thread hasn't finished calculating ack(3,5) yet");
            Runtime::yield();
        }
        printf("Finished ack(3,5) = %" PRIu64 "\n", ack35);
    });
    Runtime::run_forever();
}
