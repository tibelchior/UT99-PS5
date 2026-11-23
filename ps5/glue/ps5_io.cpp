// The console's controller and sound for the title.
//  - Controller: scePad is polled on a thread and mirrored into an SDL virtual gamepad, which the
//    game opens like any joystick (it takes the first one SDL lists).
//  - Sound: SDL's dummy audio driver (patch 0004) hands every mixed block to ut_ps5_audio_play,
//    which writes it to the console's main audio output (48 kHz, 16-bit stereo).
// No SDK header declares these calls; the declarations and the pad sample layout are the ones
// ps5_console.cpp (DiRT2-PS5) and mihawk-99/PS5_VulkanTemplate run with.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <string>

#include <pthread.h>
#include <unistd.h>

#include <SDL.h>

extern "C" {
int sceSystemServiceHideSplashScreen(void);
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *user_id);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t port_type, int32_t index, const void *params);
int scePadGetHandle(int32_t user_id, int32_t port_type, int32_t index);
int scePadRead(int32_t handle, void *samples, int32_t capacity);
int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t grain, uint32_t rate, uint32_t format);
int sceAudioOutOutput(int32_t port, const void *samples);
int sceKernelUsleep(uint32_t microseconds);
const char *ut_ps5_data_dir(void);
}

namespace {


// Everything goes to ut99.log while the port is brought up on hardware (create data/quiet to keep problems only).
bool log_wanted(const char *text) {
    static const bool quiet = std::getenv("UT99_QUIET") != nullptr;
    if (!quiet) return true;
    for (const char *word : {"fail", "no ", "missing", "disabled", "error", "software renderer"})
        if (std::strstr(text, word)) return true;
    return false;
}

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (!log_wanted(line)) return;
    std::fprintf(stderr, "[ps5-io] %s\n", line);
}

// ---- sound
constexpr int kGrain = 256;
constexpr int kAudioAlreadyInitialised = int(0x8026000e);
int32_t g_port = -1;
int g_peak = 0, g_blocks = 0, g_out_errors = 0;
bool g_audio_failed = false;

bool audio_open() {
    if (g_port >= 0) return true;
    if (g_audio_failed) return false;
    const int init = sceAudioOutInit();
    if (init < 0 && init != kAudioAlreadyInitialised) {
        note("sceAudioOutInit failed: 0x%08x", unsigned(init));
        g_audio_failed = true;
        return false;
    }
    const int32_t port = sceAudioOutOpen(0xff, 0, 0, kGrain, 48000, 1);  // system user's main port, S16 stereo
    if (port < 0) {
        note("sceAudioOutOpen failed: 0x%08x", unsigned(port));
        g_audio_failed = true;
        return false;
    }
    g_port = port;
    note("sound output open: 48 kHz stereo, port %d", int(port));
    return true;
}

// ---- controller
struct Sample {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y, l2, r2;
    uint8_t reserved[66];
    int32_t connected;
    uint64_t timestamp_us;
    uint8_t extension[16];
    uint8_t connected_count;
    uint8_t remaining[15];
};
static_assert(sizeof(Sample) == 120, "the console's pad samples are 120 bytes");
static_assert(offsetof(Sample, connected) == 0x4c, "connection state sits at 0x4c");
constexpr uint32_t kIntercepted = 0x80000000u;

enum : uint32_t {
    L3 = 0x2, R3 = 0x4, OPTIONS = 0x8, UP = 0x10, RIGHT = 0x20, DOWN = 0x40, LEFT = 0x80,
    L1 = 0x400, R1 = 0x800, TRIANGLE = 0x1000, CIRCLE = 0x2000, CROSS = 0x4000, SQUARE = 0x8000,
    TOUCHPAD = 0x100000,
};

SDL_Joystick *g_virtual = nullptr;

