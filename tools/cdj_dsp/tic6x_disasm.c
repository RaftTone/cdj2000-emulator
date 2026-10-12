/* SPDX-License-Identifier: GPL-2.0-or-later
 * Small raw-memory front end for GNU libopcodes' TI C6x disassembler.
 *
 * Usage: tic6x-disasm IMAGE BASE START END
 *        tic6x-disasm IMAGE BASE --stdin (one exact address per input line)
 */
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACKAGE "cdj-tic6x-disasm"
#include "bfd.h"
#include "dis-asm.h"

extern int print_insn_tic6x(bfd_vma, disassemble_info *);

static int plain_fprintf(void *stream, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vfprintf(stream, format, arguments);
    va_end(arguments);
    return result;
}

static int styled_fprintf(void *stream, enum disassembler_style style,
                          const char *format, ...)
{
    (void)style;
    va_list arguments;
    va_start(arguments, format);
    int result = vfprintf(stream, format, arguments);
    va_end(arguments);
    return result;
}

static bool number(const char *text, bfd_vma *value)
{
    char *end;
    errno = 0;
    uintmax_t parsed = strtoumax(text, &end, 0);
    if (errno || !*text || *end) return false;
    *value = parsed;
    return true;
}

int main(int argc, char **argv)
{
    bool batch = argc == 4 && !strcmp(argv[3], "--stdin");
    if (!batch && argc != 5) return 2;
    bfd_vma base, start = 0, end = 0;
    if (!number(argv[2], &base)) return 2;
    if (!batch && (!number(argv[3], &start) || !number(argv[4], &end) ||
                  end < start || start < base)) return 2;

    FILE *image_file = fopen(argv[1], "rb");
    if (!image_file) { perror(argv[1]); return 2; }
    if (fseek(image_file, 0, SEEK_END) || ftell(image_file) < 0) return 2;
    long image_size = ftell(image_file);
    rewind(image_file);
    bfd_byte *image = malloc((size_t)image_size);
    if (!image || fread(image, 1, (size_t)image_size, image_file) !=
                      (size_t)image_size || fclose(image_file)) return 2;
    if (!batch && end - base > (bfd_vma)image_size) return 2;

    disassemble_info info;
    init_disassemble_info(&info, stdout, plain_fprintf, styled_fprintf);
    info.arch = bfd_arch_tic6x;
    info.mach = 0;
    info.endian = BFD_ENDIAN_LITTLE;
    info.endian_code = BFD_ENDIAN_LITTLE;
    info.buffer = image;
    info.buffer_vma = base;
    info.buffer_length = (size_t)image_size;
    info.read_memory_func = buffer_read_memory;
    info.memory_error_func = perror_memory;
    info.print_address_func = generic_print_address;
    info.symbol_at_address_func = generic_symbol_at_address;
    info.symbol_is_valid = generic_symbol_is_valid;
    disassemble_init_for_target(&info);

    char line[128];
    for (bfd_vma pc = start; batch || pc < end;) {
        if (batch) {
            if (!fgets(line, sizeof(line), stdin)) break;
            char *newline = strchr(line, '\n');
            if (!newline) return 2;
            *newline = 0;
            if (!number(line, &pc) || pc < base || (pc & 1) ||
                pc - base >= (bfd_vma)image_size) return 2;
        }
        printf("0x%08" PRIx64 ":\t", (uint64_t)pc);
        int bytes = print_insn_tic6x(pc, &info);
        if (batch) printf("\t%d", bytes);
        putchar('\n');
        if (bytes <= 0) return 1;
        pc += bytes;
    }
    if (batch && ferror(stdin)) return 2;
    free(image);
    return 0;
}
