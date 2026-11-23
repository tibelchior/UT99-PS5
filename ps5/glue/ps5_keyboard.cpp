// The on-screen keyboard. The console has no text input for a title, and the game's menus only know keyboard
// text, so a name or an address could not be typed. Like the Motorhead port, this keyboard is drawn by the port
// over the finished frame (ut_ps5_present calls ut_ps5_kb_draw) and driven by the pad:
//   the engine opens it (ut_ps5_kb_open) when a text field is clicked, feeds it pad events (ut_ps5_kb_button /
//   ut_ps5_kb_axis) while it is open, and types the result into the field when it closes with OK.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "ps5_kbfont.h"

namespace {

constexpr const char *kLower[4] = {"1234567890", "qwertyuiop", "asdfghjkl_", "zxcvbnm.-:"};
constexpr const char *kUpper[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL_", "ZXCVBNM.-:"};
constexpr int kRows = 4, kCols = 10, kCommands = 5;  // SHIFT, SPACE, DEL, OK, CANCEL
constexpr const char *kCommandNames[kCommands] = {"SHIFT", "SPACE", "DEL", "OK", "CANCEL"};

// SDL's game controller button numbers (SDL_CONTROLLER_BUTTON_*) and axes: the values do not change.
enum { kBtnA = 0, kBtnB = 1, kBtnX = 2, kBtnY = 3, kBtnBack = 4, kBtnStart = 6, kBtnLeftShoulder = 9, kBtnRightShoulder = 10,
       kBtnDpadUp = 11, kBtnDpadDown = 12, kBtnDpadLeft = 13, kBtnDpadRight = 14 };
enum { kAxisLeftX = 0, kAxisLeftY = 1 };

struct Keyboard {
    bool active = false, upper = true, done = false, confirmed = false;
    std::string text;
    size_t max = 32;
    int row = 0, col = 0;
    int held_x = 0, held_y = 0;  // stick directions already acted on
} g;

int row_width(int row) { return row < kRows ? kCols : kCommands; }

void move(int dx, int dy) {
    const int rows = kRows + 1;
    if (dy) {
        const int from = g.row;
        g.row = (g.row + dy + rows) % rows;
        if (g.row == kRows) g.col = g.col * kCommands / kCols;
        else if (from == kRows) g.col = std::min(kCols - 1, g.col * kCols / kCommands + 1);
    }
    if (dx) g.col = (g.col + dx + row_width(g.row)) % row_width(g.row);
}

void type(char c) {
    if (g.text.size() >= g.max) return;
    const bool first = g.text.empty();
    g.text += c;
    if (first && g.upper) g.upper = false;  // a name starts with a capital, then goes on in lower case
}

void erase() {
    if (!g.text.empty()) g.text.pop_back();
}

void press() {
    if (g.row < kRows) {
        type((g.upper ? kUpper : kLower)[g.row][g.col]);
        return;
    }
    switch (g.col) {
        case 0: g.upper = !g.upper; break;
        case 1: type(' '); break;
        case 2: erase(); break;
        case 3: g.done = g.confirmed = true; break;
        default: g.done = true; break;
    }
}

std::string g_result;
bool g_have_result = false;

}  // namespace

extern "C" int ut_ps5_kb_active(void) { return g.active ? 1 : 0; }

extern "C" void ut_ps5_kb_open(const char *initial, unsigned max_chars) {
    g.text = initial ? initial : "";
    g.max = max_chars ? max_chars : 32;
    if (g.max > 60) g.max = 60;
    if (g.text.size() > g.max) g.text.resize(g.max);
    g.row = g.col = 0;
    g.upper = g.text.empty();
    g.done = g.confirmed = false;
    g.held_x = g.held_y = 0;
    g.active = true;
    g_have_result = false;
}

// One pad button press while the keyboard is open. Returns 1 while it stays open; 2 once it closed with OK
// (the text is then available from ut_ps5_kb_result); 3 once it closed without it.
extern "C" int ut_ps5_kb_button(int button) {
    if (!g.active) return 0;
    switch (button) {
        case kBtnDpadUp: move(0, -1); break;
        case kBtnDpadDown: move(0, 1); break;
        case kBtnDpadLeft: move(-1, 0); break;
        case kBtnDpadRight: move(1, 0); break;
        case kBtnA: press(); break;                           // cross
        case kBtnX: erase(); break;                           // square
        case kBtnY: type(' '); break;                         // triangle
        case kBtnB: g.done = true; break;                     // circle: cancel
        case kBtnLeftShoulder: g.upper = !g.upper; break;
        case kBtnRightShoulder:
        case kBtnStart: g.done = g.confirmed = true; break;
        default: break;
    }
    if (!g.done) return 1;
    g.active = false;
    if (g.confirmed) {
        g_result = g.text;
        g_have_result = true;
        return 2;
    }
    return 3;
}

// The left stick moves the highlight, one step each time it is pushed past the threshold.
extern "C" void ut_ps5_kb_axis(int axis, int value) {
    if (!g.active) return;
    const int dir = value < -16000 ? -1 : value > 16000 ? 1 : 0;
    if (axis == kAxisLeftX) {
        if (dir && dir != g.held_x) move(dir, 0);
        g.held_x = dir;
    } else if (axis == kAxisLeftY) {
        if (dir && dir != g.held_y) move(0, dir);
        g.held_y = dir;
    }
}

extern "C" int ut_ps5_kb_result(char *out, int capacity) {
    if (!g_have_result || !out || capacity <= 0) return 0;
    std::snprintf(out, size_t(capacity), "%s", g_result.c_str());
    g_have_result = false;
    return 1;
}

namespace {

struct Canvas {
    uint32_t *pixels;
    int w, h, pitch_words;
    void fill(float fx, float fy, float fw, float fh, uint32_t color, int alpha) const {
        const int x0 = std::max(0, int(fx)), y0 = std::max(0, int(fy));
        const int x1 = std::min(w, int(fx + fw)), y1 = std::min(h, int(fy + fh));
        for (int y = y0; y < y1; ++y) {
            uint32_t *row = pixels + size_t(y) * pitch_words;
            for (int x = x0; x < x1; ++x) {
                if (alpha >= 255) {
                    row[x] = color;
                } else {
                    const uint32_t d = row[x];
                    const int r = (int((color >> 16) & 255) * alpha + int((d >> 16) & 255) * (255 - alpha)) / 255;
                    const int gr = (int((color >> 8) & 255) * alpha + int((d >> 8) & 255) * (255 - alpha)) / 255;
                    const int b = (int(color & 255) * alpha + int(d & 255) * (255 - alpha)) / 255;
                    row[x] = uint32_t((r << 16) | (gr << 8) | b);
                }
            }
        }
    }
    // Draws text with the 5x8 font; `px` is the size of one font pixel. Returns the width drawn.
    float text(float x, float y, const std::string &s, float px, uint32_t color) const {
        float cx = x;
        for (char ch : s) {
            const unsigned char code = (unsigned char)ch < 128 ? (unsigned char)ch : '?';
            for (int row = 0; row < 8; ++row) {
                const uint8_t bits = kKbFont[code][row];
                for (int col = 0; col < 5; ++col)
                    if (bits & (0x10 >> col)) fill(cx + float(col) * px, y + float(row) * px, px, px, color, 255);
            }
            cx += 6 * px;
        }
        return cx - x;
    }
};

constexpr uint32_t rgb(int r, int g_, int b) { return uint32_t((r << 16) | (g_ << 8) | b); }

}  // namespace

// Draws the keyboard onto the finished frame (XRGB8888).
extern "C" void ut_ps5_kb_draw(void *pixels, int w, int h, int pitch) {
    if (!g.active || !pixels || (pitch & 3)) return;
    const Canvas c{static_cast<uint32_t *>(pixels), w, h, pitch / 4};
    const int px = std::max(2, h / 270);                 // one font pixel: 4 at 1080p
    const float cell = float(8 * px);                    // one text line
    const float kw = cell * 2.2F, kh = cell * 1.8F;      // a key
    const float panel_w = kw * kCols + cell * 2, panel_h = kh * (kRows + 1) + cell * 7.5F;
    const float x0 = (float(w) - panel_w) / 2, y0 = (float(h) - panel_h) / 2;

    c.fill(0, 0, float(w), float(h), rgb(0, 0, 0), 150);                              // dims the menu
    c.fill(x0 - 4, y0 - 4, panel_w + 8, panel_h + 8, rgb(255, 100, 0), 255);          // border
    c.fill(x0, y0, panel_w, panel_h, rgb(8, 8, 56), 255);
    c.text(x0 + cell, y0 + cell * 0.5F, "TYPE THE TEXT", px, rgb(255, 224, 96));
    c.fill(x0 + cell, y0 + cell * 1.9F, panel_w - cell * 2, cell * 1.6F, rgb(0, 0, 24), 255);
    std::string shown = g.text + "_";
    const size_t visible = size_t((panel_w - cell * 3) / float(6 * px));              // the end of a long text stays visible
    if (shown.size() > visible) shown = shown.substr(shown.size() - visible);
    c.text(x0 + cell * 1.5F, y0 + cell * 2.2F, shown, px, rgb(255, 255, 255));

    const float grid_y = y0 + cell * 4.0F;
    for (int row = 0; row < kRows; ++row)
        for (int col = 0; col < kCols; ++col) {
            const bool sel = g.row == row && g.col == col;
            const float cx = x0 + cell + float(col) * kw, cy = grid_y + float(row) * kh;
            c.fill(cx + 2, cy + 2, kw - 4, kh - 4, sel ? rgb(255, 224, 96) : rgb(24, 24, 96), 255);
            const std::string key(1, (g.upper ? kUpper : kLower)[row][col]);
            c.text(cx + (kw - float(6 * px)) / 2, cy + (kh - cell) / 2, key, px, sel ? rgb(0, 0, 0) : rgb(255, 255, 255));
        }
    const float cmd_y = grid_y + float(kRows) * kh + cell * 0.3F;
    const float cmd_w = (kw * kCols) / kCommands;
    for (int i = 0; i < kCommands; ++i) {
        const bool sel = g.row == kRows && g.col == i;
        const float cx = x0 + cell + float(i) * cmd_w;
        c.fill(cx + 2, cmd_y + 2, cmd_w - 4, kh - 4, sel ? rgb(255, 224, 96) : rgb(96, 40, 8), 255);
        const std::string name = kCommandNames[i];
        const float tw = float(name.size() * size_t(6 * px));
        c.text(cx + (cmd_w - tw) / 2, cmd_y + (kh - cell) / 2, name, px, sel ? rgb(0, 0, 0) : rgb(255, 255, 255));
    }
    const int small = std::max(1, px - 1);
    const float hint_y = cmd_y + kh + cell * 0.5F;
    c.text(x0 + cell, hint_y, "CROSS SELECT   SQUARE DELETE   TRIANGLE SPACE", small, rgb(200, 200, 255));
    c.text(x0 + cell, hint_y + float(10 * small), "L1 SHIFT   R1 OR START OK   CIRCLE CANCEL", small, rgb(200, 200, 255));
}

// ---- statistics overlay: the same layout and information as the DiRT games' (DXVK) statistics display ---------------
// DXVK_HUD=fps,frametimes,drawcalls,pipelines: top left the frame rate (updated twice a second) and, below it, the
// draw/render/barrier counts of one frame and the pipeline counts; bottom left the frame-time graph (420 frames,
// newest at the right) with the minimum and maximum above it. The engine switches it from the "Show Statistics"
// check box in Preferences > Video (NSDLViewport.cpp); the numbers are taken at every finished frame here.
#include <chrono>

// Counters the renderer (NVulkanDrv.cpp) bumps.
extern "C" {
volatile unsigned ut_ps5_stat_draw_calls = 0, ut_ps5_stat_render_passes = 0, ut_ps5_stat_barriers = 0,
                  ut_ps5_stat_graphics_pipelines = 0;
}

namespace {
constexpr int kStatFrames = 420;
struct Stats {
    bool on = false;
    float ms[kStatFrames] = {};
    int head = 0, count = 0;  // head: the next slot; the newest value is at head-1
    std::chrono::steady_clock::time_point last{}, sampled{};
    bool have_last = false;
    unsigned prev_draws = 0, prev_passes = 0, prev_barriers = 0;
    unsigned draws = 0, passes = 0, barriers = 0;  // of the last frame, refreshed twice a second
    int frames = 0;
    std::string fps = "0.0";
} s_stats;

constexpr uint32_t kRed = rgb(255, 64, 64), kBlue = rgb(64, 128, 255), kMagenta = rgb(255, 64, 255), kWhite = rgb(255, 255, 255);
}  // namespace

extern "C" void ut_ps5_stats_set(int on) { s_stats.on = on != 0; }
extern "C" int ut_ps5_stats_get(void) { return s_stats.on ? 1 : 0; }

extern "C" void ut_ps5_stats_draw(void *pixels, int w, int h, int pitch) {
    const auto now = std::chrono::steady_clock::now();
    const unsigned draws = ut_ps5_stat_draw_calls, passes = ut_ps5_stat_render_passes, barriers = ut_ps5_stat_barriers;
    if (s_stats.have_last) {
        const float dt = std::chrono::duration<float, std::milli>(now - s_stats.last).count();
        s_stats.ms[s_stats.head] = std::min(dt, 500.0F);
        s_stats.head = (s_stats.head + 1) % kStatFrames;
        s_stats.count = std::min(s_stats.count + 1, kStatFrames);
        ++s_stats.frames;
        const auto since = std::chrono::duration_cast<std::chrono::microseconds>(now - s_stats.sampled).count();
        if (since >= 500000) {
            const long long fps = 10000000LL * s_stats.frames / since;
            s_stats.fps = std::to_string(fps / 10) + "." + std::to_string(fps % 10);
            s_stats.frames = 0;
            s_stats.sampled = now;
            s_stats.draws = draws - s_stats.prev_draws;
            s_stats.passes = passes - s_stats.prev_passes;
            s_stats.barriers = barriers - s_stats.prev_barriers;
        }
    } else {
        s_stats.sampled = now;
    }
    s_stats.prev_draws = draws; s_stats.prev_passes = passes; s_stats.prev_barriers = barriers;
    s_stats.last = now;
    s_stats.have_last = true;
    if (!s_stats.on || !pixels || (pitch & 3) || s_stats.count < 2) return;

    const Canvas c{static_cast<uint32_t *>(pixels), w, h, pitch / 4};
    // Text: DXVK's sizes are pixels of text height; `y` is the baseline, the 5x8 font is 8 units high.
    const auto label = [&](float x, float y, const std::string &t, int size, uint32_t color) {
        const float px = float(size) / 8.0F;
        c.text(x, y - float(size) * 0.8F, t, px, color);
    };

    // Just the frame rate and the frame-time graph, at the left edge in the middle of the screen: the game's own
    // information is at the top and the bottom. `hb` plays the part of the screen's bottom edge for the layout below.
    const float hb = float(h) / 2 + 92;
    const float fps_y = hb - 152;
    label(8, fps_y, "FPS:", 16, kRed);
    label(8 + 60, fps_y, s_stats.fps, 16, kWhite);

    // Frame times: average, minimum and maximum over the graph; the graph's scale follows the maximum.
    float sum = 0, mn = 1e9F, mx = 0;
    for (int i = 0; i < s_stats.count; ++i) {
        const float v = s_stats.ms[i];
        sum += v; mn = std::min(mn, v); mx = std::max(mx, v);
    }
    const float avg = sum / float(s_stats.count);
    const float ms_max = std::min(std::max(std::max(mx, avg * 2.0F), 20.0F), 200.0F);
    char text[32];
    std::snprintf(text, sizeof text, "%.1f ms", mn);
    label(12, hb - 128, "min:", 12, kRed);
    label(12 + 48, hb - 128, text, 12, kWhite);
    std::snprintf(text, sizeof text, "%.1f ms", mx);
    label(162, hb - 128, "max:", 12, kRed);
    label(162 + 48, hb - 128, text, 12, kWhite);

    const float gx = 8, gy = hb - 120, gh = 80;
    c.fill(gx, gy, float(kStatFrames), gh, rgb(0, 0, 0), 191);                    // 75% black
    const auto at = [&](int column) {                                            // column 0 = oldest on the left
        const int back = kStatFrames - 1 - column;                               // frames before the newest
        if (back >= s_stats.count) return -1.0F;
        return s_stats.ms[(s_stats.head - 1 - back + 2 * kStatFrames) % kStatFrames];
    };
    for (int col = 0; col < kStatFrames; ++col) {
        const float a = at(col), b = col + 1 < kStatFrames ? at(col + 1) : a;
        if (a < 0 || b < 0) continue;
        const float ya = gh - std::min(a / ms_max, 1.0F) * gh, yb = gh - std::min(b / ms_max, 1.0F) * gh;
        const float top = std::min(ya, yb), len = std::max(1.0F, std::abs(ya - yb) + 1.0F);
        c.fill(gx + float(col), gy + top, 1, len, rgb(198, 207, 225), 255);       // a line, like DXVK's graph
    }
}
