#include <jni.h>
#include <stdio.h>

#include "parser.h"
#include "vm.h"

#ifdef ZSHARP_HAS_GAME_RUNTIME
#include <SDL3/SDL.h>
#endif

JNIEXPORT jstring JNICALL
Java_com_zombieos_zsharp_porttest_MainActivity_nativeVmSmokeTest(
    JNIEnv *env, jclass activity_class, jstring project_root) {
    static const char source[] =
        "zsharp = type.script\n"
        "noticed room AndroidTest[] (\n"
        " noticed brain Start[] (\n"
        "  Print(\"Android Z# VM ready\"):\n"
        " )\n"
        ")\n";
    ZSharpProgram program;
    ZSharpDiagnostic diagnostic;
    char error[512] = {0};
    char result[640];
    const char *root = (*env)->GetStringUTFChars(env, project_root, NULL);
    int parsed;
    int ran = 0;
    (void)activity_class;
    if (root == NULL) return NULL;
    parsed = zsharp_parse_source(source, "AndroidTest.zsharp", &program,
                                 &diagnostic);
    if (parsed) ran = zsharp_vm_run(&program, root, error, sizeof(error));
    if (parsed && ran) {
        snprintf(result, sizeof(result), "Native Z# VM smoke test passed");
    } else if (!parsed) {
        snprintf(result, sizeof(result), "Native Z# parser failed at %u:%u: %s",
                 diagnostic.line, diagnostic.column, diagnostic.message);
    } else {
        snprintf(result, sizeof(result), "Native Z# VM failed: %s", error);
    }
    zsharp_program_free(&program);
    (*env)->ReleaseStringUTFChars(env, project_root, root);
    return (*env)->NewStringUTF(env, result);
}

#ifdef ZSHARP_HAS_GAME_RUNTIME
/* SDLActivity calls this on its own native thread. This is only a display
 * smoke test; package launch is deliberately not claimed until the Android
 * lifecycle and input path are connected to zsharp_game_run. */
int SDL_main(int argc, char **argv) {
    SDL_Window *window;
    SDL_Renderer *renderer;
    Uint64 deadline;
    (void)argc;
    (void)argv;
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    window = SDL_CreateWindow("Z# Android Renderer Test", 640, 360, 0);
    if (window == NULL) {
        SDL_Quit();
        return 1;
    }
    renderer = SDL_CreateRenderer(window, NULL);
    if (renderer == NULL) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    deadline = SDL_GetTicks() + 3000;
    while (SDL_GetTicks() < deadline) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) goto done;
        }
        SDL_SetRenderDrawColor(renderer, 35, 50, 100, 255);
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
done:
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
#endif
