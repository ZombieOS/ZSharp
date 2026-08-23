#include "zsharp_provider.h"

#include <stdio.h>
#include <string.h>

static char player_id[64] = "playfab-player-123";
static int32_t player_x = 0;
static int32_t player_y = 0;

static int get_variable(void *user_data, const char *file, const char *room,
                        const char *variable, ZSharpProviderValue *value,
                        char *error, size_t error_size) {
    (void)user_data;
    if (strcmp(file, "Player") == 0 && strcmp(room, "Data") == 0 &&
        strcmp(variable, "PlayerId") == 0) {
        value->type = ZSHARP_PROVIDER_TEXT;
        value->text = player_id;
        return 1;
    }
    snprintf(error, error_size, "unknown value %s.%s.%s", file, room,
             variable);
    return 0;
}

static int set_variable(void *user_data, const char *file, const char *room,
                        const char *variable,
                        const ZSharpProviderValue *value, char *error,
                        size_t error_size) {
    (void)user_data;
    if (strcmp(file, "Player") == 0 && strcmp(room, "Data") == 0 &&
        strcmp(variable, "PlayerId") == 0 &&
        value->type == ZSHARP_PROVIDER_TEXT) {
        snprintf(player_id, sizeof(player_id), "%s", value->text);
        return 1;
    }
    snprintf(error, error_size, "unknown or incompatible value %s.%s.%s",
             file, room, variable);
    return 0;
}

static int get_member(void *user_data, const char *file, const char *room,
                      const char *object, const char *member,
                      ZSharpProviderValue *value, char *error,
                      size_t error_size) {
    (void)user_data;
    if (strcmp(file, "Entities") == 0 && strcmp(room, "Players") == 0 &&
        strcmp(object, "User") == 0 &&
        (strcmp(member, "X") == 0 || strcmp(member, "Y") == 0)) {
        value->type = ZSHARP_PROVIDER_NUMBER;
        value->number = strcmp(member, "X") == 0 ? player_x : player_y;
        return 1;
    }
    snprintf(error, error_size, "unknown field %s.%s.%s.%s", file, room,
             object, member);
    return 0;
}

static int set_member(void *user_data, const char *file, const char *room,
                      const char *object, const char *member,
                      const ZSharpProviderValue *value, char *error,
                      size_t error_size) {
    (void)user_data;
    if (strcmp(file, "Entities") == 0 && strcmp(room, "Players") == 0 &&
        strcmp(object, "User") == 0 &&
        value->type == ZSHARP_PROVIDER_NUMBER) {
        if (strcmp(member, "X") == 0) {
            player_x = value->number;
            return 1;
        }
        if (strcmp(member, "Y") == 0) {
            player_y = value->number;
            return 1;
        }
    }
    snprintf(error, error_size, "unknown or incompatible field %s.%s.%s.%s",
             file, room, object, member);
    return 0;
}

static int call_method(void *user_data, const char *file, const char *room,
                       const char *object, const char *method,
                       const ZSharpProviderValue *arguments,
                       size_t argument_count, char *error,
                       size_t error_size) {
    (void)user_data;
    if (strcmp(file, "Entities") == 0 && strcmp(room, "Players") == 0 &&
        strcmp(object, "User") == 0 && strcmp(method, "Move") == 0 &&
        argument_count == 2 && arguments[0].type == ZSHARP_PROVIDER_NUMBER &&
        arguments[1].type == ZSHARP_PROVIDER_NUMBER) {
        player_x += arguments[0].number;
        player_y += arguments[1].number;
        return 1;
    }
    snprintf(error, error_size, "unknown method or arguments %s.%s.%s.%s",
             file, room, object, method);
    return 0;
}

static int call_function(void *user_data, const char *file, const char *room,
                         const char *function, char *error,
                         size_t error_size) {
    (void)user_data;
    if (strcmp(file, "Client") == 0 && strcmp(room, "Player") == 0 &&
        strcmp(function, "Refresh") == 0) {
        puts("external function worked");
        return 1;
    }
    snprintf(error, error_size, "unknown function %s.%s.%s", file, room,
             function);
    return 0;
}

static const ZSharpProviderV1 PROVIDER = {
    .abi_version = ZSHARP_PROVIDER_ABI_VERSION,
    .user_data = NULL,
    .get_variable = get_variable,
    .call_function = call_function,
    .set_variable = set_variable,
    .get_member = get_member,
    .set_member = set_member,
    .call_method = call_method
};

ZSHARP_PROVIDER_EXPORT const ZSharpProviderV1 *zsharp_provider_v1(void) {
    return &PROVIDER;
}
