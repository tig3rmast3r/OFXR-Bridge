#include "xrfg/native_ingress/Rdr2RuntimeBinding.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace binding = xrfg::native_ingress::rdr2_runtime_binding;
using binding::detail::value;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Image {
    std::uintptr_t base{0x10000000};
    std::vector<unsigned char> bytes;
    template<class T> void put(std::size_t at, T v) {
        require(at <= bytes.size() && sizeof(v) <= bytes.size()-at, "fixture write bounds");
        std::memcpy(bytes.data()+at, &v, sizeof(v));
    }
    void code(std::size_t at, std::initializer_list<unsigned char> v) {
        require(at <= bytes.size() && v.size() <= bytes.size()-at, "fixture code bounds");
        std::copy(v.begin(), v.end(), bytes.begin()+at);
    }
    auto discover(unsigned* refusal = nullptr) const {
        return binding::discover(base, [&](std::uintptr_t address, void* to, std::size_t size) {
            if (address < base || address-base > bytes.size() || size > bytes.size()-(address-base)) return false;
            std::memcpy(to, bytes.data()+address-base, size); return true;
        }, refusal);
    }
};
struct Fixture {
    Image image;
    binding::Layout layout;
    std::uint32_t wrapper{}, gun{}, type{};
};
Fixture fixture(std::uint32_t shift = 0) {
    Fixture f;
    auto& m = f.image;
    m.bytes.resize(0xb000);
    m.code(0, {'M','Z'}); m.put<std::uint32_t>(0x3c, 0x100);
    m.put<std::uint32_t>(0x100, 0x4550); m.put<std::uint16_t>(0x104, 0x8664);
    m.put<std::uint16_t>(0x106, 4); m.put<std::uint16_t>(0x114, 240);
    m.put<std::uint16_t>(0x118, 0x20b); m.put<std::uint32_t>(0x150, 0xb000);
    m.put<std::uint32_t>(0x184, 16);
    m.put<std::uint32_t>(0x1a0, 0xa000); m.put<std::uint32_t>(0x1a4, 24);
    const auto section = [&](unsigned n, unsigned rva, unsigned size, unsigned flags) {
        const auto at = 0x208+n*40;
        m.put(at+8, size); m.put(at+12, rva); m.put(at+16, size); m.put(at+36, flags);
    };
    section(0, 0x1000, 0x3000, 0x60000020);
    section(1, 0x5000, 0x3000, 0x40000040);
    section(2, 0x8000, 0x2000, 0xc0000040);
    section(3, 0xa000, 0x1000, 0x40000040);
    f.layout = {0x9c00+shift, 0x5208+shift, 0x2000+shift, 0x3000+shift,
        0x12620+shift, 0x12610+shift, 0x12000+shift, 0x180, 0x60};
    const auto& l = f.layout;
    f.wrapper = 0x1100+shift; f.gun = f.wrapper+0x40; f.type = 0x8800+shift;
    constexpr char name[] = ".?AVruntime_RDR2@@";
    std::memcpy(m.bytes.data()+f.type+16, name, sizeof(name));
    const auto col = 0x5100+shift;
    m.put<std::uint32_t>(col, 1); m.put(col+12, f.type); m.put(col+20, col);
    m.put<std::uint64_t>(l.vtable-8, m.base+col);
    for (unsigned n = 0; n < 3; ++n) m.put<std::uint64_t>(l.vtable+n*8, m.base+l.matcher);
    for (auto start : {f.wrapper, f.gun}) {
        m.code(start, {0x4c,0x8b,0xc1,0x48,0x8b,0x0d,0,0,0,0,0x48,0x85,0xc9,0x74,7,
            0x33,0xd2,0xe9,0,0,0,0,0xc3});
        m.put<std::int32_t>(start+6, l.owner-(start+10));
        m.put<std::int32_t>(start+18, l.callback-(start+22));
    }
    m.code(f.gun+15, {0xb2,1});
    m.put(0xa000, l.callback); m.put(0xa004, l.callback+0x300);
    m.put(0xa00c, l.matcher); m.put(0xa010, l.matcher+0x300);
    m.code(l.callback, {0x48,0x8b,0xc4,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57});
    m.code(l.callback+0x20, {0x48,0x8b,0xf9});
    m.code(l.callback+0x40, {0xf0,0x48,0x0f,0xc1,0x97}); m.put(l.callback+0x45, l.render);
    m.code(l.callback+0x60, {0x48,0x8d,0x97,0,0,0,0,0xb9,4,0,0,0}); m.put(l.callback+0x63, l.ring);
    m.code(l.matcher, {0x4a,0x8b,0xbc,0x39}); m.put(l.matcher+4, l.ring);
    m.code(l.matcher+0x10, {0x49,0x69,0xd6,0,0,0,0,0x48,0x81,0xc2,0,0,0,0,
        0x49,0x03,0xd7,0x44,0x8d,0x43,0x30,0x48,0x8d,0x4d,0x80,0xe8,0,0,0,0,
        0x85,0xc0,0x75,9,0x49,0x89,0xbf,0,0,0,0});
    m.put(l.matcher+0x13, l.stride); m.put(l.matcher+0x1a, l.ring+l.matrix);
    m.put(l.matcher+0x35, l.selected);
    m.code(l.matcher+0x60, {0x49,0x89,0xbf}); m.put(l.matcher+0x63, l.selected);
    return f;
}
void tests() {
    for (auto shift : {0U, 0x80U, 0x180U}) {
        auto f = fixture(shift);
        require(f.image.discover() == f.layout, "relocated code, RTTI and object offsets must discover");
        f.image.put<std::uint32_t>(0x108, 0xffffffff);
        require(f.image.discover() == f.layout, "unknown timestamp must not affect discovery");
        f.image.bytes[f.layout.callback+0x22] = 0xd9;
        f.image.bytes[f.layout.callback+0x44] = 0x93;
        f.image.bytes[f.layout.callback+0x62] = 0x93;
        require(f.image.discover() == f.layout, "consistent owner register allocation may change");
        // Owner lives beyond initialized .data: inspect code/RTTI without
        // copying the potentially very large zero-initialized state reserve.
        f.image.put<std::uint32_t>(0x208+2*40+16, 0x1000);
        require(f.image.discover() == f.layout, "owner in BSS must remain discoverable");
        f.image.put<std::uint32_t>(0x208+2*40+16, 0x2000);
        const auto original = f.image;
        unsigned refusal{};
        f.image.bytes[f.type+16] = '!';
        require(!f.image.discover(&refusal) && refusal == 2, "wrong runtime RTTI must refuse");
        f.image = original;
        f.image.put<std::int32_t>(f.gun+6, 0);
        require(!f.image.discover(), "unlinked gun wrapper must refuse");
        f.image = original;
        for (unsigned n=0; n<3; ++n) f.image.put<std::uint64_t>(f.layout.vtable+n*8, f.image.base+f.layout.callback);
        require(!f.image.discover(), "matching code outside RDR2 vtable must refuse");
        f.image = original; f.image.put(f.layout.matcher+4, f.layout.ring+8);
        require(!f.image.discover(), "disagreement between camera writer and matcher must refuse");
        f.image = original; f.image.bytes[f.layout.matcher+0x60] = 0;
        require(!f.image.discover(), "incomplete selected-render stores must refuse");
        f.image = original; f.image.bytes[f.layout.callback] = 0;
        require(!f.image.discover(), "unknown callback ABI must refuse");
        f.image = original;
        const auto duplicate = f.wrapper+0x80;
        std::copy_n(f.image.bytes.begin()+f.wrapper, 23, f.image.bytes.begin()+duplicate);
        f.image.put<std::int32_t>(duplicate+6, f.layout.owner-(duplicate+10));
        f.image.put<std::int32_t>(duplicate+18, f.layout.callback-(duplicate+22));
        require(!f.image.discover(&refusal) && refusal == 4, "ambiguous callback binding must refuse");
        f.image = original; f.image.put<std::uint32_t>(0x208+16, 0xfffffff0);
        f.image.put<std::uint32_t>(0x208+8, 0xfffffff0);
        require(!f.image.discover(&refusal) && refusal == 1, "out of image section must refuse");
        f.image = original; f.image.bytes.resize(0x9000);
        require(!f.image.discover(), "unreadable image must refuse");
    }
    std::cout << "Structural discovery: relocated fixtures and refusal cases pass\n";
}
// Offline validation only: map bytes into their preferred image layout. Never
// load or execute a supplied DLL. Proprietary binaries are not test fixtures.
Image map_file(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "open supplied DLL");
    std::vector<unsigned char> raw{std::istreambuf_iterator<char>(stream), {}};
    const auto nt = value<std::uint32_t>(raw, 0x3c);
    require(raw.size() >= 4096 && nt <= raw.size()-264 && value<std::uint32_t>(raw, nt) == 0x4550, "DLL PE header");
    const auto size = value<std::uint32_t>(raw, nt+24+56);
    require(size >= 4096 && size <= 512*1024*1024, "DLL image size");
    Image m; m.base = value<std::uint64_t>(raw, nt+24+24); m.bytes.resize(size);
    const auto headers = value<std::uint32_t>(raw, nt+24+60);
    require(headers <= raw.size() && headers <= size, "DLL headers size");
    std::copy_n(raw.begin(), headers, m.bytes.begin());
    const auto table = nt+24+value<std::uint16_t>(raw, nt+20);
    const auto count = value<std::uint16_t>(raw, nt+6);
    require(count <= 32 && table <= raw.size() && count*40 <= raw.size()-table, "DLL sections");
    for (unsigned n = 0; n < count; ++n) {
        const auto at = table+n*40, rva = value<std::uint32_t>(raw, at+12);
        const auto bytes = value<std::uint32_t>(raw, at+16), offset = value<std::uint32_t>(raw, at+20);
        require(offset <= raw.size() && bytes <= raw.size()-offset && rva <= size && bytes <= size-rva, "DLL section bounds");
        std::copy_n(raw.begin()+offset, bytes, m.bytes.begin()+rva);
    }
    return m;
}
int main(int argc, char** argv) {
    try {
        tests();
        for (int n=1; n<argc; ++n) {
            auto m = map_file(argv[n]);
            unsigned refusal{};
            const auto start = std::chrono::steady_clock::now();
            const auto l = m.discover(&refusal);
            if (!l) { std::cerr << argv[n] << " refusal=" << refusal << '\n'; return 1; }
            const auto elapsed = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            std::cout << argv[n] << " " << elapsed << "ms" << std::hex
                << " owner=" << l->owner << " vtable=" << l->vtable << " callback=" << l->callback
                << " matcher=" << l->matcher << " render=" << l->render << " selected=" << l->selected
                << " ring=" << l->ring << " stride=" << l->stride << " matrix=" << l->matrix << std::dec << '\n';
            const auto nt = value<std::uint32_t>(m.bytes, 0x3c);
            m.put<std::uint32_t>(nt+8, 0xdeadbeef);
            require(m.discover() == l, "supplied DLL unknown timestamp regression");
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
