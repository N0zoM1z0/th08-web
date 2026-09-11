#include "linux_compat.hpp"

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <dinput.h>
#include <dsound.h>
#ifdef TH08_MODERN_WEB
#include <emscripten.h>
#include <emscripten/atomic.h>
#include <emscripten/threading_legacy.h>
#endif

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#ifndef TH08_MODERN_WEB
#include <fontconfig/fontconfig.h>
#endif
#include <glob.h>
#ifndef TH08_MODERN_WEB
#include <iconv.h>
#endif
#include <limits.h>
#include <math.h>
#include <map>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace
{
enum HandleKind { HANDLE_FILE, HANDLE_THREAD, HANDLE_EVENT, HANDLE_MUTEX, HANDLE_FIND, HANDLE_WEB_FILE };

struct LinuxHandle
{
    explicit LinuxHandle(HandleKind value) : kind(value) {}
    virtual ~LinuxHandle() {}
    HandleKind kind;
};

struct FileHandle : LinuxHandle
{
    explicit FileHandle(int value) : LinuxHandle(HANDLE_FILE), fd(value) {}
    ~FileHandle() { if (fd >= 0) close(fd); }
    int fd;
};

#ifdef TH08_MODERN_WEB
struct WebFileHandle : LinuxHandle
{
    WebFileHandle(int index_, DWORD size_)
        : LinuxHandle(HANDLE_WEB_FILE), index(index_), size(size_), position(0), cacheOffset(0),
          cacheSize(0), blobFetches(0), cacheHits(0), bytesFetched(0) {}
    ~WebFileHandle()
    {
        if (index == 1 && blobFetches != 0)
        {
            fprintf(stderr,
                    "th08-web: thbgm.dat read cache: %lu Blob fetches, %lu cache hits, %lu bytes fetched\n",
                    static_cast<unsigned long>(blobFetches), static_cast<unsigned long>(cacheHits),
                    static_cast<unsigned long>(bytesFetched));
        }
    }
    int index;
    DWORD size;
    DWORD position;
    DWORD cacheOffset;
    DWORD cacheSize;
    DWORD blobFetches;
    DWORD cacheHits;
    DWORD bytesFetched;
    std::vector<BYTE> cache;
};

DWORD g_retailFileSizes[2];
BYTE *g_retailFileData[2];
bool g_retailReadLogged[2];
bool g_webIoDiagnostics;
uint32_t g_webVirtualKeys[256];
uint32_t g_webVirtualKeyPresses[256];

int RetailFileIndex(const char *path)
{
    if (path == NULL)
        return -1;
    const char *basename = path;
    for (const char *cursor = path; *cursor != '\0'; ++cursor)
        if (*cursor == '/' || *cursor == '\\')
            basename = cursor + 1;
    if (strcmp(basename, "th08.dat") == 0)
        return 0;
    if (strcmp(basename, "thbgm.dat") == 0)
        return 1;
    return -1;
}

bool ReadRetailFile(WebFileHandle *handle, void *destination, DWORD size, DWORD *bytesRead)
{
    if (handle->position >= handle->size || size == 0)
    {
        if (bytesRead != NULL)
            *bytesRead = 0;
        return true;
    }

    const DWORD available = handle->size - handle->position;
    const DWORD requested = size < available ? size : available;
    if (g_retailFileData[handle->index] != NULL)
    {
        memcpy(destination, g_retailFileData[handle->index] + handle->position, requested);
        handle->position += requested;
        if (bytesRead != NULL)
            *bytesRead = requested;
        if (!g_retailReadLogged[handle->index])
        {
            fprintf(stderr, "th08-web: local %s session-memory reads are active\n",
                    handle->index == 0 ? "th08.dat" : "thbgm.dat");
            g_retailReadLogged[handle->index] = true;
        }
        return true;
    }

    if (handle->cacheSize != 0 && handle->position >= handle->cacheOffset)
    {
        const DWORD cachePosition = handle->position - handle->cacheOffset;
        if (cachePosition <= handle->cacheSize && requested <= handle->cacheSize - cachePosition)
        {
            memcpy(destination, &handle->cache[cachePosition], requested);
            handle->position += requested;
            handle->cacheHits++;
            if (bytesRead != NULL)
                *bytesRead = requested;
            return true;
        }
    }

    // BGM streaming is sequential, but the authored DirectSound-shaped code
    // asks for relatively small pieces. Read ahead so one browser Blob promise
    // serves many synchronous Win32-compatible ReadFile calls.
    const DWORD readAheadSize = 1024 * 1024;
    // Avoid turning one-off metadata reads into a full 1 MiB fetch. Enable
    // read-ahead only after this handle demonstrates sequential use.
    const bool useReadAhead = handle->index == 1 && handle->blobFetches != 0 && requested < readAheadSize;
    DWORD fetchSize = useReadAhead ? readAheadSize : requested;
    if (fetchSize > available)
        fetchSize = available;
    if (useReadAhead && handle->cache.size() < fetchSize)
        handle->cache.resize(fetchSize);
    void *fetchDestination = useReadAhead ? static_cast<void *>(&handle->cache[0]) : destination;

    const double readStarted = g_webIoDiagnostics ? emscripten_get_now() : 0.0;
    volatile uint32_t status = 0;
    MAIN_THREAD_ASYNC_EM_ASM({
        const files = Module['th08RetailFiles'];
        const file = files && files[$0];
        const statusIndex = $4 >>> 2;
        const finish = (value) => {
            Atomics.store(HEAP32, statusIndex, value);
            Atomics.notify(HEAP32, statusIndex, 1);
        };
        if (!file) {
            finish(-1);
            return;
        }
        const prefetches = Module['th08RetailPrefetches'] ||
            (Module['th08RetailPrefetches'] = []);
        const pending = prefetches[$0];
        let usedPrefetch = !!pending && pending.offset === $1 && pending.size === $3;
        let readPromise = usedPrefetch
            ? pending.promise.then((buffer) => {
                if (buffer !== null) return buffer;
                usedPrefetch = false;
                return file.slice($1, $1 + $3).arrayBuffer();
              })
            : file.slice($1, $1 + $3).arrayBuffer();
        readPromise.then((buffer) => {
            const bytes = new Uint8Array(buffer);
            HEAPU8.set(bytes, $2);
            if ($5) {
                const nextOffset = $1 + bytes.length;
                const nextSize = Math.min($3, file.size - nextOffset);
                if (nextSize > 0) {
                    prefetches[$0] = {
                        offset: nextOffset,
                        size: nextSize,
                        promise: file.slice(nextOffset, nextOffset + nextSize).arrayBuffer()
                            .catch((error) => {
                                console.warn('Unable to prefetch TH08 BGM data:', error);
                                return null;
                            }),
                    };
                } else {
                    prefetches[$0] = null;
                }
            } else if (usedPrefetch) {
                prefetches[$0] = null;
            }
            finish((bytes.length + 1) | (usedPrefetch ? 0x80000000 : 0));
        }).catch((error) => {
            console.error('Unable to read the selected TH08 data file:', error);
            finish(-1);
        });
    }, handle->index, handle->position, fetchDestination, fetchSize, &status,
       useReadAhead ? 1 : 0);

    uint32_t *statusAddress = const_cast<uint32_t *>(&status);
    emscripten_atomic_wait_u32(statusAddress, 0, 30000000000LL);
    const uint32_t result = emscripten_atomic_load_u32(statusAddress);
    if (result == 0 || result == 0xffffffffU)
    {
        if (bytesRead != NULL)
            *bytesRead = 0;
        return false;
    }

    const bool usedPrefetch = (result & 0x80000000U) != 0;
    const DWORD count = (result & 0x7fffffffU) - 1;
    if (g_webIoDiagnostics)
    {
        fprintf(stderr,
                "th08-web: retail read: %s offset %lu, %lu bytes, %.3f ms (%s)\n",
                handle->index == 0 ? "th08.dat" : "thbgm.dat",
                static_cast<unsigned long>(handle->position),
                static_cast<unsigned long>(count), emscripten_get_now() - readStarted,
                usedPrefetch ? "prefetched" : "direct");
    }
    handle->blobFetches++;
    handle->bytesFetched += count;
    DWORD delivered = count;
    if (useReadAhead)
    {
        handle->cacheOffset = handle->position;
        handle->cacheSize = count;
        delivered = requested < count ? requested : count;
        memcpy(destination, &handle->cache[0], delivered);
    }
    handle->position += delivered;
    if (!g_retailReadLogged[handle->index])
    {
        fprintf(stderr, "th08-web: local %s Blob range reads are active\n",
                handle->index == 0 ? "th08.dat" : "thbgm.dat");
        g_retailReadLogged[handle->index] = true;
    }
    if (bytesRead != NULL)
        *bytesRead = delivered;
    return true;
}
#endif

struct ThreadHandle : LinuxHandle
{
    ThreadHandle() : LinuxHandle(HANDLE_THREAD), finished(false), joined(false), result(0), id(0) {}
    pthread_t thread;
    volatile bool finished;
    bool joined;
    DWORD result;
    DWORD id;
    LPTHREAD_START_ROUTINE start;
    LPVOID parameter;
};

struct EventHandle : LinuxHandle
{
    EventHandle(bool manualReset, bool initial)
        : LinuxHandle(HANDLE_EVENT), manual(manualReset), signaled(initial)
    {
        pthread_mutex_init(&mutex, NULL);
        pthread_cond_init(&condition, NULL);
    }
    ~EventHandle()
    {
        pthread_cond_destroy(&condition);
        pthread_mutex_destroy(&mutex);
    }
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool manual;
    bool signaled;
};

struct MutexHandle : LinuxHandle
{
    MutexHandle() : LinuxHandle(HANDLE_MUTEX) { pthread_mutex_init(&mutex, NULL); }
    ~MutexHandle() { pthread_mutex_destroy(&mutex); }
    pthread_mutex_t mutex;
};

struct FindHandle : LinuxHandle
{
    FindHandle() : LinuxHandle(HANDLE_FIND), index(0) {}
    std::vector<std::string> paths;
    size_t index;
};

struct GdiObject
{
    enum Kind { BITMAP, FONT } kind;
    virtual ~GdiObject() {}
};

struct GdiBitmap : GdiObject
{
    GdiBitmap(int width_, int height_, int bits_)
        : width(width_), height(height_ < 0 ? -height_ : height_), bits(bits_)
    {
        kind = BITMAP;
        pitch = ((width * bits + 31) / 32) * 4;
        pixels.resize(pitch * height);
    }
    int width, height, bits, pitch;
    std::vector<BYTE> pixels;
};

struct GdiFont : GdiObject
{
    GdiFont() : font(NULL), height(16), weight(FW_NORMAL) { kind = FONT; }
    GdiFont(TTF_Font *font_, int height_, int weight_)
        : font(font_), height(height_ > 0 ? height_ : 16), weight(weight_) { kind = FONT; }
    ~GdiFont() { if (font != NULL) TTF_CloseFont(font); }
    TTF_Font *font;
    int height;
    int weight;
};

struct GdiDc
{
    GdiDc() : bitmap(NULL), font(&stockFont), color(0xffffffff) {}
    GdiBitmap *bitmap;
    GdiFont stockFont;
    GdiFont *font;
    COLORREF color;
};

DWORD g_lastError;
WNDPROC g_windowProcedure;
SDL_Window *g_window;
std::map<DWORD, std::vector<MSG> > g_threadMessages;
pthread_mutex_t g_messageMutex = PTHREAD_MUTEX_INITIALIZER;

std::string ExecutableSiblingPath(const char *filename)
{
#ifdef TH08_MODERN_WEB
    (void)filename;
    return std::string();
#else
    char path[PATH_MAX + 1];
    ssize_t count = readlink("/proc/self/exe", path, PATH_MAX);
    if (count <= 0 || count > PATH_MAX)
        return std::string();
    path[count] = '\0';
    char *separator = strrchr(path, '/');
    if (separator == NULL)
        return std::string();
    separator[1] = '\0';
    return std::string(path) + filename;
#endif
}

void SetApplicationIcon(SDL_Window *window)
{
    if (window == NULL)
        return;
    const std::string iconPath = ExecutableSiblingPath("th08-modern.png");
    if (iconPath.empty())
        return;
    SDL_Surface *icon = IMG_Load(iconPath.c_str());
    if (icon == NULL)
        return;
    SDL_SetWindowIcon(window, icon);
    SDL_FreeSurface(icon);
}

DWORD CurrentThreadIdImpl()
{
    return static_cast<DWORD>(pthread_self());
}

std::string ConvertCp932ToUtf8(const char *text, size_t length)
{
    if (text == NULL || length == 0) return std::string();
#ifdef TH08_MODERN_WEB
    // The first full-link milestone has no bundled font or encoding table.
    // Keep the bytes intact; the Web text backend will replace this fallback.
    return std::string(text, length);
#else
    iconv_t converter = iconv_open("UTF-8", "CP932");
    if (converter == reinterpret_cast<iconv_t>(-1)) return std::string(text, length);

    std::vector<char> output(length * 4 + 8, 0);
    char *input = const_cast<char *>(text);
    char *destination = &output[0];
    size_t inputLeft = length;
    size_t outputLeft = output.size() - 1;
    if (iconv(converter, &input, &inputLeft, &destination, &outputLeft) == static_cast<size_t>(-1))
    {
        iconv_close(converter);
        return std::string(text, length);
    }
    iconv_close(converter);
    return std::string(&output[0], destination - &output[0]);
#endif
}

const char *ResolveJapaneseFont()
{
#ifdef TH08_MODERN_WEB
    // Retail assets never provide the Web font. A later milestone will mount
    // an explicitly licensed Japanese font at a stable virtual path.
    return NULL;
#else
    static std::string path;
    static bool resolved;
    if (resolved) return path.empty() ? NULL : path.c_str();
    resolved = true;

    const char *overridePath = getenv("TH08_FONT");
    if (overridePath != NULL && access(overridePath, R_OK) == 0)
    {
        path = overridePath;
        return path.c_str();
    }

    if (!FcInit()) return NULL;
    FcPattern *pattern = FcPatternCreate();
    if (pattern == NULL) return NULL;
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8 *>("VL Gothic"));
    FcPatternAddString(pattern, FC_LANG, reinterpret_cast<const FcChar8 *>("ja"));
    FcPatternAddInteger(pattern, FC_SPACING, FC_MONO);
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    FcPattern *match = FcFontMatch(NULL, pattern, &result);
    FcPatternDestroy(pattern);
    if (match == NULL) return NULL;
    FcChar8 *file = NULL;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file != NULL)
        path = reinterpret_cast<const char *>(file);
    FcPatternDestroy(match);
    return path.empty() ? NULL : path.c_str();
