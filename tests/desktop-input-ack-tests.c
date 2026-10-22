#define _CRT_SECURE_NO_WARNINGS
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* This includes the real transport, not a mock SendMessageTimeout. The test
 * creates only invisible, process-owned controls on its own receiver thread.
 * It never calls the full click/type action, activates a window, reads another
 * application's text, sends physical input, or accesses the clipboard. */
#include "../native/src/desktop_api.c"

#define ACK_STOP (WM_APP + 37)
static struct {
    HANDLE ready, processed;
    HWND parent, edit, rich;
    WNDPROC original;
    HMODULE rich_module;
    volatile LONG delay_ms, insert_count, packet_count, bad_parameters;
    DWORD creation_error;
} fixture;
static unsigned assertions;

static void require(int ok, const char *description) {
    if (!ok) {
        fprintf(stderr, "desktop acknowledged text fixture failed: %s\n", description);
        exit(1);
    }
    assertions++;
}

static LRESULT CALLBACK slow_edit(HWND window, UINT message, WPARAM key, LPARAM flags) {
    LRESULT result;
    if (message == WM_KEYDOWN && key == VK_PACKET)
        InterlockedIncrement(&fixture.packet_count);
    if (message != EM_REPLACESEL) return CallWindowProcW(fixture.original, window, message, key, flags);
    InterlockedIncrement(&fixture.insert_count);
    if (key != TRUE || !flags) InterlockedIncrement(&fixture.bad_parameters);
    {
        LONG delay = InterlockedCompareExchange(&fixture.delay_ms, 0, 0);
        if (delay) Sleep((DWORD)delay); /* Deliberately slow recipient, not sender pacing. */
    }
    result = CallWindowProcW(fixture.original, window, message, key, flags);
    SetEvent(fixture.processed);
    return result;
}

static LRESULT CALLBACK parent_proc(HWND window, UINT message, WPARAM key, LPARAM flags) {
    if (message == ACK_STOP) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, key, flags);
}

static DWORD WINAPI receiver(void *unused) {
    WNDCLASSW klass;
    MSG message;
    HINSTANCE instance = GetModuleHandleW(NULL);
    (void)unused;
    memset(&klass, 0, sizeof(klass));
    klass.lpfnWndProc = parent_proc;
    klass.hInstance = instance;
    klass.lpszClassName = L"ZSharpAcknowledgedTextFixture";
    if (!RegisterClassW(&klass)) goto failed;
    fixture.parent = CreateWindowExW(0, klass.lpszClassName, L"Invisible Z# test",
        WS_OVERLAPPED, 0, 0, 400, 300, NULL, NULL, instance, NULL);
    if (!fixture.parent) goto failed;
    fixture.edit = CreateWindowExW(0, L"Edit", L"", WS_CHILD | ES_MULTILINE |
        ES_AUTOVSCROLL | ES_AUTOHSCROLL, 0, 0, 200, 100,
        fixture.parent, NULL, instance, NULL);
    if (!fixture.edit) goto failed;
    fixture.original = (WNDPROC)(LONG_PTR)SetWindowLongPtrW(fixture.edit, GWLP_WNDPROC,
        (LONG_PTR)slow_edit);
    if (!fixture.original) goto failed;
    fixture.rich_module = LoadLibraryW(L"Msftedit.dll");
    if (!fixture.rich_module) goto failed;
    fixture.rich = CreateWindowExW(0, L"RICHEDIT50W", L"", WS_CHILD | ES_MULTILINE |
        ES_AUTOVSCROLL | ES_AUTOHSCROLL, 0, 0, 200, 100,
        fixture.parent, NULL, instance, NULL);
    if (!fixture.rich) goto failed;
    SetEvent(fixture.ready);
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    FreeLibrary(fixture.rich_module);
    return 0;
failed:
    fixture.creation_error = GetLastError();
    SetEvent(fixture.ready);
    if (fixture.parent) DestroyWindow(fixture.parent);
    if (fixture.rich_module) FreeLibrary(fixture.rich_module);
    return 1;
}

