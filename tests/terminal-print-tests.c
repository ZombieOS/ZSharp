#include "../native/src/terminal.c"

int main(void) {
    FILE *stream = tmpfile();
    char bytes[128] = {0};
    size_t count;
    const char expected[] = "Header\n\rLong text\rX        \rX\rDone\nNext\n";
    if (stream == NULL) return 1;
    print_line(stream, "Header", 0, 0);
    print_line(stream, "Long text", 1, 0);
    print_line(stream, "X", 1, 9);
    print_line(stream, "Done", 0, 1);
    print_line(stream, "Next", 0, 0);
    rewind(stream);
    count = fread(bytes, 1, sizeof(bytes), stream);
    fclose(stream);
    if (count != sizeof(expected) - 1 || memcmp(bytes, expected, count) != 0)
        return 2;
    return 0;
}