#endif
}

void PutGdiTextPixel(GdiBitmap *bitmap, int x, int y, COLORREF color, BYTE coverage)
{
    if (bitmap == NULL || x < 0 || y < 0 || x >= bitmap->width || y >= bitmap->height || coverage == 0) return;
    const int red = color & 0xff;
    const int green = (color >> 8) & 0xff;
    const int blue = (color >> 16) & 0xff;
    BYTE *pixel = &bitmap->pixels[y * bitmap->pitch + x * bitmap->bits / 8];
    if (bitmap->bits == 16)
    {
        uint16_t packed;
        memcpy(&packed, pixel, sizeof(packed));
        int oldRed = ((packed >> 10) & 0x1f) * 255 / 31;
        int oldGreen = ((packed >> 5) & 0x1f) * 255 / 31;
        int oldBlue = (packed & 0x1f) * 255 / 31;
        oldRed = (oldRed * (255 - coverage) + red * coverage) / 255;
        oldGreen = (oldGreen * (255 - coverage) + green * coverage) / 255;
        oldBlue = (oldBlue * (255 - coverage) + blue * coverage) / 255;
        packed = static_cast<uint16_t>(((oldRed >> 3) << 10) | ((oldGreen >> 3) << 5) | (oldBlue >> 3));
        memcpy(pixel, &packed, sizeof(packed));
    }
    else if (bitmap->bits == 32)
    {
        pixel[0] = static_cast<BYTE>((pixel[0] * (255 - coverage) + blue * coverage) / 255);
        pixel[1] = static_cast<BYTE>((pixel[1] * (255 - coverage) + green * coverage) / 255);
        pixel[2] = static_cast<BYTE>((pixel[2] * (255 - coverage) + red * coverage) / 255);
        pixel[3] = 0;
    }
}

void *ThreadTrampoline(void *opaque)
{
    ThreadHandle *handle = static_cast<ThreadHandle *>(opaque);
    handle->id = CurrentThreadIdImpl();
    handle->result = handle->start(handle->parameter);
    handle->finished = true;
    return NULL;
}

bool FillFindData(FindHandle *handle, WIN32_FIND_DATAA *data)
{
    if (handle->index >= handle->paths.size())
        return false;
    memset(data, 0, sizeof(*data));
    const std::string &path = handle->paths[handle->index++];
    const char *name = strrchr(path.c_str(), '/');
    strncpy(data->cFileName, name != NULL ? name + 1 : path.c_str(), MAX_PATH - 1);
    struct stat info;
    if (stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode))
        data->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    return true;
}

void PumpSdlEvents(MSG *message, bool *hasMessage)
{
    *hasMessage = false;
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0)
        return;
    SDL_Event event;
    if (!SDL_PollEvent(&event))
        return;
    memset(message, 0, sizeof(*message));
    message->hwnd = reinterpret_cast<HWND>(g_window);
    if (event.type == SDL_QUIT)
        message->message = WM_CLOSE;
    else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
    {
        message->message = WM_ACTIVATEAPP;
        message->wParam = TRUE;
    }
    else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
    {
        message->message = WM_ACTIVATEAPP;
        message->wParam = FALSE;
    }
    else
        message->message = 0;
    *hasMessage = true;
}

