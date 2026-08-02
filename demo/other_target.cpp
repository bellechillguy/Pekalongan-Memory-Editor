#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static std::uint32_t currentPid() { return GetCurrentProcessId(); }
#else
#include <unistd.h>
static std::uint32_t currentPid() { return static_cast<std::uint32_t>(getpid()); }
#endif

struct TargetState {
    std::int32_t health;
    std::int32_t credits;
    std::int32_t magic;
    std::int32_t decoys[64];
};

// Pointer global volatile mencegah optimizer menghapus state dan decoy. Target
// ini memang disediakan agar bisa dipindai oleh process lain.
volatile TargetState* gState = nullptr;

int main() {
    gState = new TargetState();
    volatile TargetState* state = gState;
    state->health = 150;
    state->credits = 25;
    state->magic = 42'424'242;
    for (std::size_t index = 0; index < 64; ++index) {
        state->decoys[index] = index % 3 == 0 ? 150 : 25;
    }

    std::cout << "OTHER TARGET // PID " << currentPid() << '\n'
              << "Health turun dan credits naik setiap 5 detik. Ubah magic "
                 "42424242 menjadi 1337 untuk menyelesaikan integration test."
              << std::endl;

    int tick = 0;
    while (state->magic != 1337) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        ++tick;
        if (tick % 10 == 0) {
            state->health = std::max<std::int32_t>(0, state->health - 5);
            state->credits = state->credits + 10;
            std::cout << "HP " << state->health << " | credits "
                      << state->credits << std::endl;
        }
    }
    std::cout << "OTHER TARGET CHEATED // magic = 1337" << std::endl;
    return 0;
}
