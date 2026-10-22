#define _CRT_SECURE_NO_WARNINGS
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Manual, opt-in receiver regression. Merely launching this executable never
 * activates a window or injects input. A person must press its Run button.
 * Even then, the production backend can capture only this owned window: if
 * foreground changes, it sees NULL and stops instead of targeting another app.
 * No clipboard contents or existing application windows are touched. */
static HWND live_window, live_edit, live_status, live_run;
static HWND WINAPI live_owned_foreground(void) {
    HWND actual = GetForegroundWindow();
    return actual && actual == live_window ? actual : NULL;
}
#define GetForegroundWindow live_owned_foreground
#include "../native/src/desktop_api.c"
#undef GetForegroundWindow

#define LIVE_RUN 101
#define LIVE_CLOSE 102
#define LIVE_PREPARE (WM_APP + 11)
#define LIVE_VERIFY (WM_APP + 12)
#define LIVE_FINISHED (WM_APP + 13)
#define LIVE_TIMER 201
#define LIVE_CAPACITY 8192

typedef struct LiveCase { const char *name, *text; int rich; } LiveCase;
typedef struct LiveVerify {
    WCHAR expected[LIVE_CAPACITY], actual[LIVE_CAPACITY];
    HANDLE done;
    ULONGLONG deadline, exact_since;
    int matched;
} LiveVerify;
static LiveVerify live_verify;
static HWND live_close;
static HANDLE live_worker;
static HMODULE live_richedit;
static WCHAR live_log_path[MAX_PATH];
static int live_started, live_exit_code = 2, live_rich;
static const char live_paragraph[] =
    "A black hole is an astronomical body so compact that its gravity prevents anything, "
    "including light, from escaping. Albert Einstein's theory of general relativity, "
    "which describes gravitation as the curvature of spacetime, predicts that any "
    "sufficiently compact mass will form a black hole. The boundary of no escape is "
    "called the event horizon.";
static const char live_unicode[] =
    "Unicode input: caf\xc3\xa9, na\xc3\xafve, \xce\xa9, \xe4\xb8\xad\xe6\x96\x87, "
    "\xf0\x9f\x98\x80.\n"
    "Second line after LF.\r\nThird line after CRLF.\n"
    "The paragraph boundary remains intact: "
    "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ "
    "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ "
    "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ "
    "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ "
    "\xf0\x9f\x8c\x8c end.";
static char live_sequence[1400];

static int live_make_expected(const char *text) {
    WCHAR source[LIVE_CAPACITY];
    size_t i, next = 0;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                             source, LIVE_CAPACITY)) return 0;
    for (i = 0; source[i]; i++) {
        if (source[i] == L'\r' || source[i] == L'\n') {
            if (source[i] == L'\r' && source[i + 1] == L'\n') i++;
            if (next + 2 >= LIVE_CAPACITY) return 0;
            live_verify.expected[next++] = L'\r';
            live_verify.expected[next++] = L'\n';
        } else {
            if (next + 1 >= LIVE_CAPACITY) return 0;
            live_verify.expected[next++] = source[i];
        }
    }
    live_verify.expected[next] = 0;
    return 1;
}

static void live_poll_text(void) {
    ULONGLONG now = GetTickCount64();
    GetWindowTextW(live_edit, live_verify.actual, LIVE_CAPACITY);
    if (!wcscmp(live_verify.expected, live_verify.actual)) {
        if (!live_verify.exact_since) live_verify.exact_since = now;
        /* Check a brief stable receiver result, not merely accepted INPUTs. */
        if (now - live_verify.exact_since >= 120) live_verify.matched = 1;
    } else live_verify.exact_since = 0;
    if (live_verify.matched || now >= live_verify.deadline) {
        KillTimer(live_window, LIVE_TIMER);
        SetEvent(live_verify.done);
    }
}

static void live_log_mismatch(FILE *log) {
    size_t i = 0;
    while (live_verify.expected[i] && live_verify.expected[i] == live_verify.actual[i]) i++;
    fprintf(log, "First different UTF-16 unit: %zu; expected length: %zu; received length: %zu\n",
            i, wcslen(live_verify.expected), wcslen(live_verify.actual));
    fprintf(log, "Expected unit: U+%04X; received unit: U+%04X\n",
            (unsigned)live_verify.expected[i], (unsigned)live_verify.actual[i]);
}

