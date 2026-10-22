#define _CRT_SECURE_NO_WARNINGS
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Include the production backend with every focus/input observation or write
 * replaced below. No physical input, foreground changes, windows or processes
 * are created by this test. Other, uncalled desktop features retain their
 * normal imports so this remains a faithful compile of desktop_api.c. */
#define MAIN_WINDOW ((HWND)(ULONG_PTR)0x101)
#define CHILD_WINDOW ((HWND)(ULONG_PTR)0x102)
#define CHANGED_CHILD_WINDOW ((HWND)(ULONG_PTR)0x103)
#define OTHER_WINDOW ((HWND)(ULONG_PTR)0x201)
#define PROCESS_HANDLE ((HANDLE)(ULONG_PTR)0x301)
#define TIMER_HANDLE ((HANDLE)(ULONG_PTR)0x302)
#define CHILD_PROCESS_HANDLE ((HANDLE)(ULONG_PTR)0x303)
#define MONITOR_HANDLE ((HMONITOR)(ULONG_PTR)0x401)
#define FIXTURE_PID 424242
#define EVENT_LIMIT 50000
#define BATCH_LIMIT 20016
#define PACKET_QUEUE_LIMIT 64

typedef struct InputBatch { unsigned offset, requested, delivered; } InputBatch;
static struct {
    HWND foreground;
    BOOL valid, visible, enabled, iconic, focus_allowed, screen_available;
    BOOL process_alive, process_access, occluded, occluded_after_pointer_move;
    BOOL focus_lies, restore_pending;
    BOOL focus_async, null_fails, focus_allowed_after_prompt;
    BOOL unicode_control, class_available, insertion_timeout_accepted;
    BOOL child_coordinates_fail, child_coordinates_outside, read_only;
    BOOL focus_changes_after_native_click, foreground_changes_after_native_click;
    BOOL modifier_after_native_click, modifier_after_native_down, read_only_after_native_click;
    BOOL gui_focus_available, child_process, child_process_alive;
    BOOL high_resolution_timer_available, timer_available, queue_overflow;
    unsigned foreground_reads, foreground_changes_at;
    unsigned focus_changes_after_batches, modifier_after_batches;
    unsigned insertion_calls, insertion_fail_at, class_queries, unicode_queries;
    unsigned native_click_calls, native_click_fail_at, native_point_reads, cancel_calls;
    unsigned selection_start, selection_end, click_selection_start, click_selection_end;
    unsigned null_calls, prompt_calls;
    int prompt_result;
    HWND pending_foreground;
    WCHAR control_class[64];
    unsigned process_stops_on_wait, timer_set_fail_at, timer_wait_fail_at, timer_timeout_at;
    DWORD creation_time, current_pid;
    BOOL held[256];
    int forced[BATCH_LIMIT];
    INPUT events[EVENT_LIMIT];
    InputBatch batches[BATCH_LIMIT];
    unsigned event_count, batch_count, foreground_calls, restore_calls;
    unsigned process_opens, process_closes, lock_depth;
    unsigned timer_creates, timer_closes, timer_sets, timer_waits, timer_milliseconds;
    unsigned prime_waits, click_waits, scalar_waits, queue_count, queue_high_water, output_count;
    WCHAR packet_queue[PACKET_QUEUE_LIMIT], output[EVENT_LIMIT];
    POINT pointed;
} mock;
static unsigned assertions;

static void require(int ok, const char *description) {
    if (!ok) {
        fprintf(stderr, "desktop Windows input fixture failed: %s\n", description);
        exit(1);
    }
    assertions++;
}

static void reset_mock(void) {
    unsigned i;
    memset(&mock, 0, sizeof(mock));
    mock.foreground = MAIN_WINDOW;
    mock.valid = mock.visible = mock.enabled = mock.focus_allowed = TRUE;
    mock.screen_available = mock.process_alive = mock.process_access = TRUE;
    mock.gui_focus_available = mock.child_process_alive = TRUE;
    mock.unicode_control = mock.class_available = TRUE;
    wcscpy(mock.control_class, L"CustomWidget");
    mock.prompt_result = IDCANCEL;
    mock.high_resolution_timer_available = mock.timer_available = TRUE;
    mock.creation_time = 10;
    mock.current_pid = FIXTURE_PID + 99;
    for (i = 0; i < BATCH_LIMIT; i++) mock.forced[i] = -1;
    require(_putenv_s("ZSHARP_DESKTOP_DISABLE_ACTIONS", "") == 0,
            "clear action guard only inside isolated mocked fixture");
}

