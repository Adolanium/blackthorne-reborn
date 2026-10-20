#include "data/archive.h"

#include <array>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace bt {

Archive::Archive(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    data_.assign(std::istreambuf_iterator<char>(in), {});
    const std::uint32_t n = read_u32(data_, 0);
    offsets_.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i)
        offsets_.push_back(read_u32(data_, 4 + 4 * i) & 0x3FFFFFFF);
}

Bytes Archive::entry(int index) const {
    const std::uint32_t off = offsets_.at(index);
    const std::uint32_t size = read_u32(data_, off);
    return lzss_decode(data_.data() + off + 4, data_.size() - off - 4, size);
}

// Mirrors sub_10349: zeroed 4 KiB ring, write head at 0, LSB-first flag bits
// (1 = literal), reference word = 12-bit absolute ring offset | (len - 3) << 12.
Bytes lzss_decode(const std::uint8_t* src, std::size_t src_size, std::size_t out_size) {
    std::array<std::uint8_t, 4096> ring{};
    std::size_t r = 0, i = 0;
    unsigned flags = 0;
    Bytes out;
    out.reserve(out_size);
    while (out.size() < out_size && i < src_size) {
        flags >>= 1;
        if (!(flags & 0x100)) flags = src[i++] | 0xFF00;
        if (flags & 1) {
            const std::uint8_t c = src[i++];
            out.push_back(c);
            ring[r] = c;
            r = (r + 1) & 0xFFF;
        } else {
            const unsigned word = src[i] | (src[i + 1] << 8);
            i += 2;
            const unsigned pos = word & 0xFFF;
            const unsigned len = (word >> 12) + 3;
            for (unsigned k = 0; k < len && out.size() < out_size; ++k) {
                const std::uint8_t c = ring[(pos + k) & 0xFFF];
                out.push_back(c);
                ring[r] = c;
                r = (r + 1) & 0xFFF;
            }
        }
    }
    return out;
}

}  // namespace bt
