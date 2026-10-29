// AT168Shot - Screenshot capture tool for AnyTone 168 radio displays
// Cross-platform C++ + Dear ImGui + GLFW implementation

// Platform detection
#if defined(_WIN32)
    #define RADSHOT_WINDOWS
#elif defined(__APPLE__)
    #define RADSHOT_MACOS
#else
    #define RADSHOT_LINUX
#endif

#ifdef RADSHOT_WINDOWS
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
    #include <setupapi.h>
    #include <shlobj.h>
#else
    #include <termios.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <dirent.h>
    #include <sys/stat.h>
    #include <errno.h>
#endif

#ifdef RADSHOT_MACOS
    #include <CoreFoundation/CoreFoundation.h>
    #include <IOKit/serial/ioss.h>
    #include <sys/ioctl.h>
#endif

#include <GLFW/glfw3.h>
#ifdef RADSHOT_WINDOWS
    #define GLFW_EXPOSE_NATIVE_WIN32
    #include <GLFW/glfw3native.h>
#endif

// The Windows SDK ships an OpenGL 1.1 gl.h, so GL 1.2+ tokens are missing.
// Every driver we care about supports them at runtime; just declare them.
#ifndef GL_CLAMP_TO_EDGE
    #define GL_CLAMP_TO_EDGE 0x812F
#endif

#include <vector>
#include <string>
#include <set>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <climits>
#include <cstdarg>
#include <cmath>
#include <cstdlib>
#include <ctime>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_glfw.h"
#include "imgui/imgui_impl_opengl3.h"
#include "tinyfiledialogs.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// =============================================================================
// Constants
// =============================================================================

constexpr const char* APP_VERSION = "0.2";

// The radio answers the "SCR" command with one raw 16-bit RGB565 (little
// endian) framebuffer dump. The scanlines come out transposed relative to what
// the display shows: RAW_WIDTH pixels per row, RAW_HEIGHT rows. Rotating (and
// optionally mirroring) that recovers the 128x160 portrait image.
constexpr int RAW_WIDTH = 160;
constexpr int RAW_HEIGHT = 128;
constexpr int DISPLAY_WIDTH = 128;
constexpr int DISPLAY_HEIGHT = 160;
constexpr int FRAME_SIZE = RAW_WIDTH * RAW_HEIGHT * 2;  // 40960 bytes
constexpr int RGBA_SIZE = DISPLAY_WIDTH * DISPLAY_HEIGHT * 4;
// The AT-D168UV runs its program-mode serial link at 921600 (at168-cps and the
// at168-flasher tools both do); 115200 is what qdmr uses for other AnyTone
// radios, and 4 Mbaud is the firmware-update/license speed. Which one the
// screenshot firmware answers on is a property of that firmware, so the rate is
// selectable and remembered rather than compiled in.
constexpr int BAUD_RATES[] = { 921600, 115200, 230400, 460800, 4000000 };
constexpr int BAUD_RATE_COUNT = (int)(sizeof(BAUD_RATES) / sizeof(BAUD_RATES[0]));
constexpr int DEFAULT_BAUD_INDEX = 0;
constexpr uint8_t SCREENSHOT_CMD[] = { 'S', 'C', 'R' };
// Give up when the radio has sent nothing for this long. Time based, not a
// frame count: the read loop runs once per rendered frame, so a frame-based
// limit changes meaning with the refresh rate and stalls when the window is
// occluded and rendering slows down.
constexpr double CAPTURE_QUIET_TIMEOUT_S = 5.0;
constexpr int READS_PER_FRAME = 64;
constexpr int PATH_MAX_LEN = 4096;

// Saved and copied PNGs are upscaled by this (nearest neighbour).
constexpr int EXPORT_SCALES[] = { 1, 2, 3, 4, 6 };
constexpr int EXPORT_SCALE_COUNT = (int)(sizeof(EXPORT_SCALES) / sizeof(EXPORT_SCALES[0]));
constexpr int DEFAULT_EXPORT_SCALE = 3;

// Gallery thumbnail width in (unscaled) pixels, and the preview zoom (0 = fit).
constexpr int THUMB_MIN = 96, THUMB_MAX = 256, THUMB_STEP = 16, DEFAULT_THUMB = 128;
constexpr int ZOOM_MAX = 6;

// How long a delete can be undone.
constexpr double UNDO_SECONDS = 6.0;
// How often the port list is rescanned, to notice the radio being unplugged
// or plugged back in.
constexpr double PORT_POLL_S = 2.0;
// Sequence exports bigger than this are refused rather than allocated.
constexpr int MAX_EXPORT_SIDE = 16384;
constexpr double MAX_EXPORT_AREA = 64e6;

// =============================================================================
// DPI scaling
// =============================================================================

// Content scale of the monitor the window is on (1.0 = 96 DPI, 1.5 = 150%, ...).
// Multiply hardcoded pixel sizes by this so the UI keeps its physical size.
static float g_dpi_scale = 1.0f;

// Saved window sizes are already in pixels, so only the built-in default gets
// scaled up for the monitor.
static bool g_have_saved_size = false;

// Style at scale 1.0. ScaleAllSizes() compounds, so rescaling has to start here.
static ImGuiStyle g_base_style;

// Virtual monitors from some accessibility tools report a scale of 0, and the
// backend passes that through, so clamp every scale on the way in.
static float QueryDpiScale(GLFWwindow* window) {
    float scale = ImGui_ImplGlfw_GetContentScaleForWindow(window);
    return (scale > 0.0f) ? scale : 1.0f;
}

static void ApplyDpiScale(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = g_base_style;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;  // re-rasterizes the font at the new size
    g_dpi_scale = scale;
}

// Orientation applied to the raw frame. Rotation is clockwise, the mirror is
// applied afterwards; these four combinations are the only transforms that turn
// a 160x128 frame into a 128x160 one, so one of them is always the right answer.
enum Rotation { ROT_CW_90 = 0, ROT_CCW_90 = 1 };
constexpr int DEFAULT_ROTATION = ROT_CW_90;
constexpr bool DEFAULT_MIRROR = true;   // rotate + mirror == transpose

// =============================================================================
// Colours (Dear ImGui's dark style, plus the few the app adds)
// =============================================================================

static const ImVec4 COL_PRIMARY        = ImVec4(0.141f, 0.439f, 0.816f, 1.00f);  // #2470d0
static const ImVec4 COL_PRIMARY_HOVER  = ImVec4(0.122f, 0.388f, 0.729f, 1.00f);
static const ImVec4 COL_OK             = ImVec4(0.00f, 0.80f, 0.00f, 1.00f);
static const ImVec4 COL_ERROR          = ImVec4(0.90f, 0.35f, 0.35f, 1.00f);
static const ImVec4 COL_DIM            = ImVec4(0.63f, 0.63f, 0.63f, 1.00f);
static const ImVec4 COL_DANGER_TEXT    = ImVec4(1.00f, 0.60f, 0.60f, 1.00f);
static const ImVec4 COL_DANGER_BG      = ImVec4(0.90f, 0.35f, 0.35f, 0.18f);
static const ImVec4 COL_DANGER_HOVER   = ImVec4(0.90f, 0.35f, 0.35f, 0.45f);
static const ImU32  COL_CARD           = IM_COL32(255, 255, 255, 10);
static const ImU32  COL_CARD_HOVER     = IM_COL32(255, 255, 255, 20);
static const ImU32  COL_CARD_PICKED    = IM_COL32(66, 150, 250, 64);
static const ImU32  COL_ACCENT         = IM_COL32(66, 150, 250, 255);

// =============================================================================
// Screenshot Structure
// =============================================================================

struct Screenshot {
    int id = 0;
    char name[256] = {0};
    time_t date = 0;
    bool saved = false;             // written out as a PNG (or zip) at least once
    uint8_t raw[FRAME_SIZE];        // RGB565 little endian, as received
    uint8_t rgba[RGBA_SIZE];        // decoded with the current orientation, 1x
    GLuint texture = 0;

    Screenshot() = default;
    ~Screenshot() { if (texture) glDeleteTextures(1, &texture); }
    Screenshot(const Screenshot&) = delete;
    Screenshot& operator=(const Screenshot&) = delete;
};

// A screenshot taken out of the gallery, kept until its undo expires.
struct Removed {
    Screenshot* ss;
    int index;
};

enum SeqFormat { SEQ_SHEET = 0, SEQ_STRIP = 1, SEQ_GIF = 2 };
enum SeqBackground { BG_DARK = 0, BG_LIGHT = 1, BG_NONE = 2 };

// Contact sheet / strip / animated GIF options; remembered between runs.
struct SeqOptions {
    int format = SEQ_SHEET;
    int columns = 4;
    bool vertical = false;      // strip direction
    int gap = 8;
    int background = BG_DARK;
    bool labels = false;
    int delay_ms = 800;
    int scale = 2;
};

enum StatusKind { STATUS_INFO, STATUS_OK, STATUS_ERR };

// =============================================================================
// Application State
// =============================================================================

struct AppState {
    // Window
    GLFWwindow* window = nullptr;
    int window_width = 1100;
    int window_height = 820;
    bool running = true;

    // Serial
    std::vector<std::string> com_ports;
    int selected_port = -1;
#ifdef RADSHOT_WINDOWS
    HANDLE serial_handle = INVALID_HANDLE_VALUE;
#else
    int serial_fd = -1;
#endif
    bool is_connected = false;
    int baud_index = DEFAULT_BAUD_INDEX;
    char status_message[512] = "Disconnected";
    int status_kind = STATUS_INFO;

    // Capture
    bool is_capturing = false;
    int capture_progress = 0;
    uint8_t capture_buffer[FRAME_SIZE];
    int capture_bytes = 0;
    double capture_last_data = 0.0;
    bool line_was_busy = false;   // radio was still talking when SCR went out

    // Frame orientation (see Rotation)
    int rotation = DEFAULT_ROTATION;
    bool mirror = DEFAULT_MIRROR;

    // Screenshots, in gallery order
    std::vector<Screenshot*> screenshots;
    int next_id = 1;
    // Multi-selection: every picked screenshot. `selected` is the one shown in
    // the preview, `anchor` where a Shift-click range starts.
    std::set<Screenshot*> picked;
    int selected = -1;
    int anchor = -1;
    std::vector<int> dragging;    // ids being dragged within the gallery

    // Undo
    std::vector<Removed> undo;
    char undo_message[128] = {0};
    double undo_until = 0.0;

    // View
    int thumb_size = DEFAULT_THUMB;
    int zoom = 0;

    // Export
    int export_scale = DEFAULT_EXPORT_SCALE;
    bool save_as_zip = true;
    SeqOptions seq;
    std::vector<Screenshot*> export_list;
    GLuint export_texture = 0;
    int export_w = 0, export_h = 0;
    bool export_dirty = true;
    int gif_frame = 0;
    double gif_next = 0.0;

    // Connection
    bool auto_connect = true;
    bool reconnect = false;       // reopen last_port_name; cleared by a manual Disconnect
    double next_port_poll = 0.0;

    // Captures stored on disk, so they survive a restart
    bool storage_ok = false;
    char capture_dir[PATH_MAX_LEN] = {0};

    // UI
    char rename_buffer[256] = {0};
    bool focus_rename = false;
    bool open_card_menu = false;
    bool open_export = false;
    std::vector<Screenshot*> menu_targets;
    bool show_exit_popup = false;
    bool pending_close = false;

    // Settings persistence
    char last_save_directory[PATH_MAX_LEN] = {0};
    char last_port_name[256] = {0};
    int window_x = -1;
    int window_y = -1;
};

static AppState g_state;

static void SetStatus(int kind, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_state.status_message, sizeof(g_state.status_message), fmt, args);
    va_end(args);
    g_state.status_kind = kind;
}

// =============================================================================
// Settings Persistence
// =============================================================================

static void MakeDir(const char* path) {
#ifdef RADSHOT_WINDOWS
    CreateDirectoryA(path, nullptr);
#else
    mkdir(path, 0755);
#endif
}

static void GetSettingsPath(char* path, size_t pathSize) {
#ifdef RADSHOT_WINDOWS
    char exePath[PATH_MAX_LEN];
    GetModuleFileNameA(nullptr, exePath, PATH_MAX_LEN);
    char* dot = strrchr(exePath, '.');
    if (dot) {
        strcpy(dot, ".ini");
    } else {
        strcat(exePath, ".ini");
    }
    strncpy(path, exePath, pathSize - 1);
    path[pathSize - 1] = 0;
#elif defined(RADSHOT_MACOS)
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    char dir[PATH_MAX_LEN];
    snprintf(dir, sizeof(dir), "%s/Library/Application Support/at168shot", home);
    mkdir(dir, 0755);
    snprintf(path, pathSize, "%s/at168shot.ini", dir);
#else
    const char* configHome = getenv("XDG_CONFIG_HOME");
    char dir[PATH_MAX_LEN];
    if (configHome && configHome[0]) {
        snprintf(dir, sizeof(dir), "%s/at168shot", configHome);
    } else {
        const char* home = getenv("HOME");
        if (!home) home = "/tmp";
        snprintf(dir, sizeof(dir), "%s/.config/at168shot", home);
    }
    mkdir(dir, 0755);
    snprintf(path, pathSize, "%s/at168shot.ini", dir);
#endif
}

// Where captures are kept between runs: per user, not next to the executable,
// since that is often a read-only or shared location.
static bool GetCaptureDir(char* path, size_t pathSize) {
    char base[PATH_MAX_LEN];
#ifdef RADSHOT_WINDOWS
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base))) return false;
    snprintf(path, pathSize, "%s\\at168shot", base);
    MakeDir(path);
    snprintf(path, pathSize, "%s\\at168shot\\captures", base);