void FillKeyboard(BYTE *state, bool directInput)
{
    memset(state, 0, 256);
    SDL_PumpEvents();
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
#define MAP_KEY(win, sdl) state[(win)] = keys[(sdl)] ? 0x80 : 0
    if (directInput)
    {
        MAP_KEY(DIK_UP, SDL_SCANCODE_UP); MAP_KEY(DIK_DOWN, SDL_SCANCODE_DOWN);
        MAP_KEY(DIK_LEFT, SDL_SCANCODE_LEFT); MAP_KEY(DIK_RIGHT, SDL_SCANCODE_RIGHT);
        MAP_KEY(DIK_NUMPAD1, SDL_SCANCODE_KP_1); MAP_KEY(DIK_NUMPAD2, SDL_SCANCODE_KP_2);
        MAP_KEY(DIK_NUMPAD3, SDL_SCANCODE_KP_3); MAP_KEY(DIK_NUMPAD4, SDL_SCANCODE_KP_4);
        MAP_KEY(DIK_NUMPAD6, SDL_SCANCODE_KP_6); MAP_KEY(DIK_NUMPAD7, SDL_SCANCODE_KP_7);
        MAP_KEY(DIK_NUMPAD8, SDL_SCANCODE_KP_8); MAP_KEY(DIK_NUMPAD9, SDL_SCANCODE_KP_9);
        MAP_KEY(DIK_HOME, SDL_SCANCODE_HOME); MAP_KEY(DIK_P, SDL_SCANCODE_P);
        MAP_KEY(DIK_D, SDL_SCANCODE_D); MAP_KEY(DIK_Z, SDL_SCANCODE_Z); MAP_KEY(DIK_X, SDL_SCANCODE_X);
        MAP_KEY(DIK_LSHIFT, SDL_SCANCODE_LSHIFT); MAP_KEY(DIK_RSHIFT, SDL_SCANCODE_RSHIFT);
        MAP_KEY(DIK_ESCAPE, SDL_SCANCODE_ESCAPE); MAP_KEY(DIK_LCONTROL, SDL_SCANCODE_LCTRL);
        MAP_KEY(DIK_RCONTROL, SDL_SCANCODE_RCTRL); MAP_KEY(DIK_Q, SDL_SCANCODE_Q);
        MAP_KEY(DIK_S, SDL_SCANCODE_S); MAP_KEY(DIK_R, SDL_SCANCODE_R); MAP_KEY(DIK_RETURN, SDL_SCANCODE_RETURN);
    }
    else
    {
        MAP_KEY(VK_UP, SDL_SCANCODE_UP); MAP_KEY(VK_DOWN, SDL_SCANCODE_DOWN);
        MAP_KEY(VK_LEFT, SDL_SCANCODE_LEFT); MAP_KEY(VK_RIGHT, SDL_SCANCODE_RIGHT);
        MAP_KEY(VK_NUMPAD1, SDL_SCANCODE_KP_1); MAP_KEY(VK_NUMPAD2, SDL_SCANCODE_KP_2);
        MAP_KEY(VK_NUMPAD3, SDL_SCANCODE_KP_3); MAP_KEY(VK_NUMPAD4, SDL_SCANCODE_KP_4);
        MAP_KEY(VK_NUMPAD6, SDL_SCANCODE_KP_6); MAP_KEY(VK_NUMPAD7, SDL_SCANCODE_KP_7);
        MAP_KEY(VK_NUMPAD8, SDL_SCANCODE_KP_8); MAP_KEY(VK_NUMPAD9, SDL_SCANCODE_KP_9);
        MAP_KEY(VK_HOME, SDL_SCANCODE_HOME); MAP_KEY('P', SDL_SCANCODE_P); MAP_KEY('D', SDL_SCANCODE_D);
        MAP_KEY('Z', SDL_SCANCODE_Z); MAP_KEY('X', SDL_SCANCODE_X); MAP_KEY(VK_SHIFT, SDL_SCANCODE_LSHIFT);
        MAP_KEY(VK_ESCAPE, SDL_SCANCODE_ESCAPE); MAP_KEY(VK_CONTROL, SDL_SCANCODE_LCTRL);
        MAP_KEY('Q', SDL_SCANCODE_Q); MAP_KEY('S', SDL_SCANCODE_S); MAP_KEY('R', SDL_SCANCODE_R);
        MAP_KEY(VK_RETURN, SDL_SCANCODE_RETURN);
    }
#ifdef TH08_MODERN_WEB
#define MAP_WEB_KEY(output, virtualKey) \
    do { \
        const bool held = emscripten_atomic_load_u32(&g_webVirtualKeys[(virtualKey)]) != 0; \
        const bool pressed = emscripten_atomic_exchange_u32(&g_webVirtualKeyPresses[(virtualKey)], 0) != 0; \
        if (held || pressed) state[(output)] = 0x80; \
    } while (0)
    if (directInput)
    {
        MAP_WEB_KEY(DIK_UP, VK_UP); MAP_WEB_KEY(DIK_DOWN, VK_DOWN);
        MAP_WEB_KEY(DIK_LEFT, VK_LEFT); MAP_WEB_KEY(DIK_RIGHT, VK_RIGHT);
        MAP_WEB_KEY(DIK_NUMPAD1, VK_NUMPAD1); MAP_WEB_KEY(DIK_NUMPAD2, VK_NUMPAD2);
        MAP_WEB_KEY(DIK_NUMPAD3, VK_NUMPAD3); MAP_WEB_KEY(DIK_NUMPAD4, VK_NUMPAD4);
        MAP_WEB_KEY(DIK_NUMPAD6, VK_NUMPAD6); MAP_WEB_KEY(DIK_NUMPAD7, VK_NUMPAD7);
        MAP_WEB_KEY(DIK_NUMPAD8, VK_NUMPAD8); MAP_WEB_KEY(DIK_NUMPAD9, VK_NUMPAD9);
        MAP_WEB_KEY(DIK_HOME, VK_HOME); MAP_WEB_KEY(DIK_P, 'P'); MAP_WEB_KEY(DIK_D, 'D');
        MAP_WEB_KEY(DIK_Z, 'Z'); MAP_WEB_KEY(DIK_X, 'X');
        MAP_WEB_KEY(DIK_LSHIFT, VK_SHIFT); MAP_WEB_KEY(DIK_RSHIFT, VK_SHIFT);
        MAP_WEB_KEY(DIK_ESCAPE, VK_ESCAPE);
        MAP_WEB_KEY(DIK_LCONTROL, VK_CONTROL); MAP_WEB_KEY(DIK_RCONTROL, VK_CONTROL);
        MAP_WEB_KEY(DIK_Q, 'Q'); MAP_WEB_KEY(DIK_S, 'S'); MAP_WEB_KEY(DIK_R, 'R');
        MAP_WEB_KEY(DIK_RETURN, VK_RETURN);
    }
    else
    {
        const int virtualKeys[] = {
            VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3,
            VK_NUMPAD4, VK_NUMPAD6, VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9, VK_HOME,
            'P', 'D', 'Z', 'X', VK_SHIFT, VK_ESCAPE, VK_CONTROL, 'Q', 'S', 'R', VK_RETURN
        };
        for (unsigned int index = 0; index < sizeof(virtualKeys) / sizeof(virtualKeys[0]); ++index)
            MAP_WEB_KEY(virtualKeys[index], virtualKeys[index]);
    }
#undef MAP_WEB_KEY
#endif
#undef MAP_KEY
}

