// Symbols of the game's modules by name: their COFF symbol tables, read from the files on disk.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.h"

namespace coop {

class SymbolTable {
public:
    // Reads the module's symbol table (the file the loaded module came from) and keeps the addresses of the names asked
    // for. false when the file has no symbol table or could not be read.
    bool load(HMODULE module, const std::vector<std::string>& wanted);
    // The address of a symbol, 0 when it was not found (or not asked for)
    uintptr_t find(const std::string& name) const;
    size_t count() const { return found_.size(); }

private:
    std::unordered_map<std::string, uintptr_t> found_;
};

}  // namespace coop