#elif defined(RADSHOT_MACOS)
    const char* home = getenv("HOME");
    if (!home) return false;
    snprintf(base, sizeof(base), "%s/Library/Application Support/at168shot", home);
    MakeDir(base);
    snprintf(path, pathSize, "%s/captures", base);
#else
    const char* dataHome = getenv("XDG_DATA_HOME");
    if (dataHome && dataHome[0]) {
        snprintf(base, sizeof(base), "%s", dataHome);
    } else {
        const char* home = getenv("HOME");
        if (!home) return false;
        snprintf(base, sizeof(base), "%s/.local", home);
        MakeDir(base);
        snprintf(base, sizeof(base), "%s/.local/share", home);
    }
    MakeDir(base);
    snprintf(path, pathSize, "%s/at168shot", base);
    MakeDir(path);
    snprintf(path, pathSize, "%s/at168shot/captures", base);
#endif
    MakeDir(path);
    return true;
}

static void LoadSettings() {
    char iniPath[PATH_MAX_LEN];
    GetSettingsPath(iniPath, sizeof(iniPath));

    FILE* f = fopen(iniPath, "r");
    if (!f) return;

    char line[PATH_MAX_LEN + 64];
    while (fgets(line, sizeof(line), f)) {
        // Remove newline
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char* cr = strchr(line, '\r');
        if (cr) *cr = 0;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* key = line;
        const char* value = eq + 1;
        int v = atoi(value);
        SeqOptions& q = g_state.seq;

        if (strcmp(key, "window_x") == 0) {
            g_state.window_x = v;
        } else if (strcmp(key, "window_y") == 0) {
            g_state.window_y = v;
        } else if (strcmp(key, "window_width") == 0) {
            if (v >= 400 && v <= 8192) { g_state.window_width = v; g_have_saved_size = true; }
        } else if (strcmp(key, "window_height") == 0) {
            if (v >= 300 && v <= 8192) { g_state.window_height = v; g_have_saved_size = true; }
        } else if (strcmp(key, "last_port") == 0) {
            strncpy(g_state.last_port_name, value, sizeof(g_state.last_port_name) - 1);
        } else if (strcmp(key, "last_save_directory") == 0) {
            strncpy(g_state.last_save_directory, value, sizeof(g_state.last_save_directory) - 1);
        } else if (strcmp(key, "baud_index") == 0) {
            if (v >= 0 && v < BAUD_RATE_COUNT) g_state.baud_index = v;
        } else if (strcmp(key, "rotation") == 0) {
            if (v == ROT_CW_90 || v == ROT_CCW_90) g_state.rotation = v;
        } else if (strcmp(key, "mirror") == 0) {
            g_state.mirror = (v != 0);
        } else if (strcmp(key, "thumb_size") == 0) {
            if (v >= THUMB_MIN && v <= THUMB_MAX) g_state.thumb_size = v;
        } else if (strcmp(key, "zoom") == 0) {
            if (v >= 0 && v <= ZOOM_MAX) g_state.zoom = v;
        } else if (strcmp(key, "export_scale") == 0) {
            for (int s : EXPORT_SCALES) if (s == v) g_state.export_scale = v;
        } else if (strcmp(key, "save_as_zip") == 0) {
            g_state.save_as_zip = (v != 0);
        } else if (strcmp(key, "auto_connect") == 0) {
            g_state.auto_connect = (v != 0);
        } else if (strcmp(key, "reconnect") == 0) {
            g_state.reconnect = (v != 0);
        } else if (strcmp(key, "seq_format") == 0) {
            if (v >= SEQ_SHEET && v <= SEQ_GIF) q.format = v;
        } else if (strcmp(key, "seq_columns") == 0) {
            if (v >= 1 && v <= 10) q.columns = v;
        } else if (strcmp(key, "seq_vertical") == 0) {
            q.vertical = (v != 0);
        } else if (strcmp(key, "seq_gap") == 0) {
            if (v >= 0 && v <= 48) q.gap = v;
        } else if (strcmp(key, "seq_background") == 0) {
            if (v >= BG_DARK && v <= BG_NONE) q.background = v;
        } else if (strcmp(key, "seq_labels") == 0) {
            q.labels = (v != 0);
        } else if (strcmp(key, "seq_delay_ms") == 0) {
            if (v >= 200 && v <= 4000) q.delay_ms = v;
        } else if (strcmp(key, "seq_scale") == 0) {
            for (int s : EXPORT_SCALES) if (s == v) q.scale = v;
        }
    }
    fclose(f);
}

static void SaveSettings() {
    char iniPath[PATH_MAX_LEN];
    GetSettingsPath(iniPath, sizeof(iniPath));

    FILE* f = fopen(iniPath, "w");
    if (!f) return;

    const SeqOptions& q = g_state.seq;
    fprintf(f, "window_x=%d\n", g_state.window_x);
    fprintf(f, "window_y=%d\n", g_state.window_y);
    fprintf(f, "window_width=%d\n", g_state.window_width);
    fprintf(f, "window_height=%d\n", g_state.window_height);
    fprintf(f, "last_port=%s\n", g_state.last_port_name);
    fprintf(f, "last_save_directory=%s\n", g_state.last_save_directory);
    fprintf(f, "baud_index=%d\n", g_state.baud_index);
    fprintf(f, "rotation=%d\n", g_state.rotation);
    fprintf(f, "mirror=%d\n", g_state.mirror ? 1 : 0);
    fprintf(f, "thumb_size=%d\n", g_state.thumb_size);
    fprintf(f, "zoom=%d\n", g_state.zoom);
    fprintf(f, "export_scale=%d\n", g_state.export_scale);
    fprintf(f, "save_as_zip=%d\n", g_state.save_as_zip ? 1 : 0);
    fprintf(f, "auto_connect=%d\n", g_state.auto_connect ? 1 : 0);
    fprintf(f, "reconnect=%d\n", g_state.reconnect ? 1 : 0);
    fprintf(f, "seq_format=%d\n", q.format);
    fprintf(f, "seq_columns=%d\n", q.columns);
    fprintf(f, "seq_vertical=%d\n", q.vertical ? 1 : 0);
    fprintf(f, "seq_gap=%d\n", q.gap);
    fprintf(f, "seq_background=%d\n", q.background);
    fprintf(f, "seq_labels=%d\n", q.labels ? 1 : 0);
    fprintf(f, "seq_delay_ms=%d\n", q.delay_ms);
    fprintf(f, "seq_scale=%d\n", q.scale);
    fclose(f);
}

// =============================================================================
// Frame Processing (RGB565 -> RGBA)
// =============================================================================

// The frame arrives as 128 consecutive runs of 160 pixels; each run is one
// column of the display, so the raw image is the transpose of what the radio
// shows. Maps a display pixel back to its index in the raw frame.
inline int RawPixelIndex(int x, int y, int rotation, bool mirror) {
    int mx = mirror ? (DISPLAY_WIDTH - 1 - x) : x;
    int sx, sy;
    if (rotation == ROT_CW_90) {
        sx = y;
        sy = RAW_HEIGHT - 1 - mx;
    } else {
        sx = RAW_WIDTH - 1 - y;
        sy = mx;
    }
    return sy * RAW_WIDTH + sx;
}

void ProcessFrame(const uint8_t* raw, int rotation, bool mirror, uint8_t* rgba) {
    for (int y = 0; y < DISPLAY_HEIGHT; y++) {
        for (int x = 0; x < DISPLAY_WIDTH; x++) {
            // Decode one RGB565 little endian pixel
            int src = RawPixelIndex(x, y, rotation, mirror) * 2;
            int pixel = raw[src] | (raw[src + 1] << 8);

            uint8_t* color = rgba + (y * DISPLAY_WIDTH + x) * 4;
            color[0] = (uint8_t)((((pixel >> 11) & 0x1F) * 255) / 31);  // R
            color[1] = (uint8_t)((((pixel >> 5) & 0x3F) * 255) / 63);   // G
            color[2] = (uint8_t)(((pixel & 0x1F) * 255) / 31);          // B
            color[3] = 0xFF;
        }
    }
}

// Creates the texture, or refills an existing one in place.
static void UploadTexture(GLuint* tex, const uint8_t* rgba, int width, int height) {
    if (!*tex) {
        glGenTextures(1, tex);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, *tex);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

// Re-decode a screenshot from its raw frame, e.g. after the orientation changed
void RebuildScreenshot(Screenshot* ss) {
    ProcessFrame(ss->raw, g_state.rotation, g_state.mirror, ss->rgba);
    UploadTexture(&ss->texture, ss->rgba, DISPLAY_WIDTH, DISPLAY_HEIGHT);
}

void RebuildAllScreenshots() {
    for (Screenshot* ss : g_state.screenshots) {
        RebuildScreenshot(ss);
    }
    g_state.export_dirty = true;
}

// Nearest-neighbour upscale of a 1x RGBA image.
static std::vector<uint8_t> ScaleRGBA(const uint8_t* src, int w, int h, int scale) {
    std::vector<uint8_t> out((size_t)w * scale * h * scale * 4);
    int W = w * scale;
    for (int y = 0; y < h * scale; y++) {
        const uint8_t* row = src + (y / scale) * w * 4;
        uint8_t* dst = out.data() + (size_t)y * W * 4;
        for (int x = 0; x < W; x++) memcpy(dst + x * 4, row + (x / scale) * 4, 4);
    }
    return out;
}

// =============================================================================
// Capture Storage (raw frames on disk, so a restart or crash loses nothing)
// =============================================================================
//
// captures/<id>.raw holds each frame exactly as received; captures/index.txt
// lists them in gallery order as "id<TAB>date<TAB>saved<TAB>name". Frames are
// re-decoded with the current orientation when loaded.

static void RawPath(int id, char* path, size_t size) {
    snprintf(path, size, "%s/%d.raw", g_state.capture_dir, id);
}

static void WriteRaw(const Screenshot* ss) {
    if (!g_state.storage_ok) return;
    char path[PATH_MAX_LEN];
    RawPath(ss->id, path, sizeof(path));
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(ss->raw, 1, FRAME_SIZE, f);
    fclose(f);
}

static void RemoveRaw(const Screenshot* ss) {
    if (!g_state.storage_ok) return;
    char path[PATH_MAX_LEN];
    RawPath(ss->id, path, sizeof(path));
    remove(path);
}

// Written to a temporary file and renamed over the old one, so a crash
// mid-write never leaves a truncated index.
static void SaveIndex() {
    if (!g_state.storage_ok) return;
    char path[PATH_MAX_LEN], tmp[PATH_MAX_LEN];
    snprintf(path, sizeof(path), "%s/index.txt", g_state.capture_dir);
    snprintf(tmp, sizeof(tmp), "%s/index.tmp", g_state.capture_dir);
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "at168shot-captures 1\n");
    for (const Screenshot* ss : g_state.screenshots) {
        fprintf(f, "%d\t%lld\t%d\t%s\n", ss->id, (long long)ss->date, ss->saved ? 1 : 0, ss->name);
    }
    fclose(f);
#ifdef RADSHOT_WINDOWS
    MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
#else
    rename(tmp, path);
#endif
}

static Screenshot* NewScreenshot(int id, const char* name, time_t date, const uint8_t* raw, bool saved) {
    Screenshot* ss = new Screenshot();
    ss->id = id;
    strncpy(ss->name, name, sizeof(ss->name) - 1);
    ss->date = date;
    ss->saved = saved;
    memcpy(ss->raw, raw, FRAME_SIZE);
    RebuildScreenshot(ss);
    return ss;
}

static void LoadCaptures() {
    g_state.storage_ok = GetCaptureDir(g_state.capture_dir, sizeof(g_state.capture_dir));
    if (!g_state.storage_ok) return;

    char path[PATH_MAX_LEN];
    snprintf(path, sizeof(path), "%s/index.txt", g_state.capture_dir);
    FILE* f = fopen(path, "r");
    if (!f) return;

    char line[512];
    std::vector<uint8_t> raw(FRAME_SIZE);
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        int id = 0, saved = 0;
        long long date = 0;
        int consumed = 0;
        if (sscanf(line, "%d\t%lld\t%d\t%n", &id, &date, &saved, &consumed) < 3 || id <= 0) continue;

        char rawPath[PATH_MAX_LEN];
        RawPath(id, rawPath, sizeof(rawPath));
        FILE* rf = fopen(rawPath, "rb");
        if (!rf) continue;
        size_t n = fread(raw.data(), 1, FRAME_SIZE, rf);
        fclose(rf);
        if (n != FRAME_SIZE) continue;

        g_state.screenshots.push_back(NewScreenshot(id, line + consumed, (time_t)date, raw.data(), saved != 0));
        g_state.next_id = std::max(g_state.next_id, id + 1);
    }
    fclose(f);
}

// Defined with the selection code below; called when a capture completes.
static void AddCapture(const uint8_t* raw);

// =============================================================================
// Serial Port Functions
// =============================================================================

void EnumerateComPorts() {
    g_state.com_ports.clear();

#ifdef RADSHOT_WINDOWS
    static const GUID GUID_DEVINTERFACE_COMPORT =
        { 0x86E0D1E0L, 0x8089, 0x11D0, { 0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73 } };

    HDEVINFO hDevInfo = SetupDiGetClassDevs(
        &GUID_DEVINTERFACE_COMPORT, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    );

    if (hDevInfo == INVALID_HANDLE_VALUE) return;

    SP_DEVINFO_DATA devInfo = { sizeof(SP_DEVINFO_DATA) };

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfo); i++) {
        HKEY hKey = SetupDiOpenDevRegKey(
            hDevInfo, &devInfo, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ
        );

        if (hKey != INVALID_HANDLE_VALUE) {
            char portName[256];
            DWORD size = sizeof(portName);
            DWORD type;

            if (RegQueryValueExA(hKey, "PortName", nullptr, &type,
                                 (LPBYTE)portName, &size) == ERROR_SUCCESS) {
                if (strncmp(portName, "COM", 3) == 0) {
                    g_state.com_ports.push_back(portName);
                }
            }
            RegCloseKey(hKey);
        }
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);

    std::sort(g_state.com_ports.begin(), g_state.com_ports.end(),
        [](const std::string& a, const std::string& b) {
            return atoi(a.c_str() + 3) < atoi(b.c_str() + 3);
        });