int16_t stick(uint8_t v) {
    const int s = (int(v) - 128) * 256;
    return int16_t(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

void *pad_main(void *) {
    (void)sceUserServiceInitialize(nullptr);
    if (scePadInit() < 0) {
        note("scePadInit failed");
        return nullptr;
    }
    int32_t handle = -1;
    static Sample samples[16];
    uint32_t last_buttons = ~0u;
    bool was_connected = false;
    unsigned tick = 0;
    int r2_peak = 0, l2_peak = 0;
    time_t window = time(nullptr);
    for (;; sceKernelUsleep(4000)) {
        if (handle < 0) {
            if (tick++ % 250 != 0) continue;  // retry about once a second
            int32_t user = -1;
            if (sceUserServiceGetInitialUser(&user) < 0) continue;
            handle = scePadOpen(user, 0, 0, nullptr);
            if (handle < 0) handle = scePadGetHandle(user, 0, 0);
            if (handle >= 0) note("controller opened for user %d", int(user));
            continue;
        }
        const int count = scePadRead(handle, samples, 16);
        if (count <= 0) continue;
        const Sample &s = samples[count - 1];
        if (s.r2 > r2_peak) r2_peak = s.r2;
        if (s.l2 > l2_peak) l2_peak = s.l2;
        if (time(nullptr) - window >= 5) {  // peak trigger values of the last 5 s, when any was used
            r2_peak = l2_peak = 0;
            window = time(nullptr);
        }
        const bool connected = s.connected != 0 && !(s.buttons & kIntercepted);
        if (connected != was_connected) {
            was_connected = connected;
            note("controller %s", connected ? "connected" : "disconnected / taken by the system");
        }
        if (!g_virtual) continue;
        const uint32_t b = connected ? s.buttons : 0;
        const auto btn = [&](int index, bool down) { SDL_JoystickSetVirtualButton(g_virtual, index, down ? 1 : 0); };
        btn(SDL_CONTROLLER_BUTTON_A, b & CROSS);
        btn(SDL_CONTROLLER_BUTTON_B, b & CIRCLE);
        btn(SDL_CONTROLLER_BUTTON_X, b & SQUARE);
        btn(SDL_CONTROLLER_BUTTON_Y, b & TRIANGLE);
        btn(SDL_CONTROLLER_BUTTON_BACK, b & TOUCHPAD);
        btn(SDL_CONTROLLER_BUTTON_START, b & OPTIONS);
        btn(SDL_CONTROLLER_BUTTON_LEFTSTICK, b & L3);
        btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK, b & R3);
        btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, b & L1);
        btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, b & R1);
        btn(SDL_CONTROLLER_BUTTON_DPAD_UP, b & UP);
        btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN, b & DOWN);
        btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT, b & LEFT);
        btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, b & RIGHT);
        const auto axis = [&](int index, int16_t v) { SDL_JoystickSetVirtualAxis(g_virtual, index, v); };
        axis(SDL_CONTROLLER_AXIS_LEFTX, connected ? stick(s.left_x) : 0);
        axis(SDL_CONTROLLER_AXIS_LEFTY, connected ? stick(s.left_y) : 0);
        axis(SDL_CONTROLLER_AXIS_RIGHTX, connected ? stick(s.right_x) : 0);
        axis(SDL_CONTROLLER_AXIS_RIGHTY, connected ? stick(s.right_y) : 0);
        // Triggers rest at the bottom of the axis range (a released trigger is -32768).
        axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, connected ? int16_t(int(s.l2) * 65535 / 255 - 32768) : -32768);
        axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, connected ? int16_t(int(s.r2) * 65535 / 255 - 32768) : -32768);
        if (b != last_buttons) last_buttons = b;
    }
    return nullptr;
}

}  // namespace

// One line in the log, for the Vulkan scene renderer.
extern "C" void ut_ps5_log(const char *text) { note("%s", text); }

// Called by SDL's dummy audio driver for each mixed block; blocks at the console's pace.
extern "C" void ut_ps5_audio_play(const void *frames, int count) {
    if (!audio_open()) {
        sceKernelUsleep(uint32_t(int64_t(count) * 1000000 / 48000));  // keep the game's clock moving
        return;
    }
    alignas(64) static int16_t block[kGrain * 2];
    const int16_t *src = static_cast<const int16_t *>(frames);
    for (int done = 0; done < count; done += kGrain) {
        const int n = count - done < kGrain ? count - done : kGrain;
        std::memcpy(block, src + done * 2, size_t(n) * 4);
        if (n < kGrain) std::memset(block + n * 2, 0, size_t(kGrain - n) * 4);
        for (int i = 0; i < kGrain * 2; i++) {
            const int v = block[i] < 0 ? -block[i] : block[i];
            if (v > g_peak) g_peak = v;
        }
        const int rc = sceAudioOutOutput(g_port, block);
        if (rc < 0) {
            if (g_out_errors++ < 5) note("sceAudioOutOutput failed: 0x%08x", unsigned(rc));
            sceKernelUsleep(5000);
        }
        ++g_blocks;
    }
}

// The shell keeps the launch background over the title's frames until this is called.
extern "C" void ut_ps5_hide_splash(void) {
    static bool done = false;
    if (done) return;
    done = true;
    note("splash dismissed: %d", sceSystemServiceHideSplashScreen());
}

// Called before the game starts: creates the virtual gamepad and the thread that feeds it.
extern "C" void ut_ps5_input_start(void) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0) {
        note("SDL joystick init failed: %s", SDL_GetError());
        return;
    }
    const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
    if (index < 0) {
        note("virtual joystick failed: %s", SDL_GetError());
        return;
    }
    g_virtual = SDL_JoystickOpen(index);
    if (g_virtual) {  // triggers rest at the bottom of the axis range
        SDL_JoystickSetVirtualAxis(g_virtual, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
        SDL_JoystickSetVirtualAxis(g_virtual, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
    }
    note("virtual gamepad attached (index %d, %s)", index, g_virtual ? "opened" : "not opened");
    pthread_t thread;
    if (pthread_create(&thread, nullptr, pad_main, nullptr) != 0) note("no thread for the controller");
}
