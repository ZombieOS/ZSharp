#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_u16(unsigned char *bytes, uint16_t value) {
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8);
}

static void put_u32(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8);
    bytes[2] = (unsigned char)(value >> 16);
    bytes[3] = (unsigned char)(value >> 24);
}

static uint32_t crc32_update(uint32_t crc, const unsigned char *data,
                             size_t length) {
    size_t index;
    crc = ~crc;
    for (index = 0; index < length; index++) {
        unsigned bit;
        crc ^= data[index];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int)(crc & 1u));
    }
    return ~crc;
}

int main(int argc, char **argv) {
    FILE *input;
    FILE *output;
    unsigned char *data;
    unsigned char local[30] = {0};
    unsigned char central[46] = {0};
    unsigned char end[22] = {0};
    long length;
    size_t name_length;
    uint32_t crc;
    uint32_t central_offset;
    if (argc != 4) return 2;
    input = fopen(argv[1], "rb");
    if (input == NULL || fseek(input, 0, SEEK_END) != 0 ||
        (length = ftell(input)) < 0 || (uint64_t)length > UINT32_MAX ||
        fseek(input, 0, SEEK_SET) != 0) {
        if (input != NULL) fclose(input);
        return 1;
    }
    name_length = strlen(argv[3]);
    if (name_length == 0 || name_length > UINT16_MAX) {
        fclose(input);
        return 1;
    }
    data = (unsigned char *)malloc((size_t)length);
    if (data == NULL || fread(data, 1, (size_t)length, input) !=
                        (size_t)length || fclose(input) != 0) {
        free(data);
        return 1;
    }
    crc = crc32_update(0, data, (size_t)length);
    output = fopen(argv[2], "wb");
    if (output == NULL) {
        free(data);
        return 1;
    }
    put_u32(local, 0x04034b50u);
    put_u16(local + 4, 20);
    put_u16(local + 6, 0x0800u);
    put_u16(local + 8, 0);
    put_u32(local + 14, crc);
    put_u32(local + 18, (uint32_t)length);
    put_u32(local + 22, (uint32_t)length);
    put_u16(local + 26, (uint16_t)name_length);
    if (fwrite(local, 1, sizeof(local), output) != sizeof(local) ||
        fwrite(argv[3], 1, name_length, output) != name_length ||
        fwrite(data, 1, (size_t)length, output) != (size_t)length) {
        fclose(output);
        free(data);
        return 1;
    }
    central_offset = (uint32_t)(sizeof(local) + name_length + (size_t)length);
    put_u32(central, 0x02014b50u);
    put_u16(central + 4, 20);
    put_u16(central + 6, 20);
    put_u16(central + 8, 0x0800u);
    put_u16(central + 10, 0);
    put_u32(central + 16, crc);
    put_u32(central + 20, (uint32_t)length);
    put_u32(central + 24, (uint32_t)length);
    put_u16(central + 28, (uint16_t)name_length);
    if (fwrite(central, 1, sizeof(central), output) != sizeof(central) ||
        fwrite(argv[3], 1, name_length, output) != name_length) {
        fclose(output);
        free(data);
        return 1;
    }
    put_u32(end, 0x06054b50u);
    put_u16(end + 8, 1);
    put_u16(end + 10, 1);
    put_u32(end + 12, (uint32_t)(sizeof(central) + name_length));
    put_u32(end + 16, central_offset);
    if (fwrite(end, 1, sizeof(end), output) != sizeof(end) ||
        fclose(output) != 0) {
        free(data);
        return 1;
    }
    free(data);
    return 0;
}