#elif defined(RADSHOT_MACOS)
    DIR* dir = opendir("/dev");
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Match /dev/cu.* but exclude Bluetooth
        if (strncmp(entry->d_name, "cu.", 3) == 0 &&
            strstr(entry->d_name, "Bluetooth") == nullptr) {
            std::string path = std::string("/dev/") + entry->d_name;
            g_state.com_ports.push_back(path);
        }
    }
    closedir(dir);
    std::sort(g_state.com_ports.begin(), g_state.com_ports.end());

#else // Linux
    DIR* dir = opendir("/dev");
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "ttyUSB", 6) == 0 ||
            strncmp(entry->d_name, "ttyACM", 6) == 0) {
            std::string path = std::string("/dev/") + entry->d_name;
            g_state.com_ports.push_back(path);
        }
    }
    closedir(dir);
    std::sort(g_state.com_ports.begin(), g_state.com_ports.end());
#endif
}

#ifndef RADSHOT_WINDOWS
// Maps a baud rate to its termios constant. Rates above 230400 have no constant
// on macOS - there they are set with IOSSIOSPEED after tcsetattr, which is what
// pyserial (and therefore at168-cps) does.
static bool BaudConstant(int baud, speed_t* out) {
    switch (baud) {
        case 115200: *out = B115200; return true;
        case 230400: *out = B230400; return true;
#ifdef B460800
        case 460800: *out = B460800; return true;
#endif
#ifdef B921600
        case 921600: *out = B921600; return true;
#endif
#ifdef B4000000
        case 4000000: *out = B4000000; return true;
#endif
        default: return false;
    }
}
#endif

static bool SerialDrainInput(double quiet_s = 0.05, double max_s = 0.5);

bool SerialConnect(const char* portName) {
    int baudrate = BAUD_RATES[g_state.baud_index];

#ifdef RADSHOT_WINDOWS
    char fullPath[256];
    snprintf(fullPath, sizeof(fullPath), "\\\\.\\%s", portName);

    g_state.serial_handle = CreateFileA(
        fullPath, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, 0, nullptr
    );

    if (g_state.serial_handle == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        const char* hint = "";
        if (err == ERROR_ACCESS_DENIED) hint = " (port busy - close any other CPS/terminal)";
        else if (err == ERROR_FILE_NOT_FOUND) hint = " (port gone - unplug/replug, then Refresh)";
        SetStatus(STATUS_ERR,
                 "Failed to open %s: error %lu%s", portName, (unsigned long)err, hint);
        return false;
    }

    DCB dcb = { sizeof(DCB) };
    if (!GetCommState(g_state.serial_handle, &dcb)) {
        DWORD err = GetLastError();
        CloseHandle(g_state.serial_handle);
        g_state.serial_handle = INVALID_HANDLE_VALUE;
        SetStatus(STATUS_ERR,
                 "Failed to get port state: error %lu", (unsigned long)err);
        return false;
    }

    dcb.BaudRate = (DWORD)baudrate;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;

    // dcb came from GetCommState, so it carries whatever the previous user of
    // the port left behind. Flow control still enabled from an earlier session
    // stalls every write waiting for CTS/DSR that the radio never asserts.
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;

    if (!SetCommState(g_state.serial_handle, &dcb)) {
        DWORD err = GetLastError();
        CloseHandle(g_state.serial_handle);
        g_state.serial_handle = INVALID_HANDLE_VALUE;
        SetStatus(STATUS_ERR,
                 "Failed to configure port: error %lu", (unsigned long)err);
        return false;
    }

    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 1000;
    SetCommTimeouts(g_state.serial_handle, &timeouts);

    PurgeComm(g_state.serial_handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

#else // POSIX
    int fd = open(portName, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        SetStatus(STATUS_ERR,
                 "Failed to open %s: %s", portName, strerror(errno));
        return false;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        SetStatus(STATUS_ERR,
                 "Failed to get port attributes: %s", strerror(errno));
        return false;
    }

    // The speed goes through cfset*speed, never OR-ed into c_cflag: the baud
    // constants are cflag bits on Linux but plain numbers on macOS, where
    // OR-ing one would corrupt the control flags.
    speed_t speed;
    bool standard_speed = BaudConstant(baudrate, &speed);
    if (!standard_speed) {
#ifdef RADSHOT_MACOS
        speed = B115200;  // placeholder, replaced by IOSSIOSPEED below
#else
        close(fd);
        SetStatus(STATUS_ERR,
                 "Unsupported baud rate %d", baudrate);
        return false;
#endif
    }

    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    // Clear only the framing bits we are about to set: on Linux the speed lives
    // in c_cflag too and cfset*speed just put it there. Hardware flow control
    // left on by whatever used the port last would stall us waiting for CTS.
    tty.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
#ifdef CRTSCTS
    tty.c_cflag &= ~CRTSCTS;
#endif
    tty.c_cflag |= CS8 | CLOCAL | CREAD;
    tty.c_iflag = IGNPAR;
    tty.c_oflag = 0;
    tty.c_lflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    tcflush(fd, TCIOFLUSH);

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        SetStatus(STATUS_ERR,
                 "Failed to configure port: %s", strerror(errno));
        return false;
    }

#ifdef RADSHOT_MACOS
    if (!standard_speed) {
        speed_t requested = (speed_t)baudrate;
        if (ioctl(fd, IOSSIOSPEED, &requested) < 0) {
            close(fd);
            SetStatus(STATUS_ERR,
                     "Failed to set %d baud: %s", baudrate, strerror(errno));
            return false;
        }
    }
#endif

    g_state.serial_fd = fd;
#endif

    g_state.is_connected = true;

    // A previous session may have left the radio mid-transfer, so start from a
    // quiet line rather than inheriting its leftovers.
    SerialDrainInput();

    strncpy(g_state.last_port_name, portName, sizeof(g_state.last_port_name) - 1);
    SetStatus(STATUS_OK, "Connected to %s at %d baud", portName, baudrate);
    return true;
}

void SerialDisconnect() {
#ifdef RADSHOT_WINDOWS
    if (g_state.serial_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_state.serial_handle);
        g_state.serial_handle = INVALID_HANDLE_VALUE;
    }
#else
    if (g_state.serial_fd >= 0) {
        close(g_state.serial_fd);
        g_state.serial_fd = -1;
    }
#endif
    g_state.is_connected = false;
    g_state.is_capturing = false;
    SetStatus(STATUS_INFO, "Disconnected");
}