static HWND WINAPI fixture_GetForegroundWindow(void) {
    mock.foreground_reads++;
    if (mock.foreground_changes_at && mock.foreground_reads >= mock.foreground_changes_at)
        mock.foreground = OTHER_WINDOW;
    return mock.foreground;
}
static BOOL WINAPI fixture_IsWindow(HWND window) {
    return mock.valid && (window == MAIN_WINDOW || window == CHILD_WINDOW ||
                         window == CHANGED_CHILD_WINDOW || window == OTHER_WINDOW);
}
static BOOL WINAPI fixture_IsWindowVisible(HWND window) { (void)window; return mock.visible; }
static BOOL WINAPI fixture_IsWindowEnabled(HWND window) { (void)window; return mock.enabled; }
static BOOL WINAPI fixture_IsIconic(HWND window) { (void)window; return mock.iconic; }
static BOOL WINAPI fixture_ShowWindowAsync(HWND window, int command) {
    require(window == MAIN_WINDOW && command == SW_RESTORE, "focus only restores selected minimized window");
    mock.restore_calls++;
    if (!mock.restore_pending) mock.iconic = FALSE;
    return TRUE;
}
static BOOL WINAPI fixture_SetForegroundWindow(HWND window) {
    mock.foreground_calls++;
    if (mock.focus_allowed) {
        if (!mock.focus_lies) {
            if (mock.focus_async) mock.pending_foreground = window;
            else mock.foreground = window;
        }
        return TRUE;
    }
    SetLastError(ERROR_ACCESS_DENIED); return FALSE;
}
static DWORD WINAPI fixture_GetWindowThreadProcessId(HWND window, LPDWORD pid) {
    if (pid) *pid = window == OTHER_WINDOW ? FIXTURE_PID + 1 :
        mock.child_process && window == CHILD_WINDOW ? FIXTURE_PID + 2 : FIXTURE_PID;
    return mock.child_process && window == CHILD_WINDOW ? 7002 : 7001;
}
static DWORD WINAPI fixture_GetCurrentProcessId(void) { return mock.current_pid; }
static BOOL WINAPI fixture_GetGUIThreadInfo(DWORD thread, PGUITHREADINFO info) {
    require(thread == 7001 && info->cbSize == sizeof(*info),
            "text focus is read from the pinned root GUI thread with the correct structure size");
    if (!mock.gui_focus_available) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    info->hwndFocus = (mock.focus_changes_after_batches &&
        mock.batch_count >= mock.focus_changes_after_batches) ||
        (mock.focus_changes_after_native_click && mock.native_click_calls >= 2)
        ? CHANGED_CHILD_WINDOW : CHILD_WINDOW;
    return TRUE;
}
static BOOL WINAPI fixture_IsWindowUnicode(HWND window) {
    require(window == CHILD_WINDOW, "Unicode class detection only inspects the pinned text control");
    mock.unicode_queries++;
    return mock.unicode_control;
}
static int WINAPI fixture_GetClassNameW(HWND window, LPWSTR name, int capacity) {
    size_t length = wcslen(mock.control_class);
    require(window == CHILD_WINDOW && capacity == 64,
            "class detection reads only the pinned text control with a bounded name buffer");
    mock.class_queries++;
    if (!mock.class_available) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    require(length < (size_t)capacity, "fixture control class remains within the backend buffer");
    memcpy(name, mock.control_class, (length + 1) * sizeof(WCHAR));
    return (int)length;
}
static LRESULT WINAPI fixture_SendMessageTimeoutW(HWND window, UINT message,
    WPARAM wparam, LPARAM lparam, UINT flags, UINT timeout, PDWORD_PTR result) {
    require(result != NULL && timeout == 1000, "desktop message acknowledgments are bounded with a result pointer");
    *result = 0; /* Native edit insertion/click WndProcs may return zero. */
    if (message == WM_NULL) {
        require(window == MAIN_WINDOW && !wparam && !lparam &&
                flags == (SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT),
                "focus synchronization uses one non-input message on the selected main window");
        mock.null_calls++;
        if (mock.null_fails) { SetLastError(ERROR_TIMEOUT); return 0; }
        if (mock.pending_foreground) {
            mock.foreground = mock.pending_foreground; mock.pending_foreground = NULL;
        }
        return 1;
    }
    require(window == CHILD_WINDOW && flags == (SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT),
            "native edit messages target only the pinned child and use bounded acknowledgment flags");
    if (message == WM_CANCELMODE) {
        require(!wparam && !lparam && mock.native_click_calls && !mock.insertion_calls,
                "failed native click cleanup only cancels original control capture without replaying button or text input");
        mock.cancel_calls++;
        return 1;
    }
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) {
        require((message == WM_LBUTTONDOWN && wparam == MK_LBUTTON) ||
                (message == WM_LBUTTONUP && !wparam),
                "native edit click has exactly the appropriate down/up button state");
        require((short)LOWORD(lparam) >= 0 && (short)HIWORD(lparam) >= 0 &&
                (short)LOWORD(lparam) < 600 && (short)HIWORD(lparam) < 300,
                "native edit click uses validated child client coordinates");
        require(!mock.insertion_calls && mock.batch_count == 1 &&
                mock.batches[0].requested == 1 && mock.events[0].type == INPUT_MOUSE &&
                !(mock.events[0].mi.dwFlags & (MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_LEFTUP)),
                "native edit click is acknowledged after pointer movement without queued button events");
        mock.native_click_calls++;
        if (message == WM_LBUTTONUP) {
            mock.selection_start = mock.click_selection_start;
            mock.selection_end = mock.click_selection_end;
            if (mock.foreground_changes_after_native_click) mock.foreground = OTHER_WINDOW;
        }
        if (mock.native_click_fail_at == mock.native_click_calls) { SetLastError(ERROR_TIMEOUT); return 0; }
        return 1;
    }
    require(message == EM_REPLACESEL && wparam == TRUE && lparam && mock.native_click_calls == 2,
            "native text uses one undoable selected-range insertion after acknowledged clicks, never WM_CHAR or WM_SETTEXT");
    mock.insertion_calls++;
    if (mock.insertion_fail_at != mock.insertion_calls || mock.insertion_timeout_accepted) {
        const WCHAR *text = (const WCHAR *)lparam;
        size_t length = wcslen(text), tail;
        require(mock.selection_start <= mock.selection_end && mock.selection_end <= mock.output_count,
                "native edit fixture selection remains inside its existing document");
        tail = mock.output_count - mock.selection_end;
        require(mock.selection_start + length + tail < EVENT_LIMIT,
                "native edit selected-range insertion stays bounded");
        memmove(mock.output + mock.selection_start + length, mock.output + mock.selection_end,
                (tail + 1) * sizeof(WCHAR));
        memcpy(mock.output + mock.selection_start, text, length * sizeof(WCHAR));
        mock.output_count = mock.selection_start + (unsigned)length + (unsigned)tail;
        mock.selection_start += (unsigned)length; mock.selection_end = mock.selection_start;
    }
    if (mock.insertion_fail_at == mock.insertion_calls) { SetLastError(ERROR_TIMEOUT); return 0; }
    return 1;
}
static int WINAPI fixture_MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR title, UINT flags) {
    require(!owner && text && text[0] && title && title[0] &&
            flags == (MB_OKCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND),
            "foreground permission fallback is an explicit, cancellable, fixed explanatory prompt");
    require(wcsstr(text, L"Target: 424242") != NULL,
            "foreground confirmation identifies the intended application selector");
    mock.prompt_calls++;
    if (mock.prompt_result == IDOK && mock.focus_allowed_after_prompt) mock.focus_allowed = TRUE;
    return mock.prompt_result;
}
static HWND WINAPI fixture_GetWindow(HWND window, UINT command) {
    (void)window;
    require(command == GW_OWNER, "window selection only asks for owner");
    return NULL;
}
static BOOL WINAPI fixture_EnumWindows(WNDENUMPROC callback, LPARAM context) {
    return callback(MAIN_WINDOW, context);
}
static HWND WINAPI fixture_GetAncestor(HWND window, UINT flags) {
    require(flags == GA_ROOT || flags == GA_ROOTOWNER, "only root ancestry is queried");
    return window == CHILD_WINDOW || window == CHANGED_CHILD_WINDOW ? MAIN_WINDOW : window;
}
static BOOL WINAPI fixture_GetClientRect(HWND window, LPRECT bounds) {
    require(window == MAIN_WINDOW || window == CHILD_WINDOW,
            "client region belongs only to captured foreground window or pinned native edit");
    bounds->left = bounds->top = 0; bounds->right = 600; bounds->bottom = 300;
    return TRUE;
}
static BOOL WINAPI fixture_ClientToScreen(HWND window, LPPOINT point) {
    require(window == MAIN_WINDOW, "screen mapping belongs to captured foreground window");
    point->x += 100; point->y += 200; return TRUE;
}
static BOOL WINAPI fixture_ScreenToClient(HWND window, LPPOINT point) {
    require(window == CHILD_WINDOW, "native edit coordinates are mapped only to the selected child");
    mock.native_point_reads++;
    if (mock.child_coordinates_fail) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    point->x -= 100; point->y -= 200;
    if (mock.child_coordinates_outside) point->x = -1;
    return TRUE;
}
static LONG_PTR WINAPI fixture_GetWindowLongPtrW(HWND window, int index) {
    require(window == CHILD_WINDOW && index == GWL_STYLE,
            "native edit validation reads only its own window style");
    return mock.read_only || (mock.read_only_after_native_click && mock.native_click_calls >= 2)
        ? ES_READONLY : 0;
}
static HWND WINAPI fixture_WindowFromPoint(POINT point) {
    mock.pointed = point;
    return mock.occluded || (mock.occluded_after_pointer_move && mock.batch_count)
        ? OTHER_WINDOW : CHILD_WINDOW;
}
static int WINAPI fixture_GetSystemMetrics(int metric) {
    switch (metric) {
    case SM_XVIRTUALSCREEN: return -1920;
    case SM_YVIRTUALSCREEN: return 0;
    case SM_CXVIRTUALSCREEN: return 3840;
    case SM_CYVIRTUALSCREEN: return 1080;
    default: require(0, "unexpected system metrics query"); return 0;
    }
}
static HMONITOR WINAPI fixture_MonitorFromPoint(POINT point, DWORD flags) {
    (void)point; (void)flags;
    return mock.screen_available ? MONITOR_HANDLE : NULL;
}
static SHORT WINAPI fixture_GetAsyncKeyState(int key) {
    if (mock.modifier_after_batches && mock.batch_count >= mock.modifier_after_batches)
        mock.held[VK_LCONTROL] = TRUE;
    if (mock.modifier_after_native_click && mock.native_click_calls >= 2)
        mock.held[VK_LCONTROL] = TRUE;
    if (mock.modifier_after_native_down && mock.native_click_calls >= 1)
        mock.held[VK_LCONTROL] = TRUE;
    return key >= 0 && key < 256 && mock.held[key] ? (SHORT)0x8000 : 0;
}
static UINT WINAPI fixture_SendInput(UINT count, LPINPUT inputs, int input_size) {
    InputBatch *batch;
    UINT delivered, i;
    require(input_size == sizeof(INPUT), "production SendInput uses correct event size");
    require(mock.batch_count < BATCH_LIMIT && count <= EVENT_LIMIT - mock.event_count,
            "mock input log remains bounded");
    batch = &mock.batches[mock.batch_count];
    batch->offset = mock.event_count; batch->requested = count;
    memcpy(mock.events + mock.event_count, inputs, (size_t)count * sizeof(INPUT));
    delivered = mock.forced[mock.batch_count] >= 0 ? (UINT)mock.forced[mock.batch_count] : count;
    if (delivered > count) delivered = count;
    batch->delivered = delivered;
    mock.batch_count++; mock.event_count += count;
    for (i = 0; i < delivered; i++) if (inputs[i].type == INPUT_KEYBOARD) {
        if (inputs[i].ki.wVk < 256)
            mock.held[inputs[i].ki.wVk] = !(inputs[i].ki.dwFlags & KEYEVENTF_KEYUP);
        if ((inputs[i].ki.dwFlags & (KEYEVENTF_UNICODE | KEYEVENTF_KEYUP)) == KEYEVENTF_UNICODE) {
            /* Model a receiver with a finite untranslated VK_PACKET queue. Its
             * message pump runs at the scheduled fixture waits, never during
             * SendInput. A paragraph-sized submission would overflow it. */
            if (mock.queue_count == PACKET_QUEUE_LIMIT) mock.queue_overflow = TRUE;
            else mock.packet_queue[mock.queue_count++] = inputs[i].ki.wScan;
            if (mock.queue_count > mock.queue_high_water) mock.queue_high_water = mock.queue_count;
        }
    }
    if (delivered != count) SetLastError(ERROR_ACCESS_DENIED);
    return delivered;
}
static HANDLE WINAPI fixture_OpenProcess(DWORD rights, BOOL inherit, DWORD pid) {
    require((rights & (PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE)) ==
            (PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE) && !inherit &&
            (pid == FIXTURE_PID || (mock.child_process && pid == FIXTURE_PID + 2)),
            "automation pins only selected fixture root or child processes");
    mock.process_opens++;
    if (!mock.process_access) { SetLastError(ERROR_ACCESS_DENIED); return NULL; }
    return pid == FIXTURE_PID ? PROCESS_HANDLE : CHILD_PROCESS_HANDLE;
}
static BOOL WINAPI fixture_GetProcessTimes(HANDLE process, LPFILETIME created,
                                          LPFILETIME exited, LPFILETIME kernel, LPFILETIME user) {
    require(process == PROCESS_HANDLE || process == CHILD_PROCESS_HANDLE,
            "read pinned fixture process creation time");
    memset(created, 0, sizeof(*created)); created->dwLowDateTime = mock.creation_time;
    memset(exited, 0, sizeof(*exited)); memset(kernel, 0, sizeof(*kernel)); memset(user, 0, sizeof(*user));
    return TRUE;
}
static VOID WINAPI fixture_GetSystemTimeAsFileTime(LPFILETIME value) {
    memset(value, 0, sizeof(*value)); value->dwLowDateTime = 100;
}
static DWORD WINAPI fixture_WaitForSingleObject(HANDLE handle, DWORD milliseconds) {
    require((handle == PROCESS_HANDLE || handle == CHILD_PROCESS_HANDLE) && milliseconds == 0,
            "only poll pinned fake root or child processes");
    return (handle == CHILD_PROCESS_HANDLE ? mock.child_process_alive : mock.process_alive) ? WAIT_TIMEOUT : WAIT_OBJECT_0;
}
static HANDLE WINAPI fixture_CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES attributes, LPCWSTR name,
                                                   DWORD flags, DWORD rights) {
    require(!attributes && !name && (flags == 0x2 || flags == 0) &&
            rights == (TIMER_MODIFY_STATE | SYNCHRONIZE),
            "typing creates a private bounded-rights high-resolution timer or its regular fallback");
    mock.timer_creates++;
    if (!mock.timer_available || (flags && !mock.high_resolution_timer_available)) {
        SetLastError(ERROR_INVALID_PARAMETER); return NULL;
    }
    return TIMER_HANDLE;
}
static BOOL WINAPI fixture_SetWaitableTimer(HANDLE timer, const LARGE_INTEGER *due, LONG period,
                                           PTIMERAPCROUTINE callback, LPVOID context, BOOL resume) {
    require(timer == TIMER_HANDLE && due && due->QuadPart < 0 && !period && !callback && !context && !resume,
            "typing arms a relative one-shot timer without an APC or system wake");
    mock.timer_sets++;
    if (mock.timer_set_fail_at == mock.timer_sets) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    mock.timer_milliseconds = (unsigned)(-due->QuadPart / 10000);
    require(mock.timer_milliseconds == 1 || mock.timer_milliseconds == 10 || mock.timer_milliseconds == 2,
            "typing cadence uses only initialization, click-settle and scalar delays");
    return TRUE;
}
static DWORD WINAPI fixture_WaitForMultipleObjects(DWORD count, const HANDLE *handles,
                                                   BOOL all, DWORD milliseconds) {
    unsigned i;
    require(count == 2 && handles[0] == PROCESS_HANDLE && handles[1] == TIMER_HANDLE && !all &&
            milliseconds == mock.timer_milliseconds + 1000,
            "typing waits are bounded and prioritize target exit over the timer");
    mock.timer_waits++;
    if (mock.process_stops_on_wait == mock.timer_waits) mock.process_alive = FALSE;
    if (!mock.process_alive) return WAIT_OBJECT_0;
    if (mock.timer_wait_fail_at == mock.timer_waits) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    if (mock.timer_timeout_at == mock.timer_waits) return WAIT_TIMEOUT;
    if (mock.timer_milliseconds == 1) mock.prime_waits++;
    else if (mock.timer_milliseconds == 10) mock.click_waits++;
    else mock.scalar_waits++;
    require(mock.queue_count <= EVENT_LIMIT - mock.output_count - 1,
            "mock receiver output remains bounded");
    for (i = 0; i < mock.queue_count; i++) mock.output[mock.output_count++] = mock.packet_queue[i];
    mock.output[mock.output_count] = 0; mock.queue_count = 0;
    return WAIT_OBJECT_0 + 1;
}
static BOOL WINAPI fixture_CloseHandle(HANDLE handle) {
    require(handle == PROCESS_HANDLE || handle == CHILD_PROCESS_HANDLE || handle == TIMER_HANDLE,
            "release only pinned fake process or timer handles");
    if (handle == TIMER_HANDLE) mock.timer_closes++;
    else mock.process_closes++;
    return TRUE;
}
static VOID WINAPI fixture_AcquireSRWLockExclusive(PSRWLOCK lock) {
    (void)lock; require(mock.lock_depth == 0, "automation is serialized"); mock.lock_depth++;
}
static VOID WINAPI fixture_ReleaseSRWLockExclusive(PSRWLOCK lock) {
    (void)lock; require(mock.lock_depth == 1, "automation lock released exactly once"); mock.lock_depth--;
}
static HMODULE WINAPI fixture_GetModuleHandleW(LPCWSTR module) { (void)module; return (HMODULE)(ULONG_PTR)0x501; }
static FARPROC WINAPI fixture_GetProcAddress(HMODULE module, LPCSTR name) {
    (void)module; (void)name; return NULL;
}

