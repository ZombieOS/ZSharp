#define _CRT_SECURE_NO_WARNINGS
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Opt-in manual test only. Launching this executable does nothing to other
 * apps. The operator must press Start and then activate the specifically named
 * empty Notepad fixture. These wrappers prevent the production action from
 * selecting another foreground root, using its generic keyboard fallback, or
 * delivering text to a different child control. */
static HWND WINAPI fixture_foreground(void);
static UINT WINAPI fixture_input(UINT count, LPINPUT inputs, int size);
static LRESULT WINAPI fixture_message(HWND window, UINT message, WPARAM key,
    LPARAM flags, UINT behavior, UINT timeout, PDWORD_PTR result);
#define GetForegroundWindow fixture_foreground
#define SendInput fixture_input
#define SendMessageTimeoutW fixture_message
#include "../native/src/desktop_api.c"
#undef GetForegroundWindow
#undef SendInput
#undef SendMessageTimeoutW

#define FIXTURE_FILENAME L"ZSharp-Notepad-Typing-Test.txt"
#define FIXTURE_DONE (WM_APP + 71)
#define FIXTURE_START 101
#define FIXTURE_CLOSE 102
#define FIXTURE_VERIFY 103
#define FIXTURE_LIMIT 65536
static struct {
    HWND gui, status, start, verify, close, target, document;
    HANDLE process, worker;
    DWORD pid, thread;
    DWORD query_thread, gui_error, focus_thread, focus_pid;
    HWND observed_focus, observed_root;
    BOOL gui_ok;
    GUITHREADINFO after_root, after_document;
    BOOL after_root_ok, after_document_ok, document_visible, document_enabled, document_unicode;
    DWORD after_root_error, after_document_error, document_thread, document_pid;
    HWND after_foreground, document_root;
    WCHAR after_class[128];
    DWORD_PTR read_length_hint, read_result;
    BOOL read_length_ok, read_text_ok, verify_only;
    DWORD read_length_error, read_error;
    FILETIME created;
    WCHAR class_name[128], message[1024], log_path[MAX_PATH];
    volatile LONG running;
    int passed;
} manual;

static int fixture_identity(HWND window) {
    DWORD pid = 0;
    FILETIME created, exited, kernel, user;
    WCHAR title[1024];
    return manual.target && window == manual.target && IsWindow(window) &&
        GetWindowThreadProcessId(window, &pid) == manual.thread && pid == manual.pid &&
        manual.process && WaitForSingleObject(manual.process, 0) == WAIT_TIMEOUT &&
        GetProcessTimes(manual.process, &created, &exited, &kernel, &user) &&
        CompareFileTime(&created, &manual.created) == 0 &&
        GetWindowTextW(window, title, 1024) && wcsstr(title, FIXTURE_FILENAME);
}

static int fixture_document_current(void) {
    GUITHREADINFO info;
    DWORD pid = 0;
    memset(&info, 0, sizeof(info)); info.cbSize = sizeof(info);
    return fixture_identity(manual.target) && GetForegroundWindow() == manual.target &&
        IsWindow(manual.document) && IsWindowUnicode(manual.document) &&
        GetAncestor(manual.document, GA_ROOT) == manual.target &&
        GetWindowThreadProcessId(manual.document, &pid) && pid == manual.pid &&
        GetGUIThreadInfo(manual.thread, &info) && info.hwndFocus == manual.document;
}

static HWND WINAPI fixture_foreground(void) {
    HWND current = GetForegroundWindow();
    return fixture_identity(current) ? current : NULL;
}

static UINT WINAPI fixture_input(UINT count, LPINPUT inputs, int size) {
    UINT i;
    if (!fixture_document_current()) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    for (i = 0; i < count; i++) {
        if (inputs[i].type != INPUT_MOUSE || inputs[i].mi.dwFlags !=
            (MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK)) {
            SetLastError(ERROR_ACCESS_DENIED); return 0;
        }
    }
    return SendInput(count, inputs, size);
}

static LRESULT WINAPI fixture_message(HWND window, UINT message, WPARAM key,
    LPARAM flags, UINT behavior, UINT timeout, PDWORD_PTR result) {
    if ((message == WM_CHAR || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
         message == EM_REPLACESEL || message == WM_CANCELMODE) &&
        (window != manual.document || !fixture_document_current())) {
        SetLastError(ERROR_ACCESS_DENIED); return 0;
    }
    return SendMessageTimeoutW(window, message, key, flags, behavior, timeout, result);
}

