// Test target: locals, structs, STL containers, recursion, stdout + OutputDebugString,
// a late-loaded DLL, and optional crash / exception modes selected by argv[1].
#include <windows.h>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

struct Vec3 {
    float x, y, z;
};

struct Player {
    std::string name;
    int health;
    Vec3 position;
    std::vector<int> inventory;
    Player* target;
};

static int g_counter = 0;
volatile int g_watchme = 0;

int Factorial(int n) {
    if (n <= 1)
        return 1;  // LINE_FACTORIAL_BASE
    return n * Factorial(n - 1);
}

float Distance2(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;  // LINE_DISTANCE_RETURN
}

void UpdatePlayer(Player& p, int tick) {
    p.health -= tick;
    p.position.x += 1.5f;
    g_counter++;
    g_watchme = tick * 10;  // LINE_WATCH_WRITE
}

void Crash() {
    int* volatile p = nullptr;
    *p = 42;  // LINE_CRASH
}

void ThrowIt() {
    throw std::runtime_error("something bad happened");
}

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "normal";  // LINE_MAIN_FIRST
    printf("basic starting mode=%s\n", mode.c_str());
    fflush(stdout);
    OutputDebugStringA("basic: hello from OutputDebugString\n");

    Player hero{"Hero", 100, {(float)argc, 2.0f, 3.0f}, {1, 2, 3}, nullptr};
    Player villain{"Villain", 80, {10.0f, 0.0f, -5.0f}, {7}, &hero};
    hero.target = &villain;

    std::map<std::string, int> scores{{"alpha", 1}, {"beta", 2}};

    for (int tick = 0; tick < 5; ++tick) {
        UpdatePlayer(hero, tick);  // LINE_LOOP_CALL
        printf("tick %d health %d\n", tick, hero.health);
    }
    fflush(stdout);

    int f = Factorial(5);
    float d = Distance2(hero.position, villain.position);
    printf("factorial=%d dist2=%f scores=%zu\n", f, d, scores.size());
    fflush(stdout);

    if (mode == "dll") {
        HMODULE h = LoadLibraryA("plugin.dll");
        if (h) {
            using Fn = int (*)(int);
            Fn fn = (Fn)GetProcAddress(h, "PluginCompute");
            printf("plugin=%d\n", fn ? fn(7) : -1);
            fflush(stdout);
        }
    } else if (mode == "crash") {
        Crash();
    } else if (mode == "throw") {
        try {
            ThrowIt();
        } catch (const std::exception& e) {
            printf("caught: %s\n", e.what());
        }
        ThrowIt();  // uncaught
    } else if (mode == "wait") {
        // Long-running loop for attach / pause tests.
        for (int i = 0; i < 600; ++i) {
            g_counter++;
            Sleep(100);
        }
    }
    return f == 120 ? 0 : 1;
}
