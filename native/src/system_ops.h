#ifndef ZSHARP_SYSTEM_OPS_H
#define ZSHARP_SYSTEM_OPS_H

#include "bytecode.h"
#include <string.h>

/* Shared language metadata; this header deliberately has no OS dependencies. */
typedef struct ZSharpSystemOperation {
    const char *name;
    const char *family;
    unsigned minimum_arguments;
    unsigned maximum_arguments;
    int property;
    ZSharpValueType result; /* Zero denotes a statement. */
} ZSharpSystemOperation;

static const ZSharpSystemOperation *zsharp_system_operation(const char *name) {
    static const ZSharpSystemOperation operations[] = {
        {"Speech.createVoice", "Speech", 2, 2, 0, 0},
        {"Speech.prepare", "Speech", 1, 1, 0, 0},
        {"Speech.speak", "Speech", 1, 2, 0, 0},
        {"Microphone.getInput", "Microphone", 0, 0, 1, ZVALUE_TEXT},
        {"Microphone.getInputDevice", "Microphone", 0, 0, 1, ZVALUE_TEXT},
        {"Microphone.setInputDevice", "Microphone", 0, 1, 0, 0},
        {"Microphone.record", "Microphone", 2, 2, 0, 0},
        {"Microphone.level", "Microphone", 0, 0, 1, ZVALUE_NUMBER},
        {"Speaker.getOutputDevice", "Speaker", 0, 0, 1, ZVALUE_TEXT},
        {"Speaker.setOutputDevice", "Speaker", 0, 1, 0, 0},
        {"Desktop.closeApp", "Desktop", 1, 1, 0, 0},
        {"Desktop.openApp", "Desktop", 1, 1, 0, 0},
        {"Desktop.minimizeApp", "Desktop", 1, 1, 0, 0},
        {"Desktop.maximizeApp", "Desktop", 1, 1, 0, 0},
        {"Desktop.scaleApp", "Desktop", 3, 3, 0, 0},
        {"Desktop.moveApp", "Desktop", 2, 2, 0, 0},
        {"Desktop.focusWindow", "Desktop", 1, 1, 0, 0},
        {"Desktop.typeText", "Desktop", 2, 2, 0, 0},
        {"Desktop.sendKeys", "Desktop", 1, 1, 0, 0}
    };
    size_t i;
    if (name == NULL) return NULL;
    for (i = 0; i < sizeof(operations) / sizeof(operations[0]); i++)
        if (strcmp(operations[i].name, name) == 0) return &operations[i];
    return NULL;
}

#endif
