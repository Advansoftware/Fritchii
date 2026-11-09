// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include "common/common_funcs.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/nso.h"

namespace {

constexpr u64 OcarinaTitleId = 0x0400486024DEC000;
constexpr u64 OtherTitleId = 0x0100000000010000;
constexpr std::string_view MaxResolution = "Max Resolution (built-in)";
constexpr std::string_view OcarinaBuildIdHex = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348";
constexpr FileSys::PatchManager::BuildID OcarinaBuildId{
    0xE0, 0xF3, 0xCC, 0xFA, 0xF5, 0x00, 0x65, 0xAE, 0x4E, 0xA0,
    0x1E, 0xB5, 0xAE, 0xB1, 0x3B, 0x1A, 0x45, 0xDA, 0x33, 0x48,
};
constexpr size_t PatchOffset = sizeof(Loader::NSOHeader) + 0x805BD0;
constexpr u32 TbzInstruction = 0x360041E8;
constexpr u32 BranchInstruction = 0x1400020F;
constexpr u32 NopInstruction = 0xD503201F;

constexpr std::array<std::pair<Common::FS::EdenPath, const char*>, 6> TestDirectories{{
    {Common::FS::EdenPath::NANDDir, "nand"},
    {Common::FS::EdenPath::SDMCDir, "sdmc"},
    {Common::FS::EdenPath::LoadDir, "load"},
    {Common::FS::EdenPath::DumpDir, "dump"},
    {Common::FS::EdenPath::KeysDir, "keys"},
    {Common::FS::EdenPath::ConfigDir, "config"},
}};

void WriteIps(const std::filesystem::path& path, u32 word) {
    std::vector<u8> ips{'P', 'A', 'T', 'C', 'H'};
    ips.push_back(static_cast<u8>(PatchOffset >> 16));
    ips.push_back(static_cast<u8>(PatchOffset >> 8));
    ips.push_back(static_cast<u8>(PatchOffset));
    ips.push_back(0);
    ips.push_back(sizeof(word));
    for (size_t i = 0; i < sizeof(word); ++i) {
        ips.push_back(static_cast<u8>(word >> (i * 8)));
    }
    ips.insert(ips.end(), {'E', 'O', 'F'});
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(ips.data()), static_cast<std::streamsize>(ips.size()));
}

class PatchManagerTestContext {
public:
    explicit PatchManagerTestContext(std::optional<u32> user_mod_word = std::nullopt) {
        if (const auto it = Settings::values.disabled_addons.find(OcarinaTitleId);
            it != Settings::values.disabled_addons.end()) {
            previous_disabled = it->second;
        }
        for (size_t i = 0; i < TestDirectories.size(); ++i) {
            const auto& [path, name] = TestDirectories[i];
            previous[i] = Common::FS::GetEdenPath(path);
            std::filesystem::create_directories(root / name);
            Common::FS::SetEdenPath(path, root / name);
        }
        const auto title_dir = root / "load" / fmt::format("{:016X}", OcarinaTitleId);
        std::filesystem::create_directories(title_dir);
        if (user_mod_word) {
            const auto exefs = title_dir / "UserMod" / "exefs";
            std::filesystem::create_directories(exefs);
            WriteIps(exefs / fmt::format("{}.ips", OcarinaBuildIdHex), *user_mod_word);
        }
        system.emplace();
        system->SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
        system->SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
        system->GetFileSystemController().CreateFactories(*system->GetFilesystem());
    }