#define GetForegroundWindow fixture_GetForegroundWindow
#define IsWindow fixture_IsWindow
#define IsWindowVisible fixture_IsWindowVisible
#define IsWindowEnabled fixture_IsWindowEnabled
#define IsIconic fixture_IsIconic
#define ShowWindowAsync fixture_ShowWindowAsync
#define SetForegroundWindow fixture_SetForegroundWindow
#define GetWindowThreadProcessId fixture_GetWindowThreadProcessId
#define GetCurrentProcessId fixture_GetCurrentProcessId
#define GetGUIThreadInfo fixture_GetGUIThreadInfo
#define IsWindowUnicode fixture_IsWindowUnicode
#define GetClassNameW fixture_GetClassNameW
#define SendMessageTimeoutW fixture_SendMessageTimeoutW
#define MessageBoxW fixture_MessageBoxW
#define GetWindow fixture_GetWindow
#define EnumWindows fixture_EnumWindows
#define GetAncestor fixture_GetAncestor
#define GetClientRect fixture_GetClientRect
#define ClientToScreen fixture_ClientToScreen
#define ScreenToClient fixture_ScreenToClient
#define GetWindowLongPtrW fixture_GetWindowLongPtrW
#define WindowFromPoint fixture_WindowFromPoint
#define GetSystemMetrics fixture_GetSystemMetrics
#define MonitorFromPoint fixture_MonitorFromPoint
#define GetAsyncKeyState fixture_GetAsyncKeyState
#define SendInput fixture_SendInput
#define OpenProcess fixture_OpenProcess
#define GetProcessTimes fixture_GetProcessTimes
#define GetSystemTimeAsFileTime fixture_GetSystemTimeAsFileTime
#define WaitForSingleObject fixture_WaitForSingleObject
#define CreateWaitableTimerExW fixture_CreateWaitableTimerExW
#define SetWaitableTimer fixture_SetWaitableTimer
#define WaitForMultipleObjects fixture_WaitForMultipleObjects
#define CloseHandle fixture_CloseHandle
#define AcquireSRWLockExclusive fixture_AcquireSRWLockExclusive
#define ReleaseSRWLockExclusive fixture_ReleaseSRWLockExclusive
#define GetModuleHandleW fixture_GetModuleHandleW
#define GetProcAddress fixture_GetProcAddress
#include "../native/src/desktop_api.c"

