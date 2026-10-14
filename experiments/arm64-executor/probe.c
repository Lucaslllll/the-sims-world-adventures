// Native 64-bit executor smoke test. Game instructions are read from the user's
// local Mach-O; no EA code is compiled into this executable.
#include <unicorn/unicorn.h>
#include <unicorn/arm.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fail(const char* message) {
    fprintf(stderr, "CPU probe failed: %s\n", message);
    return 1;
}

static uint32_t le32(const unsigned char* bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static int check(uc_err error, const char* operation) {
    if (error == UC_ERR_OK) return 0;
    fprintf(stderr, "%s: %s\n", operation, uc_strerror(error));
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 2) return fail("usage: ts3wa_cpu_probe <local sims3wa_iphone executable>");
    if (sizeof(void*) != 8) return fail("this test requires a 64-bit host process");
    FILE* file = fopen(argv[1], "rb");
    if (!file) return fail("cannot open local game executable");
    unsigned char header[28];
    if (fread(header, 1, sizeof header, file) != sizeof header ||
        le32(header) != 0xfeedface || le32(header + 4) != 12 || le32(header + 12) != 2) {
        fclose(file);
        return fail("not a thin ARM32 Mach-O executable");
    }
    // This original pure method returns the M3G VertexArray class ID (20).
    // The fixed VM address is from the analyzed 1.0.0 executable, not an ELF address.
    const uint32_t entry = 0xdf3a0, stop = 0x02000000;
    uint32_t file_offset = 0;
    int found = 0, decrypted = 0;
    uint64_t at = 28, end = at + le32(header + 20);
    uint32_t count = le32(header + 16);
    for (uint32_t i = 0; i < count; ++i) {
        unsigned char command[56];
        if (at + 8 > end || fseek(file, (long)at, SEEK_SET) != 0 || fread(command, 1, 8, file) != 8) break;
        uint32_t cmd = le32(command), size = le32(command + 4);
        if (size < 8 || at + size > end) break;
        if (cmd == 1 && size >= 56 && fread(command + 8, 1, 48, file) == 48) {
            uint32_t address = le32(command + 24), offset = le32(command + 32), bytes = le32(command + 36);
            if (entry >= address && (uint64_t)entry + 8 <= (uint64_t)address + bytes) {
                file_offset = offset + entry - address;
                found = 1;
            }
        } else if (cmd == 0x21 && size >= 20) {
            if (fread(command + 8, 1, 12, file) == 12 && le32(command + 16) == 0) decrypted = 1;
        }
        at += size;
    }
    unsigned char code[8];
    if (!found || !decrypted || fseek(file, file_offset, SEEK_SET) != 0 || fread(code, 1, sizeof code, file) != sizeof code) {
        fclose(file);
        return fail("method unavailable or executable encrypted/truncated");
    }
    fclose(file);

    uc_engine* cpu = NULL;
    if (check(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &cpu), "uc_open")) return 1;
    int error = check(uc_mem_map(cpu, entry & ~4095u, 4096, UC_PROT_ALL), "map code") ||
                check(uc_mem_write(cpu, entry, code, sizeof code), "write original code") ||
                check(uc_mem_protect(cpu, entry & ~4095u, 4096, UC_PROT_READ | UC_PROT_EXEC), "protect code") ||
                check(uc_mem_map(cpu, stop, 4096, UC_PROT_READ | UC_PROT_EXEC), "map stop") ||
                check(uc_reg_write(cpu, UC_ARM_REG_LR, &stop), "set guest LR") ||
                check(uc_emu_start(cpu, entry, stop, 1000000, 100), "execute ARM32");
    uint32_t result = 0, pc = 0;
    error = error || check(uc_reg_read(cpu, UC_ARM_REG_R0, &result), "read result") ||
                    check(uc_reg_read(cpu, UC_ARM_REG_PC, &pc), "read PC");
    uc_close(cpu);
    if (error) return 1;
    if (pc != stop || result != 20) return fail("original method did not return class ID 20");
    printf("CPU probe OK: host pointers=%zu bits, ARM32 guest returned=%u, original method=%#x\n",
           sizeof(void*) * 8, result, entry);
    return 0;
}
