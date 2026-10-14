#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MachOSection {
    std::string segname;
    std::string sectname;
    uint64_t addr;  // runtime (slid) address
    uint64_t size;
    uint32_t flags;
};

struct MachOSegment {
    std::string name;
    uint64_t vmaddr;  // runtime (slid) address
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    int prot;
};

struct MachOImage {
    uint8_t* map_base = nullptr;  // start of our mapping (== slid __TEXT)
    size_t map_size = 0;
    int64_t slide = 0;
    uint64_t entry = 0;  // runtime address of main()
    std::vector<MachOSegment> segments;  // in load-command order (incl. __PAGEZERO)
    std::vector<MachOSection> sections;

    const MachOSection* section(const char* seg, const char* sect) const;
    // Runtime address for an unslid vmaddr.
    uint64_t slid(uint64_t vmaddr) const { return vmaddr + slide; }
    // Find the containing function start/name info for diagnostics.
    bool contains(uint64_t addr) const {
        return addr >= (uint64_t)map_base && addr < (uint64_t)map_base + map_size;
    }
};

// Loads the decrypted thin ARM32 Mach-O executable, reconstructs internal
// fixups and binds legacy indirect/external imports. Unknown calls are logged.
bool macho_load(const std::string& path, MachOImage& img);

// Global handle to the loaded game image (for crash diagnostics).
extern MachOImage* g_game_image;
