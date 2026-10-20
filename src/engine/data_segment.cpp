#include "engine/data_segment.h"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace bt {

DataSegment::DataSegment(const std::filesystem::path& exe_path) {
    std::ifstream in(exe_path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + exe_path.string());
    const std::vector<std::uint8_t> exe(std::istreambuf_iterator<char>(in), {});
    const std::size_t start = kSeg003Linear - 0x10000 + kExeHeaderSize;
    if (exe.size() < start + kSeg003InitSize || exe[0] != 'M' || exe[1] != 'Z')
        throw std::runtime_error(exe_path.string() + " is not the expected BTHORNE.EXE");
    std::copy_n(exe.begin() + static_cast<std::ptrdiff_t>(start), kSeg003InitSize, mem_.begin());
}

}  // namespace bt