#ifdef TH08_MODERN_WEB
bool RenderWebText(GdiDc *dc, int x, int y, const char *text, int length)
{
    if (dc == NULL || dc->bitmap == NULL || dc->font == NULL)
        return false;

    volatile uint32_t status = 0;
    MAIN_THREAD_ASYNC_EM_ASM({
        const statusIndex = $12 >>> 2;
        const finish = (value) => {
            Atomics.store(HEAP32, statusIndex, value);
            Atomics.notify(HEAP32, statusIndex, 1);
        };
        try {
            if (typeof TextDecoder === 'undefined') {
                finish(-1);
                return;
            }

            // TextDecoder rejects SharedArrayBuffer-backed views.  The Web
            // build uses pthreads, so copy this short Shift-JIS string into a
            // regular ArrayBuffer before decoding it on the browser thread.
            const bytes = new Uint8Array($1);
            bytes.set(HEAPU8.subarray($0, $0 + $1));
            if (!Module.th08WebShiftJisDecoder)
                Module.th08WebShiftJisDecoder = new TextDecoder('shift_jis');
            const value = Module.th08WebShiftJisDecoder.decode(bytes);
            if (!value) {
                finish(1);
                return;
            }

            let canvas = Module.th08WebTextCanvas;
            if (!canvas) {
                canvas = document.createElement('canvas');
                Module.th08WebTextCanvas = canvas;
            }
            let context = canvas.getContext('2d', {willReadFrequently: true});
            if (!context) {
                finish(-1);
                return;
            }

            const fontHeight = Math.max(1, $10 | 0);
            const fontWeight = ($11 | 0) >= 600 ? 600 : 400;
            const font = fontWeight + ' ' + fontHeight + 'px sans-serif';
            context.font = font;
            const measuredWidth = Math.ceil(context.measureText(value).width) + 4;
            const remainingWidth = ($4 | 0) - ($2 | 0);
            const remainingHeight = ($5 | 0) - ($3 | 0);
            if (remainingWidth <= 0 || remainingHeight <= 0) {
                finish(1);
                return;
            }
            const drawWidth = Math.max(1, Math.min(measuredWidth, remainingWidth));
            const drawHeight = Math.max(1, Math.min(fontHeight + 8, remainingHeight));

            if (canvas.width !== drawWidth || canvas.height !== drawHeight) {
                canvas.width = drawWidth;
                canvas.height = drawHeight;
                context = canvas.getContext('2d', {willReadFrequently: true});
            }
            context.clearRect(0, 0, drawWidth, drawHeight);
            context.font = font;
            context.textBaseline = 'top';
            context.fillStyle = '#fff';
            context.fillText(value, 0, 0);

            const alpha = context.getImageData(0, 0, drawWidth, drawHeight).data;
            const destination = $6 | 0;
            const pitch = $7 | 0;
            const bits = $8 | 0;
            const color = $9 >>> 0;
            const red = color & 255;
            const green = (color >>> 8) & 255;
            const blue = (color >>> 16) & 255;
            let paintedPixels = 0;
            for (let row = 0; row < drawHeight; ++row) {
                const outputY = ($3 | 0) + row;
                if (outputY < 0 || outputY >= ($5 | 0))
                    continue;
                for (let column = 0; column < drawWidth; ++column) {
                    const outputX = ($2 | 0) + column;
                    if (outputX < 0 || outputX >= ($4 | 0))
                        continue;
                    const coverage = alpha[(row * drawWidth + column) * 4 + 3];
                    if (coverage === 0)
                        continue;
                    paintedPixels++;
                    const inverse = 255 - coverage;
                    const pixel = destination + outputY * pitch + outputX * (bits >>> 3);
                    if (bits === 32) {
                        HEAPU8[pixel] = (HEAPU8[pixel] * inverse + blue * coverage) / 255;
                        HEAPU8[pixel + 1] = (HEAPU8[pixel + 1] * inverse + green * coverage) / 255;
                        HEAPU8[pixel + 2] = (HEAPU8[pixel + 2] * inverse + red * coverage) / 255;
                        HEAPU8[pixel + 3] = 0;
                    } else if (bits === 16) {
                        const wordIndex = pixel >>> 1;
                        const packed = HEAPU16[wordIndex];
                        let oldRed = ((packed >>> 10) & 31) * 255 / 31;
                        let oldGreen = ((packed >>> 5) & 31) * 255 / 31;
                        let oldBlue = (packed & 31) * 255 / 31;
                        oldRed = (oldRed * inverse + red * coverage) / 255;
                        oldGreen = (oldGreen * inverse + green * coverage) / 255;
                        oldBlue = (oldBlue * inverse + blue * coverage) / 255;
                        HEAPU16[wordIndex] = ((oldRed >>> 3) << 10) |
                            ((oldGreen >>> 3) << 5) | (oldBlue >>> 3);
                    }
                }
            }
            finish(Math.min(paintedPixels, 0x7ffffffe) + 1);
        } catch (error) {
            if (!Module.th08WebTextErrorLogged) {
                console.error('TH08 Web text rendering failed:', error);
                Module.th08WebTextErrorLogged = true;
            }
            finish(-1);
        }
    }, text, length, x, y, dc->bitmap->width, dc->bitmap->height,
       &dc->bitmap->pixels[0], dc->bitmap->pitch, dc->bitmap->bits, dc->color,
       dc->font->height, dc->font->weight, &status);

    uint32_t *statusAddress = const_cast<uint32_t *>(&status);
    emscripten_atomic_wait_u32(statusAddress, 0, 30000000000LL);
    const uint32_t result = emscripten_atomic_load_u32(statusAddress);
    static unsigned int loggedRenders;
    if (loggedRenders < 3 && result != 0 && result != 0xffffffffU)
    {
        fprintf(stderr, "th08-web: browser text rasterized %lu covered pixels\n",
                static_cast<unsigned long>(result - 1));
        loggedRenders++;
    }
    return result != 0 && result != 0xffffffffU;
}
#endif
} // namespace

extern "C" SDL_Window *th08_linux_get_window() { return g_window; }

extern "C" {
#ifdef TH08_MODERN_WEB
EMSCRIPTEN_KEEPALIVE void th08_web_set_retail_file_sizes(DWORD gameDataSize, DWORD bgmDataSize)
{
    g_retailFileSizes[0] = gameDataSize;
    g_retailFileSizes[1] = bgmDataSize;
    fprintf(stderr, "th08-web: registered local retail data (%lu and %lu bytes)\n",
            static_cast<unsigned long>(gameDataSize), static_cast<unsigned long>(bgmDataSize));
}

EMSCRIPTEN_KEEPALIVE void th08_web_configure_io_diagnostics(BOOL enabled)
{
    g_webIoDiagnostics = enabled != FALSE;
}

EMSCRIPTEN_KEEPALIVE void *th08_web_allocate_game_data(DWORD gameDataSize)
{
    BYTE *data = static_cast<BYTE *>(malloc(gameDataSize));
    if (data != NULL)
    {
        g_retailFileData[0] = data;
        g_retailFileSizes[0] = gameDataSize;
    }
    return data;
}

EMSCRIPTEN_KEEPALIVE void th08_web_set_key_state(DWORD virtualKey, BOOL pressed)
{
    if (virtualKey < 256)
    {
        if (pressed != FALSE && emscripten_atomic_load_u32(&g_webVirtualKeys[virtualKey]) == 0)
            emscripten_atomic_store_u32(&g_webVirtualKeyPresses[virtualKey], 1);
        emscripten_atomic_store_u32(&g_webVirtualKeys[virtualKey], pressed != FALSE ? 1 : 0);
    }
}

EMSCRIPTEN_KEEPALIVE DWORD th08_web_get_key_state(DWORD virtualKey)
{
    return virtualKey < 256 ? emscripten_atomic_load_u32(&g_webVirtualKeys[virtualKey]) : 0;
}

EMSCRIPTEN_KEEPALIVE void th08_web_clear_key_state()
{
    for (unsigned int virtualKey = 0; virtualKey < 256; ++virtualKey)
    {
        emscripten_atomic_store_u32(&g_webVirtualKeys[virtualKey], 0);
        emscripten_atomic_store_u32(&g_webVirtualKeyPresses[virtualKey], 0);
    }
}
#endif

HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD, LPVOID, DWORD disposition, DWORD, HANDLE)
{
#ifdef TH08_MODERN_WEB
    const int retailIndex = RetailFileIndex(path);
    if (retailIndex >= 0 && disposition == OPEN_EXISTING &&
        (access & (GENERIC_WRITE | FILE_APPEND_DATA)) == 0 && g_retailFileSizes[retailIndex] != 0)
    {
        return new WebFileHandle(retailIndex, g_retailFileSizes[retailIndex]);
    }
#endif
    int flags = (access & (GENERIC_WRITE | FILE_APPEND_DATA)) ? O_WRONLY : O_RDONLY;
    if ((access & GENERIC_READ) && (access & GENERIC_WRITE)) flags = O_RDWR;
    if (access & FILE_APPEND_DATA) flags |= O_APPEND;
    if (disposition == CREATE_ALWAYS) flags |= O_CREAT | O_TRUNC;
    if (disposition == OPEN_ALWAYS) flags |= O_CREAT;
    int fd = open(path, flags, 0666);
    if (fd < 0) { g_lastError = errno; return INVALID_HANDLE_VALUE; }
    return new FileHandle(fd);
}

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPVOID security, DWORD disposition, DWORD attrs, HANDLE templ)
{
    char converted[PATH_MAX];
    if (wcstombs(converted, path, sizeof(converted) - 1) == static_cast<size_t>(-1))
        return INVALID_HANDLE_VALUE;
    converted[sizeof(converted) - 1] = 0;
    return CreateFileA(converted, access, share, security, disposition, attrs, templ);
}

BOOL ReadFile(HANDLE raw, LPVOID data, DWORD size, LPDWORD readSize, LPVOID)
{
    if (raw == INVALID_HANDLE_VALUE || raw == NULL) return FALSE;
#ifdef TH08_MODERN_WEB
    LinuxHandle *handle = static_cast<LinuxHandle *>(raw);
    if (handle->kind == HANDLE_WEB_FILE)
        return ReadRetailFile(static_cast<WebFileHandle *>(handle), data, size, readSize) ? TRUE : FALSE;
#endif
    ssize_t result = read(static_cast<FileHandle *>(raw)->fd, data, size);
    if (readSize != NULL) *readSize = result < 0 ? 0 : static_cast<DWORD>(result);
    return result >= 0;
}

