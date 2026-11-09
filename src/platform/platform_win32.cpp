#include "platform/platform.h"
#include "engine/engine.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>
#include <stdio.h>

static_assert(sizeof(SYNCHRONIZATION_BARRIER) <= sizeof(PlatformBarrier),
              "PlatformBarrier opaque storage is too small");

#define PLATFORM_MAX_INPUT_EVENTS 4096

typedef struct Win32State
{
    HWND hwnd;
    b32 quit;
    u32 width, height;
    PlatformInput input;
    i32 last_mouse_x, last_mouse_y;
    f32 wheel_accum;

    PlatformInputEvent events[PLATFORM_MAX_INPUT_EVENTS];
    u32 event_count;
    u32 events_dropped;
} Win32State;
static Win32State g_win32;

static void win32_log(const char *message);

static void win32_push_event(const PlatformInputEvent *ev)
{
    if (g_win32.event_count < PLATFORM_MAX_INPUT_EVENTS)
        g_win32.events[g_win32.event_count++] = *ev;
    else
        g_win32.events_dropped++;
}

static LRESULT CALLBACK win32_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CLOSE:
    case WM_DESTROY:
        g_win32.quit = true;
        return 0;
    case WM_SIZE:
        g_win32.width = LOWORD(lp);
        g_win32.height = HIWORD(lp);
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    {
        b32 was_down = (lp & (1 << 30)) != 0;
        if (wp < 256)
            g_win32.input.keys[wp] = 1;
        if (!was_down && wp < 256)
        {
            PlatformInputEvent ev = {0};
            ev.type = PLATFORM_INPUT_KEY;
            ev.key.key = (u32)wp;
            ev.key.down = true;
            win32_push_event(&ev);
        }
    }
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
    {
        if (wp < 256)
            g_win32.input.keys[wp] = 0;
        PlatformInputEvent ev = {0};
        ev.type = PLATFORM_INPUT_KEY;
        ev.key.key = (u32)wp;
        ev.key.down = false;
        win32_push_event(&ev);
    }
        return 0;
    case WM_CHAR:
    {
        PlatformInputEvent ev = {0};
        ev.type = PLATFORM_INPUT_TEXT;
        ev.text.codepoint = (u32)wp;
        win32_push_event(&ev);
    }
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    {
        u32 button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)   ? PLATFORM_MOUSE_LEFT
                     : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? PLATFORM_MOUSE_RIGHT
                                                                      : PLATFORM_MOUSE_MIDDLE;
        b32 down = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);
        g_win32.input.mouse[button] = down ? 1 : 0;
        PlatformInputEvent ev = {0};
        ev.type = PLATFORM_INPUT_MOUSE_BUTTON;
        ev.mouse_button.button = button;
        ev.mouse_button.down = down;
        win32_push_event(&ev);
    }
        return 0;

    case WM_MOUSEMOVE:
    {
        i32 x = (i16)LOWORD(lp), y = (i16)HIWORD(lp);
        PlatformInputEvent ev = {0};
        ev.type = PLATFORM_INPUT_MOUSE_MOVE;
        ev.mouse_move.x = x;
        ev.mouse_move.y = y;
        ev.mouse_move.dx = x - g_win32.input.mouse_x;
        ev.mouse_move.dy = y - g_win32.input.mouse_y;
        g_win32.input.mouse_x = x;
        g_win32.input.mouse_y = y;
        win32_push_event(&ev);
    }
        return 0;

    case WM_MOUSEWHEEL:
    {
        f32 d = (f32)GET_WHEEL_DELTA_WPARAM(wp) / (f32)WHEEL_DELTA;
        g_win32.wheel_accum += d;
        PlatformInputEvent ev = {0};
        ev.type = PLATFORM_INPUT_MOUSE_WHEEL;
        ev.wheel.delta = d;
        win32_push_event(&ev);
    }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static b32 win32_process_start(PlatformProcess *out, const char *command_line, const char *working_dir)
{
    out->process = 0;
    out->output = 0;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), 0, TRUE}; // handles the child may inherit
    HANDLE read_end = 0, write_end = 0;
    if (!CreatePipe(&read_end, &write_end, &sa, 64 * 1024))
    {
        return false;
    }

    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, 0);
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cmd.exe /s /c \"%s\"", command_line);
    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = (nul != INVALID_HANDLE_VALUE) ? nul : 0;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessA(0, cmd, 0, 0, TRUE, CREATE_NO_WINDOW, 0, working_dir, &si, &pi);
    CloseHandle(write_end); // child has its own copy
    if (nul != INVALID_HANDLE_VALUE)
    {
        CloseHandle(nul);
    }

    if (!ok)
    {
        CloseHandle(read_end);
        return false;
    }
    CloseHandle(pi.hThread);
    out->process = pi.hProcess;
    out->output = read_end;
    return true;
}

