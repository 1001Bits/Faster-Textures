#include "DownscalerRules.h"

#include <cstdlib>
#include <iostream>

namespace {
    void Require(bool value, const char* name) {
        if (!value) {
            std::cerr << "FAILED: " << name << '\n';
            std::exit(1);
        }
    }

    // Texture Downscaler 2.7.0's shipped [Textures] and the start of its [Folders], in file order.
    DownscalerRules::Rules Shipped() {
        DownscalerRules::Rules rules;
        for (const auto& [key, value] : std::initializer_list<std::pair<const char*, long>>{
                 {"_shell", 0}, {"\\interface\\", 0}, {"\\sky\\", 0}, {"\\terrain\\", 0}, {"\\lod\\", 0},
                 {"Normal:\\landscape\\mountains\\", 2048}, {"\\landscape\\mountains\\", 0}, {"Normal:grass", 2048}, {"grass", 0},
                 {"Normal:rocks", 2048}, {"rocks", 0}, {"Normal:\\pbr\\", 1024}, {"\\pbr\\", 2048}})
            Require(DownscalerRules::AddFolder(rules, key, value), key);
        return rules;
    }
}

int main() {
    using namespace DownscalerRules;

    Require(Fold("Textures/Armor\\Iron") == "textures\\armor\\iron", "fold: lower case, forward slashes as backslashes");

    Require(KindOf("textures\\armor\\iron\\cuirass_n.dds") == Kind::kNormal, "kind: _n");
    Require(KindOf("textures\\actors\\character\\male\\malebody_1_msn.dds") == Kind::kNormal, "kind: _msn is normal, not mask");
    Require(KindOf("textures\\x\\door_em.dds") == Kind::kMask, "kind: _em");
    Require(KindOf("textures\\x\\skin_sk.dds") == Kind::kMask, "kind: _sk");
    Require(KindOf("textures\\x\\rock_p.dds") == Kind::kParallax, "kind: _p");
    Require(KindOf("textures\\x\\lamp_g.dds") == Kind::kGlow, "kind: _g");
    Require(KindOf("textures\\x\\metal_m.dds") == Kind::kMask, "kind: _m");
    Require(KindOf("textures\\x\\metal_s.dds") == Kind::kMask, "kind: _s");
    Require(KindOf("textures\\x\\back_b.dds") == Kind::kMask, "kind: _b");
    Require(KindOf("textures\\pbr\\x\\stone_rmaos.dds") == Kind::kMaterial, "kind: _rmaos");
    Require(KindOf("textures\\x\\plank01.dds") == Kind::kDiffuse, "kind: no ending is diffuse");
    Require(KindOf("textures\\x\\_n.dds") == Kind::kDiffuse, "kind: an ending alone is not a type");
    Require(KindOf("textures\\x_n\\plank.dds") == Kind::kDiffuse, "kind: only the file name counts");
    Require(KindOf("") == Kind::kDiffuse, "kind: no name is diffuse");

    const auto rules = Shipped();
    Require(LimitFor(rules, "textures\\landscape\\mountains\\mountainslab01_n.dds") == 2048, "limit: typed folder rule");
    Require(LimitFor(rules, "textures\\landscape\\mountains\\mountainslab01.dds") == 0, "limit: the untyped rule after it keeps full size");
    Require(LimitFor(rules, "textures\\interface\\map.dds") == 0, "limit: folder at full size");
    Require(LimitFor(rules, "textures\\architecture\\whiterun\\wrplank01.dds") == 1024, "limit: no folder, the type's");
    Require(LimitFor(rules, "textures\\pbr\\stone_n.dds") == 1024, "limit: first match wins");
    Require(LimitFor(rules, "textures\\pbr\\stone.dds") == 2048, "limit: the next rule for other types");
    Require(LimitFor(rules, "textures\\plants\\grass01.dds") == 0, "limit: a part anywhere in the path");
    Require(LimitFor(rules, "") == 1024, "limit: no name falls back to diffuse");

    Rules bad;
    Require(!AddFolder(bad, "Foo:\\x\\", 512) && bad.folders.empty(), "rules: an unknown type is skipped");
    Require(AddFolder(bad, "NORMAL:\\X\\", 2) && bad.folders.size() == 1 && bad.folders[0].part == "\\x\\" &&
                bad.folders[0].kind == Kind::kNormal && bad.folders[0].maxSize == kSmallest,
            "rules: type names fold, the part folds, a tiny limit is raised to the smallest");
    Require(Clamp(0) == 0 && Clamp(-5) == 0 && Clamp(1) == kSmallest && Clamp(2048) == 2048, "clamp");

    Require(Skip(1024, 4096, 4096, 13, true) == 2, "skip: 4096 to 1024");
    Require(Skip(1024, 2048, 2048, 12, true) == 1, "skip: 2048 to 1024");
    Require(Skip(1024, 1024, 1024, 11, true) == 0, "skip: already fits");
    Require(Skip(0, 4096, 4096, 13, true) == 0, "skip: 0 keeps full size");
    Require(Skip(1024, 4096, 1024, 13, true) == 2, "skip: both sides have to fit");
    Require(Skip(1024, 4096, 4096, 2, true) == 1, "skip: at least one level stays");
    Require(Skip(4, 16, 16, 5, false) == 2, "skip: down to the smallest side");
    Require(Skip(1024, 4096, 12, 3, true) == 0, "skip: block-compressed sides stay multiples of 4");
    Require(Skip(1024, 4096, 12, 3, false) == 1, "skip: other formats may end on any size");

    Require(BlockCompressed(71) && BlockCompressed(77) && BlockCompressed(98) && !BlockCompressed(28) && !BlockCompressed(87), "block-compressed formats");

    std::cout << "downscaler rules: all passed\n";
    return 0;
}
