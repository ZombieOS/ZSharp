#ifdef NDEBUG
#undef NDEBUG
#endif
#include "parser.h"
#include "system_ops.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char source[] =
    "zsharp = type.script\n"
    "noticed room Main[] (\n"
    " import ZSharp.Speech(): import ZSharp.Microphone():\n"
    " import ZSharp.Speaker(): import ZSharp.Desktop():\n"
    " brain Start[] (\n"
    "  Speech.createVoice(\"reference.mp3\", \"Voice.zvoice\"):\n"
    "  Speech.prepare(\"Voice.zvoice\"):\n"
    "  Speech.speak(\"Hello\", \"Voice.zvoice\"):\n"
    "  Speech.speak(\"Default voice\"):\n"
    "  text Device = Microphone.getInputDevice:\n"
    "  text Output = Speaker.getOutputDevice:\n"
    "  text Words = Microphone.getInput:\n"
    "  number Level = Microphone.level:\n"
    "  Microphone.record(\"Recording.wav\", 5s):\n"
    "  Microphone.record(\"Recording2.wav\", 200ms):\n"
    "  Microphone.setInputDevice(Device): Microphone.setInputDevice():\n"
    "  Speaker.setOutputDevice(Output): Speaker.setOutputDevice():\n"
    "  Desktop.openApp(\"example.exe\"):\n"
    "  Desktop.closeApp(\"example.exe\"):\n"
    "  Desktop.minimizeApp(\"example.exe\"):\n"
    "  Desktop.maximizeApp(\"example.exe\"):\n"
    "  Desktop.scaleApp(\"example.exe\", 640, 480):\n"
    "  Desktop.moveApp(\"example.exe\", 0):\n"
    "  Desktop.focusWindow(12345):\n"
    "  Desktop.typeText(\"Hello\\nUnicode: \xce\xa9\", \"center\"):\n"
    "  Desktop.sendKeys(\"CTRL+SHIFT+S\"):\n"
    "  Desktop.sendKeys(\"MEDIA_PLAY_PAUSE\"):\n"
    "  Desktop.sendKeys(\"MEDIA_NEXT\"):\n"
    "  Desktop.sendKeys(\"MEDIA_PREVIOUS\"):\n"
    " )\n)\n";

static void check(const ZSharpProgram *program) {
    const ZSharpFunction *function = &program->rooms[0].functions[0];
    size_t i, count = 0;
    for (i = 0; i < function->instruction_count; i++) {
        const ZSharpInstruction *instruction = &function->instructions[i];
        if (instruction->op == ZOP_SYSTEM) {
            const ZSharpSystemOperation *operation =
                zsharp_system_operation(instruction->operand);
            assert(operation != NULL);
            assert(instruction->argument_count >= operation->minimum_arguments);
            assert(instruction->argument_count <= operation->maximum_arguments);
            count++;
        }
    }
    assert(count == 26);
    assert(ZOP_SYSTEM == 73);
}

static void reject(const char *imports, const char *body, const char *message) {
    char text[2048];
    ZSharpProgram program;
    ZSharpDiagnostic diagnostic;
    snprintf(text, sizeof(text),
             "zsharp = type.script noticed room Main[] (%s brain Start[] (%s))",
             imports, body);
    assert(!zsharp_parse_source(text, "Invalid", &program, &diagnostic));
    if (strstr(diagnostic.message, message) == NULL) {
        fprintf(stderr, "Expected %s, received %s\n", message, diagnostic.message);
        assert(0);
    }
    zsharp_program_free(&program);
}

int main(int argc, char **argv) {
    ZSharpProgram program, decoded;
    ZSharpDiagnostic diagnostic;
    unsigned char identity[ZSHARP_SHA256_SIZE], hash[ZSHARP_SHA256_SIZE];
    char error[512] = {0};
    FILE *file;
    assert(argc == 2);
    assert(zsharp_parse_source(source, "System", &program, &diagnostic));
    check(&program);
    file = fopen(argv[1], "rb");
    if (file != NULL) { fclose(file); return 1; }
    assert(zsharp_bytecode_write(argv[1], &program, "system_test", identity,
                                 hash, error, sizeof(error)));
    assert(zsharp_bytecode_read(argv[1], &decoded, error, sizeof(error)));
    check(&decoded);
    assert(remove(argv[1]) == 0);
    zsharp_program_free(&decoded);
    zsharp_program_free(&program);
    reject("", "Speech.speak(\"Hi\"):", "import ZSharp.Speech");
    reject("", "Speech.prepare(\"Voice.zvoice\"):", "import ZSharp.Speech");
    reject("import ZSharp.Speech():", "Speech.prepare():", "too few");
    reject("import ZSharp.Speech():", "Speech.prepare(\"a\", \"b\"):", "too many");
    reject("", "text Device = Microphone.getInputDevice:", "import ZSharp.Microphone");
    reject("import ZSharp.Speech():", "Speech.speak():", "too few");
    reject("import ZSharp.Speech():", "Speech.speak(\"a\", \"b\", \"c\"):", "too many");
    reject("import ZSharp.Speech():", "text Value = Speech.speak(\"Hi\"):", "statement");
    reject("import ZSharp.Desktop():", "Desktop.unknown():", "unknown desktop");
    reject("import ZSharp.Desktop():", "Desktop.scaleApp(\"a\", 640):", "too few");
    reject("", "Desktop.focusWindow(12345):", "import ZSharp.Desktop");
    reject("", "Desktop.typeText(\"Hello\", \"center\"):", "import ZSharp.Desktop");
    reject("", "Desktop.sendKeys(\"CTRL+S\"):", "import ZSharp.Desktop");
    reject("import ZSharp.Desktop():", "Desktop.focusWindow():", "too few");
    reject("import ZSharp.Desktop():", "Desktop.typeText(\"Hello\"):", "too few");
    reject("import ZSharp.Desktop():", "Desktop.sendKeys(\"CTRL+S\", \"x\"):", "too many");
    reject("import ZSharp.Desktop():", "text Result = Desktop.sendKeys(\"CTRL+S\"):", "statement");
    reject("import ZSharp.Microphone():", "text Device = Microphone.getInputDevice():", "':'");
    reject("import ZSharp.Microphone():", "Microphone.record(\"file.wav\", 5):", "requires s or ms");
    puts("PASS: desktop API imports, signatures, expressions, diagnostics and bytecode roundtrip");
    return 0;
}
