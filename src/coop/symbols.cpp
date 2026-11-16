#include "symbols.h"

#include <cstdio>
#include <unordered_set>

namespace coop {

namespace {

#pragma pack(push, 1)
struct CoffSymbol {
    union {
        char short_name[8];
        struct {
            uint32_t zeroes;
            uint32_t offset;
        } longname;
    } name;
    uint32_t value;
    int16_t section;
    uint16_t type;
    uint8_t storage_class;
    uint8_t aux_count;
};
#pragma pack(pop)
static_assert(sizeof(CoffSymbol) == 18, "COFF symbol");

struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~File() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};

bool read_at(HANDLE h, uint64_t offset, void* dst, size_t n) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    uint8_t* p = (uint8_t*)dst;
    while (n) {
        DWORD chunk = n > (1u << 30) ? (1u << 30) : (DWORD)n, got = 0;
        if (!ReadFile(h, p, chunk, &got, nullptr) || !got) return false;
        p += got, n -= got;
    }
    return true;
}

}  // namespace

bool SymbolTable::load(HMODULE module, const std::vector<std::string>& wanted) {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return false;
    File f;
    f.h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                      FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f.h == INVALID_HANDLE_VALUE) return false;
    // (the headers as loaded: the sections' addresses, the symbol table's place in the file)
    const auto* dos = (const IMAGE_DOS_HEADER*)module;
    const auto* nt = (const IMAGE_NT_HEADERS64*)((const uint8_t*)module + dos->e_lfanew);
    const uint64_t sym_off = nt->FileHeader.PointerToSymbolTable;
    const uint32_t nsyms = nt->FileHeader.NumberOfSymbols;
    if (!sym_off || !nsyms) return false;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    const int nsec = nt->FileHeader.NumberOfSections;
    std::vector<CoffSymbol> syms(nsyms);
    if (!read_at(f.h, sym_off, syms.data(), (size_t)nsyms * sizeof(CoffSymbol))) return false;
    uint32_t strtab_size = 0;
    if (!read_at(f.h, sym_off + (uint64_t)nsyms * sizeof(CoffSymbol), &strtab_size, 4) || strtab_size < 4) return false;
    std::vector<char> strtab(strtab_size + 1, 0);
    if (!read_at(f.h, sym_off + (uint64_t)nsyms * sizeof(CoffSymbol), strtab.data(), strtab_size)) return false;
    std::unordered_set<std::string> want(wanted.begin(), wanted.end());
    const uintptr_t base = (uintptr_t)module;
    for (uint32_t i = 0; i < nsyms; i += 1 + syms[i].aux_count) {
        const CoffSymbol& s = syms[i];
        if (s.section <= 0 || s.section > nsec) continue;
        const char* name;
        char shortbuf[9];
        if (s.name.longname.zeroes == 0) {
            if (s.name.longname.offset >= strtab_size) continue;
            name = strtab.data() + s.name.longname.offset;
        } else {
            memcpy(shortbuf, s.name.short_name, 8);
            shortbuf[8] = 0;
            name = shortbuf;
        }
        auto it = want.find(name);
        if (it == want.end()) continue;
        found_[*it] = base + sec[s.section - 1].VirtualAddress + s.value;
        // (a local and a global of the same name: the first one kept)
        want.erase(it);
        if (want.empty()) break;
    }
    return true;
}

uintptr_t SymbolTable::find(const std::string& name) const {
    auto it = found_.find(name);
    return it == found_.end() ? 0 : it->second;
}

}  // namespace coop
