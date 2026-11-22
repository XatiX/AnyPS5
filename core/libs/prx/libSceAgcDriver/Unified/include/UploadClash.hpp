#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_UPLOADCLASH_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_UNIFIED_INCLUDE_UPLOADCLASH_HPP

#include "prx/libSceAgcDriver/Unified/include/PageTable.hpp"
#include <cstdint>
#include <span>
#include <string>

namespace AgcDriver::Unified::UploadClash {

inline constexpr std::size_t HeldWrites = 65536;
inline constexpr std::uint64_t Reports = 32;

[[nodiscard]] bool Active();
void NoteWrite(std::uint64_t address, std::uint64_t size, std::uint64_t program);
void NoteTracked(std::uint64_t address, std::uint64_t size);
void CheckUploads(std::span<const DirtyRange> ranges);
[[nodiscard]] std::string Line();

}

#endif
