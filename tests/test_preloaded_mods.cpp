#include "mod_runtime.h"
#include "recomp_launcher.h"
#include "crc32.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool g_active = false;

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

void reset_view() {
    g_active = false;
    (void)gba_mod_set_adaptive_view_enabled(0);
}

void activate_view() {
    g_active = true;
    (void)gba_mod_set_adaptive_view_enabled(1);
}

bool write_stored_package(const fs::path& path,
                          const std::string& manifest) {
    std::vector<std::uint8_t> zip;
    const auto le16 = [&](std::uint16_t value) {
        zip.push_back(static_cast<std::uint8_t>(value));
        zip.push_back(static_cast<std::uint8_t>(value >> 8));
    };
    const auto le32 = [&](std::uint32_t value) {
        le16(static_cast<std::uint16_t>(value));
        le16(static_cast<std::uint16_t>(value >> 16));
    };
    const std::string name = "manifest.toml";
    const std::uint32_t crc = gba::crc32(manifest.data(), manifest.size());

    le32(0x04034b50u);
    le16(20);
    le16(0);
    le16(0);  // stored
    le16(0);
    le16(0);
    le32(crc);
    le32(static_cast<std::uint32_t>(manifest.size()));
    le32(static_cast<std::uint32_t>(manifest.size()));
    le16(static_cast<std::uint16_t>(name.size()));
    le16(0);
    zip.insert(zip.end(), name.begin(), name.end());
    zip.insert(zip.end(), manifest.begin(), manifest.end());

    const std::uint32_t central_offset =
        static_cast<std::uint32_t>(zip.size());
    le32(0x02014b50u);
    le16(20);
    le16(20);
    le16(0);
    le16(0);
    le16(0);
    le16(0);
    le32(crc);
    le32(static_cast<std::uint32_t>(manifest.size()));
    le32(static_cast<std::uint32_t>(manifest.size()));
    le16(static_cast<std::uint16_t>(name.size()));
    le16(0);
    le16(0);
    le16(0);
    le16(0);
    le32(0);
    le32(0);
    zip.insert(zip.end(), name.begin(), name.end());

    const std::uint32_t central_size =
        static_cast<std::uint32_t>(zip.size()) - central_offset;
    le32(0x06054b50u);
    le16(0);
    le16(0);
    le16(1);
    le16(1);
    le32(central_size);
    le32(central_offset);
    le16(0);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(zip.data()),
               static_cast<std::streamsize>(zip.size()));
    return static_cast<bool>(file);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return fail("expected the preloaded mods root");

    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("minish-preloaded-mods-" + std::to_string(nonce));
    std::error_code ec;
    fs::copy(argv[1], root, fs::copy_options::recursive, ec);
    if (ec) return fail("could not stage catalog: " + ec.message());

    if (!gba_mod_register_reset_callback(reset_view) ||
        !gba_mod_register_activation_plugin(
            "minish-cap.adaptive-view", activate_view)) {
        return fail("could not register Minish Cap plugin");
    }

    std::string error;
    if (!gbarecomp::mod_runtime_initialize(
            root, "minish-cap-us",
            "b4bd50e4131b027c334547b4524e2dbbd4227130", &error)) {
        return fail("catalog initialization failed: " + error);
    }

    const RecompLauncherCModProvider* provider =
        gbarecomp::mod_runtime_launcher_provider();
    if (!provider || provider->package_count(provider->ctx) != 1 ||
        provider->feature_count(provider->ctx) != 1 ||
        !provider->archive_extension ||
        std::string(provider->archive_extension) != ".gbamod") {
        return fail("catalog did not expose one package and one feature");
    }

    const std::string update_manifest =
        "format_version = 1\n"
        "id = \"minish-cap.enhancement.adaptive-view\"\n"
        "version = \"1.1.0\"\n"
        "name = \"Minish Cap Adaptive Widescreen\"\n"
        "author = \"mstan\"\n"
        "description = \"Stored archive installation test.\"\n"
        "license = \"MIT\"\n"
        "resolver = \"declarative\"\n"
        "save_compatibility = \"shared\"\n\n"
        "[[target]]\n"
        "game_id = \"minish-cap-us\"\n"
        "rom_sha1 = \"b4bd50e4131b027c334547b4524e2dbbd4227130\"\n\n"
        "[[feature]]\n"
        "id = \"adaptive-view\"\n"
        "name = \"Adaptive Widescreen\"\n"
        "group = \"Display\"\n"
        "default_enabled = false\n\n"
        "[[plugin]]\n"
        "feature = \"adaptive-view\"\n"
        "id = \"minish-cap.adaptive-view\"\n";
    const fs::path archive = root / "adaptive-update.gbamod";
    if (!write_stored_package(archive, update_manifest) ||
        !provider->install_archive(provider->ctx, archive.string().c_str()) ||
        provider->version_count(
            provider->ctx, "minish-cap.enhancement.adaptive-view") != 2) {
        return fail(std::string("could not install .gbamod update: ") +
                    provider->last_error(provider->ctx));
    }

    RecompLauncherCModFeature feature{};
    if (!provider->feature_get(provider->ctx, 0, &feature) ||
        std::string(feature.package_id) !=
            "minish-cap.enhancement.adaptive-view" ||
        std::string(feature.id) != "adaptive-view" ||
        feature.enabled) {
        return fail("adaptive view was not exposed disabled by default");
    }

    if (!provider->feature_enable(
            provider->ctx, feature.package_id, feature.id, 1) ||
        !provider->commit(provider->ctx, nullptr)) {
        return fail(std::string("could not enable feature: ") +
                    provider->last_error(provider->ctx));
    }
    gbarecomp::mod_runtime_activate_plugins();
    if (!g_active || !gba_mod_adaptive_view_enabled())
        return fail("enabled adaptive view did not activate");

    if (!provider->feature_enable(
            provider->ctx, feature.package_id, feature.id, 0) ||
        !provider->commit(provider->ctx, nullptr)) {
        return fail(std::string("could not disable feature: ") +
                    provider->last_error(provider->ctx));
    }
    gbarecomp::mod_runtime_activate_plugins();
    if (g_active || gba_mod_adaptive_view_enabled())
        return fail("disabled adaptive view did not restore native view");

    fs::remove_all(root, ec);
    std::cout << "Minish Cap preloaded mod: archive install, UI provider "
                 "toggle, and trusted adaptive-view activation passed\n";
    return 0;
}