static void clear_control(HWND window) {
    require(SendMessageW(window, WM_SETTEXT, 0, (LPARAM)L""), "reset only our own control");
    SendMessageW(window, EM_SETSEL, 0, 0);
}

static void expect_text(HWND window, const WCHAR *expected, const char *description) {
    size_t capacity = wcslen(expected) + 32;
    WCHAR *actual = (WCHAR *)calloc(capacity, sizeof(WCHAR));
    require(actual != NULL, "allocate only expected fixture text");
    SendMessageW(window, WM_GETTEXT, (WPARAM)capacity, (LPARAM)actual);
    require(!wcscmp(actual, expected), description);
    free(actual);
}

static void insert_text(HWND window, const WCHAR *text) {
    char error[512];
    WCHAR *normalized = desktop_windows_text_edit_string(text);
    require(normalized != NULL, "normalize native insertion paragraphs");
    /* Test the insertion helper directly. Production uses this pointer-bearing
     * transport only across processes, where Windows marshals the text. These
     * owned same-process helper tests keep their buffers valid until complete. */
    require(desktop_windows_text_insert(window, normalized, error, sizeof(error)),
            "real native insertion is acknowledged without keyboard injection");
    free(normalized);
}

int main(void) {
    static const WCHAR black_hole[] =
        L"A black hole is an astronomical body so compact that its gravity prevents anything, "
        L"including light, from escaping. Albert Einstein's theory of general relativity, which "
        L"describes gravitation as the curvature of spacetime, predicts that any sufficiently "
        L"compact mass will form a black hole. The boundary of no escape is called the event horizon.";
    static const WCHAR minecraft[] =
        L"Minecraft is a sandbox video game developed and published by the Swedish company Mojang "
        L"Studios. Following its initial public alpha release as an early access game in May 2009, "
        L"it was fully released in November 2011 for personal computers.";
    static const WCHAR unicode[] = L"Caf\x00e9 \x20ac \xd83d\xde80 \x65e5\x672c\x8a9e";
    HANDLE thread;
    WCHAR *long_text;
    char error[512];
    size_t i, length = wcslen(black_hole);
    DWORD wait;

    memset(&fixture, 0, sizeof(fixture));
    fixture.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    fixture.processed = CreateEventW(NULL, TRUE, FALSE, NULL);
    require(fixture.ready && fixture.processed, "create private receiver synchronization");
    thread = CreateThread(NULL, 0, receiver, NULL, 0, NULL);
    require(thread != NULL, "create private invisible receiver thread");
    require(WaitForSingleObject(fixture.ready, 5000) == WAIT_OBJECT_0 &&
            fixture.creation_error == 0 && fixture.edit && fixture.rich,
            "owned native Edit and RichEdit controls were created");
    require(!IsWindowVisible(fixture.parent) && !IsWindowVisible(fixture.edit) &&
            !IsWindowVisible(fixture.rich), "fixture windows remain invisible");
    require(desktop_windows_text_native_edit(fixture.edit) &&
            desktop_windows_text_native_edit(fixture.rich) &&
            !desktop_windows_text_native_edit(fixture.parent),
            "native classifier accepts Unicode Edit and RichEdit but not custom windows");

    InterlockedExchange(&fixture.delay_ms, 4);
    insert_text(fixture.edit, black_hole);
    expect_text(fixture.edit, black_hole, "slow native Edit receives the exact reported black-hole paragraph");
    InterlockedExchange(&fixture.delay_ms, 0);
    clear_control(fixture.edit);
    insert_text(fixture.edit, minecraft);
    expect_text(fixture.edit, minecraft, "Minecraft paragraph is not corrupted or repeated");

    long_text = (WCHAR *)calloc(length * 24 + 1, sizeof(WCHAR));
    require(long_text != NULL, "allocate long paragraph fixture");
    for (i = 0; i < 24; i++) memcpy(long_text + length * i, black_hole, length * sizeof(WCHAR));
    clear_control(fixture.edit);
    insert_text(fixture.edit, long_text);
    expect_text(fixture.edit, long_text, "many paragraphs preserve every character");
    free(long_text);

    clear_control(fixture.edit);
    insert_text(fixture.edit, unicode);
    expect_text(fixture.edit, unicode, "native Edit preserves UTF-16 including supplementary characters");
    clear_control(fixture.rich);
    insert_text(fixture.rich, black_hole);
    expect_text(fixture.rich, black_hole, "real RichEdit receives the full paragraph");
    clear_control(fixture.rich);
    insert_text(fixture.rich, unicode);
    expect_text(fixture.rich, unicode, "real RichEdit preserves supplementary and international characters");

    SendMessageW(fixture.edit, WM_SETTEXT, 0, (LPARAM)L"left right");
    SendMessageW(fixture.edit, EM_SETSEL, 5, 5);
    insert_text(fixture.edit, L"middle ");
    expect_text(fixture.edit, L"left middle right", "characters insert at the caret without replacing existing text");
    SendMessageW(fixture.edit, EM_SETSEL, 5, 12);
    insert_text(fixture.edit, L"new ");
    expect_text(fixture.edit, L"left new right", "characters replace only the selected range");
    clear_control(fixture.edit);
    insert_text(fixture.edit, L"one\rtwo\tthree");
    expect_text(fixture.edit, L"one\r\ntwo\tthree", "normalized carriage return and tab have native edit semantics");
    require(SendMessageW(fixture.edit, EM_CANUNDO, 0, 0) != 0,
            "character insertion retains native undo support");
    clear_control(fixture.rich);
    insert_text(fixture.rich, L"one\rtwo\nthree\r\nfour");
    expect_text(fixture.rich, L"one\r\ntwo\r\nthree\r\nfour",
                "RichEdit retains all normalized paragraph endings");
    {
        LONG_PTR style = GetWindowLongPtrW(fixture.edit, GWL_STYLE);
        SetWindowLongPtrW(fixture.edit, GWL_STYLE, style | ES_READONLY);
        require(!desktop_windows_text_insert(fixture.edit, L"not inserted", error, sizeof(error)) &&
                strstr(error, "read-only"), "read-only native control is rejected before insertion");
        expect_text(fixture.edit, L"one\r\ntwo\tthree", "read-only rejection preserves existing content");
        SetWindowLongPtrW(fixture.edit, GWL_STYLE, style);
    }

    clear_control(fixture.edit);
    ResetEvent(fixture.processed);
    InterlockedExchange(&fixture.delay_ms, 1500);
    require(!desktop_windows_text_insert(fixture.edit, L"Z", error, sizeof(error)) &&
            strstr(error, "no further input") && strstr(error, "may have been entered"),
            "slow insertion acknowledgement times out clearly instead of retrying");
    require(WaitForSingleObject(fixture.processed, 3000) == WAIT_OBJECT_0,
            "timed-out owned receiver finishes within the test deadline");
    InterlockedExchange(&fixture.delay_ms, 0);
    expect_text(fixture.edit, L"Z", "timed-out delivery is not duplicated by retry or fallback");
    require(fixture.packet_count == 0 && fixture.bad_parameters == 0,
            "native transport uses no VK_PACKET state and keeps insertion undo enabled");
    require(GetForegroundWindow() != fixture.parent && GetForegroundWindow() != fixture.edit &&
            GetForegroundWindow() != fixture.rich,
            "invisible fixture is never the foreground input target");

    require(PostMessageW(fixture.parent, ACK_STOP, 0, 0), "request closure only of the hidden fixture");
    wait = WaitForSingleObject(thread, 5000);
    require(wait == WAIT_OBJECT_0, "owned receiver exits cleanly");
    CloseHandle(thread);
    CloseHandle(fixture.ready);
    CloseHandle(fixture.processed);
    printf("desktop acknowledged text fixtures: %u assertions passed; invisible owned controls only\n", assertions);
    return 0;
}
#else
int main(void) { return 0; }
#endif