static int candidate(HWND window) {
    WCHAR title[1024], path[32768];
    WCHAR *base;
    HANDLE process;
    FILETIME created, exited, kernel, user;
    DWORD pid = 0, count = 32768, thread;
    if (!window || !GetWindowTextW(window, title, 1024) ||
        !wcsstr(title, FIXTURE_FILENAME)) return 0;
    thread = GetWindowThreadProcessId(window, &pid);
    if (!thread || !pid) return 0;
    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) return 0;
    if (!QueryFullProcessImageNameW(process, 0, path, &count) ||
        !GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        CloseHandle(process); return 0;
    }
    base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    if (_wcsicmp(base, L"Notepad.exe")) { CloseHandle(process); return 0; }
    manual.target = window; manual.pid = pid; manual.thread = thread;
    manual.process = process; manual.created = created;
    if (fixture_identity(window)) return 1;
    CloseHandle(process); manual.process = NULL; manual.target = NULL;
    return 0;
}

static void release_candidate(void) {
    if (manual.process) CloseHandle(manual.process);
    manual.process = NULL; manual.target = manual.document = NULL;
    manual.pid = manual.thread = 0;
}

static int wait_for_document(ULONGLONG deadline) {
    while (GetTickCount64() < deadline) {
        GUITHREADINFO info;
        HWND foreground = GetForegroundWindow();
        if (manual.process && (!fixture_identity(manual.target) || foreground != manual.target))
            release_candidate();
        if (!manual.process) (void)candidate(foreground);
        if (manual.process && fixture_identity(manual.target) && GetForegroundWindow() == manual.target) {
            memset(&info, 0, sizeof(info)); info.cbSize = sizeof(info);
            manual.query_thread = manual.thread;
            SetLastError(ERROR_SUCCESS);
            manual.gui_ok = GetGUIThreadInfo(manual.thread, &info);
            manual.gui_error = manual.gui_ok ? ERROR_SUCCESS : GetLastError();
            manual.observed_focus = info.hwndFocus;
            manual.observed_root = NULL; manual.focus_thread = manual.focus_pid = 0;
            if (manual.gui_ok && info.hwndFocus) {
                manual.focus_thread = GetWindowThreadProcessId(info.hwndFocus, &manual.focus_pid);
                manual.observed_root = GetAncestor(info.hwndFocus, GA_ROOT);
                if (manual.observed_root == manual.target && manual.focus_pid == manual.pid &&
                    GetClassNameW(info.hwndFocus, manual.class_name, 128) &&
                    IsWindowUnicode(info.hwndFocus) && !_wcsnicmp(manual.class_name, L"RichEdit", 8)) {
                    manual.document = info.hwndFocus;
                    if (fixture_document_current()) return 1;
                }
            }
        }
        Sleep(25); /* Activation and the subsequent document focus can settle separately. */
    }
    return 0;
}

static WCHAR *read_document(size_t *length) {
    DWORD_PTR count = 0, received = 0;
    WCHAR *text;
    manual.read_length_ok = manual.read_text_ok = FALSE;
    manual.read_length_error = manual.read_error = ERROR_SUCCESS;
    manual.read_length_hint = manual.read_result = 0;
    if (!fixture_document_current()) { manual.read_error = ERROR_ACCESS_DENIED; return NULL; }
    SetLastError(ERROR_SUCCESS);
    manual.read_length_ok = SendMessageTimeoutW(manual.document, WM_GETTEXTLENGTH, 0, 0,
        SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 1000, &count) != 0;
    manual.read_length_error = manual.read_length_ok ? ERROR_SUCCESS : GetLastError();
    manual.read_length_hint = count;
    /* RichEdit's length query can count internal CR paragraph markers while
     * WM_GETTEXT expands them to CRLF. Do not size the read from that hint. */
    text = (WCHAR *)calloc(FIXTURE_LIMIT, sizeof(WCHAR));
    if (!text) return NULL;
    if (!fixture_document_current()) {
        manual.read_error = ERROR_ACCESS_DENIED; free(text); return NULL;
    }
    SetLastError(ERROR_SUCCESS);
    manual.read_text_ok = SendMessageTimeoutW(manual.document, WM_GETTEXT, FIXTURE_LIMIT, (LPARAM)text,
        SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 1000, &received) != 0;
    manual.read_error = manual.read_text_ok ? ERROR_SUCCESS : GetLastError();
    manual.read_result = received;
    if (!manual.read_text_ok || received >= FIXTURE_LIMIT || wcslen(text) != (size_t)received) {
        free(text); return NULL;
    }
    *length = (size_t)received;
    return text;
}

