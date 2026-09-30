#include "zsharp_c.h"

ZSHARP_C_EXPORT int zsharp_c_register_v1(ZSharpCSyntaxRegistry *registry,
                                          char *error, size_t error_size) {
    return registry->add_statement(registry->context,
                                   "Print {value}", "bad",
                                   error, error_size);
}
