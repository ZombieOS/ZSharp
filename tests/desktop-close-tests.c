#define _CRT_SECURE_NO_WARNINGS
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "desktop_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Every desktop action in this fixture targets an executable copied into a
 * unique, owned directory. Helpers are suspended until assigned to a private
 * kill-on-close job. Only those jobs/held process handles are used for cleanup;
 * no user application, startup registration, or arbitrary PID is touched. */
#define NODE_LIMIT 6
#define FIXTURE_LIMIT 12
#define WAIT_LIMIT 10000
#define FIXTURE_PROBE (WM_APP + 77)

typedef struct FixtureNode {
    int parent;
    int windows;
    int veto;
    int owned_dialog;
    volatile LONG pid;
    volatile LONG closes;
    volatile LONG dialog_closes;
    volatile LONG failure;
    ULONG_PTR first_window;
} FixtureNode;

typedef struct FixtureState {
    int count;
    FixtureNode nodes[NODE_LIMIT];
    WCHAR child_executable[MAX_PATH];
} FixtureState;

typedef struct Fixture {
    WCHAR prefix[128];
    WCHAR executable[MAX_PATH];
    HANDLE mapping;
    FixtureState *state;
    HANDLE ready[NODE_LIMIT];
    HANDLE closed[NODE_LIMIT];
    HANDLE probed[NODE_LIMIT];
    HANDLE stop;
    HANDLE job;
    HANDLE process;
    DWORD pid;
    int count;
} Fixture;

static Fixture *fixtures[FIXTURE_LIMIT];
static size_t fixture_count;
static WCHAR owned_directory[MAX_PATH];
static int owned_directory_created;
static WCHAR copied_executables[FIXTURE_LIMIT][MAX_PATH];
static size_t copied_count;
static unsigned assertions;
static unsigned sequence;

static void cleanup_fixture(Fixture *fixture) {
    int i;
    if (!fixture) return;
    /* Closing this private job terminates only helpers created by this test,
     * including a deliberately vetoing or headless helper. */
    if (fixture->job) { CloseHandle(fixture->job); fixture->job = NULL; }
    if (fixture->process) {
        WaitForSingleObject(fixture->process, WAIT_LIMIT);
        CloseHandle(fixture->process);
        fixture->process = NULL;
    }
    for (i = 0; i < fixture->count; i++) {
        if (fixture->ready[i]) CloseHandle(fixture->ready[i]);
        if (fixture->closed[i]) CloseHandle(fixture->closed[i]);
        if (fixture->probed[i]) CloseHandle(fixture->probed[i]);
        fixture->ready[i] = fixture->closed[i] = NULL;
        fixture->probed[i] = NULL;
    }
    if (fixture->stop) { CloseHandle(fixture->stop); fixture->stop = NULL; }
    if (fixture->state) { UnmapViewOfFile(fixture->state); fixture->state = NULL; }
    if (fixture->mapping) { CloseHandle(fixture->mapping); fixture->mapping = NULL; }
}

static void cleanup_all(void) {
    size_t i;
    for (i = 0; i < fixture_count; i++) {
        cleanup_fixture(fixtures[i]);
        free(fixtures[i]);
        fixtures[i] = NULL;
    }
    fixture_count = 0;
    for (i = 0; i < copied_count; i++) DeleteFileW(copied_executables[i]);
    copied_count = 0;
    if (owned_directory_created) RemoveDirectoryW(owned_directory);
    owned_directory_created = 0;
    owned_directory[0] = 0;
}

static void require(int ok, const char *description, const char *detail) {
    if (!ok) {
        fprintf(stderr, "desktop close fixture failed: %s: %s (Windows %lu)\n",
                description, detail ? detail : "", (unsigned long)GetLastError());
        exit(1);
    }
    assertions++;
}

static void event_name(WCHAR *out, size_t count, const WCHAR *prefix,
                       const WCHAR *kind, int index) {
    swprintf(out, count, L"%ls-%ls-%d", prefix, kind, index);
}

static HANDLE fixture_event(const WCHAR *prefix, const WCHAR *kind, int index,
                            int create) {
    WCHAR name[192];
    event_name(name, sizeof(name) / sizeof(name[0]), prefix, kind, index);
    return create ? CreateEventW(NULL, TRUE, FALSE, name)
                  : OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
}