static int call_action(const char *operation, const char *first, const char *second,
                       size_t count, char *error, size_t capacity) {
    const char *arguments[] = {first, second};
    int ok = zsharp_desktop_action(operation, arguments, count, error, capacity);
    require(mock.lock_depth == 0, "every automation result releases its serialization lock");
    require(mock.process_closes == mock.process_opens || !mock.process_access,
            "every successful process pin is released");
    require(mock.timer_closes == (unsigned)(mock.timer_available && mock.timer_creates != 0),
            "every successfully created pacing timer is released on success or failure");
    return ok;
}

static unsigned keyboard_events(WORD key, DWORD flags, int unicode) {
    unsigned count = 0, i;
    for (i = 0; i < mock.event_count; i++) {
        INPUT *event = &mock.events[i];
        if (event->type == INPUT_KEYBOARD &&
            (unicode ? event->ki.wScan : event->ki.wVk) == key &&
            (event->ki.dwFlags & (KEYEVENTF_KEYUP | KEYEVENTF_UNICODE)) == flags) count++;
    }
    return count;
}

static void require_received_text(const char *expected, const char *description) {
    WCHAR wide[EVENT_LIMIT];
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, expected, -1,
                                     wide, EVENT_LIMIT);
    require(length > 0, "expected regression text is valid UTF-8");
    require(!mock.queue_overflow && !mock.queue_count &&
            mock.output_count == (unsigned)length - 1 && !wcscmp(mock.output, wide), description);
}

static void require_scalar_batches(void) {
    unsigned i;
    require(mock.batches[0].requested == 3, "region click is a separate three-event batch");
    for (i = 1; i < mock.batch_count; i++) {
        InputBatch *batch = &mock.batches[i];
        INPUT *events = mock.events + batch->offset;
        require((batch->requested == 2 || batch->requested == 4) && batch->delivered == batch->requested,
                "each successful text batch contains exactly one complete Unicode scalar");
        require(events[0].type == INPUT_KEYBOARD && events[0].ki.wVk == 0 &&
                events[0].ki.dwFlags == KEYEVENTF_UNICODE &&
                events[1].ki.wScan == events[0].ki.wScan &&
                events[1].ki.dwFlags == (KEYEVENTF_UNICODE | KEYEVENTF_KEYUP),
                "every scalar starts with its own paired Unicode code unit");
        if (batch->requested == 4) {
            require(events[0].ki.wScan >= 0xd800 && events[0].ki.wScan <= 0xdbff &&
                    events[2].type == INPUT_KEYBOARD && events[2].ki.wVk == 0 &&
                    events[2].ki.wScan >= 0xdc00 && events[2].ki.wScan <= 0xdfff &&
                    events[2].ki.dwFlags == KEYEVENTF_UNICODE &&
                    events[3].ki.wScan == events[2].ki.wScan &&
                    events[3].ki.dwFlags == (KEYEVENTF_UNICODE | KEYEVENTF_KEYUP),
                    "supplementary characters keep both surrogate pairs in a single scalar batch");
        } else require(events[0].ki.wScan < 0xd800 || events[0].ki.wScan > 0xdfff,
                       "a UTF-16 surrogate is never submitted as a stand-alone scalar");
    }
}