static u32 win32_process_read(PlatformProcess *proc, char *buffer, u32 capacity)
{
    DWORD available = 0, got = 0;
    if (!proc->output || !PeekNamedPipe((HANDLE)proc->output, 0, 0, 0, &available, 0) || available == 0)
        return 0;
    if (!ReadFile((HANDLE)proc->output, buffer, Min(available, (DWORD)capacity), &got, 0))
        return 0;
    return (u32)got;
}

static b32 win32_process_exited(PlatformProcess *proc, i32 *out_exit_code)
{
    if (!proc->process || WaitForSingleObject((HANDLE)proc->process, 0) != WAIT_OBJECT_0)
        return false;
    DWORD code = 1;
    GetExitCodeProcess((HANDLE)proc->process, &code);
    *out_exit_code = (i32)code;
    return true;
}

static void win32_process_release(PlatformProcess *proc)
{
    if (proc->process)
        CloseHandle((HANDLE)proc->process);
    if (proc->output)
        CloseHandle((HANDLE)proc->output);
    proc->process = 0;
    proc->output = 0;
}

// memory
static void *win32_mem_reserve(usize size) { return VirtualAlloc(0, size, MEM_RESERVE, PAGE_READWRITE); }
static b32 win32_mem_commit(void *ptr, usize size) { return VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE) != 0; }
static void win32_mem_release(void *ptr, usize size)
{
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
}

// files
static PlatformFileContents win32_file_read(const char *path, Arena *arena)
{
    PlatformFileContents r = {0};
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE)
        return r;
    LARGE_INTEGER size;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0)
    {
        void *data = arena_push(arena, (usize)size.QuadPart, 16);
        DWORD got = 0;
        if (ReadFile(h, data, (DWORD)size.QuadPart, &got, 0) && got == (DWORD)size.QuadPart)
        {
            r.data = data;
            r.size = (usize)size.QuadPart;
            r.ok = true;
        }
    }
    CloseHandle(h);
    return r;
}

static b32 win32_file_write(const char *path, const void *data, usize size)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD wrote = 0;
    b32 ok = WriteFile(h, data, (DWORD)size, &wrote, 0) && wrote == (DWORD)size;
    CloseHandle(h);
    return ok;
}

static b32 win32_file_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static u64 win32_file_modified_time(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d))
        return 0;
    return ((u64)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
}

static b32 win32_file_copy(const char *src, const char *dst, b32 overwrite)
{
    if (CopyFileA(src, dst, overwrite ? FALSE : TRUE))
        return true;
    DWORD err = GetLastError();
    char msg[640];
    snprintf(msg, sizeof(msg), "win32: copy '%s' -> '%s' failed (error %lu)", src, dst, err);
    win32_log(msg);
    return false;
}