static DWORD WINAPI live_run_tests(LPVOID unused) {
    LiveCase cases[] = {
        {"EDIT user black-hole paragraph", live_paragraph, 0},
        {"EDIT Unicode and multiline text", live_unicode, 0},
        {"EDIT 1,200-character distinguishable sequence", live_sequence, 0},
        {"Rich Edit user black-hole paragraph", live_paragraph, 1},
        {"Rich Edit 1,200-character distinguishable sequence", live_sequence, 1}
    };
    FILE *log = NULL;
    size_t i;
    int success = 1;
    (void)unused;
    if (_wfopen_s(&log, live_log_path, L"wb") || !log) {
        PostMessageW(live_window, LIVE_FINISHED, FALSE, 1); return 1;
    }
    fputs("Opt-in owned-window text receiver test. No existing apps or clipboard used.\n", log);
    fflush(log);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *arguments[] = {cases[i].text, "center"};
        char error[2048] = {0};
        DWORD_PTR prepared = 0, verify_started = 0;
        if (!live_make_expected(cases[i].text) ||
            !SendMessageTimeoutW(live_window, LIVE_PREPARE, (WPARAM)i, (LPARAM)cases[i].rich,
                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &prepared) || !prepared) {
            fprintf(log, "FAIL %s: test window was not foreground, or receiver unavailable.\n", cases[i].name);
            success = 0; break;
        }
        if (!zsharp_desktop_action("typeText", arguments, 2, error, sizeof(error))) {
            fprintf(log, "FAIL %s: %s\n", cases[i].name, error);
            success = 0; break;
        }
        ResetEvent(live_verify.done);
        live_verify.matched = 0;
        live_verify.exact_since = 0;
        if (!SendMessageTimeoutW(live_window, LIVE_VERIFY, 0, 0,
                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &verify_started) ||
            !verify_started || WaitForSingleObject(live_verify.done, 3000) != WAIT_OBJECT_0) {
            fprintf(log, "FAIL %s: receiver verification timed out.\n", cases[i].name);
            success = 0; break;
        }
        if (!live_verify.matched) {
            fprintf(log, "FAIL %s: received text differs.\n", cases[i].name);
            live_log_mismatch(log); success = 0; break;
        }
        fprintf(log, "PASS %s (%zu UTF-8 bytes; %zu received UTF-16 units).\n",
                cases[i].name, strlen(cases[i].text), wcslen(live_verify.actual));
        fflush(log);
    }
    fprintf(log, "%s: exact native EDIT receiver comparison.\n", success ? "PASS" : "FAIL");
    fclose(log);
    PostMessageW(live_window, LIVE_FINISHED, (WPARAM)success, 0);
    return success ? 0 : 1;
}

static void live_layout(HWND window) {
    RECT bounds;
    int width, height;
    GetClientRect(window, &bounds);
    width = bounds.right; height = bounds.bottom;
    MoveWindow(live_status, 18, 14, width - 36, 60, TRUE);
    MoveWindow(live_edit, 18, 80, width - 36, height - 150, TRUE);
    MoveWindow(live_run, 18, height - 52, 190, 32, TRUE);
    MoveWindow(live_close, width - 118, height - 52, 100, 32, TRUE);
}

