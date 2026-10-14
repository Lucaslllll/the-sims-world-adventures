// Legacy ARM32 Mach-O executable loader. Internal rebasing is experimental:
// this non-PIE binary has no local relocations; ARM literals and pointer tables
// are reconstructed using its retained symbol table and addressable sections.
#include "loader/macho.h"
#include "loader/symbols.h"
#include "loader/abi32.h"
#include "loader/animation_trace.h"
#include "hle/config.h"
#include "hle/common.h"
#include "objc/runtime.h"

#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <unordered_map>
#include <unordered_set>

MachOImage* g_game_image = nullptr;

namespace {
struct Header { uint32_t magic, cpu, subtype, type, ncmds, sizeofcmds, flags; };
struct Command { uint32_t cmd, size; };
struct Segment {
    uint32_t cmd, size;
    char name[16];
    uint32_t address, vmsize, offset, filesize, maxprot, prot, nsects, flags;
};
struct Section {
    char name[16], segment[16];
    uint32_t address, size, offset, align, reloff, nreloc, flags, reserved1, reserved2;
};
struct Symbol { uint32_t name; uint8_t type, section; uint16_t desc; uint32_t value; };
static_assert(sizeof(Symbol) == 12 && sizeof(Segment) == 56 && sizeof(Section) == 68);
struct SymbolCommand { uint32_t cmd, size, offset, count, strings, stringsize; };
struct DynamicCommand { uint32_t cmd, size, fields[18]; };
struct Relocation { uint32_t address, bits; };

std::string name16(const char* p) { return std::string(p, strnlen(p, 16)); }

struct Loader {
    MachOImage& image;
    std::vector<uint8_t> file;
    const SymbolCommand* sym = nullptr;
    const DynamicCommand* dyn = nullptr;
    std::vector<Section> original_sections;
    std::unordered_set<uint32_t> targets;
    std::map<uint32_t, bool> code_modes;
    std::unordered_map<std::string, void*> resolved;
    uint32_t original_main = 0;
    size_t unresolved = 0;