int main(void) {
    char error[2048];
    static const char paragraph[] =
        "A black hole is an astronomical body so compact that its gravity prevents anything, including light, from escaping. "
        "Albert Einstein's theory of general relativity, which describes gravitation as the curvature of spacetime, predicts "
        "that any sufficiently compact mass will form a black hole. The boundary of no escape is called the event horizon.";
    static const struct { const char *region; LONG x, y; } regions[] = {
        {"topleft", 200, 250}, {"topmiddle", 400, 250}, {"topright", 600, 250},
        {"middleleft", 200, 350}, {"center", 400, 350}, {"middleright", 600, 350},
        {"bottomleft", 200, 450}, {"bottommiddle", 400, 450}, {"bottomright", 600, 450}
    };
    size_t i;

    reset_mock(); mock.foreground = OTHER_WINDOW;
    require(call_action("focusWindow", "424242", NULL, 1, error, sizeof(error)),
            "focusWindow selects the exact fixture PID");
    require(mock.foreground == MAIN_WINDOW && mock.foreground_calls == 1 && !mock.event_count,
            "focusing changes only mocked foreground and injects no input");

    reset_mock(); mock.foreground = OTHER_WINDOW; mock.iconic = TRUE;
    require(call_action("focusWindow", "424242", NULL, 1, error, sizeof(error)) && mock.restore_calls == 1,
            "focusWindow restores a minimized selected window");
    reset_mock(); mock.iconic = TRUE; mock.restore_pending = TRUE;
    require(!call_action("focusWindow", "424242", NULL, 1, error, sizeof(error)) &&
            strstr(error, "restoration is still pending") && mock.restore_calls == 1 &&
            !mock.event_count && mock.foreground == MAIN_WINDOW,
            "pending asynchronous restoration is an error even when foreground already matches");

    reset_mock(); mock.foreground = OTHER_WINDOW; mock.focus_allowed = FALSE;
    require(!call_action("focusWindow", "424242", NULL, 1, error, sizeof(error)) && !mock.event_count &&
            mock.prompt_calls == 1 && strstr(error, "cancelled"),
            "denied focus offers one cancellable user switch without synthetic input");
    reset_mock(); mock.foreground = OTHER_WINDOW; mock.focus_lies = TRUE;
    require(!call_action("focusWindow", "424242", NULL, 1, error, sizeof(error)) && !mock.event_count,
            "foreground verification catches a successful API return without actual focus");
    reset_mock(); mock.foreground = OTHER_WINDOW; mock.focus_async = TRUE;
    require(call_action("focusWindow", "424242", NULL, 1, error, sizeof(error)) &&
            mock.foreground == MAIN_WINDOW && mock.foreground_calls == 1 && mock.null_calls == 1 &&
            !mock.prompt_calls && !mock.event_count,
            "asynchronous cross-thread activation is acknowledged before foreground is checked");
    reset_mock(); mock.foreground = OTHER_WINDOW; mock.focus_allowed = FALSE;
    mock.prompt_result = IDOK; mock.focus_allowed_after_prompt = TRUE;
    require(call_action("focusWindow", "424242", NULL, 1, error, sizeof(error)) &&
            mock.foreground == MAIN_WINDOW && mock.foreground_calls == 2 && mock.null_calls == 2 &&
            mock.prompt_calls == 1 && !mock.event_count,
            "one explicit user-approved switch permits a foreground retry without injecting keys");
    reset_mock(); mock.foreground = OTHER_WINDOW; mock.focus_allowed = FALSE; mock.prompt_result = IDOK;
    require(!call_action("focusWindow", "424242", NULL, 1, error, sizeof(error)) &&
            mock.prompt_calls == 1 && mock.foreground_calls == 2 && !mock.event_count,
            "continued foreground denial after approval fails without repeated prompts or input tricks");
    reset_mock(); mock.foreground = OTHER_WINDOW; mock.null_fails = TRUE;
    require(!call_action("focusWindow", "424242", NULL, 1, error, sizeof(error)) &&
            mock.null_calls == 1 && !mock.prompt_calls && !mock.event_count && strstr(error, "did not respond"),
            "unresponsive foreground activation fails on a bounded acknowledgment without prompting or injection");
    reset_mock();
    require(call_action("typeText", "", "center", 2, error, sizeof(error)) &&
            !mock.event_count && !mock.process_opens && !mock.foreground_reads,
            "empty text is a validated no-op without a click or foreground lookup");

    for (i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
        reset_mock();
        require(call_action("typeText", "A", regions[i].region, 2, error, sizeof(error)),
                "typeText supports every approved grid region");
        require(mock.pointed.x == regions[i].x && mock.pointed.y == regions[i].y,
                "grid point is relative to foreground client rectangle");
        require(mock.batch_count == 2 && mock.event_count == 5 && mock.prime_waits == 1 &&
                mock.click_waits == 1 && mock.scalar_waits == 1,
                "click and text are separate batches with timer priming, focus settling and final scalar pacing");
        require(mock.events[0].type == INPUT_MOUSE &&
                (mock.events[0].mi.dwFlags & (MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK)) ==
                    (MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK),
                "region click uses absolute virtual-desktop coordinates");
        require(keyboard_events('A', KEYEVENTF_UNICODE, 1) == 1 &&
                keyboard_events('A', KEYEVENTF_UNICODE | KEYEVENTF_KEYUP, 1) == 1,
                "text emits paired Unicode code units rather than shortcut keys");
        require_received_text("A", "the finite packet-queue receiver gets the requested region text exactly once");
    }

    reset_mock();
    require(call_action("typeText", "\xc3\xa9\xf0\x9f\x98\x80\r\n\t", "center", 2, error, sizeof(error)),
            "Unicode, supplementary characters, CRLF and tab are supported");
    require(keyboard_events(0xe9, KEYEVENTF_UNICODE, 1) == 1 &&
            keyboard_events(0xd83d, KEYEVENTF_UNICODE, 1) == 1 &&
            keyboard_events(0xde00, KEYEVENTF_UNICODE, 1) == 1,
            "non-BMP text emits correct UTF-16 surrogate pair");
    require(keyboard_events('\r', KEYEVENTF_UNICODE, 1) == 1 &&
            keyboard_events('\n', KEYEVENTF_UNICODE, 1) == 0,
            "CRLF is one newline, not two");
    require_scalar_batches();
    require_received_text("\xc3\xa9\xf0\x9f\x98\x80\r\t", "Unicode output preserves scalars and normalizes CRLF only once");

    reset_mock();
    require(call_action("typeText", paragraph, "center", 2, error, sizeof(error)),
            "the reported black-hole paragraph completes without a large input burst");
    require(strlen(paragraph) == 345 && mock.batch_count == 346 && mock.scalar_waits == 345 &&
            mock.queue_high_water == 1 && !mock.queue_overflow,
            "all 345 paragraph characters are paced below the receiver's finite packet capacity");
    require_received_text(paragraph, "the exact reported paragraph reaches the receiver without repeated characters or periods");
    require_scalar_batches();
    {
        char long_text[10001], boundary_text[256];
        static const char varied[] = "0123456789 aZ-+,.!? [](){}:/\\ Black holes 9876";
        for (i = 0; i < sizeof(long_text) - 1; i++) long_text[i] = varied[(i * 13 + i / 37) % (sizeof(varied) - 1)];
        long_text[sizeof(long_text) - 1] = 0;
        reset_mock();
        require(call_action("typeText", long_text, "center", 2, error, sizeof(error)) &&
                mock.batch_count == 10001 && mock.scalar_waits == 10000 && mock.queue_high_water == 1,
                "ten thousand varied characters never overflow a finite input receiver queue");
        require_received_text(long_text, "long varied text retains every character in order without replay");
        require_scalar_batches();
        memset(boundary_text, 'a', 63);
        memcpy(boundary_text + 63, "\xf0\x9f\x98\x80", 4);
        memset(boundary_text + 67, 'z', 63);
        memcpy(boundary_text + 130, "\xf0\x9f\x9a\x80!", 6);
        reset_mock();
        require(call_action("typeText", boundary_text, "center", 2, error, sizeof(error)) &&
                mock.queue_high_water == 2 && !mock.queue_overflow,
                "surrogates near old burst boundaries stay together without queue surge");
        require_received_text(boundary_text, "supplementary characters survive long-text boundaries exactly");
        require_scalar_batches();
    }

    /* Modern Notepad hosts a Unicode RichEdit control. Its click and insert
     * must complete in order without a late queued click moving the caret. */
    reset_mock(); wcscpy(mock.control_class, L"RichEditD2DPT");
    require(call_action("typeText", paragraph, "center", 2, error, sizeof(error)) &&
            mock.insertion_calls == 1 && mock.native_click_calls == 2 && mock.batch_count == 1 &&
            mock.event_count == 1 && !mock.scalar_waits && !mock.queue_high_water,
            "modern Notepad's RichEdit paragraph uses acknowledged click and insertion without queued clicks or Unicode packets");
    require_received_text(paragraph, "the native edit receiver gets every black-hole paragraph character exactly once");
    {
        char long_text[10001];
        static const char varied[] = "Minecraft sandboxes and black holes: 0123456789 - []{}.,!? ";
        for (i = 0; i < sizeof(long_text) - 1; i++) long_text[i] = varied[(i * 17 + i / 23) % (sizeof(varied) - 1)];
        long_text[sizeof(long_text) - 1] = 0;
        reset_mock(); wcscpy(mock.control_class, L"RichEditD2DPT");
        require(call_action("typeText", long_text, "center", 2, error, sizeof(error)) &&
                mock.insertion_calls == 1 && mock.batch_count == 1 && !mock.queue_high_water && !mock.scalar_waits,
                "ten thousand varied RichEdit characters use one acknowledged insertion without a keyboard burst or replay");
        require_received_text(long_text, "acknowledged long RichEdit text retains every character in order");
    }
    {
        static const WCHAR *const classes[] = {L"Edit", L"eDiT", L"RICHEDIT50W", L"RichEdit20W"};
        for (i = 0; i < sizeof(classes) / sizeof(classes[0]); i++) {
            reset_mock(); wcscpy(mock.control_class, classes[i]);
            require(call_action("typeText", "\xc3\xa9\xf0\x9f\x98\x80\r\n\t", "center", 2, error, sizeof(error)) &&
                    mock.insertion_calls == 1 && mock.batch_count == 1 && mock.class_queries >= 1,
                    "supported Unicode Edit/RichEdit classes preserve UTF-16 text despite zero WndProc results");
            require_received_text("\xc3\xa9\xf0\x9f\x98\x80\r\n\t", "native edit Unicode and supplementary characters preserve exact order and CRLF");
        }
    }
    reset_mock(); wcscpy(mock.control_class, L"RichEditD2DPT");
    require(call_action("typeText", "First line\nSecond line\rThird line\r\nFourth line", "center", 2, error, sizeof(error)) &&
            mock.insertion_calls == 1,
            "native edit insertion supports mixed paragraph newline encodings");
    require_received_text("First line\r\nSecond line\r\nThird line\r\nFourth line",
                          "native edit normalizes every paragraph boundary to exactly one CRLF");
    reset_mock(); wcscpy(mock.control_class, L"RichEditD2DPT");
    wcscpy(mock.output, L"before OLD after"); mock.output_count = (unsigned)wcslen(mock.output);
    mock.click_selection_start = 7; mock.click_selection_end = 10;
    require(call_action("typeText", "NEW", "center", 2, error, sizeof(error)) && mock.insertion_calls == 1,
            "native edit insertion uses the control's selected range after the acknowledged click");
    require_received_text("before NEW after", "native edit insertion preserves all existing document text outside the selected range");
    reset_mock(); wcscpy(mock.control_class, L"Edit");
    wcscpy(mock.output, L"before after"); mock.output_count = (unsigned)wcslen(mock.output);
    mock.click_selection_start = mock.click_selection_end = 7;
    require(call_action("typeText", "NEW ", "center", 2, error, sizeof(error)) && mock.insertion_calls == 1,
            "native edit insertion follows an interior caret rather than replacing or appending the entire document");
    require_received_text("before NEW after", "interior-caret insertion preserves document prefix and suffix");
    reset_mock(); wcscpy(mock.control_class, L"Edit"); mock.insertion_fail_at = 1;
    require(!call_action("typeText", "ABCDE", "center", 2, error, sizeof(error)) &&
            mock.insertion_calls == 1 && mock.batch_count == 1 &&
            strstr(error, "ABCDE") == NULL,
            "native insertion timeout stops once, never retries, falls back to keyboard packets or discloses text");
    require_received_text("", "a native insertion rejected before acceptance leaves the document untouched");
    reset_mock(); wcscpy(mock.control_class, L"RichEditD2DPT");
    mock.insertion_fail_at = 1; mock.insertion_timeout_accepted = TRUE;
    require(!call_action("typeText", "ABCDE", "center", 2, error, sizeof(error)) &&
            mock.insertion_calls == 1 && mock.batch_count == 1,
            "timeout after insertion acceptance never repeats the possibly inserted text");
    require_received_text("ABCDE", "possibly accepted timed-out insertion remains singular rather than being replayed");
    reset_mock(); wcscpy(mock.control_class, L"RichEditD2DPT"); mock.focus_changes_after_native_click = TRUE;
    require(!call_action("typeText", "ABCDE", "center", 2, error, sizeof(error)) &&
            !mock.insertion_calls && mock.batch_count == 1,
            "native delivery stops before insertion when the acknowledged click focuses a different control");
    require_received_text("", "changed control focus never redirects the native text insertion");
    reset_mock(); wcscpy(mock.control_class, L"RichEditD2DPT"); mock.foreground_changes_after_native_click = TRUE;
    require(!call_action("typeText", "ABCDE", "center", 2, error, sizeof(error)) &&
            !mock.insertion_calls && mock.batch_count == 1,
            "native delivery stops before insertion when another application takes the foreground");
    require_received_text("", "native foreground changes never redirect the requested text");
    reset_mock(); wcscpy(mock.control_class, L"Edit"); mock.modifier_after_native_click = TRUE;
    require(!call_action("typeText", "ABCDE", "center", 2, error, sizeof(error)) &&
            !mock.insertion_calls && mock.held[VK_LCONTROL],
            "native insertion stops after click if the user begins holding a shortcut modifier");
    reset_mock(); wcscpy(mock.control_class, L"Edit"); mock.modifier_after_native_down = TRUE;
    require(!call_action("typeText", "ABCDE", "center", 2, error, sizeof(error)) &&
            !mock.insertion_calls && mock.native_click_calls == 1 && mock.cancel_calls == 1 && mock.held[VK_LCONTROL],
            "native button-down is followed only by capture cancellation if user modifiers change before button-up");
    reset_mock(); wcscpy(mock.control_class, L"Edit"); mock.occluded_after_pointer_move = TRUE;
    require(!call_action("typeText", "ABCDE", "center", 2, error, sizeof(error)) &&
            mock.event_count == 1 && !mock.native_click_calls && !mock.insertion_calls,
            "a native edit covered after pointer movement receives no click or text");
    reset_mock(); wcscpy(mock.control_class, L"Edit"); mock.modifier_after_batches = 1;
    require(!call_action("typeText", "ABCDE", "center", 2, error, sizeof(error)) &&
            mock.event_count == 1 && !mock.native_click_calls && !mock.insertion_calls && mock.held[VK_LCONTROL],
            "user modifier changes after pointer movement prevent all native click and insertion messages");
    {
        unsigned failure;
        for (failure = 1; failure <= 2; failure++) {
            reset_mock(); wcscpy(mock.control_class, L"Edit"); mock.native_click_fail_at = failure;
            require(!call_action("typeText", "ABCDE", "center", 2, error, sizeof(error)) &&
                    !mock.insertion_calls && mock.native_click_calls == failure && mock.cancel_calls == 1,
                    "unacknowledged native down/up never proceeds to insertion or keyboard fallback");
        }
    }
    reset_mock(); wcscpy(mock.control_class, L"Edit"); mock.child_coordinates_fail = TRUE;
    require(!call_action("typeText", "private", "center", 2, error, sizeof(error)) &&
            !mock.event_count && !mock.native_click_calls && !mock.insertion_calls,
            "unmappable child coordinates fail before any pointer, click or text input");
    reset_mock(); wcscpy(mock.control_class, L"Edit"); mock.child_coordinates_outside = TRUE;
    require(!call_action("typeText", "private", "center", 2, error, sizeof(error)) &&
            !mock.event_count && !mock.native_click_calls && !mock.insertion_calls,
            "out-of-bounds child coordinates fail before any desktop input");
    reset_mock(); wcscpy(mock.control_class, L"Edit"); mock.read_only = TRUE;
    require(!call_action("typeText", "private", "center", 2, error, sizeof(error)) &&
            !mock.native_click_calls && !mock.insertion_calls && !mock.event_count,
            "a read-only native edit never receives pointer, click or insertion attempts");
    reset_mock(); wcscpy(mock.control_class, L"Edit"); mock.read_only_after_native_click = TRUE;
    require(!call_action("typeText", "private", "center", 2, error, sizeof(error)) &&
            mock.native_click_calls == 2 && !mock.insertion_calls && mock.event_count == 1,
            "a control becoming read-only after its click is checked again before insertion");
    reset_mock(); wcscpy(mock.control_class, L"Edit"); mock.unicode_control = FALSE;
    require(call_action("typeText", "ANSI fallback", "center", 2, error, sizeof(error)) &&
            !mock.insertion_calls && !mock.class_queries && mock.scalar_waits == 13,
            "non-Unicode controls retain keyboard input rather than receiving UTF-16 WM_CHAR");
    require_received_text("ANSI fallback", "the ANSI-control fallback preserves its original keyboard behavior");
    reset_mock(); mock.class_available = FALSE;
    require(call_action("typeText", "unknown", "center", 2, error, sizeof(error)) &&
            !mock.insertion_calls && mock.scalar_waits == 7,
            "unavailable control class retains the paced keyboard path");
    require_received_text("unknown", "unknown-class fallback retains exact paced Unicode output");
    reset_mock(); wcscpy(mock.control_class, L"Edit"); mock.current_pid = FIXTURE_PID;
    require(call_action("typeText", "self target", "center", 2, error, sizeof(error)) &&
            !mock.insertion_calls && !mock.native_click_calls && !mock.native_point_reads &&
            mock.scalar_waits == 11 && mock.batches[0].requested == 3,
            "same-process Unicode edit controls retain paced keyboard input rather than borrowing a timed-out string pointer");
    require_received_text("self target", "same-process edit fallback preserves exact paced Unicode text");
    reset_mock(); wcscpy(mock.control_class, L"RichEditD2DPT");
    mock.current_pid = FIXTURE_PID; mock.child_process = TRUE;
    require(call_action("typeText", "external child", "center", 2, error, sizeof(error)) &&
            mock.insertion_calls == 1 && mock.native_click_calls == 2 && mock.process_opens == 2 &&
            !mock.scalar_waits,
            "the native insertion eligibility check uses the hit control's PID, not its root window's PID");
    require_received_text("external child", "an external child process still receives acknowledged native insertion");

    reset_mock(); mock.high_resolution_timer_available = FALSE;
    require(call_action("typeText", "fallback", "center", 2, error, sizeof(error)) && mock.timer_creates == 2,
            "regular private timer preserves pacing when high-resolution creation is unsupported");
    require_received_text("fallback", "fallback pacing preserves exact output");
    reset_mock(); mock.timer_available = FALSE;
    require(!call_action("typeText", "private", "center", 2, error, sizeof(error)) &&
            mock.timer_creates == 2 && !mock.batch_count && strstr(error, "timer"),
            "unavailable pacing timer fails before the region click or any text");
    reset_mock(); mock.timer_set_fail_at = 1;
    require(!call_action("typeText", "private", "center", 2, error, sizeof(error)) && !mock.batch_count,
            "timer priming failure aborts before the region click");
    reset_mock(); mock.timer_wait_fail_at = 1;
    require(!call_action("typeText", "private", "center", 2, error, sizeof(error)) && !mock.batch_count,
            "timer initialization wait failure aborts before the region click");
    reset_mock(); mock.timer_timeout_at = 1;
    require(!call_action("typeText", "private", "center", 2, error, sizeof(error)) && !mock.batch_count,
            "timer initialization wait is bounded and aborts without a click on timeout");
    reset_mock(); mock.gui_focus_available = FALSE;
    require(!call_action("typeText", "private", "center", 2, error, sizeof(error)) &&
            mock.batch_count == 1 && !mock.output_count && strstr(error, "focus"),
            "unavailable focused text control prevents all Unicode input after the click");
    reset_mock(); mock.focus_changes_after_batches = 3;
    require(!call_action("typeText", "ABCDE", "center", 2, error, sizeof(error)) && mock.batch_count == 3,
            "changing the focused child control inside the same foreground window stops remaining text");
    require_received_text("AB", "focus change preserves only the delivered prefix without redirecting or replaying it");
    reset_mock(); mock.modifier_after_batches = 3;
    require(!call_action("typeText", "ABCDE", "center", 2, error, sizeof(error)) &&
            mock.batch_count == 3 && mock.held[VK_LCONTROL],
            "a user-held modifier appearing during typing stops safely without releasing the user's key");
    require_received_text("AB", "mid-text modifier changes do not replay an accepted prefix");
    reset_mock(); mock.process_stops_on_wait = 4;
    require(!call_action("typeText", "ABCDE", "center", 2, error, sizeof(error)) &&
            mock.batch_count == 3 && mock.timer_waits == 4 && strstr(error, "exited"),
            "target process exit interrupts scalar pacing without further injection");
    reset_mock(); mock.timer_set_fail_at = 4;
    require(!call_action("typeText", "ABCDE", "center", 2, error, sizeof(error)) && mock.batch_count == 3,
            "a later timer failure stops after the accepted prefix instead of retrying it");
    reset_mock(); mock.child_process = TRUE;
    require(call_action("typeText", "child", "center", 2, error, sizeof(error)) && mock.process_opens == 2,
            "a focused text control hosted by a child process receives its own lifetime pin");
    require_received_text("child", "cross-process child control receives exact paced text");
    reset_mock(); mock.child_process = TRUE; mock.child_process_alive = FALSE;
    require(!call_action("typeText", "private", "center", 2, error, sizeof(error)) && mock.batch_count == 1,
            "an exited focused child process cannot receive Unicode text");

    reset_mock();
    require(!call_action("typeText", "secret text", "unknown", 2, error, sizeof(error)) && !mock.event_count &&
            !mock.foreground_calls && !mock.process_opens, "invalid region rejected before focus or input");
    require(strstr(error, "secret text") == NULL, "invalid region error does not disclose typed content");
    reset_mock();
    require(!call_action("typeText", "\xc0\xaf", "center", 2, error, sizeof(error)) && !mock.event_count,
            "malformed UTF-8 never reaches input injection");
    reset_mock(); mock.occluded = TRUE;
    require(!call_action("typeText", "private", "center", 2, error, sizeof(error)) && !mock.event_count,
            "occluded target never types into another window");
    reset_mock(); mock.screen_available = FALSE;
    require(!call_action("typeText", "private", "center", 2, error, sizeof(error)) && !mock.event_count,
            "off-monitor target never receives input");
    reset_mock(); mock.held[VK_LCONTROL] = TRUE;
    require(!call_action("typeText", "private", "center", 2, error, sizeof(error)) && !mock.event_count,
            "held shortcut modifier prevents accidental command during text input");
    reset_mock(); mock.held[VK_LBUTTON] = TRUE;
    require(!call_action("typeText", "private", "center", 2, error, sizeof(error)) && !mock.event_count,
            "held mouse button prevents accidental drag during region click");

    reset_mock();
    require(call_action("sendKeys", "Ctrl+Shift+A", NULL, 1, error, sizeof(error)),
            "shortcut input supports multiple modifiers");
    require(mock.batch_count == 1 && mock.event_count == 6,
            "shortcut submits modifier-down, key pair, modifier-up in one batch");
    require(keyboard_events('A', 0, 0) == 1 && keyboard_events('A', KEYEVENTF_KEYUP, 0) == 1,
            "shortcut emits paired virtual-key base events");
    reset_mock(); mock.held[VK_LCONTROL] = TRUE;
    require(call_action("sendKeys", "Ctrl+A", NULL, 1, error, sizeof(error)),
            "existing required modifier can be reused");
    require(mock.event_count == 2 && mock.held[VK_LCONTROL],
            "shortcut never releases a modifier already held by the user");
    reset_mock(); mock.held[VK_RCONTROL] = TRUE;
    require(call_action("sendKeys", "Ctrl+Shift+A", NULL, 1, error, sizeof(error)),
            "right-side held modifier is reused while other requested modifiers are inserted");
    require(mock.event_count == 4 && mock.held[VK_RCONTROL] &&
            keyboard_events(VK_LCONTROL, KEYEVENTF_KEYUP, 0) == 0 &&
            keyboard_events(VK_RCONTROL, KEYEVENTF_KEYUP, 0) == 0,
            "no side of an existing modifier receives a synthetic release");
    reset_mock(); mock.held[VK_LMENU] = TRUE;
    require(!call_action("sendKeys", "Ctrl+A", NULL, 1, error, sizeof(error)) && !mock.event_count,
            "unrequested user-held modifier refuses an ambiguous shortcut");
    reset_mock(); mock.held['A'] = TRUE;
    require(!call_action("sendKeys", "Ctrl+A", NULL, 1, error, sizeof(error)) && !mock.event_count,
            "an already held shortcut base key is never released by automation");
    reset_mock();
    require(!call_action("sendKeys", "Ctrl++A", NULL, 1, error, sizeof(error)) && !mock.event_count &&
            !mock.process_opens, "invalid shortcut rejected before input observation");
    {
        static const struct { const char *name; WORD key; } media[] = {
            {"MEDIA_PLAY_PAUSE", VK_MEDIA_PLAY_PAUSE},
            {"media_next", VK_MEDIA_NEXT_TRACK},
            {"Media_Previous", VK_MEDIA_PREV_TRACK}
        };
        for (i = 0; i < sizeof(media) / sizeof(media[0]); i++) {
            reset_mock();
            require(call_action("sendKeys", media[i].name, NULL, 1, error, sizeof(error)) &&
                    mock.batch_count == 1 && mock.event_count == 2 && !mock.insertion_calls,
                    "media keys map case-insensitively to a single mocked consumer-key pair");
            require(mock.events[0].type == INPUT_KEYBOARD && mock.events[0].ki.wVk == media[i].key &&
                    mock.events[0].ki.dwFlags == KEYEVENTF_EXTENDEDKEY &&
                    mock.events[1].ki.wVk == media[i].key &&
                    mock.events[1].ki.dwFlags == (KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP),
                    "media consumer keys use the expected extended virtual-key down and up");
        }
        reset_mock();
        require(!call_action("sendKeys", "CTRL+MEDIA_NEXT", NULL, 1, error, sizeof(error)) &&
                !mock.event_count && !mock.process_opens,
                "modified media keys are rejected before observing or injecting desktop input");
        reset_mock(); mock.held[VK_MEDIA_PLAY_PAUSE] = TRUE;
        require(!call_action("sendKeys", "MEDIA_PLAY_PAUSE", NULL, 1, error, sizeof(error)) &&
                !mock.event_count && mock.held[VK_MEDIA_PLAY_PAUSE],
                "user-held media key is not replayed or released by automation");
        reset_mock(); mock.held[VK_LCONTROL] = TRUE;
        require(!call_action("sendKeys", "MEDIA_NEXT", NULL, 1, error, sizeof(error)) &&
                !mock.event_count && mock.held[VK_LCONTROL],
                "a held modifier prevents an unintended media combination without changing that modifier");
    }

    reset_mock(); mock.forced[0] = 1;
    require(!call_action("sendKeys", "Ctrl+A", NULL, 1, error, sizeof(error)),
            "partially delivered shortcut reports failure");
    require(mock.batch_count == 2 && mock.batches[1].requested == 1 &&
            mock.events[mock.batches[1].offset].type == INPUT_KEYBOARD &&
            (mock.events[mock.batches[1].offset].ki.dwFlags & KEYEVENTF_KEYUP),
            "partial shortcut releases only its own unmatched modifier down");
    reset_mock(); mock.forced[0] = 2;
    require(!call_action("typeText", "A", "center", 2, error, sizeof(error)),
            "partially delivered click reports failure");
    require(mock.batch_count == 2 && mock.batches[1].requested == 1 &&
            mock.events[mock.batches[1].offset].type == INPUT_MOUSE &&
            (mock.events[mock.batches[1].offset].mi.dwFlags & MOUSEEVENTF_LEFTUP),
            "partial click releases only its own unmatched button down");
    reset_mock(); mock.forced[1] = 1;
    require(!call_action("typeText", "A", "center", 2, error, sizeof(error)),
            "partially delivered Unicode pair reports failure without replay");
    require(mock.batch_count == 3 && mock.batches[2].requested == 1 &&
            mock.events[mock.batches[2].offset].type == INPUT_KEYBOARD &&
            mock.events[mock.batches[2].offset].ki.wScan == 'A' &&
            mock.events[mock.batches[2].offset].ki.dwFlags == (KEYEVENTF_UNICODE | KEYEVENTF_KEYUP),
            "partial Unicode pair releases only its own accepted code unit");
    reset_mock(); mock.forced[4] = 1;
    require(!call_action("typeText", "ABCDE", "center", 2, error, sizeof(error)) &&
            mock.batch_count == 6 && mock.output_count == 3 && !wcsncmp(mock.output, L"ABC", 3) &&
            mock.batches[5].requested == 1 &&
            mock.events[mock.batches[5].offset].ki.wScan == 'D' &&
            mock.events[mock.batches[5].offset].ki.dwFlags == (KEYEVENTF_UNICODE | KEYEVENTF_KEYUP),
            "a partial later Unicode batch only releases its unmatched down, never replays text or sends the suffix");
    reset_mock(); mock.forced[0] = 0;
    require(!call_action("sendKeys", "Ctrl+A", NULL, 1, error, sizeof(error)) && mock.batch_count == 1,
            "zero accepted events cause no replay or unmatched-release cleanup");
    reset_mock(); mock.held[VK_RCONTROL] = TRUE; mock.forced[0] = 1;
    require(!call_action("sendKeys", "Ctrl+Shift+A", NULL, 1, error, sizeof(error)) &&
            mock.batch_count == 2 && mock.held[VK_RCONTROL] &&
            mock.events[mock.batches[1].offset].ki.wVk == VK_LSHIFT &&
            keyboard_events(VK_RCONTROL, KEYEVENTF_KEYUP, 0) == 0,
            "partial cleanup preserves user-held modifiers while releasing only inserted ones");

    reset_mock(); mock.process_alive = FALSE;
    require(!call_action("sendKeys", "Enter", NULL, 1, error, sizeof(error)) && !mock.event_count,
            "exited foreground process cannot receive keystrokes");
    reset_mock(); mock.creation_time = 200;
    require(!call_action("sendKeys", "Enter", NULL, 1, error, sizeof(error)) && !mock.event_count,
            "process created after the observation boundary is rejected as recycled");
    reset_mock(); mock.process_access = FALSE;
    require(!call_action("sendKeys", "Enter", NULL, 1, error, sizeof(error)) && !mock.event_count,
            "process permission denial never falls back to unpinned input");
    reset_mock(); mock.foreground_changes_at = 2;
    require(!call_action("sendKeys", "Enter", NULL, 1, error, sizeof(error)) && !mock.event_count,
            "foreground change immediately before submission cancels keyboard input");
    reset_mock(); mock.foreground_changes_at = 2;
    require(!call_action("typeText", "private", "center", 2, error, sizeof(error)) && !mock.event_count,
            "foreground change immediately before submission cancels text and click");
    reset_mock(); mock.enabled = FALSE;
    require(!call_action("sendKeys", "Enter", NULL, 1, error, sizeof(error)) && !mock.event_count,
            "disabled foreground cannot receive keystrokes");
    reset_mock();
    require(_putenv_s("ZSHARP_DESKTOP_DISABLE_ACTIONS", "1") == 0, "enable desktop guard in fixture");
    require(!call_action("sendKeys", "Enter", NULL, 1, error, sizeof(error)) && strstr(error, "disabled") &&
            !mock.event_count && !mock.process_opens, "desktop guard prevents all mocked injection");
    require(!call_action("focusWindow", "424242", NULL, 1, error, sizeof(error)) && !mock.foreground_calls,
            "desktop guard prevents foreground changes");
    require(!call_action("typeText", "private", "center", 2, error, sizeof(error)) && !mock.event_count,
            "desktop guard prevents region click and Unicode typing");
    require(_putenv_s("ZSHARP_DESKTOP_DISABLE_ACTIONS", "") == 0, "clear fixture-only desktop guard");

    printf("desktop Windows input fixtures: %u assertions passed; all input APIs mocked\n", assertions);
    return 0;
}
#else
#include <stdio.h>
int main(void) { puts("desktop Windows input fixtures: Windows-only fixture skipped"); return 0; }
#endif
