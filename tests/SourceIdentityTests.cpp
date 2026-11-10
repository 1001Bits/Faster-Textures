#include "SourceIdentity.h"
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <vector>

namespace {
    void Require(bool value, const char* name) {
        if (!value) { std::cerr << "FAILED: " << name << '\n'; std::exit(1); }
    }
    void Put(std::vector<std::byte>& bytes, std::size_t at, std::uint32_t value) {
        std::memcpy(bytes.data() + at, &value, 4);
    }
    std::vector<std::byte> Dds(std::uint32_t code, std::uint32_t format = 0) {
        const bool dx10 = code == SkyrimSource::FourCC('D','X','1','0');
        std::vector<std::byte> bytes((dx10 ? 148 : 128) + 112);
        Put(bytes, 0, SkyrimSource::FourCC('D','D','S',' '));
        Put(bytes, 4, 124); Put(bytes, 8, 0x1007); Put(bytes, 12, 8); Put(bytes, 16, 8);
        Put(bytes, 28, 4); Put(bytes, 76, 32); Put(bytes, 80, 4); Put(bytes, 84, code); Put(bytes, 108, 0x1000);
        if (dx10) { Put(bytes, 128, format); Put(bytes, 132, 3); Put(bytes, 140, 1); }
        return bytes;
    }
}
int main(int argc, char** argv) {
    using namespace SkyrimSource;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            std::ifstream stream(argv[i], std::ios::binary | std::ios::ate);
            Require(stream.good(), "DDS sample readable");
            const auto size = stream.tellg();
            Require(size > 0, "DDS sample nonempty");
            std::vector<std::byte> bytes(static_cast<std::size_t>(size));
            stream.seekg(0);
            Require(static_cast<bool>(stream.read(reinterpret_cast<char*>(bytes.data()), size)), "DDS sample complete");
            const auto info = Parse(bytes);
            Require(info.has_value(), argv[i]);
            std::cout << argv[i] << " format=" << info->format << " dimensions=" << info->width << 'x' << info->height
                      << " mips=" << info->mips << " payload=" << info->payloadBytes << '\n';
        }
        return 0;
    }
    auto bc1 = Dds(FourCC('D','X','T','1'));
    const auto valid = Parse(bc1);
    Require(valid && valid->format == 71 && valid->payloadBytes == 56 && valid->mips == 4, "legacy BC1 layout");
    Require(Compatible(71, 70, 72), "BC1 typeless resource with sRGB view");
    Require(!Compatible(71, 76, 77), "same dimensions BC1 vs BC3 rejected");
    Require(!Compatible(80, 79, 81), "BC4 signedness rejected");
    Require(!Compatible(28, 87, 87), "RGBA versus BGRA rejected");
    Require(!Compatible(10, 9, 11), "float versus UNORM rejected");
    Require(Compatible(98, 97, 99), "BC7 color-space interpretation preserves bytes");
    auto malformed = bc1; Put(malformed, 4, 0); Require(!Parse(malformed), "invalid DDS header size");
    malformed = bc1; Put(malformed, 76, 0); Require(!Parse(malformed), "invalid pixel-format size");
    malformed = bc1; Put(malformed, 112, 0x200); Require(!Parse(malformed), "cubemap rejected");
    malformed = bc1; Put(malformed, 112, 0x200000); Require(!Parse(malformed), "volume rejected");
    malformed = bc1; Put(malformed, 28, 32); Require(!Parse(malformed), "oversized mip count rejected");
    malformed = bc1; Put(malformed, 16, 65536); Require(!Parse(malformed), "oversized dimensions rejected");
    malformed = bc1; malformed.resize(128 + 55); Require(!Parse(malformed), "truncated full chain rejected");
    malformed = bc1; Put(malformed, 8, 0x100F); Put(malformed, 20, 100); Require(!Parse(malformed), "padded row layout rejected");
    auto dx10 = Dds(FourCC('D','X','1','0'), 98);
    Require(Parse(dx10).has_value(), "DX10 BC7 full chain");
    malformed = dx10; Put(malformed, 140, 2); Require(!Parse(malformed), "DX10 array rejected");
    malformed = dx10; Put(malformed, 132, 4); Require(!Parse(malformed), "DX10 3D rejected");
    malformed = dx10; Put(malformed, 136, 4); Require(!Parse(malformed), "DX10 cube rejected");
    malformed = dx10; Put(malformed, 128, 327); Require(!Parse(malformed), "format truncation rejected");
    malformed = dx10; malformed.resize(140); Require(!Parse(malformed), "truncated DX10 header rejected");
    std::cout << "Source identity: 21 checks passed\n";
}
