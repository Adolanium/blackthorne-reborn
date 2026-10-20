// VM tests on hand-assembled scripts (no game data needed).

#include <cstdio>
#include <cstdlib>
#include <string>

#include "engine/vm.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// Script buffer laid out as page 0x83 (offset == address).
struct Fixture {
    bt::Bytes scripts = bt::Bytes(0x200, 0);
    bt::DataSegment ds;
    Fixture() {
        // Field table at ds:28CAh: entry k = k * 0x28.
        for (int k = 0; k < 64; ++k) ds.set_u16(static_cast<std::uint16_t>(0x28CA + 2 * k), static_cast<std::uint16_t>(k * 0x28));
    }
    void start(int slot, std::uint16_t ip) {
        ds.set_field(bt::field::kScriptSeg, slot, bt::Vm::page_segment(bt::Vm::kScriptBase, 0x83));
        ds.set_field(bt::field::kScriptIp, slot, ip);
    }
};

void test_load_store_branch() {
    Fixture f;
    // 3B 05 00      acc = 5
    // 41 1C         self.x = acc
    // 3B 03 00      acc = 3
    // 45 1C         self.x += acc         -> 8
    // 3C 1C         acc = self.x
    // 6F 08 00 18 00  if acc == 8 goto 0x18
    // 00            yield (not reached)
    // @0x18: 3B 2A 00 / 42 00 01 / 00   global[0x100] = 42; yield
    const std::uint8_t prog[] = {0x3B, 0x05, 0x00, 0x41, 0x1C, 0x3B, 0x03, 0x00, 0x45, 0x1C, 0x3C, 0x1C,
                                 0x6F, 0x08, 0x00, 0x18, 0x00, 0x00};
    std::copy(std::begin(prog), std::end(prog), f.scripts.begin() + 0x10 - 0x10);
    const std::uint8_t tail[] = {0x3B, 0x2A, 0x00, 0x42, 0x00, 0x01, 0x00};
    std::copy(std::begin(tail), std::end(tail), f.scripts.begin() + 0x18);
    bt::Vm vm(f.ds, f.scripts);
    f.start(4, 0x00);
    vm.run_actor(4);
    check(f.ds.field(bt::field::kX, 4) == 8, "self.x == 8");
    check(f.ds.u16(0x100) == 42, "global written after taken branch");
    check(f.ds.field(bt::field::kScriptIp, 4) == 0x1F, "yield saved ip after the yield opcode");
}

void test_call_return() {
    Fixture f;
    // 04 10 00   call 0x10
    // 00         yield
    // @0x10: 3B 07 00 / 42 00 01 / 06   global = 7; return
    const std::uint8_t a[] = {0x04, 0x10, 0x00, 0x00};
    const std::uint8_t b[] = {0x3B, 0x07, 0x00, 0x42, 0x00, 0x01, 0x06};
    std::copy(std::begin(a), std::end(a), f.scripts.begin());
    std::copy(std::begin(b), std::end(b), f.scripts.begin() + 0x10);
    bt::Vm vm(f.ds, f.scripts);
    f.start(0, 0);
    vm.run_actor(0);
    check(f.ds.u16(0x100) == 7, "called routine ran");
    check(f.ds.field(bt::field::kScriptIp, 0) == 4, "returned and yielded after the call");
}

void test_page_segment() {
    check(bt::Vm::page_segment(0x4000, 0x83) == 0x4000, "page 0x83 is the base");
    check(bt::Vm::page_segment(0x4000, 0x82) == 0x3800, "page 0x82 is base - 0x800");
}

}  // namespace

int main() {
    test_page_segment();
    test_load_store_branch();
    test_call_return();
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