/* RichEdit generations expose paragraph endings as CR, LF, or CRLF through
 * WM_GETTEXT. Normalize only line endings before exact content comparison. */
static size_t normalize_lines(WCHAR *text) {
    size_t read = 0, write = 0;
    while (text[read]) {
        WCHAR character = text[read++];
        if (character == L'\r') {
            if (text[read] == L'\n') read++;
            character = L'\n';
        }
        text[write++] = character;
    }
    text[write] = 0;
    return write;
}

static char *make_paragraphs(void) {
    static const char paragraphs[] =
        "A black hole is an astronomical body so compact that its gravity prevents anything, "
        "including light, from escaping. Albert Einstein's theory of general relativity, which "
        "describes gravitation as the curvature of spacetime, predicts that any sufficiently "
        "compact mass will form a black hole. The boundary of no escape is called the event horizon.\r\n\r\n"
        "Minecraft is a sandbox video game developed and published by the Swedish company Mojang "
        "Studios. Following its initial public alpha release as an early access game in May 2009, "
        "it was fully released in November 2011 for personal computers.\r\n\r\n"
        "Unicode: Caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x9a\x80 "
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\r\n"
        "Newline checks:\nLF line\rCR line\r\nCRLF line\r\n";
    char *text = (char *)calloc(16000, 1);
    size_t used = sizeof(paragraphs) - 1;
    unsigned i;
    if (!text) return NULL;
    memcpy(text, paragraphs, used);
    for (i = 0; i < 80; i++)
        used += (size_t)snprintf(text + used, 16000 - used,
            "Row %03u: ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 abcdefghijklmnopqrstuvwxyz.\r\n", i);
    return text;
}

static void snapshot_after_action(void) {
    manual.after_foreground = GetForegroundWindow();
    if (!fixture_identity(manual.target)) return;
    memset(&manual.after_root, 0, sizeof(manual.after_root));
    manual.after_root.cbSize = sizeof(manual.after_root);
    SetLastError(ERROR_SUCCESS);
    manual.after_root_ok = GetGUIThreadInfo(manual.thread, &manual.after_root);
    manual.after_root_error = manual.after_root_ok ? ERROR_SUCCESS : GetLastError();
    manual.document_thread = GetWindowThreadProcessId(manual.document, &manual.document_pid);
    if (!manual.document_thread || manual.document_pid != manual.pid) return;
    manual.document_root = GetAncestor(manual.document, GA_ROOT);
    manual.document_visible = IsWindowVisible(manual.document);
    manual.document_enabled = IsWindowEnabled(manual.document);
    manual.document_unicode = IsWindowUnicode(manual.document);
    GetClassNameW(manual.document, manual.after_class, 128);
    memset(&manual.after_document, 0, sizeof(manual.after_document));
    manual.after_document.cbSize = sizeof(manual.after_document);
    SetLastError(ERROR_SUCCESS);
    manual.after_document_ok = GetGUIThreadInfo(manual.document_thread, &manual.after_document);
    manual.after_document_error = manual.after_document_ok ? ERROR_SUCCESS : GetLastError();
}

