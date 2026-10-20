#pragma once

#include <array>
#include <cstdint>
#include <filesystem>

namespace bt {

// The game's 64 KiB data segment (seg003). Scripts address globals by raw
// offset (operand mode 2) and actor fields live at fixed offsets, so the
// engine keeps the original layout and initialises it from the user's own
// BTHORNE.EXE.
class DataSegment {
public:
    static constexpr std::size_t kSize = 0x10000;

    // Loads the initialised part of seg003 from BTHORNE.EXE; the rest is zero.
    explicit DataSegment(const std::filesystem::path& exe_path);
    DataSegment() = default;

    std::uint8_t u8(std::uint16_t addr) const { return mem_[addr]; }
    std::uint16_t u16(std::uint16_t addr) const {
        return static_cast<std::uint16_t>(mem_[addr] | (mem_[static_cast<std::uint16_t>(addr + 1)] << 8));
    }
    std::int16_t s16(std::uint16_t addr) const { return static_cast<std::int16_t>(u16(addr)); }

    void set_u8(std::uint16_t addr, std::uint8_t v) { mem_[addr] = v; }
    void set_u16(std::uint16_t addr, std::uint16_t v) {
        mem_[addr] = static_cast<std::uint8_t>(v);
        mem_[static_cast<std::uint16_t>(addr + 1)] = static_cast<std::uint8_t>(v >> 8);
    }

    // Actor field `base` (e.g. 0x15F2 for x) of the actor in `slot` (0, 2, ..., 0x26).
    std::uint16_t field(std::uint16_t base, int slot) const { return u16(static_cast<std::uint16_t>(base + slot)); }
    void set_field(std::uint16_t base, int slot, std::uint16_t v) { set_u16(static_cast<std::uint16_t>(base + slot), v); }

    const std::uint8_t* raw() const { return mem_.data(); }

    // Field addressed by script index k (table at ds:28CAh: k * 0x28 + 0x13C2).
    static std::uint16_t field_base(int k) { return static_cast<std::uint16_t>(0x13C2 + k * 0x28); }

private:
    std::array<std::uint8_t, kSize> mem_{};
};

// Seg003 location inside BTHORNE.EXE.
constexpr std::uint32_t kExeHeaderSize = 0x200;
constexpr std::uint32_t kSeg003Linear = 0x1E540;   // load image is based at linear 0x10000
constexpr std::uint32_t kSeg003InitSize = 0x7110;   // up to the stack segment

}  // namespace bt
