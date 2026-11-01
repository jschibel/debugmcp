// Test target: several named worker threads, two of which deadlock on purpose.
#include <windows.h>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

std::mutex g_a, g_b;
volatile long g_work = 0;

void Worker(int id) {
    for (;;) {
        InterlockedIncrement(&g_work);
        Sleep(10 + id);
    }
}

void DeadlockA() {
    std::lock_guard<std::mutex> la(g_a);
    Sleep(200);
    std::lock_guard<std::mutex> lb(g_b);
}

void DeadlockB() {
    std::lock_guard<std::mutex> lb(g_b);
    Sleep(200);
    std::lock_guard<std::mutex> la(g_a);
}

// "hot" mode: every thread hammers the same small call chain, so temporary breakpoints planted
// by stepping one thread (step-over return addresses, step-out guards) are constantly hit by others.
__declspec(noinline) int Leaf(int x) {
    return x * 3 + 1;
}

__declspec(noinline) int Shared(int x) {
    int a = Leaf(x);  // LINE_SHARED_CALL
    int b = Leaf(a);
    return a + b;
}

void HotWorker() {
    volatile int sink = 0;
    for (;;) sink += Shared(sink & 0xff);
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "hot") == 0) {
        std::vector<std::thread> hot;
        for (int i = 0; i < 4; ++i) hot.emplace_back(HotWorker);
        HotWorker();
    }
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(Worker, i);
        wchar_t name[32];
        swprintf_s(name, L"Worker%d", i);
        SetThreadDescription(threads.back().native_handle(), name);
    }
    std::thread a(DeadlockA), b(DeadlockB);
    SetThreadDescription(a.native_handle(), L"DeadlockA");
    SetThreadDescription(b.native_handle(), L"DeadlockB");
    printf("threads running\n");
    fflush(stdout);
    a.join();
    b.join();
    for (auto& t : threads) t.join();
    return 0;
}