// Reads and discards whatever the radio is still sending, until the line has
// been quiet for quiet_s. A capture aborted part way leaves the tail of the old
// frame in flight, and flushing once at t=0 does not catch the bytes that
// arrive after it: the next capture then reads that leftover as the head of its
// frame, never sees a full one, and the app looks stuck until it times out.
// Bounded by max_s so this can never hang the UI thread.
// Returns true if the line actually went quiet. A radio that keeps talking for
// the whole budget - a Tier 3 set busy with trunked signalling, say - returns
// false, and whatever follows is not a clean capture.
static bool SerialDrainInput(double quiet_s, double max_s) {
    if (!g_state.is_connected) return true;

    bool went_quiet = false;

    uint8_t scratch[1024];
    double start = glfwGetTime();
    double last_data = start;

    for (;;) {
        double now = glfwGetTime();
        if (now - last_data >= quiet_s) { went_quiet = true; break; }
        if (now - start >= max_s) break;

#ifdef RADSHOT_WINDOWS
        DWORD n = 0;
        if (ReadFile(g_state.serial_handle, scratch, sizeof(scratch), &n, nullptr) && n > 0) {
            last_data = glfwGetTime();
            continue;
        }
        Sleep(2);
#else
        ssize_t n = read(g_state.serial_fd, scratch, sizeof(scratch));
        if (n > 0) {
            last_data = glfwGetTime();
            continue;
        }
        usleep(2000);
#endif
    }

    // The line is quiet now, so anything still buffered is stale.
#ifdef RADSHOT_WINDOWS
    PurgeComm(g_state.serial_handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
#else
    tcflush(g_state.serial_fd, TCIOFLUSH);
#endif

    return went_quiet;
}

void StartCapture() {
    if (!g_state.is_connected || g_state.is_capturing) return;

    // The sequence is drain -> SCR -> read -> drain, so the command always goes
    // out on a line with nothing stale left on it.
    //
    // Note this drains input only. There is deliberately no tcdrain/
    // FlushFileBuffers on the write: both block with no timeout, and a radio
    // that stopped accepting output would freeze the UI thread until the window
    // manager killed us. The command is three bytes and the kernel sends it
    // regardless.
    g_state.line_was_busy = !SerialDrainInput();

#ifdef RADSHOT_WINDOWS
    DWORD written = 0;
    if (!WriteFile(g_state.serial_handle, SCREENSHOT_CMD, sizeof(SCREENSHOT_CMD),
                   &written, nullptr) || written != sizeof(SCREENSHOT_CMD)) {
        SetStatus(STATUS_ERR, "Failed to send command - reconnect the radio");
        return;
    }
#else
    if (write(g_state.serial_fd, SCREENSHOT_CMD, sizeof(SCREENSHOT_CMD)) !=
        (ssize_t)sizeof(SCREENSHOT_CMD)) {
        SetStatus(STATUS_ERR,
                 "Failed to send command: %s", strerror(errno));
        return;
    }
#endif

    g_state.is_capturing = true;
    g_state.capture_bytes = 0;
    g_state.capture_last_data = glfwGetTime();
    g_state.capture_progress = 0;
    memset(g_state.capture_buffer, 0, FRAME_SIZE);
}

void UpdateCapture() {
    if (!g_state.is_capturing) return;

    bool got_data = false;

    // Read straight into the frame buffer, asking for exactly what is still
    // missing, so a read can never return more than the frame has room for.
    // Going via a fixed scratch buffer meant a chunk crossing the end of the
    // frame had its tail consumed from the port and then dropped, leaving the
    // stream misaligned for whatever came next.
    for (int i = 0; i < READS_PER_FRAME && g_state.capture_bytes < FRAME_SIZE; i++) {
        int remaining = FRAME_SIZE - g_state.capture_bytes;
        uint8_t* dst = g_state.capture_buffer + g_state.capture_bytes;

#ifdef RADSHOT_WINDOWS
        DWORD bytesRead = 0;
        if (!ReadFile(g_state.serial_handle, dst, (DWORD)remaining, &bytesRead, nullptr) ||
            bytesRead == 0) {
            break;
        }
#else
        ssize_t bytesRead = read(g_state.serial_fd, dst, (size_t)remaining);
        // On POSIX, EAGAIN/EWOULDBLOCK means no data available (not an error)
        if (bytesRead <= 0) {
            break;
        }
#endif
        g_state.capture_bytes += (int)bytesRead;
        got_data = true;
    }

    if (got_data) {
        g_state.capture_progress = (g_state.capture_bytes * 100) / FRAME_SIZE;
        g_state.capture_last_data = glfwGetTime();

        if (g_state.capture_bytes >= FRAME_SIZE) {
            AddCapture(g_state.capture_buffer);

            g_state.is_capturing = false;

            // Discard any trailing bytes so the next capture starts clean.
            SerialDrainInput();
        }
    } else if (glfwGetTime() - g_state.capture_last_data >= CAPTURE_QUIET_TIMEOUT_S) {
        if (g_state.capture_bytes == 0) {
            // Nothing at all came back: wrong baud rate and firmware
            // without the screenshot command look identical from here.
            SetStatus(STATUS_ERR,
                     "No response at %d baud - check the baud rate and that "
                     "the radio runs screenshot-enabled firmware",
                     BAUD_RATES[g_state.baud_index]);
        } else if (g_state.line_was_busy) {
            // No framing in the protocol, so anything the radio was already
            // sending is indistinguishable from the start of the frame.
            SetStatus(STATUS_ERR,
                     "Timeout: %d/%d bytes - radio was still sending when the "
                     "request went out, so the reply may be interleaved",
                     g_state.capture_bytes, FRAME_SIZE);
        } else {
            SetStatus(STATUS_ERR,
                     "Timeout: %d/%d bytes", g_state.capture_bytes, FRAME_SIZE);
        }
        g_state.is_capturing = false;

        // A partial frame leaves the radio mid-transfer; clear the leftover
        // so the next attempt is not decoding the tail of this one.
        SerialDrainInput();
    }
}

// =============================================================================
// Selection
// =============================================================================

static void SyncRenameBuffer() {
    if (g_state.selected >= 0 && g_state.selected < (int)g_state.screenshots.size()) {
        strcpy(g_state.rename_buffer, g_state.screenshots[g_state.selected]->name);
    } else {
        g_state.rename_buffer[0] = 0;
    }
}

enum SelectMode { SELECT_SINGLE, SELECT_TOGGLE, SELECT_RANGE };

// SINGLE picks just this one, TOGGLE (Ctrl-click) adds or removes it, RANGE
// (Shift-click) picks everything from the anchor to it.
static void Select(int i, SelectMode mode = SELECT_SINGLE) {
    auto& shots = g_state.screenshots;
    if (i < 0 || i >= (int)shots.size()) {
        g_state.selected = -1;
        g_state.anchor = -1;
        g_state.picked.clear();
    } else if (mode == SELECT_TOGGLE) {
        Screenshot* ss = shots[i];
        if (g_state.picked.count(ss) && g_state.picked.size() > 1) {
            g_state.picked.erase(ss);
            if (g_state.selected == i) {
                for (int j = 0; j < (int)shots.size(); j++) {
                    if (g_state.picked.count(shots[j])) { g_state.selected = j; break; }
                }
            }
        } else {
            g_state.picked.insert(ss);
            g_state.selected = i;
        }
        g_state.anchor = i;
    } else if (mode == SELECT_RANGE && g_state.anchor >= 0 && g_state.anchor < (int)shots.size()) {
        int a = std::min(g_state.anchor, i), b = std::max(g_state.anchor, i);
        g_state.picked.clear();
        for (int j = a; j <= b; j++) g_state.picked.insert(shots[j]);
        g_state.selected = i;
    } else {
        g_state.picked.clear();
        g_state.picked.insert(shots[i]);
        g_state.selected = i;
        g_state.anchor = i;
    }
    SyncRenameBuffer();
}

static void SelectAll() {
    if (g_state.screenshots.empty()) return;
    g_state.picked.clear();
    for (Screenshot* ss : g_state.screenshots) g_state.picked.insert(ss);
    if (g_state.selected < 0) g_state.selected = 0;
    SyncRenameBuffer();
}

static std::vector<Screenshot*> PickedList() {
    std::vector<Screenshot*> out;
    for (Screenshot* ss : g_state.screenshots) {
        if (g_state.picked.count(ss)) out.push_back(ss);
    }
    return out;
}

// An action on a card applies to the whole selection when that card is part
// of it, otherwise to the card alone.
static std::vector<Screenshot*> TargetsFor(Screenshot* ss) {
    if (g_state.picked.size() > 1 && g_state.picked.count(ss)) return PickedList();
    return { ss };
}

static int IndexOf(const Screenshot* ss) {
    auto& v = g_state.screenshots;
    auto it = std::find(v.begin(), v.end(), ss);
    return it == v.end() ? -1 : (int)(it - v.begin());
}

static Screenshot* SelectedScreenshot() {
    int i = g_state.selected;
    return (i >= 0 && i < (int)g_state.screenshots.size()) ? g_state.screenshots[i] : nullptr;
}

// Called when a capture completes.
static void AddCapture(const uint8_t* raw) {
    int id = g_state.next_id++;
    char name[64];
    snprintf(name, sizeof(name), "screenshot_%03d", id);
    Screenshot* ss = NewScreenshot(id, name, time(nullptr), raw, false);
    g_state.screenshots.push_back(ss);
    Select((int)g_state.screenshots.size() - 1);
    WriteRaw(ss);
    SaveIndex();
    SetStatus(STATUS_OK, "Captured %s", ss->name);
}

// Moves `list` (kept in its gallery order) next to `target`.
static void MoveScreenshots(const std::vector<Screenshot*>& list, Screenshot* target, bool after) {
    if (std::find(list.begin(), list.end(), target) != list.end()) return;
    Screenshot* primary = SelectedScreenshot();
    std::vector<Screenshot*> rest;
    for (Screenshot* ss : g_state.screenshots) {
        if (std::find(list.begin(), list.end(), ss) == list.end()) rest.push_back(ss);
    }
    auto at = std::find(rest.begin(), rest.end(), target) + (after ? 1 : 0);
    rest.insert(at, list.begin(), list.end());
    g_state.screenshots = rest;
    g_state.selected = IndexOf(primary);
    g_state.anchor = g_state.selected;
    SaveIndex();
}

// =============================================================================
// Undo
// =============================================================================

// Frees what the last delete took out; called when its undo runs out or a
// newer delete replaces it.
static void FinalizeUndo() {
    for (Removed& r : g_state.undo) delete r.ss;
    g_state.undo.clear();
    g_state.undo_until = 0.0;
}

// Deletes take effect at once and can be undone for a few seconds, which is
// quicker than confirming every one up front.
static void RemoveScreenshots(const std::vector<Screenshot*>& list, const char* message) {
    if (list.empty()) return;
    FinalizeUndo();
    Screenshot* primary = SelectedScreenshot();
    for (Screenshot* ss : list) {
        int i = IndexOf(ss);
        if (i >= 0) g_state.undo.push_back({ ss, i });
    }
    std::sort(g_state.undo.begin(), g_state.undo.end(),
              [](const Removed& a, const Removed& b) { return a.index < b.index; });
    if (g_state.undo.empty()) return;

    auto& v = g_state.screenshots;
    v.erase(std::remove_if(v.begin(), v.end(), [&](Screenshot* s) {
        return std::find(list.begin(), list.end(), s) != list.end();
    }), v.end());
    for (Removed& r : g_state.undo) RemoveRaw(r.ss);
    SaveIndex();

    int sel = IndexOf(primary);
    if (sel < 0) sel = std::min(g_state.undo.front().index, (int)v.size() - 1);
    Select(sel);
    g_state.export_dirty = true;

    strncpy(g_state.undo_message, message, sizeof(g_state.undo_message) - 1);
    g_state.undo_until = glfwGetTime() + UNDO_SECONDS;
}

static void DeleteMany(const std::vector<Screenshot*>& list) {
    char msg[128];
    if (list.size() == 1) snprintf(msg, sizeof(msg), "Deleted %s", list[0]->name);
    else snprintf(msg, sizeof(msg), "Deleted %d screenshots", (int)list.size());
    RemoveScreenshots(list, msg);
}

static void ClearAll() {
    char msg[128];
    int n = (int)g_state.screenshots.size();
    snprintf(msg, sizeof(msg), "Cleared %d screenshot%s", n, n == 1 ? "" : "s");
    RemoveScreenshots(g_state.screenshots, msg);
}

static void UndoRemove() {
    if (g_state.undo.empty()) return;
    std::vector<Removed> removed;
    removed.swap(g_state.undo);
    g_state.undo_until = 0.0;
    // Ascending order puts each one back at the index it had.
    auto& v = g_state.screenshots;
    for (Removed& r : removed) {
        v.insert(v.begin() + std::min(r.index, (int)v.size()), r.ss);
        WriteRaw(r.ss);
    }
    SaveIndex();
    // Restored ones come back selected, ready to act on again.
    Select(IndexOf(removed.back().ss));
    if (removed.size() > 1) {
        g_state.picked.clear();
        for (Removed& r : removed) g_state.picked.insert(r.ss);
    }
    g_state.export_dirty = true;
    SetStatus(STATUS_OK, "Restored %d screenshot%s", (int)removed.size(), removed.size() == 1 ? "" : "s");
}

static int UnsavedCount() {
    int n = 0;
    for (Screenshot* ss : g_state.screenshots) if (!ss->saved) n++;
    return n;
}

static void MarkSaved(Screenshot* ss) {
    ss->saved = true;
}

// =============================================================================
// File Operations
// =============================================================================

bool BrowseForFolder(char* path, size_t pathSize, const char* startDir) {
    const char* dir = (startDir && startDir[0]) ? startDir : nullptr;
    const char* result = tinyfd_selectFolderDialog("Select Folder", dir);
    if (result) {
        strncpy(path, result, pathSize - 1);
        path[pathSize - 1] = 0;
        return true;
    }
    return false;
}

// Names are free text, so make them safe as file names.
static std::string SafeName(const char* name) {
    std::string s(name);
    for (char& c : s) {
        if (strchr("\\/:*?\"<>|", c) || (unsigned char)c < 32) c = '_';
    }
    if (s.empty()) s = "screenshot";
    return s;
}

static std::string FileStamp() {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d_%H%M", t);
    return buf;
}

static std::vector<uint8_t> ExportRGBA(const Screenshot* ss) {
    return ScaleRGBA(ss->rgba, DISPLAY_WIDTH, DISPLAY_HEIGHT, g_state.export_scale);
}

static bool WritePng(const char* path, const Screenshot* ss) {
    std::vector<uint8_t> img = ExportRGBA(ss);
    int w = DISPLAY_WIDTH * g_state.export_scale, h = DISPLAY_HEIGHT * g_state.export_scale;
    return stbi_write_png(path, w, h, 4, img.data(), w * 4) != 0;
}

static std::vector<uint8_t> EncodePng(const uint8_t* rgba, int w, int h) {
    int len = 0;
    unsigned char* png = stbi_write_png_to_mem(rgba, w * 4, w, h, 4, &len);
    std::vector<uint8_t> out;
    if (png) {
        out.assign(png, png + len);
        STBIW_FREE(png);
    }
    return out;
}

static bool WriteFile(const char* path, const std::vector<uint8_t>& data) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}

// --- Zip (stored, no compression: PNGs are already compressed) ---

struct ZipEntry {
    std::string name;
    std::vector<uint8_t> data;
    time_t date;
};

static uint32_t Crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static std::vector<uint8_t> MakeZip(const std::vector<ZipEntry>& files) {
    std::vector<uint8_t> out, central;
    auto u16 = [](std::vector<uint8_t>& v, uint32_t x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); };
    auto u32 = [&](std::vector<uint8_t>& v, uint32_t x) { u16(v, x & 0xFFFF); u16(v, x >> 16); };

    for (const ZipEntry& f : files) {
        struct tm* t = localtime(&f.date);
        uint32_t dosTime = (t->tm_hour << 11) | (t->tm_min << 5) | (t->tm_sec >> 1);
        uint32_t dosDate = ((t->tm_year - 80) << 9) | ((t->tm_mon + 1) << 5) | t->tm_mday;
        uint32_t crc = Crc32(f.data.data(), f.data.size());
        uint32_t size = (uint32_t)f.data.size();
        uint32_t offset = (uint32_t)out.size();

        u32(out, 0x04034b50); u16(out, 20); u16(out, 0x0800);  // UTF-8 names
        u16(out, 0); u16(out, dosTime); u16(out, dosDate);    // stored
        u32(out, crc); u32(out, size); u32(out, size);
        u16(out, (uint32_t)f.name.size()); u16(out, 0);
        out.insert(out.end(), f.name.begin(), f.name.end());
        out.insert(out.end(), f.data.begin(), f.data.end());

        u32(central, 0x02014b50); u16(central, 20); u16(central, 20); u16(central, 0x0800);
        u16(central, 0); u16(central, dosTime); u16(central, dosDate);
        u32(central, crc); u32(central, size); u32(central, size);
        u16(central, (uint32_t)f.name.size()); u16(central, 0); u16(central, 0);
        u16(central, 0); u16(central, 0); u32(central, 0); u32(central, offset);
        central.insert(central.end(), f.name.begin(), f.name.end());
    }
    uint32_t centralOffset = (uint32_t)out.size();
    out.insert(out.end(), central.begin(), central.end());
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0);
    u16(out, (uint32_t)files.size()); u16(out, (uint32_t)files.size());
    u32(out, (uint32_t)central.size()); u32(out, centralOffset); u16(out, 0);
    return out;
}

// Asks for a folder only once per action, starting from the last one used.
static bool PickSaveFolder(char* folder, size_t size) {
    if (!BrowseForFolder(folder, size, g_state.last_save_directory)) return false;
    strncpy(g_state.last_save_directory, folder, sizeof(g_state.last_save_directory) - 1);
    return true;
}

void SaveScreenshot(Screenshot* ss) {
    char folder[PATH_MAX_LEN] = {0};
    if (!PickSaveFolder(folder, sizeof(folder))) return;
    char path[PATH_MAX_LEN];
    snprintf(path, sizeof(path), "%s/%s.png", folder, SafeName(ss->name).c_str());
    if (WritePng(path, ss)) {
        MarkSaved(ss);
        SaveIndex();
        SetStatus(STATUS_OK, "Saved %s.png", ss->name);
    } else {
        SetStatus(STATUS_ERR, "Failed to save %s", path);
    }
}

// By default one zip; the .zip toggle beside Save All writes separate PNGs.
static void SaveMany(const std::vector<Screenshot*>& shots) {
    if (shots.empty()) return;
    if (shots.size() == 1) { SaveScreenshot(shots[0]); return; }
    char folder[PATH_MAX_LEN] = {0};
    if (!PickSaveFolder(folder, sizeof(folder))) return;

    // Unique names, so nothing in the zip (or the folder) collides.
    std::map<std::string, int> used;
    auto fileName = [&](const Screenshot* ss) {
        std::string base = SafeName(ss->name);
        int n = ++used[base];
        return n == 1 ? base + ".png" : base + " (" + std::to_string(n) + ").png";
    };
    int w = DISPLAY_WIDTH * g_state.export_scale, h = DISPLAY_HEIGHT * g_state.export_scale;

    if (!g_state.save_as_zip) {
        int saved = 0;
        for (Screenshot* ss : shots) {
            std::string path = std::string(folder) + "/" + fileName(ss);
            if (WritePng(path.c_str(), ss)) { MarkSaved(ss); saved++; }
        }
        SaveIndex();
        SetStatus(saved == (int)shots.size() ? STATUS_OK : STATUS_ERR,
                  "Saved %d/%d screenshots", saved, (int)shots.size());
        return;
    }

    std::vector<ZipEntry> files;
    for (Screenshot* ss : shots) {
        std::vector<uint8_t> img = ExportRGBA(ss);
        files.push_back({ fileName(ss), EncodePng(img.data(), w, h), ss->date });
    }
    std::string zipName = "at168shot_" + FileStamp() + ".zip";
    std::string path = std::string(folder) + "/" + zipName;
    if (WriteFile(path.c_str(), MakeZip(files))) {
        for (Screenshot* ss : shots) MarkSaved(ss);
        SaveIndex();
        SetStatus(STATUS_OK, "Saved %d screenshots to %s", (int)shots.size(), zipName.c_str());
    } else {
        SetStatus(STATUS_ERR, "Failed to write %s", path.c_str());
    }
}