static int launch_helper(const WCHAR *executable, const WCHAR *prefix, int index,
                         HANDLE job, PROCESS_INFORMATION *process) {
    WCHAR command[MAX_PATH + 256];
    STARTUPINFOW startup;
    memset(&startup, 0, sizeof(startup));
    memset(process, 0, sizeof(*process));
    startup.cb = sizeof(startup);
    /* CREATE_NO_WINDOW suppresses the console. Do not use STARTF_USESHOWWINDOW:
     * SW_HIDE would override the helper's first explicit ShowWindow call and
     * prevent its offscreen fixture window from being visible to EnumWindows. */
    if (swprintf(command, sizeof(command) / sizeof(command[0]),
                 L"\"%ls\" --fixture %ls %d", executable, prefix, index) < 0)
        return 0;
    if (!CreateProcessW(executable, command, NULL, NULL, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW,
                        NULL, NULL, &startup, process)) return 0;
    if (job && !AssignProcessToJobObject(job, process->hProcess)) {
        /* This handle is exclusively the suspended helper we just created. */
        TerminateProcess(process->hProcess, 1);
        WaitForSingleObject(process->hProcess, WAIT_LIMIT);
        CloseHandle(process->hProcess);
        CloseHandle(process->hThread);
        memset(process, 0, sizeof(*process));
        return 0;
    }
    if (ResumeThread(process->hThread) == (DWORD)-1) {
        TerminateProcess(process->hProcess, 1);
        WaitForSingleObject(process->hProcess, WAIT_LIMIT);
        CloseHandle(process->hProcess);
        CloseHandle(process->hThread);
        memset(process, 0, sizeof(*process));
        return 0;
    }
    return 1;
}

typedef struct HelperWindow {
    FixtureNode *node;
    HANDLE closed;
    HANDLE probed;
    int *remaining;
    int dialog;
} HelperWindow;

