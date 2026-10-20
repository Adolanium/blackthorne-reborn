#include "data/actors.h"

namespace bt {

std::vector<ActorType> load_actor_types(const Archive& archive) {
    const Bytes d = archive.entry(kActorTableEntry);
    std::vector<ActorType> types;
    for (int t = 0; t < kActorTypeCount; ++t) {
        const std::size_t o = static_cast<std::size_t>(t) * 16;
        const int size_code = read_u16(d, o + 10);  // low byte: width/8, high byte: height/8
        types.push_back({
            read_u16(d, o),
            d[o + 2] & 0x3F,
            d[o + 2] >> 6,
            read_u16(d, o + 3),
            read_u16(d, o + 5),
            d[o + 7],
            d[o + 8],
            d[o + 9],
            (size_code & 0xFF) * 8,
            (size_code >> 8) * 8,
            read_u16(d, o + 12),
            read_u16(d, o + 14),
        });
    }
    return types;
}

}  // namespace bt