static void SaveAll() { SaveMany(g_state.screenshots); }

// --- Clipboard ---

static void CopyImageToClipboard(const uint8_t* rgba, int w, int h, const char* what) {
#ifdef RADSHOT_WINDOWS
    // Calculate DIB size (BGR, 24-bit, DWORD-aligned rows)
    int rowBytes = ((w * 3 + 3) / 4) * 4;
    size_t dibSize = sizeof(BITMAPINFOHEADER) + (size_t)rowBytes * h;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, dibSize);
    if (!hMem) {
        SetStatus(STATUS_ERR, "Failed to allocate clipboard memory");
        return;
    }

    uint8_t* pMem = (uint8_t*)GlobalLock(hMem);
    if (!pMem) {
        GlobalFree(hMem);
        SetStatus(STATUS_ERR, "Failed to lock clipboard memory");
        return;
    }

    // Fill BITMAPINFOHEADER
    BITMAPINFOHEADER* bih = (BITMAPINFOHEADER*)pMem;
    memset(bih, 0, sizeof(BITMAPINFOHEADER));
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = w;
    bih->biHeight = h;
    bih->biPlanes = 1;
    bih->biBitCount = 24;
    bih->biCompression = BI_RGB;

    // Convert RGBA to BGR and flip vertically
    uint8_t* pixels = pMem + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < h; y++) {
        int srcY = h - 1 - y;
        for (int x = 0; x < w; x++) {
            size_t srcIdx = ((size_t)srcY * w + x) * 4;
            size_t dstIdx = (size_t)y * rowBytes + x * 3;
            pixels[dstIdx + 0] = rgba[srcIdx + 2];  // B
            pixels[dstIdx + 1] = rgba[srcIdx + 1];  // G
            pixels[dstIdx + 2] = rgba[srcIdx + 0];  // R
        }
    }

    GlobalUnlock(hMem);

    HWND hwnd = glfwGetWin32Window(g_state.window);
    if (!OpenClipboard(hwnd)) {
        GlobalFree(hMem);
        SetStatus(STATUS_ERR, "Failed to open clipboard");
        return;
    }

    EmptyClipboard();
    if (SetClipboardData(CF_DIB, hMem)) {
        SetStatus(STATUS_OK, "Copied %s to clipboard", what);
    } else {
        GlobalFree(hMem);
        SetStatus(STATUS_ERR, "Failed to set clipboard data");
    }
    CloseClipboard();

#else
    // Write to temp PNG, then use platform tools to copy to clipboard
    char tmpPath[PATH_MAX_LEN];
    snprintf(tmpPath, sizeof(tmpPath), "/tmp/at168shot_clipboard.png");
    if (!stbi_write_png(tmpPath, w, h, 4, rgba, w * 4)) {
        SetStatus(STATUS_ERR, "Failed to write temp file for clipboard");
        return;
    }

    int ret = -1;
#ifdef RADSHOT_MACOS
    char cmd[PATH_MAX_LEN];
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'set the clipboard to (read (POSIX file \"%s\") as «class PNGf»)'", tmpPath);
    ret = system(cmd);
#else
    // Try xclip first, then wl-copy
    char cmd[PATH_MAX_LEN];
    snprintf(cmd, sizeof(cmd),
        "xclip -selection clipboard -t image/png -i '%s' 2>/dev/null || "
        "wl-copy --type image/png < '%s' 2>/dev/null", tmpPath, tmpPath);
    ret = system(cmd);
#endif

    if (ret == 0) {
        SetStatus(STATUS_OK, "Copied %s to clipboard", what);
    } else {
        SetStatus(STATUS_ERR, "Clipboard copy failed (install xclip or wl-copy)");
    }

    remove(tmpPath);
#endif
}

static void CopyScreenshot(const Screenshot* ss) {
    std::vector<uint8_t> img = ExportRGBA(ss);
    CopyImageToClipboard(img.data(), DISPLAY_WIDTH * g_state.export_scale,
                         DISPLAY_HEIGHT * g_state.export_scale, ss->name);
}

// =============================================================================
// Sequence Export: contact sheet, strip, animated GIF
// =============================================================================

struct SheetLayout {
    int cols, rows, tw, th, text_scale, label_h, w, h;
    bool too_big;
};

constexpr float LABEL_FONT_SIZE = 13.0f;

static SheetLayout ComputeLayout(int n, const SeqOptions& q) {
    SheetLayout L;
    L.tw = DISPLAY_WIDTH * q.scale;
    L.th = DISPLAY_HEIGHT * q.scale;
    if (q.format == SEQ_STRIP) L.cols = q.vertical ? 1 : n;
    else L.cols = std::max(1, std::min(q.columns, n));
    L.rows = (n + L.cols - 1) / L.cols;
    // Labels use the UI font, scaled up in whole steps so it stays crisp.
    L.text_scale = std::max(1, q.scale / 2);
    L.label_h = q.labels ? (int)(LABEL_FONT_SIZE * L.text_scale * 1.6f) : 0;
    L.w = L.cols * L.tw + (L.cols + 1) * q.gap;
    L.h = L.rows * (L.th + L.label_h) + (L.rows + 1) * q.gap;
    L.too_big = L.w > MAX_EXPORT_SIDE || L.h > MAX_EXPORT_SIDE || (double)L.w * L.h > MAX_EXPORT_AREA;
    return L;
}

// Draws text into an RGBA buffer using the glyphs ImGui already rasterized
// for the UI font, so labels need no font file of their own.
struct LabelFont {
    ImFontBaked* baked = nullptr;
    ImTextureData* tex = nullptr;
    float density = 1.0f;   // atlas pixels per font unit
};

static LabelFont GetLabelFont() {
    LabelFont lf;
    lf.baked = ImGui::GetFont()->GetFontBaked(LABEL_FONT_SIZE, 1.0f);
    lf.tex = ImGui::GetIO().Fonts->TexData;
    return lf;
}

static float LabelWidth(LabelFont& lf, const char* text) {
    float w = 0;
    for (const char* p = text; *p; p++) w += lf.baked->FindGlyph((ImWchar)(unsigned char)*p)->AdvanceX;
    return w;
}

static void DrawLabel(uint8_t* img, int W, int H, int cx, int top, const char* text,
                      int maxWidth, int k, const uint8_t rgb[3]) {
    LabelFont lf = GetLabelFont();
    if (!lf.baked || !lf.tex || !lf.tex->Pixels) return;

    // Truncate with "..." to fit the tile.
    std::string s(text);
    if (LabelWidth(lf, s.c_str()) * k > maxWidth) {
        while (!s.empty() && LabelWidth(lf, (s + "...").c_str()) * k > maxWidth) s.pop_back();
        s += "...";
    }
    float x = cx - LabelWidth(lf, s.c_str()) * k / 2.0f;

    for (char ch : s) {
        // FindGlyph can bake new glyphs and grow the atlas; re-read it each time.
        ImFontGlyph* g = lf.baked->FindGlyph((ImWchar)(unsigned char)ch);
        ImTextureData* tex = ImGui::GetIO().Fonts->TexData;
        if (g->Visible && tex && tex->Pixels) {
            int u0 = (int)(g->U0 * tex->Width + 0.5f), v0 = (int)(g->V0 * tex->Height + 0.5f);
            int u1 = (int)(g->U1 * tex->Width + 0.5f), v1 = (int)(g->V1 * tex->Height + 0.5f);
            int gx = (int)(x + g->X0 * k), gy = top + (int)(g->Y0 * k);
            for (int v = v0; v < v1; v++) {
                for (int u = u0; u < u1; u++) {
                    const uint8_t* src = (const uint8_t*)tex->GetPixelsAt(u, v);
                    int a = tex->BytesPerPixel == 4 ? src[3] : src[0];
                    if (!a) continue;
                    for (int dy = 0; dy < k; dy++) {
                        for (int dx = 0; dx < k; dx++) {
                            int px = gx + (u - u0) * k + dx, py = gy + (v - v0) * k + dy;
                            if (px < 0 || py < 0 || px >= W || py >= H) continue;
                            uint8_t* d = img + ((size_t)py * W + px) * 4;
                            for (int c = 0; c < 3; c++) d[c] = (uint8_t)((rgb[c] * a + d[c] * (255 - a)) / 255);
                            d[3] = (uint8_t)std::max<int>(d[3], a);
                        }
                    }
                }
            }
        }
        x += g->AdvanceX * k;
    }
}

// Contact sheet or strip, in gallery order. Empty if too large.
static std::vector<uint8_t> RenderSheet(const std::vector<Screenshot*>& list, const SeqOptions& q, SheetLayout* out) {
    SheetLayout L = ComputeLayout((int)list.size(), q);
    if (out) *out = L;
    if (L.too_big || list.empty()) return {};

    std::vector<uint8_t> img((size_t)L.w * L.h * 4, 0);
    if (q.background != BG_NONE) {
        uint8_t v = q.background == BG_LIGHT ? 0xFF : 0x0F;
        for (size_t i = 0; i < img.size(); i += 4) { img[i] = img[i + 1] = img[i + 2] = v; img[i + 3] = 0xFF; }
    }
    const uint8_t dark[3] = { 0x22, 0x22, 0x22 }, light[3] = { 0xDD, 0xDD, 0xDD }, mid[3] = { 0x88, 0x88, 0x88 };
    const uint8_t* textColor = q.background == BG_LIGHT ? dark : q.background == BG_DARK ? light : mid;

    for (size_t i = 0; i < list.size(); i++) {
        int x0 = q.gap + (int)(i % L.cols) * (L.tw + q.gap);
        int y0 = q.gap + (int)(i / L.cols) * (L.th + L.label_h + q.gap);
        const uint8_t* src = list[i]->rgba;
        for (int y = 0; y < L.th; y++) {
            const uint8_t* row = src + (y / q.scale) * DISPLAY_WIDTH * 4;
            uint8_t* dst = img.data() + ((size_t)(y0 + y) * L.w + x0) * 4;
            for (int x = 0; x < L.tw; x++) memcpy(dst + x * 4, row + (x / q.scale) * 4, 4);
        }
        if (q.labels) {
            int top = y0 + L.th + (L.label_h - (int)(LABEL_FONT_SIZE * L.text_scale)) / 2;
            DrawLabel(img.data(), L.w, L.h, x0 + L.tw / 2, top, list[i]->name, L.tw, L.text_scale, textColor);
        }
    }
    return img;
}

// GIF image data: LZW codes packed LSB first, cut into 255-byte sub-blocks.
// Follows the reference encoder's code-size timing (bump before adding the
// code that no longer fits; clear and restart when the table is full).
static void LzwSubBlocks(const std::vector<uint8_t>& indices, int minCodeSize, std::vector<uint8_t>& out) {
    const int clearCode = 1 << minCodeSize, eoiCode = clearCode + 1;
    int codeSize = minCodeSize + 1, nextCode = eoiCode + 1;
    std::unordered_map<uint32_t, int> table;
    std::vector<uint8_t> buf;
    uint32_t cur = 0;
    int bits = 0;
    auto emit = [&](int code) {
        cur |= (uint32_t)code << bits;
        bits += codeSize;
        while (bits >= 8) { buf.push_back(cur & 0xFF); cur >>= 8; bits -= 8; }
    };

    emit(clearCode);
    int prefix = indices[0];
    for (size_t i = 1; i < indices.size(); i++) {
        int k = indices[i];
        uint32_t key = ((uint32_t)prefix << 8) | (uint32_t)k;
        auto it = table.find(key);
        if (it != table.end()) { prefix = it->second; continue; }
        emit(prefix);
        if (nextCode == 4096) {
            emit(clearCode);
            table.clear();
            nextCode = eoiCode + 1;
            codeSize = minCodeSize + 1;
        } else {
            if (nextCode >= (1 << codeSize)) codeSize++;
            table[key] = nextCode++;
        }
        prefix = k;
    }
    emit(prefix);
    emit(eoiCode);
    if (bits > 0) buf.push_back(cur & 0xFF);

    for (size_t i = 0; i < buf.size(); i += 255) {
        size_t n = std::min<size_t>(255, buf.size() - i);
        out.push_back((uint8_t)n);
        out.insert(out.end(), buf.begin() + i, buf.begin() + i + n);
    }
    out.push_back(0);  // block terminator
}