static int live_create_edit(HWND window, int rich) {
    if (rich && !live_richedit) {
        live_richedit = LoadLibraryExW(L"Msftedit.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!live_richedit) return 0;
    }
    if (live_edit) DestroyWindow(live_edit);
    live_edit = CreateWindowExW(WS_EX_CLIENTEDGE, rich ? L"RICHEDIT50W" : L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        0, 0, 0, 0, window, NULL, NULL, NULL);
    if (!live_edit) return 0;
    live_rich = rich;
    SendMessageW(live_edit, EM_SETLIMITTEXT, LIVE_CAPACITY - 1, 0);
    SendMessageW(live_edit, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return 1;
}

static LRESULT CALLBACK live_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        live_window = window;
        live_status = CreateWindowExW(0, L"STATIC",
            L"Manual test only. Click Run paragraph tests to allow typing into this new test window. "
            L"Launching alone does not focus or type. Leave this window foreground while it runs.",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, NULL, NULL, NULL);
        if (!live_create_edit(window, 0)) return -1;
        live_run = CreateWindowExW(0, L"BUTTON", L"Run paragraph tests",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, window,
            (HMENU)(INT_PTR)LIVE_RUN, NULL, NULL);
        live_close = CreateWindowExW(0, L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, window,
            (HMENU)(INT_PTR)LIVE_CLOSE, NULL, NULL);
        SendMessageW(live_status, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(live_run, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(live_close, WM_SETFONT, (WPARAM)font, TRUE);
        return 0;
    }
    case WM_SIZE: live_layout(window); return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == LIVE_CLOSE) { SendMessageW(window, WM_CLOSE, 0, 0); return 0; }
        if (LOWORD(wparam) == LIVE_RUN && HIWORD(wparam) == BN_CLICKED && !live_started) {
            if (GetForegroundWindow() != window) return 0;
            live_started = 1;
            EnableWindow(live_run, FALSE);
            SetFocus(live_edit);
            SetWindowTextW(live_status, L"Running opt-in tests. Leave this test window foreground. "
                               L"Typing stops if another app becomes foreground.");
            live_worker = CreateThread(NULL, 0, live_run_tests, NULL, 0, NULL);
            if (!live_worker) PostMessageW(window, LIVE_FINISHED, FALSE, 2);
        }
        break;
    case LIVE_PREPARE:
        if (GetForegroundWindow() != window || !IsWindow(live_edit)) return 0;
        if ((int)lparam != live_rich) {
            if (!live_create_edit(window, (int)lparam)) return 0;
            live_layout(window);
        }
        SetWindowTextW(live_edit, L"");
        SetFocus(live_edit);
        return 1;
    case LIVE_VERIFY:
        live_verify.deadline = GetTickCount64() + 2000;
        if (!SetTimer(window, LIVE_TIMER, 20, NULL)) return 0;
        live_poll_text();
        return 1;
    case WM_TIMER:
        if (wparam == LIVE_TIMER) { live_poll_text(); return 0; }
        break;
    case LIVE_FINISHED:
        live_exit_code = wparam ? 0 : 1;
        SetWindowTextW(live_status, wparam
            ? L"PASS - all five native receiver tests matched exactly. Results saved beside this executable. "
              L"This window remains open for inspection; click Close when finished."
            : L"FAIL - text differed, the window lost foreground, or a test could not run. "
              L"See desktop-input-live-result.txt beside this executable. Nothing else was targeted.");
        SetWindowTextW(live_run, wparam ? L"PASS - five tests" : L"FAIL - see result log");
        return 0;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
        live_window = NULL;
        KillTimer(window, LIVE_TIMER);
        PostQuitMessage(live_started ? live_exit_code : 2);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show) {
    WNDCLASSW cls;
    HWND window;
    MSG message;
    WCHAR *slash;
    size_t i;
    (void)previous; (void)command_line; (void)show;
    /* Distinct chunks make missing/repeated characters identifiable. */
    for (i = 0; i < 40; i++) {
        char chunk[33];
        sprintf(chunk, "%02u:abcdefghijklmnopqrstuvwxyz ", (unsigned)i);
        strcat(live_sequence, chunk);
    }
    if (!GetModuleFileNameW(NULL, live_log_path, MAX_PATH)) return 3;
    slash = wcsrchr(live_log_path, L'\\');
    if (!slash || slash - live_log_path + 31 >= MAX_PATH) return 3;
    wcscpy(slash + 1, L"desktop-input-live-result.txt");
    live_verify.done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!live_verify.done) return 3;
    memset(&cls, 0, sizeof(cls));
    cls.lpfnWndProc = live_window_proc; cls.hInstance = instance;
    cls.lpszClassName = L"ZSharpManualTextInputRegression";
    cls.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    cls.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassW(&cls)) { CloseHandle(live_verify.done); return 3; }
    window = CreateWindowExW(WS_EX_APPWINDOW, cls.lpszClassName,
        L"Z# text input regression - temporary test", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 980, 650, NULL, NULL, instance, NULL);
    if (!window) { CloseHandle(live_verify.done); return 3; }
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    /* Closing before a test finishes cancels by destroying only the owned
     * target. Process teardown owns the remaining worker/event resources. */
    if (!live_worker || WaitForSingleObject(live_worker, 0) == WAIT_OBJECT_0)
        CloseHandle(live_verify.done);
    if (live_worker) CloseHandle(live_worker);
    if (live_richedit) FreeLibrary(live_richedit);
    return (int)message.wParam;
}
#else
#error This opt-in manual receiver fixture is Windows-only.
#endif