BOOL WriteFile(HANDLE raw, LPCVOID data, DWORD size, LPDWORD written, LPVOID)
{
    if (raw == INVALID_HANDLE_VALUE || raw == NULL) return FALSE;
#ifdef TH08_MODERN_WEB
    if (static_cast<LinuxHandle *>(raw)->kind == HANDLE_WEB_FILE)
    {
        if (written != NULL) *written = 0;
        return FALSE;
    }
#endif
    ssize_t result = write(static_cast<FileHandle *>(raw)->fd, data, size);
    if (written != NULL) *written = result < 0 ? 0 : static_cast<DWORD>(result);
    return result >= 0;
}

DWORD SetFilePointer(HANDLE raw, LONG offset, LONG *, DWORD origin)
{
#ifdef TH08_MODERN_WEB
    LinuxHandle *handle = static_cast<LinuxHandle *>(raw);
    if (handle->kind == HANDLE_WEB_FILE)
    {
        WebFileHandle *file = static_cast<WebFileHandle *>(handle);
        int64_t next = origin == FILE_BEGIN ? offset :
                       origin == FILE_CURRENT ? static_cast<int64_t>(file->position) + offset :
                       static_cast<int64_t>(file->size) + offset;
        if (next < 0 || next > file->size)
            return static_cast<DWORD>(-1);
        file->position = static_cast<DWORD>(next);
        return file->position;
    }
#endif
    int whence = origin == FILE_BEGIN ? SEEK_SET : origin == FILE_CURRENT ? SEEK_CUR : SEEK_END;
    off_t result = lseek(static_cast<FileHandle *>(raw)->fd, offset, whence);
    return result < 0 ? static_cast<DWORD>(-1) : static_cast<DWORD>(result);
}

DWORD GetFileSize(HANDLE raw, LPDWORD high)
{
#ifdef TH08_MODERN_WEB
    LinuxHandle *handle = static_cast<LinuxHandle *>(raw);
    if (handle->kind == HANDLE_WEB_FILE)
    {
        if (high != NULL) *high = 0;
        return static_cast<WebFileHandle *>(handle)->size;
    }
#endif
    struct stat info;
    if (fstat(static_cast<FileHandle *>(raw)->fd, &info) != 0) return static_cast<DWORD>(-1);
    if (high != NULL) *high = static_cast<DWORD>(static_cast<unsigned long long>(info.st_size) >> 32);
    return static_cast<DWORD>(info.st_size);
}

BOOL CloseHandle(HANDLE raw)
{
    if (raw == NULL || raw == INVALID_HANDLE_VALUE) return FALSE;
    LinuxHandle *handle = static_cast<LinuxHandle *>(raw);
    if (handle->kind == HANDLE_THREAD)
    {
        ThreadHandle *thread = static_cast<ThreadHandle *>(handle);
        if (!thread->finished) return TRUE;
        if (!thread->joined) pthread_join(thread->thread, NULL);
    }
    delete handle;
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE raw)
{
#ifdef TH08_MODERN_WEB
    if (static_cast<LinuxHandle *>(raw)->kind == HANDLE_WEB_FILE)
        return TRUE;
#endif
    return fsync(static_cast<FileHandle *>(raw)->fd) == 0;
}
BOOL DeleteFileA(LPCSTR path) { return unlink(path) == 0; }

DWORD GetFileAttributesW(LPCWSTR path)
{
    char converted[PATH_MAX];
    if (wcstombs(converted, path, sizeof(converted) - 1) == static_cast<size_t>(-1)) return INVALID_FILE_ATTRIBUTES;
    struct stat info;
    if (stat(converted, &info) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(info.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

BOOL SetCurrentDirectoryW(LPCWSTR path)
{
    char converted[PATH_MAX];
    if (wcstombs(converted, path, sizeof(converted) - 1) == static_cast<size_t>(-1)) return FALSE;
    return chdir(converted) == 0;
}

HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *data)
{
    glob_t result;
    if (glob(pattern, 0, NULL, &result) != 0) return INVALID_HANDLE_VALUE;
    FindHandle *handle = new FindHandle();
    for (size_t i = 0; i < result.gl_pathc; ++i) handle->paths.push_back(result.gl_pathv[i]);
    globfree(&result);
    if (!FillFindData(handle, data)) { delete handle; return INVALID_HANDLE_VALUE; }
    return handle;
}

BOOL FindNextFileA(HANDLE raw, WIN32_FIND_DATAA *data) { return FillFindData(static_cast<FindHandle *>(raw), data); }
BOOL FindClose(HANDLE raw)
{
    if (raw == NULL || raw == INVALID_HANDLE_VALUE)
        return FALSE;
    LinuxHandle *handle = static_cast<LinuxHandle *>(raw);
    if (handle->kind != HANDLE_FIND)
        return FALSE;
    delete static_cast<FindHandle *>(handle);
    return TRUE;
}
void Sleep(DWORD milliseconds) { usleep(static_cast<useconds_t>(milliseconds) * 1000); }

DWORD timeGetTime(void)
{
    struct timeval value; gettimeofday(&value, NULL);
    return static_cast<DWORD>(value.tv_sec * 1000ULL + value.tv_usec / 1000);
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER *value) { value->QuadPart = 1000000; return TRUE; }
BOOL QueryPerformanceCounter(LARGE_INTEGER *value)
{
    struct timeval time; gettimeofday(&time, NULL);
    value->QuadPart = time.tv_sec * 1000000LL + time.tv_usec; return TRUE;
}
DWORD GetCurrentThreadId(void) { return CurrentThreadIdImpl(); }

HANDLE CreateThread(LPVOID, size_t, LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD, LPDWORD id)
{
    ThreadHandle *handle = new ThreadHandle(); handle->start = start; handle->parameter = parameter;
    if (pthread_create(&handle->thread, NULL, ThreadTrampoline, handle) != 0) { delete handle; return NULL; }
    while (handle->id == 0) sched_yield();
    if (id != NULL) *id = handle->id;
    return handle;
}

BOOL PostThreadMessageA(DWORD id, UINT message, WPARAM wparam, LPARAM lparam)
{
    MSG value; memset(&value, 0, sizeof(value)); value.message = message; value.wParam = wparam; value.lParam = lparam;
    pthread_mutex_lock(&g_messageMutex); g_threadMessages[id].push_back(value); pthread_mutex_unlock(&g_messageMutex);
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE raw, DWORD timeout)
{
    LinuxHandle *base = static_cast<LinuxHandle *>(raw);
    DWORD start = timeGetTime();
    for (;;)
    {
        if (base->kind == HANDLE_THREAD && static_cast<ThreadHandle *>(base)->finished) return WAIT_OBJECT_0;
        if (base->kind == HANDLE_EVENT)
        {
            EventHandle *event = static_cast<EventHandle *>(base);
            pthread_mutex_lock(&event->mutex);
            if (event->signaled)
            {
                if (!event->manual) event->signaled = false;
                pthread_mutex_unlock(&event->mutex); return WAIT_OBJECT_0;
            }
            pthread_mutex_unlock(&event->mutex);
        }
        if (timeout != INFINITE && timeGetTime() - start >= timeout) return WAIT_TIMEOUT;
        usleep(1000);
    }
}

DWORD MsgWaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL, DWORD timeout, DWORD)
{
    DWORD start = timeGetTime();
    for (;;)
    {
        for (DWORD i = 0; i < count; ++i) if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0) return i;
        pthread_mutex_lock(&g_messageMutex);
        bool hasMessages = !g_threadMessages[CurrentThreadIdImpl()].empty();
        pthread_mutex_unlock(&g_messageMutex);
        if (hasMessages) return count;
        if (timeout != INFINITE && timeGetTime() - start >= timeout) return WAIT_TIMEOUT;
        usleep(1000);
    }
}

HANDLE CreateEventA(LPVOID, BOOL manual, BOOL initial, LPCSTR) { return new EventHandle(manual != FALSE, initial != FALSE); }
BOOL SetEvent(HANDLE raw)
{
    EventHandle *event = static_cast<EventHandle *>(raw); pthread_mutex_lock(&event->mutex);
    event->signaled = true; pthread_cond_broadcast(&event->condition); pthread_mutex_unlock(&event->mutex); return TRUE;
}
UINT_PTR SetTimer(HWND, UINT_PTR id, UINT, void *) { return id != 0 ? id : 1; }
BOOL KillTimer(HWND, UINT_PTR) { return TRUE; }
HANDLE CreateMutexA(LPVOID, BOOL, LPCSTR) { g_lastError = 0; return new MutexHandle(); }
DWORD GetLastError(void) { return g_lastError; }
void InitializeCriticalSection(CRITICAL_SECTION *value) { pthread_mutex_init(value, NULL); }
void DeleteCriticalSection(CRITICAL_SECTION *value) { pthread_mutex_destroy(value); }
void EnterCriticalSection(CRITICAL_SECTION *value) { pthread_mutex_lock(value); }
void LeaveCriticalSection(CRITICAL_SECTION *value) { pthread_mutex_unlock(value); }