// Animated GIF, one frame per screenshot, looping forever. The radio's colours
// usually fit a 256 entry palette exactly; if not, the most used 256 are kept
// and the rest mapped to the nearest of those.
static std::vector<uint8_t> EncodeGif(const std::vector<Screenshot*>& list, int scale, int delayMs) {
    const int w = DISPLAY_WIDTH, h = DISPLAY_HEIGHT, W = w * scale, H = h * scale;
    auto rgbAt = [](const uint8_t* p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; };

    std::unordered_map<uint32_t, int> counts;
    for (const Screenshot* ss : list) {
        for (int i = 0; i < RGBA_SIZE; i += 4) counts[rgbAt(ss->rgba + i)]++;
    }
    std::vector<std::pair<uint32_t, int>> byCount(counts.begin(), counts.end());
    std::sort(byCount.begin(), byCount.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if (byCount.size() > 256) byCount.resize(256);
    std::vector<uint32_t> colors;
    std::unordered_map<uint32_t, int> index;
    for (auto& e : byCount) { index[e.first] = (int)colors.size(); colors.push_back(e.first); }
    auto lookup = [&](uint32_t c) {
        auto it = index.find(c);
        if (it != index.end()) return it->second;
        int r = c >> 16, g = (c >> 8) & 255, b = c & 255, best = 0;
        long bestD = LONG_MAX;
        for (int k = 0; k < (int)colors.size(); k++) {
            uint32_t p = colors[k];
            long dr = r - (int)(p >> 16), dg = g - (int)((p >> 8) & 255), db = b - (int)(p & 255);
            long d = dr * dr + dg * dg + db * db;
            if (d < bestD) { bestD = d; best = k; }
        }
        index[c] = best;
        return best;
    };

    std::vector<uint8_t> out;
    auto u16 = [&](int v) { out.push_back(v & 255); out.push_back((v >> 8) & 255); };
    // Header and logical screen: global 256 colour table follows.
    const char* sig = "GIF89a";
    out.insert(out.end(), sig, sig + 6);
    u16(W); u16(H);
    out.push_back(0xF7); out.push_back(0); out.push_back(0);
    for (int i = 0; i < 256; i++) {
        uint32_t c = i < (int)colors.size() ? colors[i] : 0;
        out.push_back(c >> 16); out.push_back((c >> 8) & 255); out.push_back(c & 255);
    }
    // NETSCAPE2.0 extension: loop forever.
    const uint8_t loop[] = { 0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01, 0, 0, 0 };
    out.insert(out.end(), loop, loop + sizeof(loop));

    int delay = (delayMs + 5) / 10;  // GIF counts in hundredths
    std::vector<uint8_t> idx((size_t)W * H);
    for (const Screenshot* ss : list) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint8_t k = (uint8_t)lookup(rgbAt(ss->rgba + (y * w + x) * 4));
                for (int sy = 0; sy < scale; sy++) {
                    memset(idx.data() + (size_t)(y * scale + sy) * W + x * scale, k, scale);
                }
            }
        }
        // Graphic control (frame delay), then a full-size image descriptor.
        out.push_back(0x21); out.push_back(0xF9); out.push_back(0x04); out.push_back(0x04);
        u16(delay); out.push_back(0); out.push_back(0);
        out.push_back(0x2C); u16(0); u16(0); u16(W); u16(H); out.push_back(0);
        out.push_back(8);
        LzwSubBlocks(idx, 8, out);
    }
    out.push_back(0x3B);
    return out;
}

// Several screenshots go on the clipboard as one contact sheet (or strip),
// laid out with the last used export options.
static void CopyMany(const std::vector<Screenshot*>& list) {
    if (list.empty()) return;
    if (list.size() == 1) { CopyScreenshot(list[0]); return; }
    SeqOptions q = g_state.seq;
    if (q.format == SEQ_GIF) q.format = SEQ_SHEET;
    SheetLayout L;
    std::vector<uint8_t> img = RenderSheet(list, q, &L);
    if (img.empty()) {
        SetStatus(STATUS_ERR, "Too large to copy as one image - lower the scale in Export");
        return;
    }
    char what[64];
    snprintf(what, sizeof(what), "%d screenshots as a %s", (int)list.size(), q.format == SEQ_STRIP ? "strip" : "contact sheet");
    CopyImageToClipboard(img.data(), L.w, L.h, what);
}

static void OpenExport(const std::vector<Screenshot*>& list) {
    g_state.export_list = list.empty() ? g_state.screenshots : list;
    if (g_state.export_list.empty()) return;
    g_state.export_dirty = true;
    g_state.gif_frame = 0;
    g_state.gif_next = 0.0;
    g_state.open_export = true;
}

static void SaveExport() {
    const SeqOptions& q = g_state.seq;
    const auto& list = g_state.export_list;
    bool gif = q.format == SEQ_GIF;
    std::string def = std::string(g_state.last_save_directory[0] ? g_state.last_save_directory : ".") + "/at168shot_" +
                      (gif ? "animation" : q.format == SEQ_STRIP ? "strip" : "sheet") + "_" + FileStamp() + (gif ? ".gif" : ".png");
    const char* patterns[] = { gif ? "*.gif" : "*.png" };
    const char* path = tinyfd_saveFileDialog("Save export", def.c_str(), 1, patterns, gif ? "GIF image" : "PNG image");
    if (!path) return;
    std::string chosen = path;

    // Remember the folder for next time.
    size_t slash = chosen.find_last_of("/\\");
    if (slash != std::string::npos) {
        strncpy(g_state.last_save_directory, chosen.substr(0, slash).c_str(), sizeof(g_state.last_save_directory) - 1);
    }

    bool ok;
    if (gif) {
        ok = WriteFile(chosen.c_str(), EncodeGif(list, q.scale, q.delay_ms));
    } else {
        SheetLayout L;
        std::vector<uint8_t> img = RenderSheet(list, q, &L);
        ok = !img.empty() && stbi_write_png(chosen.c_str(), L.w, L.h, 4, img.data(), L.w * 4) != 0;
    }
    if (ok) SetStatus(STATUS_OK, "Saved %s", chosen.substr(slash == std::string::npos ? 0 : slash + 1).c_str());
    else SetStatus(STATUS_ERR, "Failed to save %s", chosen.c_str());
}

// =============================================================================
// Ports: auto-reconnect and unplug detection
// =============================================================================

static void ConnectSelected() {
    if (g_state.selected_port < 0) return;
    if (SerialConnect(g_state.com_ports[g_state.selected_port].c_str())) {
        g_state.reconnect = true;
        SaveSettings();
    }
}

// Rescans the port list. A vanished port means the radio was unplugged; the
// last port reappearing (or being there at startup) reconnects to it, unless
// the user disconnected from it on purpose.
static void PollPorts(bool startup) {
    std::string selectedName = g_state.selected_port >= 0 ? g_state.com_ports[g_state.selected_port] : "";
    std::vector<std::string> before = g_state.com_ports;
    EnumerateComPorts();
    auto& ports = g_state.com_ports;
    auto has = [](const std::vector<std::string>& v, const std::string& s) {
        return !s.empty() && std::find(v.begin(), v.end(), s) != v.end();
    };

    g_state.selected_port = -1;
    for (int i = 0; i < (int)ports.size(); i++) {
        if (ports[i] == selectedName) g_state.selected_port = i;
    }

    std::string last = g_state.last_port_name;
    if (g_state.is_connected && !has(ports, last)) {
        SerialDisconnect();
        SetStatus(STATUS_ERR, "Device unplugged");
    }
    if (g_state.is_connected || !g_state.auto_connect || !g_state.reconnect || !has(ports, last)) return;
    if (!startup && has(before, last)) return;  // only when it (re)appears, not every poll

    for (int i = 0; i < (int)ports.size(); i++) {
        if (ports[i] == last) g_state.selected_port = i;
    }
    ConnectSelected();
}

// =============================================================================
// UI Helpers
// =============================================================================

static bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, COL_PRIMARY);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COL_PRIMARY_HOVER);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, COL_PRIMARY_HOVER);
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return pressed;
}

static bool DangerButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Text, COL_DANGER_TEXT);
    ImGui::PushStyleColor(ImGuiCol_Button, COL_DANGER_BG);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COL_DANGER_HOVER);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, COL_DANGER_HOVER);
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

// Text cut to `width` with "..." at the end.
static std::string Ellipsize(const char* text, float width) {
    if (ImGui::CalcTextSize(text).x <= width) return text;
    std::string s(text);
    while (!s.empty() && ImGui::CalcTextSize((s + "...").c_str()).x > width) s.pop_back();
    return s + "...";
}

// HH:MM for today's captures; older ones kept from earlier sessions show the date.
static std::string CaptureTime(time_t t) {
    time_t now = time(nullptr);
    struct tm a = *localtime(&t), b = *localtime(&now);
    char buf[16];
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) strftime(buf, sizeof(buf), "%H:%M", &a);
    else strftime(buf, sizeof(buf), "%d/%m", &a);
    return buf;
}

static ImTextureRef Tex(GLuint t) { return ImTextureRef((ImTextureID)(intptr_t)t); }

// Right-aligns a group of widgets measured on the previous frame.
struct RightAlign {
    float& width;
    explicit RightAlign(float& w) : width(w) {
        ImGui::SameLine();
        float avail = ImGui::GetContentRegionAvail().x;
        if (width > 0 && avail > width) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width);
        ImGui::BeginGroup();
    }
    ~RightAlign() {
        ImGui::EndGroup();
        width = ImGui::GetItemRectSize().x;
    }
};

// =============================================================================
// UI Rendering
// =============================================================================

static void RenderToolbar() {
    const float s = g_dpi_scale;
    ImGui::BeginDisabled(!g_state.is_connected || g_state.is_capturing);
    if (PrimaryButton("Take Screenshot", ImVec2(160 * s, 34 * s))) StartCapture();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (g_state.is_capturing) {
        char overlay[32];
        snprintf(overlay, sizeof(overlay), "Capturing... %d%%", g_state.capture_progress);
        ImGui::ProgressBar(g_state.capture_progress / 100.0f, ImVec2(180 * s, 0), overlay);
    } else {
        ImGui::TextDisabled("or press Space");
    }

    static float rightWidth = 0;
    RightAlign right(rightWidth);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Thumbs");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120 * s);
    if (ImGui::SliderInt("##thumbs", &g_state.thumb_size, THUMB_MIN, THUMB_MAX, "%dpx")) {
        g_state.thumb_size = (g_state.thumb_size + THUMB_STEP / 2) / THUMB_STEP * THUMB_STEP;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) SaveSettings();

    bool empty = g_state.screenshots.empty();
    ImGui::SameLine(0, 16 * s);
    ImGui::BeginDisabled(empty);
    if (ImGui::Button("Save All")) SaveAll();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Checkbox(".zip", &g_state.save_as_zip)) SaveSettings();
    ImGui::SetItemTooltip("Save All writes one .zip; untick to save each PNG separately");
    ImGui::SameLine();
    ImGui::BeginDisabled(empty);
    if (ImGui::Button("Export...")) OpenExport(g_state.picked.size() > 1 ? PickedList() : g_state.screenshots);
    ImGui::SetItemTooltip("Contact sheet, strip or animated GIF of the selected screenshots (or all)");
    ImGui::SameLine();
    if (ImGui::Button("Clear All")) ClearAll();
    ImGui::EndDisabled();
}

static void RenderSelectionBar() {
    int n = (int)g_state.picked.size();
    if (n < 2) return;
    const float s = g_dpi_scale;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(COL_CARD_PICKED));
    ImGui::BeginChild("selbar", ImVec2(0, ImGui::GetFrameHeight() + 12 * s), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%d selected", n);
    ImGui::SameLine();
    if (ImGui::Button("Save")) SaveMany(PickedList());
    ImGui::SameLine();
    if (ImGui::Button("Copy as sheet")) CopyMany(PickedList());
    ImGui::SameLine();
    if (ImGui::Button("Export...")) OpenExport(PickedList());
    ImGui::SameLine();
    if (DangerButton("Delete")) DeleteMany(PickedList());
    ImGui::SameLine();
    ImGui::TextDisabled("Ctrl/Shift-click to adjust - drag to reorder - Esc to clear");
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// Space between a card's thumbnail, text row and buttons.
static float CardSpacing() { return 4 * g_dpi_scale; }

static float CardHeight() {
    return 6 * g_dpi_scale * 2 + g_state.thumb_size * g_dpi_scale * 1.25f + CardSpacing() * 2 +
           ImGui::GetTextLineHeight() + ImGui::GetFrameHeight();
}

static void RenderCard(int i, float cardW, float cardH) {
    const float s = g_dpi_scale;
    Screenshot* ss = g_state.screenshots[i];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = 6 * s;
    const float thumbW = g_state.thumb_size * s, thumbH = thumbW * 1.25f;

    ImGui::PushID(ss->id);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1(p0.x + cardW, p0.y + cardH);

    // The whole card is one button; the small buttons are layered on top.
    ImGui::SetNextItemAllowOverlap();
    bool clicked = ImGui::InvisibleButton("card", ImVec2(cardW, cardH));
    bool hovered = ImGui::IsItemHovered();
    if (clicked) {
        ImGuiIO& io = ImGui::GetIO();
        Select(i, io.KeyShift ? SELECT_RANGE : (io.KeyCtrl || io.KeySuper) ? SELECT_TOGGLE : SELECT_SINGLE);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        // Right-clicking inside a multi-selection keeps it, so the menu acts on all.
        if (!g_state.picked.count(ss)) Select(i);
        g_state.menu_targets = TargetsFor(ss);
        g_state.open_card_menu = true;
    }

    // Drag to reorder (the whole selection when the card is part of it).
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
        if (!g_state.picked.count(ss)) Select(i);
        std::vector<Screenshot*> list = TargetsFor(ss);
        g_state.dragging.clear();
        for (Screenshot* d : list) g_state.dragging.push_back(d->id);
        ImGui::SetDragDropPayload("AT168_SHOTS", g_state.dragging.data(), g_state.dragging.size() * sizeof(int));
        ImGui::Image(Tex(ss->texture), ImVec2(64 * s, 80 * s));
        if (list.size() > 1) ImGui::Text("%d screenshots", (int)list.size());
        ImGui::EndDragDropSource();
    }
    bool dropBefore = false, dropAfter = false;
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("AT168_SHOTS",
            ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
        if (payload) {
            bool after = ImGui::GetIO().MousePos.x > (p0.x + p1.x) / 2;
            std::vector<Screenshot*> list;
            const int* ids = (const int*)payload->Data;
            for (int k = 0; k < payload->DataSize / (int)sizeof(int); k++) {
                for (Screenshot* d : g_state.screenshots) if (d->id == ids[k]) list.push_back(d);
            }
            bool self = std::find(list.begin(), list.end(), ss) != list.end();
            if (!self) (after ? dropAfter : dropBefore) = true;
            if (payload->IsDelivery() && !self) MoveScreenshots(list, ss, after);
        }
        ImGui::EndDragDropTarget();
    }

    bool picked = g_state.picked.count(ss) > 0;
    bool isSelected = g_state.selected == i;
    bool beingDragged = (ImGui::GetDragDropPayload() != nullptr) &&
        std::find(g_state.dragging.begin(), g_state.dragging.end(), ss->id) != g_state.dragging.end();

    // Card background and selection outline
    dl->AddRectFilled(p0, p1, picked ? COL_CARD_PICKED : hovered ? COL_CARD_HOVER : COL_CARD);
    if (isSelected) dl->AddRect(p0, p1, COL_ACCENT, 0, 0, 1.0f * s);
    if (dropBefore) dl->AddLine(ImVec2(p0.x - 4 * s, p0.y), ImVec2(p0.x - 4 * s, p1.y), COL_ACCENT, 3 * s);
    if (dropAfter) dl->AddLine(ImVec2(p1.x + 4 * s, p0.y), ImVec2(p1.x + 4 * s, p1.y), COL_ACCENT, 3 * s);

    // Thumbnail
    ImVec2 t0(p0.x + (cardW - thumbW) / 2, p0.y + pad);
    dl->AddImage(Tex(ss->texture), t0, ImVec2(t0.x + thumbW, t0.y + thumbH), ImVec2(0, 0), ImVec2(1, 1),
                 beingDragged ? IM_COL32(255, 255, 255, 100) : IM_COL32_WHITE);

    // Name and capture time; a blue dot until it has been saved.
    float textY = t0.y + thumbH + CardSpacing();
    std::string time = CaptureTime(ss->date);
    float timeW = ImGui::CalcTextSize(time.c_str()).x + (ss->saved ? 0 : 10 * s);
    float nameW = cardW - 2 * pad - timeW - 6 * s;
    dl->AddText(ImVec2(p0.x + pad, textY), ImGui::GetColorU32(ImGuiCol_Text), Ellipsize(ss->name, nameW).c_str());
    float timeX = p1.x - pad - timeW;
    dl->AddText(ImVec2(timeX, textY), ss->saved ? ImGui::GetColorU32(COL_DIM) : COL_ACCENT, time.c_str());
    if (!ss->saved) {
        float h = ImGui::GetTextLineHeight();
        dl->AddCircleFilled(ImVec2(p1.x - pad - 3 * s, textY + h / 2), 3 * s, COL_ACCENT);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !(ImGui::GetDragDropPayload() != nullptr)) {
        ImGui::SetTooltip("%s%s", ss->name, ss->saved ? "" : "\nNot saved yet");
    }

    // Save / Copy / Delete for this card alone
    float rowY = textY + ImGui::GetTextLineHeight() + CardSpacing();
    float delW = ImGui::GetFrameHeight();
    float btnW = (cardW - 2 * pad - delW - 2 * 4 * s) / 2;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, rowY));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * s, CardSpacing()));
    if (ImGui::Button("Save", ImVec2(btnW, 0))) SaveScreenshot(ss);
    ImGui::SameLine();
    if (ImGui::Button("Copy", ImVec2(btnW, 0))) CopyScreenshot(ss);
    ImGui::SameLine();
    if (DangerButton("\xC3\x97", ImVec2(delW, 0))) DeleteMany({ ss });
    ImGui::SetItemTooltip("Delete %s", ss->name);
    ImGui::PopStyleVar();

    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(cardW, cardH));
    ImGui::PopID();
}