static void win32_dir_visit_files(const char *dir, PlatformDirVisitFn visit, void *user)
{
    char pattern[MAX_PATH];
    int n = snprintf(pattern, sizeof(pattern), "%s/*", dir);
    if (n < 0 || n >= (int)sizeof(pattern))
        return;
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(pattern, &found);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (found.cFileName[0] == '.')
        {
            continue; // ".", "..", and tool directories such as .git or .vs
        }

        char path[MAX_PATH];
        n = snprintf(path, sizeof(path), "%s/%s", dir, found.cFileName);
        if (n < 0 || n >= (int)sizeof(path))
        {
            continue;
        }

        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            win32_dir_visit_files(path, visit, user);
        }
        else
        {
            visit(path, ((u64)found.ftLastWriteTime.dwHighDateTime << 32) | found.ftLastWriteTime.dwLowDateTime, user);
        }
    } while (FindNextFileA(h, &found));
    FindClose(h);
}

// libraries
static void *win32_library_load(const char *path)
{
    HMODULE m = LoadLibraryA(path);
    if (!m)
    {
        DWORD err = GetLastError(); // 193 = incomplete image, 127 = imports something this exe doesn't export
        char msg[400];
        snprintf(msg, sizeof(msg), "win32: load '%s' failed (error %lu)", path, err);
        win32_log(msg);
    }
    return m;
}

static void win32_library_unload(void *handle)
{
    if (handle)
        FreeLibrary((HMODULE)handle);
}

static void *win32_library_symbol(void *handle, const char *name) { return (void *)GetProcAddress((HMODULE)handle, name); }

// threads / sync
typedef struct Win32ThreadStart
{
    PlatformThreadFunc fn;
    void *user;
} Win32ThreadStart;

static Win32ThreadStart g_thread_starts[256];
static volatile long g_thread_start_count;

static DWORD WINAPI win32_thread_trampoline(LPVOID param)
{
    Win32ThreadStart *s = (Win32ThreadStart *)param;
    s->fn(s->user);
    return 0;
}

static u32 win32_logical_core_count(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
}

static PlatformThread win32_thread_start(PlatformThreadFunc fn, void *user)
{
    long idx = _InterlockedIncrement(&g_thread_start_count) - 1;
    g_thread_starts[idx].fn = fn;
    g_thread_starts[idx].user = user;
    PlatformThread t;
    t.handle = CreateThread(0, 0, win32_thread_trampoline, &g_thread_starts[idx], 0, 0);
    return t;
}

static void win32_thread_join(PlatformThread t)
{
    if (t.handle)
    {
        WaitForSingleObject((HANDLE)t.handle, INFINITE);
        CloseHandle((HANDLE)t.handle);
    }
}

static void win32_barrier_init(PlatformBarrier *b, u32 count) { InitializeSynchronizationBarrier((LPSYNCHRONIZATION_BARRIER)b->opaque, (LONG)count, -1); }
static void win32_barrier_wait(PlatformBarrier *b) { EnterSynchronizationBarrier((LPSYNCHRONIZATION_BARRIER)b->opaque, 0); }
static void win32_barrier_destroy(PlatformBarrier *b) { DeleteSynchronizationBarrier((LPSYNCHRONIZATION_BARRIER)b->opaque); }
static PlatformSemaphore win32_semaphore_create(u32 i, u32 m)
{
    PlatformSemaphore s;
    s.handle = CreateSemaphoreA(0, (LONG)i, (LONG)m, 0);
    return s;
}

static void win32_semaphore_wait(PlatformSemaphore s) { WaitForSingleObject((HANDLE)s.handle, INFINITE); }
static b32 win32_semaphore_wait_timeout(PlatformSemaphore s, u32 ms) { return WaitForSingleObject((HANDLE)s.handle, ms) == WAIT_OBJECT_0; }
static b32 win32_semaphore_signal(PlatformSemaphore s, u32 c) { return ReleaseSemaphore((HANDLE)s.handle, (LONG)c, 0) != 0; }
static void win32_semaphore_destroy(PlatformSemaphore s) { CloseHandle((HANDLE)s.handle); }

static u64 win32_time_ticks(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (u64)c.QuadPart;
}

static u64 win32_time_frequency(void)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (u64)f.QuadPart;
}
static void win32_sleep_ms(u32 ms) { Sleep(ms); }