static void log_result(const char *detail, size_t expected, size_t actual,
    size_t normalized, size_t mismatch) {
    FILE *file = _wfopen(manual.log_path, L"wb");
    char class_utf8[512];
    WideCharToMultiByte(CP_UTF8, 0, manual.class_name, -1, class_utf8,
        (int)sizeof(class_utf8), NULL, NULL);
    if (!file) return;
    fprintf(file, "result=%s\ncontrol_class=%s\nexpected_utf16=%zu\nreceived_utf16=%zu\n"
        "normalized_received_utf16=%zu\nfirst_mismatch=%zu\ndetail=%s\n"
        "root_query_thread=%lu\nGetGUIThreadInfo_ok=%d\nGetGUIThreadInfo_error=%lu\n"
        "observed_focus=%p\nobserved_focus_thread=%lu\nobserved_focus_pid=%lu\nobserved_focus_root=%p\n"
        "scope=explicitly named empty Notepad fixture only; no clipboard access; no keyboard fallback\n",
        manual.passed ? "PASS" : "FAIL", class_utf8, expected, actual, normalized, mismatch, detail,
        (unsigned long)manual.query_thread, (int)manual.gui_ok, (unsigned long)manual.gui_error,
        (void *)manual.observed_focus, (unsigned long)manual.focus_thread,
        (unsigned long)manual.focus_pid, (void *)manual.observed_root);
    fprintf(file, "mode=%s\nread_length_ok=%d\nread_length_error=%lu\n"
        "read_length_hint=%llu\nread_text_ok=%d\nread_text_error=%lu\nread_text_result=%llu\n",
        manual.verify_only ? "verify_existing_read_only" : "type_then_verify",
        (int)manual.read_length_ok, (unsigned long)manual.read_length_error,
        (unsigned long long)manual.read_length_hint, (int)manual.read_text_ok,
        (unsigned long)manual.read_error, (unsigned long long)manual.read_result);
    fprintf(file, "after_foreground=%p\nfixture_root=%p\nfixture_pid=%lu\n"
        "after_root_gui_ok=%d\nafter_root_gui_error=%lu\nafter_root_active=%p\n"
        "after_root_focus=%p\nafter_root_capture=%p\n"
        "document_hwnd=%p\ndocument_thread=%lu\ndocument_pid=%lu\ndocument_root=%p\n"
        "document_visible=%d\ndocument_enabled=%d\ndocument_unicode=%d\n"
        "after_document_gui_ok=%d\nafter_document_gui_error=%lu\nafter_document_active=%p\n"
        "after_document_focus=%p\nafter_document_capture=%p\n",
        (void *)manual.after_foreground, (void *)manual.target, (unsigned long)manual.pid,
        (int)manual.after_root_ok, (unsigned long)manual.after_root_error,
        (void *)manual.after_root.hwndActive, (void *)manual.after_root.hwndFocus,
        (void *)manual.after_root.hwndCapture, (void *)manual.document,
        (unsigned long)manual.document_thread, (unsigned long)manual.document_pid,
        (void *)manual.document_root, (int)manual.document_visible, (int)manual.document_enabled,
        (int)manual.document_unicode, (int)manual.after_document_ok,
        (unsigned long)manual.after_document_error, (void *)manual.after_document.hwndActive,
        (void *)manual.after_document.hwndFocus, (void *)manual.after_document.hwndCapture);
    if (manual.after_class[0]) {
        WideCharToMultiByte(CP_UTF8, 0, manual.after_class, -1, class_utf8,
            (int)sizeof(class_utf8), NULL, NULL);
        fprintf(file, "after_document_class=%s\n", class_utf8);
    }
    fclose(file);
}

static DWORD WINAPI run_fixture(void *unused) {
    ULONGLONG deadline = GetTickCount64() + 15000;
    WCHAR *initial = NULL, *expected = NULL, *actual = NULL;
    size_t initial_length = 0, expected_length = 0, actual_length = 0, normalized = 0, mismatch = 0;
    char *text = NULL, error[1024] = "";
    const char *args[2];
    const char *detail = "The named Notepad fixture was not activated within 15 seconds. Nothing was typed.";
    (void)unused;
    if (!wait_for_document(deadline)) {
        if (manual.query_thread)
            detail = "The named Notepad fixture activated, but its native RichEdit document did not acquire verifiable focus within 15 seconds. Nothing was typed.";
        goto done;
    }
    text = make_paragraphs();
    expected = text ? desktop_wide(text) : NULL;
    if (!expected) { detail = "Could not allocate fixture paragraphs. Nothing was typed."; goto done; }
    expected_length = normalize_lines(expected);
    if (!manual.verify_only) {
        initial = read_document(&initial_length);
        if (!initial || initial_length) {
            detail = "The owned Notepad document is not verifiably empty. Nothing was typed."; goto done;
        }
        args[0] = text; args[1] = "center";
        if (!zsharp_desktop_action("typeText", args, 2, error, sizeof(error))) {
            snapshot_after_action();
            detail = error[0] ? error : "Production typing action failed; no retry was attempted.";
            goto done;
        }
    }
    snapshot_after_action();
    actual = read_document(&actual_length);
    if (!actual) {
        detail = manual.verify_only ?
            "Read-only verification could not read the focused fixture safely. No input was sent." :
            "Typing returned but focus changed or the fixture text could not be read safely. No retry was attempted.";
        goto done;
    }
    normalized = normalize_lines(actual);
    while (mismatch < expected_length && mismatch < normalized && expected[mismatch] == actual[mismatch]) mismatch++;
    if (expected_length != normalized || mismatch != expected_length) {
        detail = "Received text does not exactly match the fixture paragraphs after line-ending normalization.";
        goto done;
    }
    manual.passed = 1;
    detail = manual.verify_only ?
        "Read-only verification passed: existing fixture text matches every expected character. No input was sent." :
        "Production typeText delivered every character correctly to the owned modern Notepad document.";
done:
    log_result(detail, expected_length, actual_length, normalized, mismatch);
    MultiByteToWideChar(CP_UTF8, 0, detail, -1, manual.message, 1024);
    free(initial); free(expected); free(actual); free(text);
    release_candidate();
    PostMessageW(manual.gui, FIXTURE_DONE, 0, 0);
    return 0;
}