static void RenderCardMenu() {
    if (g_state.open_card_menu) {
        ImGui::OpenPopup("card_menu");
        g_state.open_card_menu = false;
    }
    if (!ImGui::BeginPopup("card_menu")) return;
    std::vector<Screenshot*> list;
    for (Screenshot* ss : g_state.menu_targets) if (IndexOf(ss) >= 0) list.push_back(ss);
    int n = (int)list.size();
    char label[64];

    snprintf(label, sizeof(label), n > 1 ? "Save %d" : "Save", n);
    if (ImGui::MenuItem(label)) SaveMany(list);
    if (ImGui::MenuItem("Rename", "F2", false, n == 1)) g_state.focus_rename = true;
    snprintf(label, sizeof(label), n > 1 ? "Copy %d as sheet" : "Copy", n);
    if (ImGui::MenuItem(label)) CopyMany(list);
    snprintf(label, sizeof(label), n > 1 ? "Export %d..." : "Export all...", n);
    if (ImGui::MenuItem(label)) OpenExport(n > 1 ? list : g_state.screenshots);
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, COL_DANGER_TEXT);
    snprintf(label, sizeof(label), n > 1 ? "Delete %d" : "Delete", n);
    if (ImGui::MenuItem(label, "Del")) DeleteMany(list);
    ImGui::PopStyleColor();
    ImGui::EndPopup();
}

static void RenderGallery() {
    const float s = g_dpi_scale;
    RenderSelectionBar();

    ImGui::BeginChild("gallery", ImVec2(0, 0), ImGuiChildFlags_None);
    if (g_state.screenshots.empty()) {
        const char* msg = g_state.is_connected
            ? "No screenshots yet - press Take Screenshot"
            : "No screenshots yet - connect the radio below, then press Take Screenshot";
        float w = ImGui::CalcTextSize(msg).x;
        ImGui::SetCursorPos(ImVec2(std::max(0.0f, (ImGui::GetContentRegionAvail().x - w) / 2), 40 * s));
        ImGui::TextDisabled("%s", msg);
    } else {
        const float pad = 6 * s, gap = 14 * s;
        float cardW = g_state.thumb_size * s + 2 * pad;
        float cardH = CardHeight();
        float avail = ImGui::GetContentRegionAvail().x;
        int cols = std::max(1, (int)((avail + gap) / (cardW + gap)));
        // Spread spare width over the cards, like the web version's grid.
        cardW = std::max(cardW, (avail - gap * (cols - 1)) / cols);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
        for (int i = 0; i < (int)g_state.screenshots.size(); i++) {
            if (i % cols) ImGui::SameLine();
            RenderCard(i, cardW, cardH);
        }
        ImGui::PopStyleVar();
    }
    RenderCardMenu();
    ImGui::EndChild();
}

static void RenderPreviewPanel() {
    const float s = g_dpi_scale;
    Screenshot* ss = SelectedScreenshot();

    char label[300];
    snprintf(label, sizeof(label), ss ? "PREVIEW - %s" : "PREVIEW", ss ? ss->name : "");
    float zoomW = 80 * s;
    float zoomX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - zoomW;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", Ellipsize(label, ImGui::GetContentRegionAvail().x - zoomW - 8 * s).c_str());
    ImGui::SameLine(zoomX);
    ImGui::SetNextItemWidth(zoomW);
    const char* zooms[] = { "Fit", "1\xC3\x97", "2\xC3\x97", "3\xC3\x97", "4\xC3\x97", "5\xC3\x97", "6\xC3\x97" };
    if (ImGui::Combo("##zoom", &g_state.zoom, zooms, ZOOM_MAX + 1)) SaveSettings();

    // Below the image: name field and Save/Copy.
    float below = ImGui::GetTextLineHeightWithSpacing() + 2 * ImGui::GetFrameHeightWithSpacing() + 8 * s;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float maxH = std::max(80 * s, avail.y - below);
    float w, h;
    if (g_state.zoom > 0) {
        w = DISPLAY_WIDTH * g_state.zoom * s;
        h = DISPLAY_HEIGHT * g_state.zoom * s;
        // Shrink rather than overflow the panel.
        float fit = std::min(1.0f, std::min(avail.x / w, maxH / h));
        w *= fit;
        h *= fit;
    } else {
        w = std::min(avail.x, 384 * s);
        h = w * 1.25f;
        if (h > maxH) { h = maxH; w = h / 1.25f; }
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - w) / 2);
    if (ss) {
        ImGui::Image(Tex(ss->texture), ImVec2(w, h));
    } else {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(w, h));
        ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_Border));
        const char* msg = "Take a screenshot\nto see preview";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + (w - ts.x) / 2, p.y + (h - ts.y) / 2),
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled), msg);
        return;
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (g_state.focus_rename) {
        ImGui::SetKeyboardFocusHere();
        g_state.focus_rename = false;
    }
    bool enter = ImGui::InputText("##rename", g_state.rename_buffer, sizeof(g_state.rename_buffer),
                                  ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
        // Tabs and newlines would break the capture index.
        for (char* c = g_state.rename_buffer; *c; c++) if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
        if (g_state.rename_buffer[0] && strcmp(ss->name, g_state.rename_buffer) != 0) {
            strcpy(ss->name, g_state.rename_buffer);
            SaveIndex();
            g_state.export_dirty = true;
        } else {
            strcpy(g_state.rename_buffer, ss->name);
        }
    }
    float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    if (ImGui::Button("Save", ImVec2(half, 0))) SaveScreenshot(ss);
    ImGui::SameLine();
    if (ImGui::Button("Copy", ImVec2(half, 0))) CopyScreenshot(ss);
}

static void RenderMoreMenu(const ImVec2& anchor) {
    const float s = g_dpi_scale;
    ImGui::SetNextWindowPos(anchor, ImGuiCond_Appearing, ImVec2(1.0f, 1.0f));
    if (!ImGui::BeginPopup("more")) return;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 260 * s);

    ImGui::SeparatorText("Orientation");
    const char* rotations[] = { "Rotate 90 CW", "Rotate 90 CCW" };
    ImGui::SetNextItemWidth(160 * s);
    if (ImGui::Combo("Rotation", &g_state.rotation, rotations, 2)) { RebuildAllScreenshots(); SaveSettings(); }
    if (ImGui::Checkbox("Mirror", &g_state.mirror)) { RebuildAllScreenshots(); SaveSettings(); }
    ImGui::TextDisabled("Adjust until the capture matches the radio display.");

    ImGui::SeparatorText("Export");
    int current = 0;
    for (int i = 0; i < EXPORT_SCALE_COUNT; i++) if (EXPORT_SCALES[i] == g_state.export_scale) current = i;
    char preview[48];
    snprintf(preview, sizeof(preview), "%d\xC3\x97 (%d\xC3\x97%d)", g_state.export_scale,
             DISPLAY_WIDTH * g_state.export_scale, DISPLAY_HEIGHT * g_state.export_scale);
    ImGui::SetNextItemWidth(160 * s);
    if (ImGui::BeginCombo("PNG scale", preview)) {
        for (int i = 0; i < EXPORT_SCALE_COUNT; i++) {
            int sc = EXPORT_SCALES[i];
            char item[48];
            snprintf(item, sizeof(item), "%d\xC3\x97 (%d\xC3\x97%d)", sc, DISPLAY_WIDTH * sc, DISPLAY_HEIGHT * sc);
            if (ImGui::Selectable(item, i == current)) { g_state.export_scale = sc; SaveSettings(); }
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("Used by Save, Copy and Save All.");

    ImGui::SeparatorText("Connection");
    if (ImGui::Checkbox("Reconnect to last radio", &g_state.auto_connect)) SaveSettings();
    ImGui::TextDisabled("On startup and when it is plugged back in.");

    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
}

static void RenderStatusBar() {
    const float s = g_dpi_scale;
    ImVec4 color = g_state.status_kind == STATUS_ERR ? COL_ERROR
                 : g_state.status_kind == STATUS_OK ? COL_OK : COL_DIM;

    static float rightWidth = 0;
    float statusW = std::max(60 * s, ImGui::GetContentRegionAvail().x - rightWidth - 12 * s);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(Ellipsize(g_state.status_message, statusW).c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered() && ImGui::CalcTextSize(g_state.status_message).x > statusW) {
        ImGui::SetTooltip("%s", g_state.status_message);
    }

    RightAlign right(rightWidth);
    ImGui::BeginDisabled(g_state.is_connected);
    ImGui::SetNextItemWidth(190 * s);
    const char* preview = g_state.selected_port >= 0 ? g_state.com_ports[g_state.selected_port].c_str() : "Select...";
    if (ImGui::BeginCombo("##port", preview)) {
        for (int i = 0; i < (int)g_state.com_ports.size(); i++) {
            if (ImGui::Selectable(g_state.com_ports[i].c_str(), i == g_state.selected_port)) g_state.selected_port = i;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) PollPorts(false);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100 * s);
    char baudLabels[BAUD_RATE_COUNT][16];
    const char* baudItems[BAUD_RATE_COUNT];
    for (int i = 0; i < BAUD_RATE_COUNT; i++) {
        snprintf(baudLabels[i], sizeof(baudLabels[i]), "%d", BAUD_RATES[i]);
        baudItems[i] = baudLabels[i];
    }
    if (ImGui::Combo("##baud", &g_state.baud_index, baudItems, BAUD_RATE_COUNT)) SaveSettings();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!g_state.is_connected) {
        ImGui::BeginDisabled(g_state.selected_port < 0);
        if (ImGui::Button("Connect")) ConnectSelected();
        ImGui::EndDisabled();
    } else if (ImGui::Button("Disconnect")) {
        // Disconnecting on purpose means: don't reopen it next time.
        SerialDisconnect();
        g_state.reconnect = false;
        SaveSettings();
    }

    int n = (int)g_state.screenshots.size(), unsaved = UnsavedCount();
    ImGui::SameLine(0, 16 * s);
    ImGui::AlignTextToFramePadding();
    if (unsaved) ImGui::TextDisabled("%d screenshot%s - %d unsaved", n, n == 1 ? "" : "s", unsaved);
    else ImGui::TextDisabled("%d screenshot%s", n, n == 1 ? "" : "s");
    ImGui::SameLine();
    if (ImGui::Button("...")) ImGui::OpenPopup("more");
    ImGui::SetItemTooltip("More options");
    ImVec2 anchor(ImGui::GetItemRectMax().x, ImGui::GetItemRectMin().y - 4 * s);
    RenderMoreMenu(anchor);
}