static b32 win32_window_open(const char *title, u32 w, u32 h)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = win32_wndproc;
    wc.hInstance = GetModuleHandleA(0);
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.lpszClassName = "EngineWindowClass";
    if (!RegisterClassA(&wc))
        return false;

    RECT rc = {0, 0, (LONG)w, (LONG)h};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_win32.hwnd = CreateWindowExA(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                   CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                                   0, 0, wc.hInstance, 0);
    if (!g_win32.hwnd)
        return false;
    g_win32.width = w;
    g_win32.height = h;
    return true;
}

static b32 win32_window_pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
            g_win32.quit = true;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    g_win32.input.mouse_dx = g_win32.input.mouse_x - g_win32.last_mouse_x;
    g_win32.input.mouse_dy = g_win32.input.mouse_y - g_win32.last_mouse_y;
    g_win32.last_mouse_x = g_win32.input.mouse_x;
    g_win32.last_mouse_y = g_win32.input.mouse_y;
    g_win32.input.mouse_wheel = g_win32.wheel_accum;
    g_win32.wheel_accum = 0.0f;
    return !g_win32.quit;
}

static void win32_window_size(u32 *w, u32 *h)
{
    if (w)
        *w = g_win32.width;
    if (h)
        *h = g_win32.height;
}

static void *win32_window_handle(void) { return g_win32.hwnd; }
static void win32_input_snapshot(PlatformInput *out) { *out = g_win32.input; }
static u32 win32_input_poll_events(PlatformInputEvent *out, u32 max)
{
    u32 n = Min(g_win32.event_count, max);
    for (u32 i = 0; i < n; ++i)
        out[i] = g_win32.events[i];
    if (g_win32.events_dropped)
    {
        win32_log("warning: input event queue overflowed");
        g_win32.events_dropped = 0;
    }
    g_win32.event_count = 0;
    return n;
}

static void win32_log(const char *message)
{
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
    printf("%s\n", message);
}
static void win32_debug_break(void) { __debugbreak(); }

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    PlatformAPI p = {0};
    p.process_start = win32_process_start;
    p.process_read = win32_process_read;
    p.process_release = win32_process_release;
    p.process_exited = win32_process_exited;
    p.mem_reserve = win32_mem_reserve;
    p.mem_commit = win32_mem_commit;
    p.mem_release = win32_mem_release;
    p.file_read = win32_file_read;
    p.file_write = win32_file_write;
    p.file_exists = win32_file_exists;
    p.file_modified_time = win32_file_modified_time;
    p.file_copy = win32_file_copy;
    p.dir_visit_files = win32_dir_visit_files;
    p.library_load = win32_library_load;
    p.library_unload = win32_library_unload;
    p.library_symbol = win32_library_symbol;
    p.logical_core_count = win32_logical_core_count;
    p.thread_start = win32_thread_start;
    p.thread_join = win32_thread_join;
    p.barrier_init = win32_barrier_init;
    p.barrier_wait = win32_barrier_wait;
    p.barrier_destroy = win32_barrier_destroy;
    p.semaphore_create = win32_semaphore_create;
    p.semaphore_wait = win32_semaphore_wait;
    p.semaphore_wait_timeout = win32_semaphore_wait_timeout;
    p.semaphore_signal = win32_semaphore_signal;
    p.semaphore_destroy = win32_semaphore_destroy;
    p.time_ticks = win32_time_ticks;
    p.time_frequency = win32_time_frequency;
    p.sleep_ms = win32_sleep_ms;
    p.window_open = win32_window_open;
    p.window_pump = win32_window_pump;
    p.window_size = win32_window_size;
    p.window_handle = win32_window_handle;
    p.input_snapshot = win32_input_snapshot;
    p.input_poll_events = win32_input_poll_events;
    p.log = win32_log;
    p.debug_break = win32_debug_break;

    BootstrapEntryPoint(&p);
    return 0;
}