BOOL PeekMessageA(MSG *message, HWND, UINT, UINT, UINT)
{
    pthread_mutex_lock(&g_messageMutex);
    std::vector<MSG> &queue = g_threadMessages[CurrentThreadIdImpl()];
    if (!queue.empty()) { *message = queue.front(); queue.erase(queue.begin()); pthread_mutex_unlock(&g_messageMutex); return TRUE; }
    pthread_mutex_unlock(&g_messageMutex);
    bool hasMessage; PumpSdlEvents(message, &hasMessage); return hasMessage;
}
BOOL TranslateMessage(const MSG *) { return TRUE; }
LRESULT DispatchMessageA(const MSG *message) { return g_windowProcedure != NULL ? g_windowProcedure(message->hwnd, message->message, message->wParam, message->lParam) : 0; }
LRESULT DefWindowProcA(HWND, UINT, WPARAM, LPARAM) { return 0; }
BOOL RegisterClassA(const WNDCLASSA *value) { g_windowProcedure = value->lpfnWndProc; return TRUE; }

HWND CreateWindowExA(DWORD, LPCSTR, LPCSTR title, DWORD style, int, int, int width, int height, HWND, HANDLE, HINSTANCE, LPVOID)
{
#ifdef TH08_MODERN_WEB
    fprintf(stderr, "th08-web: creating the SDL/WebGL game window\n");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
    { fprintf(stderr, "th08-modern: SDL_Init failed: %s\n", SDL_GetError()); return NULL; }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1); SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
#ifdef TH08_MODERN_WEB
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
#endif
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
#ifdef TH08_MODERN_WEB
    // Browser fullscreen requires an explicit user action and hides the local
    // data provenance UI. Keep the game embedded at its native resolution.
    width = 640;
    height = 480;
#else
    if (style == WS_OVERLAPPEDWINDOW) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    else { width = 640; height = 480; }
#endif
    g_window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, flags);
    if (g_window == NULL) fprintf(stderr, "th08-modern: SDL_CreateWindow failed: %s\n", SDL_GetError());
    else SetApplicationIcon(g_window);
    return reinterpret_cast<HWND>(g_window);
}

BOOL DestroyWindow(HWND) { if (g_window != NULL) SDL_DestroyWindow(g_window); g_window = NULL; SDL_Quit(); return TRUE; }
BOOL ShowWindow(HWND, int) { return TRUE; }
BOOL MoveWindow(HWND, int, int, int, int, BOOL) { return TRUE; }
int ShowCursor(BOOL show) { return SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE); }
HCURSOR SetCursor(HCURSOR value) { return value; }
HCURSOR LoadCursorA(HINSTANCE, LPCSTR) { return reinterpret_cast<HCURSOR>(1); }
HGDIOBJ GetStockObject(int) { return NULL; }
int GetSystemMetrics(int metric) { return metric == SM_CYCAPTION ? 24 : 4; }
BOOL SystemParametersInfoA(UINT, UINT, PVOID value, UINT) { if (value != NULL) *static_cast<BOOL *>(value) = FALSE; return TRUE; }
HWND GetForegroundWindow(void) { return reinterpret_cast<HWND>(g_window); }
DWORD GetWindowThreadProcessId(HWND, LPDWORD process) { if (process != NULL) *process = getpid(); return GetCurrentThreadId(); }
BOOL AttachThreadInput(DWORD, DWORD, BOOL) { return TRUE; }
HWND SetActiveWindow(HWND window) { if (g_window != NULL) SDL_RaiseWindow(g_window); return window; }
LONG GetWindowLongA(HWND, int) { return 0; }
BOOL WINNLSEnableIME(HWND, BOOL) { return TRUE; }
BOOL GetKeyboardState(BYTE *state) { FillKeyboard(state, false); return TRUE; }
BOOL SetKeyboardState(const BYTE *) { return TRUE; }
int MessageBoxA(HWND, LPCSTR text, LPCSTR title, UINT) { fprintf(stderr, "%s: %s\n", title ? title : "TH08", text ? text : ""); return 0; }
int MessageBoxW(HWND, LPCWSTR text, LPCWSTR title, UINT) { fwprintf(stderr, L"%ls: %ls\n", title ? title : L"TH08", text ? text : L""); return 0; }

DWORD GetModuleFileNameA(HMODULE, LPSTR buffer, DWORD size)
{
    ssize_t count = readlink("/proc/self/exe", buffer, size - 1); if (count < 0) return 0;
    buffer[count] = 0; return static_cast<DWORD>(count);
}
DWORD GetConsoleTitleA(LPSTR buffer, DWORD size) { if (size) buffer[0] = 0; return 0; }
void GetStartupInfoA(STARTUPINFOA *value) { DWORD size = value->cb; memset(value, 0, size); value->cb = size; }
int MultiByteToWideChar(UINT, DWORD, LPCSTR source, int sourceSize, LPWSTR dest, int destSize)
{
    size_t result = mbstowcs(dest, source, destSize); return result == static_cast<size_t>(-1) ? 0 : static_cast<int>(result + (sourceSize < 0));
}
DWORD FormatMessageA(DWORD flags, LPCVOID, DWORD error, DWORD, LPSTR buffer, DWORD size, va_list *)
{
    const char *message = strerror(error); if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) *reinterpret_cast<char **>(buffer) = strdup(message);
    else if (size) { strncpy(buffer, message, size - 1); buffer[size - 1] = 0; } return strlen(message);
}
LPVOID LocalFree(LPVOID value) { free(value); return NULL; }
HGLOBAL GlobalAlloc(UINT, size_t size) { return calloc(1, size); }
HGLOBAL GlobalFree(HGLOBAL value) { free(value); return NULL; }
HMODULE LoadLibraryA(LPCSTR path) { return dlopen(path, RTLD_NOW); }
void *GetProcAddress(HMODULE module, LPCSTR name) { return dlsym(module, name); }
HDC CreateCompatibleDC(HDC) { return new GdiDc(); }
BOOL DeleteDC(HDC value) { delete static_cast<GdiDc *>(value); return TRUE; }
HGDIOBJ SelectObject(HDC dcRaw, HGDIOBJ objectRaw)
{
    if (dcRaw == NULL || objectRaw == NULL) return NULL;
    GdiDc *dc = static_cast<GdiDc *>(dcRaw);
    GdiObject *object = static_cast<GdiObject *>(objectRaw);
    if (object->kind == GdiObject::BITMAP)
    {
        GdiBitmap *old = dc->bitmap;
        dc->bitmap = static_cast<GdiBitmap *>(object);
        return old;
    }
    GdiFont *old = dc->font;
    dc->font = static_cast<GdiFont *>(object);
    return old;
}
BOOL DeleteObject(HGDIOBJ value) { delete static_cast<GdiObject *>(value); return TRUE; }
int SetBkMode(HDC, int mode) { return mode; }
COLORREF SetTextColor(HDC raw, COLORREF color) { GdiDc *dc = static_cast<GdiDc *>(raw); COLORREF old = dc->color; dc->color = color; return old; }
HFONT CreateFontA(int height, int, int, int, int weight, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCSTR)
{
#ifdef TH08_MODERN_WEB
    return new GdiFont(NULL, height < 0 ? -height : height, weight);
#else
    const char *path = ResolveJapaneseFont();
    if (path == NULL) return new GdiFont();
    if (!TTF_WasInit() && TTF_Init() != 0)
    {
        fprintf(stderr, "th08-modern: SDL_ttf initialization failed: %s\n", TTF_GetError());
        return new GdiFont();
    }
    TTF_Font *font = TTF_OpenFont(path, height < 0 ? -height : height);
    if (font == NULL)
    {
        fprintf(stderr, "th08-modern: unable to load Japanese font %s: %s\n", path, TTF_GetError());
        return new GdiFont();
    }
    if (weight >= FW_SEMIBOLD) TTF_SetFontStyle(font, TTF_STYLE_BOLD);
    TTF_SetFontHinting(font, TTF_HINTING_LIGHT);
    return new GdiFont(font, height < 0 ? -height : height, weight);
#endif
}
HBITMAP CreateDIBSection(HDC, const void *infoRaw, UINT, VOID **pixels, HANDLE, DWORD)
{
    const BITMAPINFO *info = static_cast<const BITMAPINFO *>(infoRaw); GdiBitmap *bitmap = new GdiBitmap(info->bmiHeader.biWidth, info->bmiHeader.biHeight, info->bmiHeader.biBitCount);
    *pixels = bitmap->pixels.empty() ? NULL : &bitmap->pixels[0]; return bitmap;
}
BOOL TextOutA(HDC dcRaw, int x, int y, LPCSTR text, int length)
{
    if (dcRaw == NULL || text == NULL || length <= 0) return FALSE;
    GdiDc *dc = static_cast<GdiDc *>(dcRaw);
    if (dc->bitmap == NULL || dc->font == NULL) return FALSE;
#ifdef TH08_MODERN_WEB
    return RenderWebText(dc, x, y, text, length) ? TRUE : FALSE;
#else
    if (dc->font->font == NULL) return FALSE;
    std::string utf8 = ConvertCp932ToUtf8(text, static_cast<size_t>(length));
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *rendered = TTF_RenderUTF8_Blended(dc->font->font, utf8.c_str(), white);
    if (rendered == NULL) return FALSE;
    SDL_Surface *glyph = SDL_ConvertSurfaceFormat(rendered, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(rendered);
    if (glyph == NULL) return FALSE;
    if (SDL_MUSTLOCK(glyph)) SDL_LockSurface(glyph);
    for (int row = 0; row < glyph->h; ++row)
    {
        const BYTE *source = static_cast<const BYTE *>(glyph->pixels) + row * glyph->pitch;
        for (int column = 0; column < glyph->w; ++column)
            PutGdiTextPixel(dc->bitmap, x + column, y + row, dc->color, source[column * 4 + 3]);
    }
    if (SDL_MUSTLOCK(glyph)) SDL_UnlockSurface(glyph);
    SDL_FreeSurface(glyph);
    return TRUE;
#endif
}
HRESULT CoInitialize(LPVOID) { return S_OK; }
void CoUninitialize(void) {}
HRESULT CoCreateInstance(REFGUID, LPVOID, DWORD, REFIID, LPVOID *) { return E_NOTIMPL; }
} // extern C