static LRESULT CALLBACK helper_window(HWND window, UINT message,
                                      WPARAM wparam, LPARAM lparam) {
    HelperWindow *context = (HelperWindow *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        context = (HelperWindow *)((CREATESTRUCTW *)lparam)->lpCreateParams;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)context);
    }
    if (context && message == WM_CLOSE) {
        if (context->dialog) {
            InterlockedIncrement(&context->node->dialog_closes);
            return 0;
        }
        InterlockedIncrement(&context->node->closes);
        SetEvent(context->closed);
        if (!context->node->veto) DestroyWindow(window);
        return 0;
    }
    if (context && message == FIXTURE_PROBE) {
        /* This posted-message barrier acknowledges all earlier posted close
         * messages on this fixture thread, including any owned dialog. */
        SetEvent(context->probed);
        return 0;
    }
    if (context && message == WM_DESTROY && !context->dialog) {
        if (--*context->remaining == 0) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static int run_helper(const char *prefix_text, int index) {
    WCHAR prefix[128], executable[MAX_PATH];
    HANDLE mapping = NULL, ready = NULL, closed = NULL, probed = NULL, stop = NULL;
    HANDLE children[NODE_LIMIT];
    unsigned child_count = 0, i;
    FixtureState *state = NULL;
    FixtureNode *node;
    WNDCLASSW window_class;
    HelperWindow contexts[4];
    HWND windows[3] = {0}, dialog = NULL;
    int remaining = 0, result = 1;
    MSG message;
    memset(children, 0, sizeof(children));
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, prefix_text, -1,
                             prefix, sizeof(prefix) / sizeof(prefix[0]))) return 20;
    mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, prefix);
    if (!mapping) return 21;
    state = (FixtureState *)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*state));
    if (!state || index < 0 || index >= state->count || state->count > NODE_LIMIT) goto done;
    node = &state->nodes[index];
    ready = fixture_event(prefix, L"ready", index, 0);
    closed = fixture_event(prefix, L"closed", index, 0);
    probed = fixture_event(prefix, L"probed", index, 0);
    stop = fixture_event(prefix, L"stop", 0, 0);
    if (!ready || !closed || !probed || !stop) goto done;
    InterlockedExchange(&node->pid, (LONG)GetCurrentProcessId());
    {
        DWORD length = GetModuleFileNameW(NULL, executable, MAX_PATH);
        if (!length || length >= MAX_PATH) goto failed;
    }
    /* Descendants inherit this root's private job automatically. They do not
     * inherit pipe/event/process handles or create unrelated job memberships. */
    for (i = 0; i < (unsigned)state->count; i++) if (state->nodes[i].parent == index) {
        PROCESS_INFORMATION child;
        if (!launch_helper(state->child_executable[0] ? state->child_executable : executable,
                           prefix, (int)i, NULL, &child)) goto failed;
        children[child_count++] = child.hProcess;
        CloseHandle(child.hThread);
    }
    memset(&window_class, 0, sizeof(window_class));
    window_class.lpfnWndProc = helper_window;
    window_class.hInstance = GetModuleHandleW(NULL);
    window_class.lpszClassName = L"ZSharpOwnedDesktopCloseFixture";
    if (!RegisterClassW(&window_class)) goto failed;
    remaining = node->windows;
    if (remaining < 0 || remaining > 3) goto failed;
    for (i = 0; i < (unsigned)node->windows; i++) {
        contexts[i].node = node;
        contexts[i].closed = closed;
        contexts[i].probed = probed;
        contexts[i].remaining = &remaining;
        contexts[i].dialog = 0;
        windows[i] = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            window_class.lpszClassName, L"Z# isolated close fixture", WS_OVERLAPPEDWINDOW,
            -32000, -32000, 32, 32, NULL, NULL, window_class.hInstance, &contexts[i]);
        if (!windows[i]) goto failed;
        if (i == 0) node->first_window = (ULONG_PTR)windows[i];
        ShowWindow(windows[i], SW_SHOWNOACTIVATE);
        if (!IsWindowVisible(windows[i])) goto failed;
    }
    if (node->owned_dialog) {
        if (!node->windows) goto failed;
        contexts[3].node = node;
        contexts[3].closed = closed;
        contexts[3].probed = probed;
        contexts[3].remaining = &remaining;
        contexts[3].dialog = 1;
        dialog = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            window_class.lpszClassName, L"Simulated unsaved-document prompt", WS_OVERLAPPEDWINDOW,
            -32000, -32000, 32, 32, windows[0], NULL, window_class.hInstance, &contexts[3]);
        if (!dialog) goto failed;
        ShowWindow(dialog, SW_SHOWNOACTIVATE);
    }
    SetEvent(ready);
    if (node->windows) {
        while (GetMessageW(&message, NULL, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    } else if (!child_count) WaitForSingleObject(stop, INFINITE);
    if (child_count) WaitForMultipleObjects(child_count, children, TRUE, INFINITE);
    result = 0;
    goto done;
failed:
    InterlockedExchange(&node->failure, (LONG)(GetLastError() ? GetLastError() : 1));
    SetEvent(ready);
done:
    for (i = 0; i < child_count; i++) CloseHandle(children[i]);
    if (ready) CloseHandle(ready);
    if (closed) CloseHandle(closed);
    if (probed) CloseHandle(probed);
    if (stop) CloseHandle(stop);
    if (state) UnmapViewOfFile(state);
    if (mapping) CloseHandle(mapping);
    return result;
}

static const WCHAR *copy_fixture(void) {
    WCHAR source[MAX_PATH], *destination;
    DWORD length;
    require(copied_count < FIXTURE_LIMIT, "fixture executable capacity", "");
    length = GetModuleFileNameW(NULL, source, MAX_PATH);
    require(length > 0 && length < MAX_PATH, "locate untruncated fixture executable", "");
    destination = copied_executables[copied_count];
    require(swprintf(destination, MAX_PATH, L"%ls\\zsharp-close-%lu-%llu-%u.exe",
        owned_directory, (unsigned long)GetCurrentProcessId(),
        (unsigned long long)GetTickCount64(), ++sequence) > 0,
        "unique fixture executable path", "");
    require(CopyFileW(source, destination, TRUE), "copy uniquely named fixture executable", "");
    copied_count++;
    return destination;
}

static Fixture *start_fixture_ex(const WCHAR *executable, const FixtureNode *nodes,
                                 int count, const WCHAR *child_executable) {
    Fixture *fixture;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    PROCESS_INFORMATION process;
    int i;
    require(fixture_count < FIXTURE_LIMIT && count > 0 && count <= NODE_LIMIT,
            "fixture tree capacity", "");
    fixture = (Fixture *)calloc(1, sizeof(*fixture));
    require(fixture != NULL, "allocate owned fixture", "");
    fixtures[fixture_count++] = fixture;
    fixture->count = count;
    wcscpy(fixture->executable, executable);
    swprintf(fixture->prefix, sizeof(fixture->prefix) / sizeof(fixture->prefix[0]),
        L"Local\\ZSharpDesktopClose-%lu-%llu-%u",
        (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64(), ++sequence);
    fixture->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                         0, sizeof(FixtureState), fixture->prefix);
    require(fixture->mapping != NULL && GetLastError() != ERROR_ALREADY_EXISTS,
            "create private fixture state", "");
    fixture->state = (FixtureState *)MapViewOfFile(fixture->mapping, FILE_MAP_ALL_ACCESS,
                                                0, 0, sizeof(FixtureState));
    require(fixture->state != NULL, "map private fixture state", "");
    memset(fixture->state, 0, sizeof(*fixture->state));
    fixture->state->count = count;
    memcpy(fixture->state->nodes, nodes, (size_t)count * sizeof(*nodes));
    if (child_executable) wcscpy(fixture->state->child_executable, child_executable);
    for (i = 0; i < count; i++) {
        fixture->ready[i] = fixture_event(fixture->prefix, L"ready", i, 1);
        fixture->closed[i] = fixture_event(fixture->prefix, L"closed", i, 1);
        fixture->probed[i] = fixture_event(fixture->prefix, L"probed", i, 1);
        require(fixture->ready[i] && fixture->closed[i] && fixture->probed[i],
                "create fixture handshake events", "");
    }
    fixture->stop = fixture_event(fixture->prefix, L"stop", 0, 1);
    require(fixture->stop != NULL, "create fixture stop event", "");
    fixture->job = CreateJobObjectW(NULL, NULL);
    memset(&limits, 0, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(fixture->job && SetInformationJobObject(fixture->job,
        JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "create private helper job", "");
    require(launch_helper(executable, fixture->prefix, 0, fixture->job, &process),
            "launch suspended, job-owned helper root", "");
    fixture->process = process.hProcess;
    fixture->pid = process.dwProcessId;
    CloseHandle(process.hThread);
    require(WaitForMultipleObjects((DWORD)count, fixture->ready, TRUE, WAIT_LIMIT) == WAIT_OBJECT_0,
            "wait for every helper window without timing sleeps", "");
    for (i = 0; i < count; i++)
        require(fixture->state->nodes[i].pid != 0 && !fixture->state->nodes[i].failure,
                "helper initialized successfully", "");
    return fixture;
}

static Fixture *start_fixture(const WCHAR *executable, const FixtureNode *nodes, int count) {
    return start_fixture_ex(executable, nodes, count, NULL);
}

static void selector_for(Fixture *fixture, char *out, size_t capacity, int by_name) {
    if (by_name) {
        const WCHAR *base = wcsrchr(fixture->executable, L'\\');
        require(WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : fixture->executable,
            -1, out, (int)capacity, NULL, NULL) > 0, "encode exact owned process name", "");
    } else snprintf(out, capacity, "%lu", (unsigned long)fixture->pid);
}

static int action(const char *operation, Fixture *fixture, int by_name,
                  char *error, size_t size) {
    char selector[256];
    const char *arguments[] = {selector};
    selector_for(fixture, selector, sizeof(selector), by_name);
    return zsharp_desktop_action(operation, arguments, 1, error, size);
}

static int action_pid(const char *operation, DWORD pid, char *error, size_t size) {
    char selector[32];
    const char *arguments[] = {selector};
    snprintf(selector, sizeof(selector), "%lu", (unsigned long)pid);
    return zsharp_desktop_action(operation, arguments, 1, error, size);
}

static void require_exited(Fixture *fixture, const char *description) {
    DWORD code = 1;
    require(WaitForSingleObject(fixture->process, WAIT_LIMIT) == WAIT_OBJECT_0,
            description, "owned helper family did not finish normally");
    require(GetExitCodeProcess(fixture->process, &code) && code == 0,
            "helper root exited normally", "");
}

int main(int argc, char **argv) {
    WCHAR temporary[MAX_PATH];
    char error[2048] = {0};
    FixtureNode family[] = {{-1, 1, 0, 0}, {0, 1, 0, 0}, {1, 1, 0, 0}};
    FixtureNode headless[] = {{-1, 0, 0, 0}, {0, 1, 0, 0}, {1, 1, 0, 0}};
    FixtureNode single[] = {{-1, 1, 0, 0}};
    FixtureNode multiple[] = {{-1, 2, 0, 0}};
    FixtureNode veto[] = {{-1, 1, 1, 1}};
    FixtureNode no_window[] = {{-1, 0, 0, 0}};
    FixtureNode subset[] = {{-1, 1, 0, 0}, {0, 1, 0, 0}, {1, 1, 0, 0}, {0, 1, 0, 0}};
    Fixture *fixture, *other;
    const WCHAR *shared_name;
    const WCHAR *child_name;
    int i;
    DWORD temporary_length;
    if (argc == 4 && !strcmp(argv[1], "--fixture")) return run_helper(argv[2], atoi(argv[3]));
    if (argc != 1) { fputs("usage: desktop-close-tests\n", stderr); return 2; }
    atexit(cleanup_all);
    /* This environment change affects only this isolated fixture executable,
     * never the parent shell, running ZVM, or any user application. */
    require(_putenv_s("ZSHARP_DESKTOP_DISABLE_ACTIONS", "") == 0,
            "enable actions only in isolated test executable", "");
    temporary_length = GetTempPathW(MAX_PATH, temporary);
    require(temporary_length > 0 && temporary_length < MAX_PATH,
            "locate untruncated fixture temporary parent", "");
    require(swprintf(owned_directory, MAX_PATH, L"%lszsharp-desktop-close-%lu-%llu",
        temporary, (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64()) > 0,
        "prepare unique owned fixture directory", "");
    require(CreateDirectoryW(owned_directory, NULL), "create exact owned fixture directory", "");
    owned_directory_created = 1;

    fixture = start_fixture(copy_fixture(), family, 3);
    require(action("closeApp", fixture, 1, error, sizeof(error)),
            "same-name root, child and grandchild form one application", error);
    require_exited(fixture, "all same-name family windows close");
    for (i = 0; i < 3; i++) require(fixture->state->nodes[i].closes == 1,
            "each family window receives exactly one graceful close", "");
    cleanup_fixture(fixture);

    shared_name = copy_fixture();
    child_name = copy_fixture();
    fixture = start_fixture_ex(shared_name, family, 3, child_name);
    require(action("closeApp", fixture, 1, error, sizeof(error)),
            "name-selected root includes differently named child executables", error);
    require_exited(fixture, "cross-executable descendants close with their root");
    for (i = 0; i < 3; i++) require(fixture->state->nodes[i].closes == 1,
            "cross-executable family receives graceful close exactly once", "");
    cleanup_fixture(fixture);

    fixture = start_fixture(copy_fixture(), subset, 4);
    {
        HANDLE selected = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                     FALSE, (DWORD)fixture->state->nodes[1].pid);
        require(selected != NULL, "hold selected owned child process against PID reuse", "");
        require(action_pid("closeApp", (DWORD)fixture->state->nodes[1].pid, error, sizeof(error)),
                "explicit child PID selects only its own subtree", error);
        require(WaitForSingleObject(selected, WAIT_LIMIT) == WAIT_OBJECT_0,
                "selected child and grandchild finish normally", "");
        CloseHandle(selected);
    }
    require(fixture->state->nodes[1].closes == 1 && fixture->state->nodes[2].closes == 1,
            "selected subtree receives both close requests", "");
    require(fixture->state->nodes[0].closes == 0 && fixture->state->nodes[3].closes == 0 &&
            WaitForSingleObject(fixture->process, 0) == WAIT_TIMEOUT,
            "child PID never closes its parent or sibling", "");
    require(action("closeApp", fixture, 0, error, sizeof(error)),
            "remaining parent and sibling close independently", error);
    require_exited(fixture, "remaining subtree finishes normally");
    cleanup_fixture(fixture);

    fixture = start_fixture(copy_fixture(), headless, 3);
    require(action("closeApp", fixture, 0, error, sizeof(error)),
            "PID selector reaches child windows of a headless root", error);
    require_exited(fixture, "headless launcher finishes after its children close");
    require(fixture->state->nodes[0].closes == 0 &&
            fixture->state->nodes[1].closes == 1 && fixture->state->nodes[2].closes == 1,
            "headless root and descendant close accounting", "");
    cleanup_fixture(fixture);

    fixture = start_fixture(copy_fixture(), multiple, 1);
    require(action("closeApp", fixture, 0, error, sizeof(error)),
            "one process can own multiple independent top-level windows", error);
    require_exited(fixture, "both independent windows close");
    require(fixture->state->nodes[0].closes == 2,
            "both independent windows receive graceful close", "");
    cleanup_fixture(fixture);

    fixture = start_fixture(copy_fixture(), veto, 1);
    require(action("closeApp", fixture, 0, error, sizeof(error)),
            "graceful close request permits an application veto", error);
    require(WaitForSingleObject(fixture->closed[0], WAIT_LIMIT) == WAIT_OBJECT_0,
            "vetoing main window received close request", "");
    require(PostMessageW((HWND)fixture->state->nodes[0].first_window,
                         FIXTURE_PROBE, 0, 0) &&
            WaitForSingleObject(fixture->probed[0], WAIT_LIMIT) == WAIT_OBJECT_0,
            "flush posted close requests before testing owned prompt preservation", "");
    require(WaitForSingleObject(fixture->process, 0) == WAIT_TIMEOUT &&
            fixture->state->nodes[0].closes == 1,
            "unsaved-document veto leaves its process running", "");
    require(fixture->state->nodes[0].dialog_closes == 0,
            "owned save prompt is not separately dismissed", "");
    cleanup_fixture(fixture);

    shared_name = copy_fixture();
    fixture = start_fixture(shared_name, single, 1);
    other = start_fixture(shared_name, single, 1);
    require(!action("closeApp", fixture, 1, error, sizeof(error)) && strstr(error, "ambiguous"),
            "unrelated same-name application families require a PID", error);
    require(WaitForSingleObject(fixture->closed[0], 0) == WAIT_TIMEOUT &&
            WaitForSingleObject(other->closed[0], 0) == WAIT_TIMEOUT,
            "ambiguous selector closes neither family", "");
    require(action("closeApp", fixture, 0, error, sizeof(error)),
            "explicit PID selects one otherwise ambiguous family", error);
    require_exited(fixture, "PID-selected unrelated family closes");
    require(WaitForSingleObject(other->process, 0) == WAIT_TIMEOUT &&
            WaitForSingleObject(other->closed[0], 0) == WAIT_TIMEOUT,
            "PID close never targets unrelated same-name family", "");
    cleanup_fixture(fixture);
    require(action("closeApp", other, 0, error, sizeof(error)),
            "remaining isolated family closes independently", error);
    require_exited(other, "second unrelated family closes");
    cleanup_fixture(other);

    fixture = start_fixture(copy_fixture(), family, 3);
    require(!action("minimizeApp", fixture, 1, error, sizeof(error)) && strstr(error, "ambiguous"),
            "non-close operations retain strict single-process selection", error);
    require(_putenv_s("ZSHARP_DESKTOP_DISABLE_ACTIONS", "1") == 0,
            "enable desktop action guard", "");
    require(!action("closeApp", fixture, 0, error, sizeof(error)) && strstr(error, "disabled"),
            "closeApp preserves desktop action guard", error);
    for (i = 0; i < 3; i++) require(WaitForSingleObject(fixture->closed[i], 0) == WAIT_TIMEOUT,
            "guarded action sends no close messages", "");
    require(_putenv_s("ZSHARP_DESKTOP_DISABLE_ACTIONS", "") == 0,
            "clear fixture-only desktop action guard", "");
    require(action("closeApp", fixture, 0, error, sizeof(error)),
            "unguarded PID family closes normally", error);
    require_exited(fixture, "guarded fixture finishes after allowed close");
    cleanup_fixture(fixture);

    fixture = start_fixture(copy_fixture(), no_window, 1);
    require(!action("closeApp", fixture, 0, error, sizeof(error)) && strstr(error, "window"),
            "headless family without UI reports a graceful-close limitation", error);
    require(WaitForSingleObject(fixture->process, 0) == WAIT_TIMEOUT,
            "no-UI application is not forcibly terminated", "");
    SetEvent(fixture->stop);
    require_exited(fixture, "owned no-UI helper exits by cooperative fixture event");
    cleanup_fixture(fixture);

    printf("desktop close fixtures: %u assertions passed; only isolated helper jobs targeted\n", assertions);
    return 0;
}
#else
#include <stdio.h>
int main(void) { puts("desktop close fixtures: Windows-only fixture skipped"); return 0; }
#endif
