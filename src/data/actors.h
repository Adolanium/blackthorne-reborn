#pragma once

#include <vector>

#include "data/archive.h"

namespace bt {

// One 16-byte record of the actor type table at the start of DATA.DAT entry 64
// (resource 0x41, loaded raw by sub_10A5E; parsed by loc_117B7).
struct ActorType {
    int bank;           // 0xFFFE = level bank, 0xFFFF = none, else resource id
    int parts;          // number of drawable parts (bits 0-5 of byte 2)
    int part_flags;     // bits 6-7 of byte 2
    int script;         // offset of the actor's script within entry 64
    int script_page;    // 0x82/0x83: selects the segment the script runs in
    int box_kind;
    int box_w, box_h;   // bounding box in pixels
    int frame_w, frame_h;  // sprite frame size in pixels (0 if none)
    int extra0, extra1;
};

constexpr int kActorTableEntry = 64;
constexpr int kActorTypeCount = 0x56;

std::vector<ActorType> load_actor_types(const Archive& archive);

}  // namespace bt