extern const GUID CLSID_ShellLink = {0};
extern const GUID IID_IShellLink = {1};
extern const GUID IID_IPersistFile = {2};
const GUID GUID_NULL = {0};
const GUID IID_IDirectSoundNotify = {3};
const GUID IID_IDirectInput8A = {4};
const GUID GUID_SysKeyboard = {5};
const GUID DIPROP_RANGE = {6};
const DIDATAFORMAT c_dfDIKeyboard = {sizeof(DIDATAFORMAT)};
const DIDATAFORMAT c_dfDIJoystick = {sizeof(DIDATAFORMAT)};

MMRESULT timeGetDevCaps(TIMECAPS *caps, UINT) { caps->wPeriodMin = 1; caps->wPeriodMax = 1000; return 0; }
MMRESULT timeBeginPeriod(UINT) { return 0; }
MMRESULT timeEndPeriod(UINT) { return 0; }
UINT timeSetEvent(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT) { static UINT id = 1; return id++; }
MMRESULT timeKillEvent(UINT) { return 0; }
MMRESULT midiOutOpen(HMIDIOUT *handle, UINT, DWORD_PTR, DWORD_PTR, DWORD) { *handle = reinterpret_cast<HMIDIOUT>(1); return 0; }
MMRESULT midiOutClose(HMIDIOUT) { return 0; }
MMRESULT midiOutReset(HMIDIOUT) { return 0; }
MMRESULT midiOutPrepareHeader(HMIDIOUT, LPMIDIHDR, UINT) { return 0; }
MMRESULT midiOutUnprepareHeader(HMIDIOUT, LPMIDIHDR, UINT) { return 0; }
MMRESULT midiOutLongMsg(HMIDIOUT, LPMIDIHDR header, UINT) { header->dwFlags |= 1; return 0; }
MMRESULT midiOutShortMsg(HMIDIOUT, DWORD) { return 0; }
MMRESULT joyGetPosEx(UINT, JOYINFOEX *) { return 1; }
MMRESULT joyGetDevCapsA(UINT_PTR, JOYCAPSA *caps, UINT) { memset(caps, 0, sizeof(*caps)); caps->wXmax = caps->wYmax = 65535; return 1; }

class LinuxInputDevice : public IDirectInputDevice8A
{
  public:
    explicit LinuxInputDevice(bool keyboard_) : refs(1), keyboard(keyboard_) {}
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT GetCapabilities(DIDEVCAPS *caps) { caps->dwAxes = keyboard ? 0 : 2; caps->dwButtons = keyboard ? 0 : 32; return S_OK; }
    HRESULT EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA, LPVOID, DWORD) { return S_OK; }
    HRESULT GetDeviceState(DWORD size, LPVOID data)
    {
        if (keyboard && size >= 256) FillKeyboard(static_cast<BYTE *>(data), true);
        else memset(data, 0, size); return S_OK;
    }
    HRESULT SetDataFormat(const DIDATAFORMAT *) { return S_OK; }
    HRESULT SetCooperativeLevel(HWND, DWORD) { return S_OK; }
    HRESULT SetProperty(REFGUID, const DIPROPHEADER *) { return S_OK; }
    HRESULT Acquire() { return S_OK; }
    HRESULT Unacquire() { return S_OK; }
    HRESULT Poll() { return S_OK; }
  private:
    ULONG refs; bool keyboard;
};

class LinuxDirectInput : public IDirectInput8A
{
  public:
    LinuxDirectInput() : refs(1) {}
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT CreateDevice(REFGUID guid, IDirectInputDevice8A **device, LPVOID)
    { *device = new LinuxInputDevice(guid.Data1 == GUID_SysKeyboard.Data1); return S_OK; }
    HRESULT EnumDevices(DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD) { return S_OK; }
  private: ULONG refs;
};

HRESULT DirectInput8Create(HINSTANCE, DWORD, REFIID, LPVOID *out, LPVOID) { *out = new LinuxDirectInput(); return S_OK; }

class LinuxSoundBuffer;

SDL_AudioDeviceID g_audioDevice;
std::vector<LinuxSoundBuffer *> g_soundBuffers;

void LockAudio()
{
    if (g_audioDevice != 0) SDL_LockAudioDevice(g_audioDevice);
}

void UnlockAudio()
{
    if (g_audioDevice != 0) SDL_UnlockAudioDevice(g_audioDevice);
}

class LinuxSoundNotify : public IDirectSoundNotify
{
  public:
    explicit LinuxSoundNotify(LinuxSoundBuffer *buffer_) : refs(1), buffer(buffer_) {}
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT SetNotificationPositions(DWORD count, const DSBPOSITIONNOTIFY *positions);
  private: ULONG refs; LinuxSoundBuffer *buffer;
};

