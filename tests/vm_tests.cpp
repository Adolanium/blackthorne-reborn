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
        for (int i = 0; i < 16; ++i) {
            ds.set_u16(static_cast<std::uint16_t>(0x288A + 2 * i), static_cast<std::uint16_t>(1u << i));
            ds.set_u16(static_cast<std::uint16_t>(0x28AA + 2 * i), static_cast<std::uint16_t>(~(1u << i)));
        }
    }
    void load(std::uint16_t at, std::initializer_list<std::uint8_t> bytes) {
        std::copy(bytes.begin(), bytes.end(), scripts.begin() + at);
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

void test_bits_and_facing() {
    Fixture f;
    f.ds.set_field(bt::field::kFlags, 2, 0x40);  // facing
    f.load(0, {
        0x97, 0x0C, 0x08,        // acc = bit 6 of self.flags      -> 1
        0x42, 0x00, 0x01,        // g[100h] = acc
        0x3B, 0x05, 0x00,        // acc = 5
        0x91, 0x1C,              // facing: self.x += acc        -> 5
        0x8D, 0x1C,              // facing: self.x -= acc        -> 0
        0x8D, 0x1C,              //                               -> -5
        0x3B, 0x01, 0x00,        // acc = 1
        0x9C, 0x1E, 0x2E,        // set bit 15 of field 23, acc becomes 0x8000
        0x42, 0x02, 0x01,        // g[102h] = acc
        0x00});
    bt::Vm vm(f.ds, f.scripts);
    f.start(2, 0);
    vm.run_actor(2);
    check(f.ds.u16(0x100) == 1, "bit test of facing flag");
    check(f.ds.field(bt::field::kX, 2) == 0xFFFB, "facing-signed add/sub");
    check(f.ds.field(0x175A, 2) == 0x8000, "bit write sets bit 15 of field 23");
    check(f.ds.u16(0x102) == 0x8000, "bit write replaces non-zero acc with the mask");
}

void test_call_if_and_probe() {
    Fixture f;
    // Class lists at ds:60CCh: offset 0 = {0x2F}.
    f.ds.set_u8(0x60CC, 0x2F);
    f.ds.set_u8(0x60CD, 0xFF);
    f.ds.set_u16(0x32C, 0x28);
    // Slot 0 (Kyle) occupies x 10..20, y 0..40; slot 2 sits directly above it.
    auto box = [&](int slot, int top, int bottom, int left, int right) {
        f.ds.set_field(bt::field::kTop, slot, static_cast<std::uint16_t>(top));
        f.ds.set_field(bt::field::kBottom, slot, static_cast<std::uint16_t>(bottom));
        f.ds.set_field(bt::field::kLeft, slot, static_cast<std::uint16_t>(left));
        f.ds.set_field(bt::field::kRight, slot, static_cast<std::uint16_t>(right));
    };
    box(0, 0, 40, 10, 20);
    box(2, 41, 60, 12, 18);
    f.ds.set_field(bt::field::kClass, 0, 0x2F);
    f.ds.set_field(bt::field::kScriptSeg, 0, 1);
    f.load(0, {
        0xB4, 0x00, 0x20, 0x00,  // if Kyle touches my top edge goto 0x20
        0x00,
    });
    f.load(0x20, {
        0x3B, 0x09, 0x00,        // acc = 9
        0x83, 0x09, 0x00, 0x30, 0x00,  // if acc == 9 call 0x30
        0x00,
    });
    f.load(0x30, {0x42, 0x00, 0x01, 0x06});  // g[100h] = acc; return
    bt::Vm vm(f.ds, f.scripts);
    f.start(2, 0);
    vm.run_actor(2);
    check(f.ds.u16(0x356) == 0, "probe found slot 0");
    check(f.ds.u16(0x32C) == 0x28, "probe restored the actor scan limit");
    check(f.ds.u16(0x100) == 9, "conditional call ran and returned");
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
    test_bits_and_facing();
    test_call_if_and_probe();
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