static void RenderUndoToast() {
    if (g_state.undo.empty()) return;
    if (glfwGetTime() > g_state.undo_until) { FinalizeUndo(); return; }
    const float s = g_dpi_scale;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float bottom = vp->WorkPos.y + vp->WorkSize.y - ImGui::GetFrameHeightWithSpacing() - 24 * s;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x / 2, bottom), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * s, 8 * s));
    ImGui::Begin("##undo", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(g_state.undo_message);
    ImGui::SameLine(0, 16 * s);
    if (PrimaryButton("Undo")) UndoRemove();
    ImGui::SetItemTooltip("Ctrl+Z");
    ImGui::End();
    ImGui::PopStyleVar();
}

static bool SegButton(const char* label, bool active, float width) {
    if (active) return PrimaryButton(label, ImVec2(width, 0));
    return ImGui::Button(label, ImVec2(width, 0));
}

static void RenderExportDialog() {
    const float s = g_dpi_scale;
    if (g_state.open_export) {
        ImGui::OpenPopup("Export###export");
        g_state.open_export = false;
    }
    ImGui::SetNextWindowSize(ImVec2(860 * s, 0), ImGuiCond_Appearing);
    char title[64];
    int n = (int)g_state.export_list.size();
    snprintf(title, sizeof(title), "Export %d screenshot%s###export", n, n == 1 ? "" : "s");
    bool open = true;
    ImGui::SetNextWindowBgAlpha(1.0f);  // the default popup background lets the gallery show through
    if (!ImGui::BeginPopupModal(title, &open)) return;

    SeqOptions& q = g_state.seq;
    SeqOptions before = q;

    // Options
    ImGui::BeginGroup();
    float optW = 300 * s, segW = (optW - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 10 * s));
    if (SegButton("Contact sheet", q.format == SEQ_SHEET, segW)) q.format = SEQ_SHEET;
    ImGui::SameLine();
    if (SegButton("Strip", q.format == SEQ_STRIP, segW)) q.format = SEQ_STRIP;
    ImGui::SameLine();
    if (SegButton("Animated GIF", q.format == SEQ_GIF, segW)) q.format = SEQ_GIF;
    ImGui::PushItemWidth(150 * s);
    if (q.format == SEQ_SHEET) {
        int maxCols = std::max(1, std::min(10, n));
        int cols = std::min(q.columns, maxCols);
        if (ImGui::SliderInt("Columns", &cols, 1, maxCols)) q.columns = cols;
    }
    if (q.format == SEQ_STRIP) {
        int dir = q.vertical ? 1 : 0;
        const char* dirs[] = { "Horizontal", "Vertical" };
        if (ImGui::Combo("Direction", &dir, dirs, 2)) q.vertical = dir == 1;
    }
    if (q.format != SEQ_GIF) {
        if (ImGui::SliderInt("Gap", &q.gap, 0, 48, "%dpx")) q.gap = (q.gap + 2) / 4 * 4;
        const char* bgs[] = { "Dark", "Light", "Transparent" };
        ImGui::Combo("Background", &q.background, bgs, 3);
        ImGui::Checkbox("Names under each", &q.labels);
    } else {
        float secs = q.delay_ms / 1000.0f;
        if (ImGui::SliderFloat("Frame time", &secs, 0.2f, 4.0f, "%.1fs")) q.delay_ms = (int)(secs * 10 + 0.5f) * 100;
    }
    int scaleIdx = 0;
    for (int i = 0; i < EXPORT_SCALE_COUNT; i++) if (EXPORT_SCALES[i] == q.scale) scaleIdx = i;
    const char* scales[] = { "1\xC3\x97", "2\xC3\x97", "3\xC3\x97", "4\xC3\x97", "6\xC3\x97" };
    if (ImGui::Combo("Scale", &scaleIdx, scales, EXPORT_SCALE_COUNT)) q.scale = EXPORT_SCALES[scaleIdx];
    ImGui::PopItemWidth();
    ImGui::PopStyleVar();

    SheetLayout L = ComputeLayout(n, q);
    if (q.format == SEQ_GIF) {
        ImGui::TextDisabled("%d \xC3\x97 %d px - %d frame%s, looping", DISPLAY_WIDTH * q.scale, DISPLAY_HEIGHT * q.scale,
                            n, n == 1 ? "" : "s");
    } else if (L.too_big) {
        ImGui::TextColored(COL_ERROR, "%d \xC3\x97 %d px - too large, lower the scale", L.w, L.h);
    } else {
        ImGui::TextDisabled("%d \xC3\x97 %d px", L.w, L.h);
    }
    ImGui::Dummy(ImVec2(optW, 0));
    ImGui::EndGroup();

    if (memcmp(&before, &q, sizeof(q)) != 0) {
        g_state.export_dirty = true;
        SaveSettings();
    }

    // Preview
    ImGui::SameLine(0, 24 * s);
    ImVec2 box(ImGui::GetContentRegionAvail().x, 420 * s);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + box.x, p.y + box.y), IM_COL32(30, 30, 30, 255));
    GLuint tex = 0;
    float iw = 0, ih = 0;
    if (q.format == SEQ_GIF) {
        double now = glfwGetTime();
        if (now >= g_state.gif_next) {
            g_state.gif_frame = (g_state.gif_frame + 1) % std::max(1, n);
            g_state.gif_next = now + q.delay_ms / 1000.0;
        }
        tex = g_state.export_list[std::min(g_state.gif_frame, n - 1)]->texture;
        iw = (float)DISPLAY_WIDTH * q.scale;
        ih = (float)DISPLAY_HEIGHT * q.scale;
    } else if (!L.too_big) {
        if (g_state.export_dirty) {
            SheetLayout rl;
            std::vector<uint8_t> img = RenderSheet(g_state.export_list, q, &rl);
            if (!img.empty()) {
                UploadTexture(&g_state.export_texture, img.data(), rl.w, rl.h);
                g_state.export_w = rl.w;
                g_state.export_h = rl.h;
            }
            g_state.export_dirty = false;
        }
        tex = g_state.export_texture;
        iw = (float)g_state.export_w;
        ih = (float)g_state.export_h;
    }
    if (tex && iw > 0) {
        // Fit inside the box; on-screen pixels are physical, so divide out DPI.
        float fit = std::min(1.0f, std::min(box.x / iw, box.y / ih));
        float dw = iw * fit, dh = ih * fit;
        ImGui::SetCursorScreenPos(ImVec2(p.x + (box.x - dw) / 2, p.y + (box.y - dh) / 2));
        ImGui::Image(Tex(tex), ImVec2(dw, dh));
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + box.y));
    ImGui::Dummy(ImVec2(box.x, 0));

    // Buttons
    ImGui::Spacing();
    float bw = 100 * s;
    int buttons = q.format == SEQ_GIF ? 2 : 3;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttons * bw -
                         (buttons - 1) * ImGui::GetStyle().ItemSpacing.x);
    bool blocked = q.format != SEQ_GIF && L.too_big;
    if (q.format != SEQ_GIF) {
        ImGui::BeginDisabled(blocked);
        if (ImGui::Button("Copy", ImVec2(bw, 0))) {
            SheetLayout cl;
            std::vector<uint8_t> img = RenderSheet(g_state.export_list, q, &cl);
            if (!img.empty()) CopyImageToClipboard(img.data(), cl.w, cl.h, q.format == SEQ_STRIP ? "the strip" : "the contact sheet");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(blocked);
    if (PrimaryButton("Save", ImVec2(bw, 0))) SaveExport();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(bw, 0)) || !open) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

static void HandleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z) && !g_state.undo.empty()) {
        UndoRemove();
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) {
        SelectAll();
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && g_state.picked.size() > 1) {
        Select(g_state.selected);  // back to just the one in the preview
    } else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !g_state.picked.empty()) {
        DeleteMany(PickedList());
    } else if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && g_state.picked.size() == 1) {
        g_state.focus_rename = true;
    } else if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && !ImGui::IsAnyItemActive() && !io.KeyCtrl &&
               !io.NavVisible) {
        // Space takes a screenshot, unless keyboard navigation has a widget
        // focused, where Space already means "press it".
        StartCapture();
    }
}

void RenderUI() {
    const float s = g_dpi_scale;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("AT168Shot", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

    RenderToolbar();
    ImGui::Separator();

    // Gallery and preview fill what is left above the status bar.
    float statusH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float mainH = std::max(100 * s, avail.y - statusH);
    float panelW = g_state.zoom > 0 ? DISPLAY_WIDTH * g_state.zoom * s + 2 * ImGui::GetStyle().WindowPadding.x
                                    : 340 * s;
    panelW = std::max(260 * s, std::min(panelW, avail.x * 0.6f));

    ImGui::BeginChild("gallery_col", ImVec2(avail.x - panelW - ImGui::GetStyle().ItemSpacing.x, mainH));
    RenderGallery();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("panel", ImVec2(panelW, mainH), ImGuiChildFlags_Borders);
    RenderPreviewPanel();
    ImGui::EndChild();

    ImGui::Separator();
    RenderStatusBar();
    ImGui::End();

    RenderUndoToast();
    RenderExportDialog();
    HandleShortcuts();

    // === Exit confirmation (only when captures could not be stored) ===
    if (g_state.show_exit_popup) {
        ImGui::OpenPopup("Exit?");
        g_state.show_exit_popup = false;
    }
    if (ImGui::BeginPopupModal("Exit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Screenshots could not be stored and %d %s not saved.\nExit anyway?",
                    UnsavedCount(), UnsavedCount() == 1 ? "is" : "are");
        ImGui::Separator();
        if (ImGui::Button("Yes", ImVec2(80 * s, 0))) {
            g_state.running = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(80 * s, 0))) {
            g_state.pending_close = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// =============================================================================
// GLFW Callbacks
// =============================================================================

// Captures are kept on disk, so closing only needs confirming when that failed.
static void glfw_window_close_callback(GLFWwindow* window) {
    if (!g_state.storage_ok && UnsavedCount() > 0) {
        g_state.show_exit_popup = true;
        g_state.pending_close = true;
        glfwSetWindowShouldClose(window, GLFW_FALSE);
    } else {
        g_state.running = false;
    }
}

// =============================================================================
// Entry Point
// =============================================================================

#ifdef RADSHOT_WINDOWS
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
#else
int main(int, char**) {
#endif
    // Load saved settings
    LoadSettings();

    // Initialize GLFW
    if (!glfwInit()) {
        return 1;
    }

    // GL 3.0 + GLSL 130
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    // Create window
    char title[256];
    snprintf(title, sizeof(title), "AT168Shot v%s - AnyTone 168 Screenshot Tool", APP_VERSION);

    // The default size is authored for 96 DPI; grow it to match the monitor.
    GLFWmonitor* primary = glfwGetPrimaryMonitor();
    if (!g_have_saved_size && primary) {
        float mscale = ImGui_ImplGlfw_GetContentScaleForMonitor(primary);
        if (mscale > 0.0f) {
            g_state.window_width = (int)(g_state.window_width * mscale);
            g_state.window_height = (int)(g_state.window_height * mscale);
        }
    }

    GLFWwindow* window = glfwCreateWindow(
        g_state.window_width, g_state.window_height, title, nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }

    g_state.window = window;

    // Restore window position
    if (g_state.window_x >= 0 && g_state.window_y >= 0) {
        glfwSetWindowPos(window, g_state.window_x, g_state.window_y);
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync
    glfwSetWindowCloseCallback(window, glfw_window_close_callback);

    // Setup Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // Disable imgui.ini

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init();

    g_base_style = ImGui::GetStyle();
    ApplyDpiScale(QueryDpiScale(window));

    // Captures from earlier sessions (needs the GL context for textures)
    LoadCaptures();
    if (!g_state.screenshots.empty()) Select((int)g_state.screenshots.size() - 1);

    // Initial port enumeration; reconnects to the last radio if it is there.
    PollPorts(true);
    if (!g_state.is_connected && g_state.selected_port < 0) {
        for (int i = 0; i < (int)g_state.com_ports.size(); i++) {
            if (g_state.com_ports[i] == g_state.last_port_name) g_state.selected_port = i;
        }
    }
    g_state.next_port_poll = glfwGetTime() + PORT_POLL_S;

    // Main loop
    while (g_state.running && !glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Track window position and size
        glfwGetWindowPos(window, &g_state.window_x, &g_state.window_y);
        glfwGetWindowSize(window, &g_state.window_width, &g_state.window_height);

        // Follow DPI changes, e.g. dragging onto a monitor with a different scale
        float scale = QueryDpiScale(window);
        if (fabsf(scale - g_dpi_scale) > 0.001f) {
            ApplyDpiScale(scale);
        }

        // Notice the radio being unplugged or plugged back in
        if (!g_state.is_capturing && glfwGetTime() >= g_state.next_port_poll) {
            PollPorts(false);
            g_state.next_port_poll = glfwGetTime() + PORT_POLL_S;
        }

        // Update capture
        UpdateCapture();

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Render UI
        RenderUI();
        if (!(ImGui::GetDragDropPayload() != nullptr)) g_state.dragging.clear();

        // Render
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Save settings before exiting
    SaveSettings();

    // Cleanup
    FinalizeUndo();
    for (Screenshot* ss : g_state.screenshots) delete ss;
    g_state.screenshots.clear();
    if (g_state.export_texture) glDeleteTextures(1, &g_state.export_texture);
    SerialDisconnect();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