    ~PatchManagerTestContext() {
        system.reset();
        for (size_t i = 0; i < TestDirectories.size(); ++i) {
            Common::FS::SetEdenPath(TestDirectories[i].first, previous[i]);
        }
        if (previous_disabled) {
            Settings::values.disabled_addons[OcarinaTitleId] = std::move(*previous_disabled);
        } else {
            Settings::values.disabled_addons.erase(OcarinaTitleId);
        }
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    PatchManagerTestContext(const PatchManagerTestContext&) = delete;
    PatchManagerTestContext& operator=(const PatchManagerTestContext&) = delete;

    FileSys::PatchManager Manager(u64 title_id = OcarinaTitleId) {
        return FileSys::PatchManager{title_id, system->GetFileSystemController(),
                                     system->GetContentProvider()};
    }

    void AddUserBytePatch(u32 offset, u8 value) {
        const auto exefs = root / "load" / fmt::format("{:016X}", OcarinaTitleId) / "UserMod" / "exefs";
        std::filesystem::create_directories(exefs);
        const std::array<u8, 16> ips{
            'I', 'P', 'S', '3', '2',
            static_cast<u8>(offset >> 24), static_cast<u8>(offset >> 16),
            static_cast<u8>(offset >> 8), static_cast<u8>(offset),
            0, 1, value, 'E', 'E', 'O', 'F',
        };
        std::ofstream file{exefs / fmt::format("{}.ips", OcarinaBuildIdHex), std::ios::binary};
        file.write(reinterpret_cast<const char*>(ips.data()), static_cast<std::streamsize>(ips.size()));
        file.flush();
        REQUIRE(file.good());
    }

private:
    std::array<std::filesystem::path, TestDirectories.size()> previous;
    std::optional<std::vector<std::string>> previous_disabled;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("builtin-patch-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::optional<Core::System> system;
};

std::vector<u8> MakeMain(const FileSys::PatchManager::BuildID& build_id, u32 word) {
    std::vector<u8> nso(PatchOffset + 0x100);
    Loader::NSOHeader header{};
    header.magic = Common::MakeMagic('N', 'S', 'O', '0');
    header.build_id = build_id;
    std::memcpy(nso.data(), &header, sizeof(header));
    std::memcpy(nso.data() + PatchOffset, &word, sizeof(word));
    return nso;
}

u32 WordAt(const std::vector<u8>& nso) {
    REQUIRE(nso.size() >= PatchOffset + sizeof(u32));
    u32 word;
    std::memcpy(&word, nso.data() + PatchOffset, sizeof(word));
    return word;
}

u32 PatchMain(PatchManagerTestContext& context, const FileSys::PatchManager::BuildID& build_id,
              u32 word, const std::string& name = "main", u64 title_id = OcarinaTitleId) {
    return WordAt(context.Manager(title_id).PatchNSO(MakeMain(build_id, word), name));
}

} // Anonymous namespace

TEST_CASE("Built-in Max Resolution patch is gated on build id and original word",
          "[patch_manager]") {
    PatchManagerTestContext context;

    SECTION("Matching build id and original word applies") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }

    SECTION("Wrong original word skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, NopInstruction) == NopInstruction);
    }

    SECTION("Other build id skips") {
        auto other = OcarinaBuildId;
        other[0] ^= 1;
        REQUIRE(PatchMain(context, other, TbzInstruction) == TbzInstruction);
    }

    SECTION("Other module skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction, "sdk") == TbzInstruction);
    }

    SECTION("Other title skips") {
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction, "main", OtherTitleId) ==
                TbzInstruction);
    }

    SECTION("Disabled in Add-Ons skips") {
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == TbzInstruction);
    }
}

TEST_CASE("Built-in Max Resolution patch defers to a user mod at the same offset",
          "[patch_manager]") {
    SECTION("A different word from the mod is kept") {
        PatchManagerTestContext context{NopInstruction};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == NopInstruction);
    }

    SECTION("The same word from the mod is applied once") {
        PatchManagerTestContext context{BranchInstruction};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }

    SECTION("Disabling the built-in patch keeps the mod") {
        PatchManagerTestContext context{BranchInstruction};
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
        REQUIRE(PatchMain(context, OcarinaBuildId, TbzInstruction) == BranchInstruction);
    }
}

TEST_CASE("Built-in Max Resolution patch is listed and loaded for Ocarina's main",
          "[patch_manager]") {
    PatchManagerTestContext context;
    const auto manager = context.Manager();

    REQUIRE(manager.HasNSOPatch(OcarinaBuildId, "main"));
    REQUIRE_FALSE(manager.HasNSOPatch(OcarinaBuildId, "sdk"));
    REQUIRE_FALSE(context.Manager(OtherTitleId).HasNSOPatch(OcarinaBuildId, "main"));

    const auto builtin_enabled = [&] {
        const auto patches = manager.GetPatches();
        const auto it = std::ranges::find(patches, MaxResolution, &FileSys::Patch::name);
        REQUIRE(it != patches.end());
        REQUIRE(it->type == FileSys::PatchType::Mod);
        return it->enabled;
    };
    REQUIRE(builtin_enabled());

    Settings::values.disabled_addons[OcarinaTitleId] = {std::string{MaxResolution}};
    REQUIRE_FALSE(builtin_enabled());
}

TEST_CASE("Built-in patches preserve user overrides on already-patched input", "[patch_manager]") {
    PatchManagerTestContext context;
    constexpr size_t CallOffset = sizeof(Loader::NSOHeader) + 0x6E6CC4;
    constexpr u32 RoutineLastByte = sizeof(Loader::NSOHeader) + 0x3C46200 + 119;
    constexpr std::array<u8, 4> DynamicCall{0x4F, 0x7D, 0xD5, 0x94};

    auto nso = MakeMain(OcarinaBuildId, NopInstruction);
    nso.resize(RoutineLastByte + 1);
    nso = context.Manager().PatchNSO(nso, "main");
    REQUIRE(nso.size() == RoutineLastByte + 1);
    REQUIRE(std::equal(DynamicCall.begin(), DynamicCall.end(), nso.begin() + CallOffset));
    REQUIRE(nso[RoutineLastByte] == 0x3F);

    constexpr u8 UserByte = 0x5A;
    context.AddUserBytePatch(RoutineLastByte, UserByte);
    auto expected = nso;
    expected[RoutineLastByte] = UserByte;
    const auto patched = context.Manager().PatchNSO(nso, "main");
    REQUIRE(std::ranges::equal(patched, expected));
}