    bool bounds(uint64_t off, uint64_t size) const {
        return off <= file.size() && size <= file.size() - off;
    }
    bool mapped(uint64_t p, size_t size = 4) const {
        return p >= (uintptr_t)image.map_base && size <= image.map_size &&
               p - (uintptr_t)image.map_base <= image.map_size - size;
    }
    template<class T> const T* at(uint64_t off) const {
        return bounds(off, sizeof(T)) ? reinterpret_cast<const T*>(file.data() + off) : nullptr;
    }
    const Symbol* symbol(uint32_t index) const {
        return index < sym->count ? at<Symbol>(sym->offset + (uint64_t)index * 12) : nullptr;
    }
    const char* symbol_name(const Symbol* s) const {
        if (!s || s->name >= sym->stringsize) return nullptr;
        const char* p = (const char*)file.data() + sym->strings + s->name;
        return memchr(p, 0, sym->stringsize - s->name) ? p : nullptr;
    }
    bool parse() {
        const Header* h = at<Header>(0);
        if (!h || h->magic != 0xfeedface || h->cpu != 12 || h->type != 2 ||
            !bounds(sizeof(Header), h->sizeofcmds) || h->ncmds > h->sizeofcmds / 8) {
            LOGE("invalid ARM32 Mach-O header"); return false;
        }
        size_t pos = sizeof(Header), end = pos + h->sizeofcmds;
        bool decrypted = false;
        for (uint32_t i = 0; i < h->ncmds; ++i) {
            auto* c = at<Command>(pos);
            if (!c || c->size < 8 || c->size > end - pos) return false;
            if (c->cmd == 1) {
                auto* s = at<Segment>(pos);
                if (!s || c->size < 56 || s->nsects > (c->size - 56) / 68 ||
                    s->filesize > s->vmsize || !bounds(s->offset, s->filesize)) return false;
                image.segments.push_back({name16(s->name), s->address, s->vmsize,
                                          s->offset, s->filesize, (int)s->prot});
                for (uint32_t j = 0; j < s->nsects; ++j) {
                    const Section* sec = at<Section>(pos + 56 + j * 68);
                    if (!sec || sec->address < s->address || (uint64_t)sec->address + sec->size >
                        (uint64_t)s->address + s->vmsize) return false;
                    original_sections.push_back(*sec);
                    image.sections.push_back({name16(sec->segment), name16(sec->name),
                                              sec->address, sec->size, sec->flags});
                }
            } else if (c->cmd == 2) {
                if (c->size != sizeof(SymbolCommand)) return false;
                sym = at<SymbolCommand>(pos);
            } else if (c->cmd == 11) {
                if (c->size != sizeof(DynamicCommand)) return false;
                dyn = at<DynamicCommand>(pos);
            } else if (c->cmd == 0x21) {
                if (c->size < 20 || *at<uint32_t>(pos + 16) != 0) {
                    LOGE("FairPlay encrypted image rejected"); return false;
                }
                decrypted = true;
            }
            pos += c->size;
        }
        if (pos != end || !sym || !dyn || !decrypted ||
            !bounds(sym->offset, (uint64_t)sym->count * 12) ||
            !bounds(sym->strings, sym->stringsize) ||
            !bounds(dyn->fields[12], (uint64_t)dyn->fields[13] * 4) ||
            !bounds(dyn->fields[14], (uint64_t)dyn->fields[15] * 8) ||
            dyn->fields[17] != 0) return false;
        for (uint32_t i = 0; i < sym->count; ++i) {
            auto* s = symbol(i);
            const char* n = symbol_name(s);
            if (!n) return false;
            if (!(s->type & 0xe0) && (s->type & 0xe) == 0xe) {
                targets.insert(s->value | ((s->desc & 8) ? 1 : 0));
                if (s->section && s->section <= original_sections.size() &&
                    name16(original_sections[s->section - 1].name) == "__text")
                    code_modes[s->value] = (s->desc & 8) != 0;
                if (!strcmp(n, "_main")) original_main = s->value | ((s->desc & 8) ? 1 : 0);
            }
        }
        if (!original_main) { LOGE("_main symbol missing"); return false; }
        return true;
    }
    bool map() {
        uint64_t low = UINT64_MAX, high = 0;
        for (auto& s : image.segments) {
            if (s.name == "__PAGEZERO" || !s.vmsize) continue;
            low = std::min(low, s.vmaddr);
            high = std::max(high, s.vmaddr + s.vmsize);
        }
        if (low == UINT64_MAX || high > UINT32_MAX || high <= low) return false;
        size_t page = sysconf(_SC_PAGESIZE);
        low &= ~(uint64_t)(page - 1);
        high = (high + page - 1) & ~(uint64_t)(page - 1);
        void* base = mmap(nullptr, high - low, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) { LOGE("mmap: %s", strerror(errno)); return false; }
        image.map_base = (uint8_t*)base;
        image.map_size = high - low;
        image.slide = (uintptr_t)base - (int64_t)low;
        for (auto& s : image.segments) {
            if (s.name == "__PAGEZERO") continue;
            s.vmaddr += image.slide;
            if (s.filesize) memcpy((void*)(uintptr_t)s.vmaddr, file.data() + s.fileoff, s.filesize);
        }
        for (auto& s : image.sections) s.addr += image.slide;
        image.entry = image.slid(original_main);
        LOGI("mapped ARM32 image at %p size %#zx slide %#llx main %#llx",
             base, image.map_size, (long long)image.slide, (long long)image.entry);
        return true;
    }
    bool pointer_target(uint32_t value) const {
        // An odd ID such as M3G node 0x2741 is not an ARM function at 0x2740.
        // Only Thumb functions have bit zero set in actual function pointers.
        if (targets.count(value)) return true;
        for (auto& s : original_sections) {
            std::string n = name16(s.name);
            if ((n == "__cstring" || n == "__objc_const" || n == "__objc_data" ||
                 n == "__const" || n == "__data" || n == "__bss" || n == "__common" ||
                 n == "__cfstring" || n == "__objc_selrefs" || n == "__objc_classrefs" || n == "__objc_superrefs" ||
                 n == "__nl_symbol_ptr" || n == "__la_symbol_ptr") &&
                value >= s.address && (uint64_t)value < (uint64_t)s.address + s.size) return true;
        }
        return false;
    }
    void internal_rebase() {
        std::unordered_set<uint32_t> slots;
        std::unordered_set<uint32_t> branch_slots;
        for (auto& s : original_sections) {
            if (!s.offset || (s.flags & 0xff) == 1) continue;
            std::string n = name16(s.name), seg = name16(s.segment);
            if (n == "__text" || n == "__symbol_stub4") {
                // ARM LDR Rt,[PC,+/-imm12]. Record literal slots, never patch instructions.
                for (uint32_t pos = 0; pos + 4 <= s.size;) {
                    uint32_t address = s.address + pos;
                    auto mode = code_modes.upper_bound(address);
                    bool thumb = mode != code_modes.begin() && std::prev(mode)->second;
                    if (thumb && n == "__text") {
                        uint16_t ins = *at<uint16_t>(s.offset + pos);
                        if ((ins & 0xf800) == 0x4800) {
                            uint16_t next = *at<uint16_t>(s.offset + pos + 2);
                            unsigned rt = (ins >> 8) & 7;
                            // PIC sequence: LDR Rt,literal; ADD Rt,PC. Literal is a delta.
                            bool relative = (next & 0xff00) == 0x4400 &&
                                            ((next >> 3) & 15) == 15 &&
                                            ((next & 7) | ((next >> 4) & 8)) == rt;
                            uint32_t literal = ((address + 4) & ~3u) + (ins & 0xff) * 4;
                            if (!relative && literal >= s.address && literal + 4 <= s.address + s.size)
                                slots.insert(literal);
                        }
                        pos += 2;
                        continue;
                    }
                    uint32_t ins = *at<uint32_t>(s.offset + pos);
                    if ((ins & 0x0ffffff0) == 0x079ff100) {
                        // LDRcc PC,[PC,Rm,LSL #2]: inline absolute switch targets.
                        for (uint32_t offset = pos + 8; offset + 4 <= s.size; offset += 4) {
                            uint32_t target = *at<uint32_t>(s.offset + offset);
                            if (target < s.address || target >= s.address + s.size) break;
                            slots.insert(s.address + offset);
                            branch_slots.insert(s.address + offset);
                        }
                    }
                    if ((ins & 0x0f7f0000) == 0x051f0000) {
                        uint32_t next = pos + 8 <= s.size ? *at<uint32_t>(s.offset + pos + 4) : 0;
                        unsigned rt = (ins >> 12) & 15;
                        bool relative = (next & 0x0ff00000) == 0x00800000 &&
                                        ((next >> 12) & 15) == rt &&
                                        ((((next >> 16) & 15) == 15 && (next & 15) == rt) ||
                                         (((next >> 16) & 15) == rt && (next & 15) == 15));
                        uint32_t pc = address + 8, imm = ins & 0xfff;
                        uint32_t literal = (ins & (1u << 23)) ? pc + imm : pc - imm;
                        if (!relative && !(literal & 3) && literal >= s.address && literal + 4 <= s.address + s.size)
                            slots.insert(literal);
                    }
                    pos += 4;
                }
            } else if (seg == "__DATA") {
                if (n == "__dyld" || n == "__gcc_except_tab") continue;
                for (uint32_t pos = 0; pos + 4 <= s.size; pos += 4) {
                    slots.insert(s.address + pos);
                }
            }
        }
        size_t count = 0;
        for (uint32_t address : slots) {
            auto* p = (uint32_t*)(uintptr_t)image.slid(address);
            bool target = pointer_target(*p);
            // The renderer's GL_MATRIX_PALETTE_OES enum happens to equal the
            // ARM address of ByteArrayInputStream::reset. Its literal pool is
            // numeric; the real reset pointer in the C++ vtable still needs sliding.
            if (address == 0xe06dc && *p == 0x8840) target = false;
            // __TEXT,__const is pure numerical/string data in this executable.
            // ANIMFACTOR_CRASHED=0xc000 and resource sizes can equal code addresses;
            // C++ pointer/vtable data lives in __DATA,__const and is handled above.
            if (branch_slots.count(address) || target) { *p += (uint32_t)image.slide; ++count; }
        }
        LOGW("experimental internal rebase: %zu pointer/literal slots reconstructed", count);
    }
    void* resolve(const char* name, bool function) {
        auto it = resolved.find(name);
        if (it != resolved.end()) return it->second;
        void* p = hle::find_symbol(name);
        if (!p) p = objc::resolve_class_symbol(name);
        if (!p) {
            ++unresolved;
            LOGW("unresolved %s import: %s", function ? "function" : "data", name);
            p = function ? make_unimplemented_stub(name) : calloc(1, 512);
        }
        if (function) p = arm32_wrap_function(p);
        resolved.emplace(name, p);
        return p;
    }
    bool bind() {
        size_t count = 0;
        for (auto& s : original_sections) {
            uint32_t type = s.flags & 0xff;
            if (type != 6 && type != 7) continue;
            for (uint32_t i = 0; i < s.size / 4; ++i) {
                uint64_t index = (uint64_t)s.reserved1 + i;
                if (index >= dyn->fields[13]) return false;
                uint32_t idx = *at<uint32_t>(dyn->fields[12] + index * 4);
                if (idx & 0xc0000000) continue;
                auto* symb = symbol(idx);
                const char* n = symbol_name(symb);
                if (!n) return false;
                auto* p = (uint32_t*)(uintptr_t)image.slid(s.address + i * 4);
                bool defined = !(symb->type & 0xe0) && (symb->type & 0xe) == 0xe;
                *p = defined ? image.slid(symb->value | ((symb->desc & 8) ? 1 : 0)) : (uintptr_t)resolve(n, type == 7);
                ++count;
            }
        }
        for (uint32_t i = 0; i < dyn->fields[15]; ++i) {
            auto* r = at<Relocation>(dyn->fields[14] + i * 8);
            if (!r || (r->address & 0x80000000) || (r->bits >> 28) != 0 ||
                ((r->bits >> 25) & 3) != 2 || (r->bits & (1u << 24)) ||
                !(r->bits & (1u << 27))) return false;
            auto* s = symbol(r->bits & 0xffffff);
            const char* n = symbol_name(s);
            uint64_t address = image.slid(r->address);
            if (!n || !mapped(address)) return false;
            // MH_EXECUTE relocations use absolute VM addresses and stored addends.
            const uint32_t* stored = at<uint32_t>(file_offset(r->address));
            if (!stored) return false;
            uint32_t addend = *stored;
            bool defined = !(s->type & 0xe0) && (s->type & 0xe) == 0xe;
            uintptr_t target = defined ? image.slid(s->value | ((s->desc & 8) ? 1 : 0)) : (uintptr_t)resolve(n, false);
            *(uint32_t*)(uintptr_t)address = target + addend;
            ++count;
        }
        LOGI("bound %zu ARM32 import slots; %zu unresolved symbols", count, unresolved);
        return true;
    }
    uint64_t file_offset(uint32_t address) const {
        for (auto& s : image.segments) {
            uint64_t original = s.vmaddr - image.slide;
            if (address >= original && address - original + 4 <= s.filesize)
                return s.fileoff + address - original;
        }
        return file.size();
    }
    bool protect() {
        size_t page = sysconf(_SC_PAGESIZE);
        for (auto& s : image.segments) {
            if (s.name == "__PAGEZERO" || !s.vmsize) continue;
            uintptr_t start = s.vmaddr & ~(uintptr_t)(page - 1);
            uintptr_t end = (s.vmaddr + s.vmsize + page - 1) & ~(uintptr_t)(page - 1);
            if (mprotect((void*)start, end - start, s.prot) != 0) {
                LOGE("mprotect %s: %s", s.name.c_str(), strerror(errno)); return false;
            }
            if (s.prot & PROT_EXEC) __builtin___clear_cache((char*)start, (char*)end);
        }
        return true;
    }
};
}

const MachOSection* MachOImage::section(const char* seg, const char* sect) const {
    for (auto& s : sections) if (s.segname == seg && s.sectname == sect) return &s;
    return nullptr;
}

bool macho_load(const std::string& path, MachOImage& image) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { LOGE("cannot open %s", path.c_str()); return false; }
    Loader loader{image, {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()}};
    if (!loader.parse() || !loader.map()) { LOGE("ARM32 parse/map failed"); return false; }
    g_game_image = &image;
    loader.internal_rebase();
    if (!loader.bind()) { LOGE("ARM32 bind failed"); return false; }
    if (hle::config().trace_animation) install_animation_trace(image);
    if (!loader.protect()) { LOGE("ARM32 protect failed"); return false; }
    return true;
}