static LRESULT CALLBACK gui_proc(HWND window, UINT message, WPARAM key, LPARAM flags) {
    if (message == WM_CREATE) {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        manual.status = CreateWindowW(L"Static",
            L"Use ZSharp-Notepad-Typing-Test.txt in modern Notepad.\r\n"
            L"Start test requires an EMPTY document and inserts the test paragraphs.\r\n"
            L"Verify existing text reads and compares only, without typing.\r\n"
            L"After either button, click that document within 15 seconds.",
            WS_CHILD | WS_VISIBLE, 18, 18, 570, 130, window, NULL, NULL, NULL);
        manual.start = CreateWindowW(L"Button", L"Start test", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            18, 165, 130, 32, window, (HMENU)(INT_PTR)FIXTURE_START, NULL, NULL);
        manual.verify = CreateWindowW(L"Button", L"Verify existing text", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            165, 165, 170, 32, window, (HMENU)(INT_PTR)FIXTURE_VERIFY, NULL, NULL);
        manual.close = CreateWindowW(L"Button", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            352, 165, 110, 32, window, (HMENU)(INT_PTR)FIXTURE_CLOSE, NULL, NULL);
        SendMessageW(manual.status, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(manual.start, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(manual.verify, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(manual.close, WM_SETFONT, (WPARAM)font, TRUE);
        return 0;
    }
    if (message == WM_COMMAND && (LOWORD(key) == FIXTURE_START || LOWORD(key) == FIXTURE_VERIFY) && !manual.running) {
        manual.verify_only = LOWORD(key) == FIXTURE_VERIFY;
        InterlockedExchange(&manual.running, 1);
        EnableWindow(manual.start, FALSE); EnableWindow(manual.verify, FALSE); EnableWindow(manual.close, FALSE);
        SetWindowTextW(manual.status, manual.verify_only ?
            L"READ-ONLY verification. Click the named Notepad fixture document within 15 seconds. No input will be sent." :
            L"Waiting for the named empty Notepad fixture.\r\nClick its document now (15 seconds). Do not switch apps during typing.");
        manual.worker = CreateThread(NULL, 0, run_fixture, NULL, 0, NULL);
        if (!manual.worker) {
            InterlockedExchange(&manual.running, 0); EnableWindow(manual.close, TRUE);
            SetWindowTextW(manual.status, L"Could not start the test worker. Nothing was typed.");
        }
        return 0;
    }
    if (message == FIXTURE_DONE) {
        if (manual.worker) { CloseHandle(manual.worker); manual.worker = NULL; }
        InterlockedExchange(&manual.running, 0); EnableWindow(manual.close, TRUE);
        SetWindowTextW(manual.status, manual.message);
        SetWindowTextW(window, manual.passed ? L"Notepad typing regression: PASS" : L"Notepad typing regression: FAIL");
        return 0;
    }
    if ((message == WM_COMMAND && LOWORD(key) == FIXTURE_CLOSE) || message == WM_CLOSE) {
        if (!manual.running) DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, key, flags);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show) {
    WNDCLASSW klass;
    MSG message;
    WCHAR *base;
    (void)previous; (void)command; (void)show;
    memset(&manual, 0, sizeof(manual));
    if (!GetModuleFileNameW(NULL, manual.log_path, MAX_PATH)) return 1;
    base = wcsrchr(manual.log_path, L'\\');
    if (!base || (size_t)(base - manual.log_path) + 1 + wcslen(L"desktop-input-notepad-result.txt") >= MAX_PATH) return 1;
    wcscpy(base + 1, L"desktop-input-notepad-result.txt");
    memset(&klass, 0, sizeof(klass));
    klass.lpfnWndProc = gui_proc; klass.hInstance = instance;
    klass.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    klass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    klass.lpszClassName = L"ZSharpNotepadManualTypingFixture";
    if (!RegisterClassW(&klass)) return 1;
    manual.gui = CreateWindowExW(0, klass.lpszClassName, L"Opt-in Notepad typing regression",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 630, 260, NULL, NULL, instance, NULL);
    if (!manual.gui) return 1;
    ShowWindow(manual.gui, SW_SHOWNORMAL);
    UpdateWindow(manual.gui);
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return manual.passed ? 0 : 1;
}
#else
int main(void) { return 0; }
#endif
