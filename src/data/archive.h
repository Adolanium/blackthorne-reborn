#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace bt {

using Bytes = std::vector<std::uint8_t>;

// DATA.DAT: u32 count, u32 offsets[count] (bit 30 = load into EMS), each entry
// a u32 unpacked size followed by an LZSS stream. See docs/FORMATS.md.
class Archive {
public:
    explicit Archive(const std::filesystem::path& path);

    int count() const { return static_cast<int>(offsets_.size()); }

    // Decompressed contents of entry `index` (0-based).
    Bytes entry(int index) const;

    // Level descriptors refer to resources by 1-based id.
    Bytes resource(int id) const { return entry(id - 1); }

private:
    Bytes data_;
    std::vector<std::uint32_t> offsets_;
};

Bytes lzss_decode(const std::uint8_t* src, std::size_t src_size, std::size_t out_size);

inline std::uint16_t read_u16(const Bytes& b, std::size_t o) {
    return static_cast<std::uint16_t>(b.at(o) | (b.at(o + 1) << 8));
}

inline std::uint32_t read_u32(const Bytes& b, std::size_t o) {
    return read_u16(b, o) | (static_cast<std::uint32_t>(read_u16(b, o + 2)) << 16);
}

}  // namespace bt