TEST_CASE("High refresh rate built-in replaces Dark's routine only while its setting is on",
          "[patch_manager]") {
    constexpr std::string_view HighRefresh = "High refresh rate (built-in, experimental)";
    constexpr std::string_view DynamicFps = "Dynamic FPS by Dark (built-in)";
    constexpr size_t Code = sizeof(Loader::NSOHeader);
    constexpr size_t Routine = Code + 0x3C46200;
    constexpr size_t CallOffset = Code + 0x6E6CC4;
    constexpr std::array<u8, 4> DynamicCall{0x4F, 0x7D, 0xD5, 0x94};
    constexpr u32 DarkThreshold = 0x3F83D70A; // Dark's routine +0x74: 1.03f
    const auto word_at = [](const std::vector<u8>& nso, size_t offset) {
        u32 word;
        std::memcpy(&word, nso.data() + offset, sizeof(word));
        return word;
    };
    const auto listed = [](const FileSys::PatchManager& manager, std::string_view name) {
        const auto patches = manager.GetPatches();
        return std::ranges::find(patches, name, &FileSys::Patch::name) != patches.end();
    };
    struct RestoreSetting {
        ~RestoreSetting() {
            Settings::values.oot_high_refresh.SetValue(false);
        }
    } restore;

    PatchManagerTestContext context;
    auto nso = MakeMain(OcarinaBuildId, NopInstruction);
    nso.resize(Routine + 0x100);

    SECTION("Off: Dark's routine as before, nothing past it, not listed") {
        Settings::values.oot_high_refresh.SetValue(false);
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(std::equal(DynamicCall.begin(), DynamicCall.end(), patched.begin() + CallOffset));
        REQUIRE(word_at(patched, Routine + 0x74) == DarkThreshold);
        for (size_t offset = 0x78; offset < 0x100; offset += 4) {
            REQUIRE(word_at(patched, Routine + offset) == 0);
        }
        REQUIRE_FALSE(FileSys::OotHighRefreshScaleOffset(std::span{patched}.subspan(Code)));
        REQUIRE_FALSE(listed(context.Manager(), HighRefresh));
        REQUIRE(listed(context.Manager(), DynamicFps));
    }

    SECTION("On: the high refresh routine, with its speed scale word") {
        Settings::values.oot_high_refresh.SetValue(true);
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(std::equal(DynamicCall.begin(), DynamicCall.end(), patched.begin() + CallOffset));
        REQUIRE(word_at(patched, Routine + 0x18) == 0xB40002CC); // cbz x12, +0x70 (Dark's)
        REQUIRE(word_at(patched, Routine + 0x6C) == 0x14000002); // b +0x74
        REQUIRE(word_at(patched, Routine + 0x70) == 0xD65F03C0); // ret
        REQUIRE(word_at(patched, Routine + 0x78) == 0x1E230848); // fmul s8, s2, s3
        REQUIRE(word_at(patched, Routine + 0x84) == 0x40400000); // 3.0f
        REQUIRE(word_at(patched, Routine + 0x88) == 0x3F800000); // 1.0f
        REQUIRE(word_at(patched, Routine + 0x8C) == 0);
        REQUIRE(FileSys::OotHighRefreshScaleOffset(std::span{patched}.subspan(Code)) ==
                std::optional<size_t>{0x3C46288});
        REQUIRE(listed(context.Manager(), HighRefresh));
    }

    SECTION("On, with the words past Dark's routine in use: Dark's routine") {
        Settings::values.oot_high_refresh.SetValue(true);
        nso[Routine + 0x80] = 1;
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(word_at(patched, Routine + 0x74) == DarkThreshold);
        REQUIRE(word_at(patched, Routine + 0x80) == 1);
        REQUIRE_FALSE(FileSys::OotHighRefreshScaleOffset(std::span{patched}.subspan(Code)));
    }

    SECTION("On, but off in Add-Ons: Dark's routine") {
        Settings::values.oot_high_refresh.SetValue(true);
        Settings::values.disabled_addons[OcarinaTitleId] = {std::string{HighRefresh}};
        const auto patched = context.Manager().PatchNSO(nso, "main");
        REQUIRE(word_at(patched, Routine + 0x74) == DarkThreshold);
        REQUIRE(word_at(patched, Routine + 0x78) == 0);
    }
}
