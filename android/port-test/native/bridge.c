#include <jni.h>
#include <stdio.h>

#include "lexer.h"
#include "zsharp.h"

JNIEXPORT jstring JNICALL
Java_com_zombieos_zsharp_porttest_MainActivity_nativeCoreStatus(
    JNIEnv *env, jclass activity_class) {
    ZSharpLexer lexer;
    ZSharpToken token;
    char status[96];
    (void)activity_class;
    zsharp_lexer_init(&lexer, "zsharp = type.script");
    token = zsharp_lexer_next(&lexer);
    snprintf(status, sizeof(status),
             "Z# %d.%d.%d.%d Android core loaded; lexer %s",
             ZSHARP_VERSION_MAJOR, ZSHARP_VERSION_MINOR,
             ZSHARP_VERSION_PATCH, ZSHARP_VERSION_REVISION,
             zsharp_token_equals(&token, "zsharp") ? "ready" : "failed");
    return (*env)->NewStringUTF(env, status);
}