class LinuxSoundBuffer : public IDirectSoundBuffer
{
  public:
    explicit LinuxSoundBuffer(const DSBUFFERDESC *desc)
        : refs(1), playing(false), looping(false), position(0), cursorFrame(0.0), volume(0), pan(0),
          hasFormat(false), locked(false)
    {
        memset(&format, 0, sizeof(format));
        if (desc != NULL)
        {
            bytes.resize(desc->dwBufferBytes);
            if (desc->lpwfxFormat != NULL) { format = *desc->lpwfxFormat; hasFormat = true; }
        }
        LockAudio(); g_soundBuffers.push_back(this); UnlockAudio();
    }
    LinuxSoundBuffer(const LinuxSoundBuffer &other)
        : refs(1), bytes(other.bytes), playing(false), looping(false), position(0), cursorFrame(0.0),
          volume(other.volume), pan(other.pan), format(other.format), hasFormat(other.hasFormat), locked(false)
    { LockAudio(); g_soundBuffers.push_back(this); UnlockAudio(); }
    ~LinuxSoundBuffer()
    {
        LockAudio();
        for (std::vector<LinuxSoundBuffer *>::iterator it = g_soundBuffers.begin(); it != g_soundBuffers.end(); ++it)
            if (*it == this) { g_soundBuffers.erase(it); break; }
        UnlockAudio();
    }
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT QueryInterface(REFIID, void **out) { *out = new LinuxSoundNotify(this); return S_OK; }
    HRESULT GetCurrentPosition(LPDWORD play, LPDWORD write)
    {
        LockAudio();
        if (play) *play = position;
        if (write) *write = position;
        UnlockAudio();
        return S_OK;
    }
    HRESULT GetStatus(LPDWORD status)
    { LockAudio(); *status = playing ? DSBSTATUS_PLAYING : 0; UnlockAudio(); return S_OK; }
    HRESULT Initialize(void *, const DSBUFFERDESC *) { return S_OK; }
    HRESULT Lock(DWORD offset, DWORD length, LPVOID *first, LPDWORD firstSize, LPVOID *second, LPDWORD secondSize, DWORD)
    {
        LockAudio(); locked = true;
        if (bytes.empty()) bytes.resize(length ? length : 1); offset %= bytes.size(); if (length == 0 || length > bytes.size()) length = bytes.size();
        DWORD contiguous = static_cast<DWORD>(bytes.size() - offset); if (contiguous > length) contiguous = length;
        *first = &bytes[offset]; *firstSize = contiguous; if (second) *second = length > contiguous ? &bytes[0] : NULL; if (secondSize) *secondSize = length - contiguous; return S_OK;
    }
    HRESULT Play(DWORD, DWORD, DWORD flags)
    { LockAudio(); playing = true; looping = (flags & DSBPLAY_LOOPING) != 0; UnlockAudio(); return S_OK; }
    HRESULT SetCurrentPosition(DWORD value)
    {
        LockAudio();
        position = bytes.empty() ? 0 : value % bytes.size();
        cursorFrame = FrameBytes() != 0 ? static_cast<double>(position / FrameBytes()) : 0.0;
        UnlockAudio();
        return S_OK;
    }
    HRESULT SetFormat(const WAVEFORMATEX *value)
    { if (value != NULL) { LockAudio(); format = *value; hasFormat = true; UnlockAudio(); } return S_OK; }
    HRESULT SetVolume(LONG value) { LockAudio(); volume = value; UnlockAudio(); return S_OK; }
    HRESULT SetPan(LONG value) { LockAudio(); pan = value; UnlockAudio(); return S_OK; }
    HRESULT Stop() { LockAudio(); playing = false; UnlockAudio(); return S_OK; }
    HRESULT Unlock(LPVOID, DWORD, LPVOID, DWORD)
    { if (locked) { locked = false; UnlockAudio(); } return S_OK; }
    HRESULT Restore() { return S_OK; }
    void SetNotifications(DWORD count, const DSBPOSITIONNOTIFY *positions)
    {
        LockAudio();
        if (count == 0)
            notifications.clear();
        else
            notifications.assign(positions, positions + count);
        UnlockAudio();
    }
    void Mix(Sint16 *output, int outputFrames)
    {
        const DWORD frameBytes = FrameBytes();
        if (!playing || !hasFormat || bytes.empty() || frameBytes == 0 || format.wFormatTag != WAVE_FORMAT_PCM)
            return;
        const DWORD sourceFrames = static_cast<DWORD>(bytes.size() / frameBytes);
        if (sourceFrames == 0) return;
        const double step = static_cast<double>(format.nSamplesPerSec) / 44100.0;
        const float gain = volume <= DSBVOLUME_MIN ? 0.0f : powf(10.0f, static_cast<float>(volume) / 2000.0f);
        const float panValue = pan < -10000 ? -1.0f : pan > 10000 ? 1.0f : static_cast<float>(pan) / 10000.0f;
        const float leftGain = gain * (panValue > 0.0f ? 1.0f - panValue : 1.0f);
        const float rightGain = gain * (panValue < 0.0f ? 1.0f + panValue : 1.0f);
        const DWORD oldPosition = position;
        bool wrapped = false;

        for (int frame = 0; frame < outputFrames && playing; ++frame)
        {
            DWORD sourceFrame = static_cast<DWORD>(cursorFrame);
            if (sourceFrame >= sourceFrames)
            {
                if (!looping) { playing = false; break; }
                cursorFrame -= sourceFrames; sourceFrame = static_cast<DWORD>(cursorFrame); wrapped = true;
            }
            const BYTE *source = &bytes[sourceFrame * frameBytes];
            int left, right;
            if (format.wBitsPerSample == 8)
            {
                left = (static_cast<int>(source[0]) - 128) << 8;
                right = format.nChannels > 1 ? (static_cast<int>(source[1]) - 128) << 8 : left;
            }
            else if (format.wBitsPerSample == 16)
            {
                INT16 leftSample, rightSample;
                memcpy(&leftSample, source, sizeof(leftSample));
                if (format.nChannels > 1) memcpy(&rightSample, source + sizeof(INT16), sizeof(rightSample));
                else rightSample = leftSample;
                left = leftSample; right = rightSample;
            }
            else
                break;

            int mixedLeft = output[frame * 2] + static_cast<int>(left * leftGain);
            int mixedRight = output[frame * 2 + 1] + static_cast<int>(right * rightGain);
            if (mixedLeft < -32768) mixedLeft = -32768; else if (mixedLeft > 32767) mixedLeft = 32767;
            if (mixedRight < -32768) mixedRight = -32768; else if (mixedRight > 32767) mixedRight = 32767;
            output[frame * 2] = static_cast<Sint16>(mixedLeft);
            output[frame * 2 + 1] = static_cast<Sint16>(mixedRight);
            cursorFrame += step;
        }

        if (cursorFrame >= sourceFrames)
        {
            if (looping) { cursorFrame = fmod(cursorFrame, static_cast<double>(sourceFrames)); wrapped = true; }
            else { cursorFrame = sourceFrames; playing = false; }
        }
        position = static_cast<DWORD>(cursorFrame) * frameBytes;
        for (size_t index = 0; index < notifications.size(); ++index)
        {
            const DWORD offset = notifications[index].dwOffset;
            if ((!wrapped && oldPosition <= offset && position > offset) ||
                (wrapped && (offset >= oldPosition || offset < position)))
                SetEvent(notifications[index].hEventNotify);
        }
    }
  private:
    DWORD FrameBytes() const
    { return hasFormat && format.nBlockAlign != 0 ? format.nBlockAlign : 0; }
    ULONG refs;
    std::vector<BYTE> bytes;
    bool playing, looping;
    DWORD position;
    double cursorFrame;
    LONG volume, pan;
    WAVEFORMATEX format;
    bool hasFormat, locked;
    std::vector<DSBPOSITIONNOTIFY> notifications;
};

HRESULT LinuxSoundNotify::SetNotificationPositions(DWORD count, const DSBPOSITIONNOTIFY *positions)
{ if (buffer == NULL || (count != 0 && positions == NULL)) return E_INVALIDARG; buffer->SetNotifications(count, positions); return S_OK; }

void AudioCallback(void *, Uint8 *stream, int length)
{
    memset(stream, 0, length);
    Sint16 *output = reinterpret_cast<Sint16 *>(stream);
    const int frames = length / (sizeof(Sint16) * 2);
    for (size_t index = 0; index < g_soundBuffers.size(); ++index)
        g_soundBuffers[index]->Mix(output, frames);
}

#ifdef TH08_MODERN_WEB
struct WebAudioOpenRequest
{
    SDL_AudioSpec *requested;
    SDL_AudioSpec *obtained;
    SDL_AudioDeviceID device;
};

void OpenWebAudioDevice(void *opaque)
{
    WebAudioOpenRequest *request = static_cast<WebAudioOpenRequest *>(opaque);
    request->device = SDL_OpenAudioDevice(NULL, 0, request->requested, request->obtained, 0);
}

struct WebAudioDeviceRequest
{
    SDL_AudioDeviceID device;
    int value;
};

void PauseWebAudioDevice(void *opaque)
{
    WebAudioDeviceRequest *request = static_cast<WebAudioDeviceRequest *>(opaque);
    SDL_PauseAudioDevice(request->device, request->value);
}

void CloseWebAudioDevice(void *opaque)
{
    WebAudioDeviceRequest *request = static_cast<WebAudioDeviceRequest *>(opaque);
    SDL_CloseAudioDevice(request->device);
}
#endif

void EnsureAudio()
{
    if (g_audioDevice != 0) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    { fprintf(stderr, "th08-modern: SDL audio initialization failed: %s\n", SDL_GetError()); return; }
    SDL_AudioSpec requested, obtained;
    memset(&requested, 0, sizeof(requested));
    requested.freq = 44100; requested.format = AUDIO_S16SYS; requested.channels = 2;
    requested.samples = 1024; requested.callback = AudioCallback;
#ifdef TH08_MODERN_WEB
    WebAudioOpenRequest openRequest = {&requested, &obtained, 0};
    emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VI, OpenWebAudioDevice, &openRequest);
    g_audioDevice = openRequest.device;
#else
    g_audioDevice = SDL_OpenAudioDevice(NULL, 0, &requested, &obtained, 0);
#endif
    if (g_audioDevice == 0)
    { fprintf(stderr, "th08-modern: SDL audio device unavailable: %s\n", SDL_GetError()); return; }
#ifdef TH08_MODERN_WEB
    WebAudioDeviceRequest pauseRequest = {g_audioDevice, 0};
    emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VI, PauseWebAudioDevice, &pauseRequest);
#else
    SDL_PauseAudioDevice(g_audioDevice, 0);
#endif
}

void ShutdownAudio()
{
    if (g_audioDevice == 0) return;
#ifdef TH08_MODERN_WEB
    WebAudioDeviceRequest closeRequest = {g_audioDevice, 0};
    emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VI, CloseWebAudioDevice, &closeRequest);
    g_audioDevice = 0;
#else
    SDL_CloseAudioDevice(g_audioDevice); g_audioDevice = 0;
#endif
}

class LinuxDirectSound : public IDirectSound8
{
  public:
    LinuxDirectSound() : refs(1) { EnsureAudio(); }
    ~LinuxDirectSound() { ShutdownAudio(); }
    ULONG Release() { if (--refs == 0) { delete this; return 0; } return refs; }
    HRESULT CreateSoundBuffer(const DSBUFFERDESC *desc, IDirectSoundBuffer **out, LPVOID) { *out = new LinuxSoundBuffer(desc); return S_OK; }
    HRESULT DuplicateSoundBuffer(IDirectSoundBuffer *source, IDirectSoundBuffer **out) { *out = new LinuxSoundBuffer(*static_cast<LinuxSoundBuffer *>(source)); return S_OK; }
    HRESULT SetCooperativeLevel(HWND, DWORD) { return S_OK; }
  private: ULONG refs;
};

HRESULT DirectSoundCreate8(const GUID *, LPDIRECTSOUND8 *out, LPVOID) { *out = new LinuxDirectSound(); return S_OK; }
