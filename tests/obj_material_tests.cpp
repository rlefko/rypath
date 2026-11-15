// SPDX-License-Identifier: GPL-3.0-only
#include "exr_image.hpp"
#include "obj.hpp"
#include "pbrt_render.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace {
using namespace rypath;
namespace fs = std::filesystem;
void require(bool c, const std::string &message) {
    if (!c)
        throw std::runtime_error(message);
}
struct Temporary {
    fs::path path = fs::temp_directory_path() /
                    ("rypath-obj-material-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temporary() { fs::create_directories(path); }
    ~Temporary() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};
obj::Asset
asset(const fs::path &directory,
      std::initializer_list<std::pair<std::string, std::vector<std::string>>> properties) {
    obj::Asset a;
    a.entry = directory / "mesh.obj";
    obj::Material m{"authored", {(directory / "mesh.mtl").string(), 4, 1}, {}};
    std::size_t line = 5;
    for (const auto &[name, arguments] : properties)
        m.properties.push_back({name, arguments, {(directory / "mesh.mtl").string(), line++, 1}});
    a.materials.push_back(std::move(m));
    return a;
}
std::string diagnostics(const obj::Conversion &c) {
    std::string s;
    for (const auto &d : c.diagnostics)
        s += d.message + "\n";
    return s;
}
obj::Conversion success(const obj::Asset &a) {
    auto c = obj::prepare_materials(a);
    require(c.exit_code() == 0, diagnostics(c));
    return c;
}
void write(const fs::path &path, const std::vector<std::uint8_t> &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(stream), "write image fixture");
}
void exr(const fs::path &path, const std::vector<float> &pixels) {
    std::vector<std::uint8_t> bytes;
    const auto error = encode_exr(
        {static_cast<std::uint32_t>(pixels.size() / 3), 1, pixels},
        [&](std::uint64_t offset, std::span<const std::uint8_t> data) {
            bytes.resize(std::max(bytes.size(), static_cast<std::size_t>(offset) + data.size()));
            std::copy(data.begin(), data.end(),
                      bytes.begin() + static_cast<std::ptrdiff_t>(offset));
            return true;
        });
    require(error.empty(), error);
    write(path, bytes);
}
std::vector<DecodedImage> derived(const obj::Conversion &c) {
    std::vector<DecodedImage> images;
    for (const auto &file : c.files) {
        if (file.path.extension() != ".exr")
            continue;
        auto decoded = decode_exr(file.bytes);
        if (!decoded.value)
            throw std::runtime_error(decoded.diagnostic);
        images.push_back(std::move(decoded.value.value()));
    }
    return images;
}
void has_texel(const obj::Conversion &c, std::array<float, 3> expected, const std::string &reason) {
    for (const auto &image : derived(c)) {
        if (image.floating_rgb.size() == 3 &&
            std::abs(image.floating_rgb[0] - expected[0]) < 2e-6 &&
            std::abs(image.floating_rgb[1] - expected[1]) < 2e-6 &&
            std::abs(image.floating_rgb[2] - expected[2]) < 2e-6)
            return;
    }
    throw std::runtime_error(reason);
}
void translate(const fs::path &directory, const obj::Conversion &c) {
    for (const auto &file : c.files)
        write(directory / file.path, file.bytes);
    const auto entry = directory / "generated.pbrt";
    std::ofstream out(entry);
    out << "Camera \"perspective\"\nFilm \"rgb\"\nPixelFilter \"box\"\nSampler "
           "\"independent\"\nIntegrator \"path\"\nWorldBegin\n";
    out << c.definitions;
    for (const auto &binding : c.materials) {
        out << "AttributeBegin\nNamedMaterial \"" << binding.name << "\"\n"
            << binding.emission << '\n';
        out << "Shape \"trianglemesh\" \"point3 P\" [0 0 2 1 0 2 0 1 2] \"point2 uv\" [0 0 1 0 0 "
               "1] \"integer indices\" [0 1 2] "
            << binding.alpha << "\nAttributeEnd\n";
    }
    out.close();
    auto checked = pbrt::check(entry);
    require(checked.exit_code() == 0, pbrt::report(checked));
    auto translated = pbrt::translate(checked, obj::Location(entry.string()));
    std::string messages;
    for (const auto &d : translated.diagnostics)
        messages += d.message + "\n";
    require(translated.exit_code() == 0, "generated PBRT translation: " + messages);
}
void constants(const fs::path &directory) {
    for (int illum = 0; illum <= 9; ++illum) {
        auto a = asset(
            directory,
            {{"illum", {std::to_string(illum)}}, {"Ns", {"250"}}, {"Ks", {".2", ".3", ".4"}}});
        auto c = success(a);
        if (illum >= 2 && illum != 4 && illum != 9)
            require(c.definitions.find("\"float roughness\" 0.25") != std::string::npos,
                    "Ns=250 gives alpha=.25");
        if (illum >= 2 && illum != 4 && illum != 9)
            require(c.definitions.find("remaproughness\" false") != std::string::npos,
                    "roughness remapping disabled");
        translate(directory, c);
    }
    auto a = asset(directory, {{"Pr", {".25"}},
                               {"Ns", {"0"}},
                               {"Pc", {".4"}},
                               {"Pcr", {".2"}},
                               {"Pm", {".5"}},
                               {"Kd", {".2", ".4", ".6"}}});
    auto c = success(a);
    require(c.definitions.find("coatedconductor") != std::string::npos, "clearcoat metallic path");
    require(diagnostics(c).find("separate legacy") != std::string::npos,
            "coat approximation diagnosed");
    translate(directory, c);
    auto pr_only = success(asset(directory, {{"Pr", {".5"}}}));
    require(pr_only.definitions.find("coateddiffuse") != std::string::npos,
            "Pr PBR dielectric uses a physical coat by default");
    auto relocated = a;
    relocated.entry = directory / "elsewhere/mesh.obj";
    for (auto &m : relocated.materials)
        m.location = obj::Location("different/mesh.mtl");
    require(success(relocated).definitions == c.definitions,
            "material identity independent of source location");
}
void images(const fs::path &directory) {
    // A one-pixel owned RGBA PNG (64,128,192,32); no encoder dependency in tests.
    write(directory / "rgba.png",
          {137, 80,  78, 71, 13, 10,  26,  10, 0,   0,   0,  13,  73,  72,  68,  82, 0, 0,
           0,   1,   0,  0,  0,  1,   8,   6,  0,   0,   0,  31,  21,  196, 137, 0,  0, 0,
           13,  73,  68, 65, 84, 120, 156, 99, 112, 104, 56, 160, 0,   0,   4,   37, 1, 161,
           11,  188, 82, 86, 0,  0,   0,   0,  73,  69,  78, 68,  174, 66,  96,  130});
    auto rgba = success(
        asset(directory, {{"map_Kd", {"rgba.png"}}, {"map_d", {"-imfchan", "m", "rgba.png"}}}));
    const auto srgb = [](float x) { return std::pow((x + .055f) / 1.055f, 2.4f); };
    has_texel(rgba, {srgb(64.f / 255), srgb(128.f / 255), srgb(192.f / 255)},
              "byte color maps use sRGB");
    has_texel(rgba, {32.f / 255, 32.f / 255, 32.f / 255}, "retained alpha is linear data");
    translate(directory, rgba);
    rgba = success(asset(directory, {{"map_Kd", {"rgba.png"}}}));
    require(rgba.materials[0].alpha.empty(), "diffuse image alpha does not imply coverage");
    rgba = success(asset(directory, {{"map_Pr", {"-imfchan", "r", "rgba.png"}}}));
    const auto data = (64.f / 255) * (64.f / 255);
    has_texel(rgba, {data, data, data}, "data map bytes stay linear");
    const auto jpeg = fs::path(__FILE__).parent_path() / "fixtures/image/baseline-rgb.jpg";
    fs::copy_file(jpeg, directory / "normal.jpg", fs::copy_options::overwrite_existing);
    auto jpeg_normal = success(asset(directory, {{"norm", {"normal.jpg"}}}));
    require(jpeg_normal.materials[0].requires_uvs, "JPEG normal requires UVs");
    translate(directory, jpeg_normal);
    exr(directory / "color.exr", {.2f, .4f, .6f});
    exr(directory / "rough.exr", {.25f, .5f, .75f});
    exr(directory / "zero-normal.exr", {.5f, .5f, .5f});
    exr(directory / "bad-metal.exr", {2, 2, 2});
    auto c = success(asset(
        directory, {{"Kd", {".5", ".25", "1"}},
                    {"map_Kd", {"-o", "-.5", "1", "-s", "2", "3", "-clamp", "on", "color.exr"}},
                    {"map_Pr", {"-imfchan", "g", "rough.exr"}}}));
    has_texel(c, {.1f, .1f, .6f}, "RGB base multiplication precedes spectral reconstruction");
    has_texel(c, {.25f, .25f, .25f}, "selected green roughness is squared before filtering");
    require(c.sources.size() == 2 && c.materials[0].requires_uvs,
            "image snapshots and UV requirement");
    require(c.definitions.find("\"float uscale\" 2") != std::string::npos, "UV map scale retained");
    translate(directory, c);
    c = success(asset(directory, {{"map_Ns", {"-imfchan", "r", "-mm", "0", "1000", "rough.exr"}}}));
    has_texel(c, {.25f, .25f, .25f}, "Ns nonlinear conversion before filtering");
    c = success(asset(directory, {{"map_d", {"-imfchan", "b", "rough.exr"}},
                                  {"map_Tr", {"-imfchan", "r", "rough.exr"}}}));
    require(!c.materials[0].alpha.empty(), "complementary d/Tr maps accepted");
    success(asset(directory, {{"d", {".75"}}, {"map_Tr", {"-imfchan", "r", "rough.exr"}}}));
    success(asset(directory, {{"map_d", {"-imfchan", "b", "rough.exr"}}, {"Tr", {".25"}}}));
    translate(directory, c);
    c = success(asset(directory, {{"map_Ke", {"color.exr"}},
                                  {"Ke", {"2", "1", ".5"}},
                                  {"norm", {"color.exr"}},
                                  {"bump", {"rough.exr"}}}));
    has_texel(c, {.4f, .4f, .3f}, "linear emission multiplier");
    require(c.materials[0].emission.find("filename") != std::string::npos, "native image emission");
    require(diagnostics(c).find("norm takes precedence") != std::string::npos,
            "normal precedence warning");
    translate(directory, c);
    c = success(asset(directory, {{"bump", {"-bm", "2", "rough.exr"}}}));
    const auto bump = static_cast<float>(2 * (.2126 * .25 + .7152 * .5 + .0722 * .75));
    has_texel(c, {bump, bump, bump}, "explicit bump multiplier");
    translate(directory, c);
}
void closure_identity(const fs::path &directory) {
    const auto original = directory / "closure-original";
    const auto changed = directory / "closure-changed";
    const auto relocated = directory / "closure-relocated";
    for (const auto &path : {original, changed, relocated})
        fs::create_directories(path);
    exr(original / "image.exr", {.2f, .4f, .6f});
    exr(changed / "image.exr", {.8f, .4f, .6f});
    fs::copy_file(original / "image.exr", relocated / "image.exr");
    auto make = [&](const fs::path &path) {
        auto a = asset(path, {{"map_Kd", {"image.exr"}}});
        a.sources.push_back({path / "mesh.obj", "obj", {'o', 'b', 'j'}});
        a.sources.push_back({path / "mesh.mtl", "mtl", {'m', 't', 'l'}});
        return success(a);
    };
    auto first = make(original);
    auto second = make(changed);
    const auto moved = make(relocated);
    require(first.default_material.name != second.default_material.name &&
                first.materials[0].name != second.materials[0].name,
            "changed image closure must change every global material namespace");
    require(first.definitions == moved.definitions &&
                first.materials[0].name == moved.materials[0].name,
            "relocated identical complete closure must retain namespace and definitions");
    // Both complete sets of declarations can coexist in one PBRT world.
    first.definitions += second.definitions;
    first.materials.insert(first.materials.end(), second.materials.begin(), second.materials.end());
    first.files.insert(first.files.end(), second.files.begin(), second.files.end());
    translate(directory / "combined-closure", first);
}
void geometry_domain(const fs::path &directory) {
    auto emitter = asset(directory, {{"Ke", {"1"}}});
    emitter.polygons.push_back({{}, {}, "", {}, 0, {emitter.entry.string(), 42, 1}});
    GeometryMesh mesh;
    mesh.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    mesh.triangles = {{0, 1, 2}};
    mesh.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    emitter.parts.push_back({mesh, 0, {0}, {0}, false});
    success(emitter);
    auto large = emitter;
    large.parts[0].geometry.positions = {{0, 0, 0}, {1e20f, 0, 0}, {0, 1e20f, 0}};
    const auto rejected_emitter = obj::prepare_materials(large);
    require(rejected_emitter.exit_code() == 1 &&
                diagnostics(rejected_emitter).find("binary32") != std::string::npos,
            "large emitter must fail input-domain preflight before publication");
    require(std::ranges::any_of(rejected_emitter.diagnostics,
                                [](const auto &d) {
                                    return d.severity == obj::Severity::error &&
                                           d.location.line == 42;
                                }),
            "emitter-domain error identifies source polygon");
    auto degenerate = asset(directory, {});
    mesh.positions = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}};
    degenerate.parts.push_back({mesh, 0, {}, {}, false});
    const auto bad_frame = obj::prepare_materials(degenerate);
    require(bad_frame.exit_code() == 1 &&
                diagnostics(bad_frame).find("texture frame") != std::string::npos,
            "authored material requires a valid binary32 texture frame even without maps");
    obj::Limits limits;
    limits.input.total_values = 5;
    require(obj::prepare_materials(emitter, limits).exit_code() == 1,
            "triangle frame temporaries obey aggregate value limit");
}
void rejected(const fs::path &directory) {
    auto expect = [&](const obj::Asset &a, int status, const std::string &text) {
        const auto c = obj::prepare_materials(a);
        require(c.exit_code() == status && diagnostics(c).find(text) != std::string::npos,
                "expected rejection " + text + ": " + diagnostics(c));
        require(!c.diagnostics.empty() && *c.diagnostics.back().location.file != "",
                "source-located rejection");
    };
    expect(asset(directory, {{"d", {"0"}}, {"Ke", {"1"}}}), 2, "fully opaque");
    expect(asset(directory, {{"d", {".4"}}, {"Tr", {".5"}}}), 1, "Contradictory");
    success(asset(directory, {{"d", {".4"}}, {"Tr", {".6"}}}));
    expect(asset(directory, {{"Pr", {"nan"}}}), 1, "finite");
    fs::create_directory(directory / "directory.png");
    expect(asset(directory, {{"map_Kd", {"directory.png"}}}), 1, "regular file");
#if defined(__unix__) || defined(__APPLE__)
    require(::mkfifo((directory / "fifo.png").c_str(), 0600) == 0, "create FIFO image fixture");
    expect(asset(directory, {{"map_Kd", {"fifo.png"}}}), 1, "regular file");
#endif
    success(asset(directory,
                  {{"Pr", {"+.5"}}, {"Ka", {"+20", "20", "20"}}, {"map_Ka", {"color.exr"}}}));
    expect(asset(directory, {{"map_Pm", {"bad-metal.exr"}}}), 1, "[0,1]");
    expect(asset(directory, {{"norm", {"zero-normal.exr"}}}), 1, "zero vector");
    expect(asset(directory, {{"Kd", {"xyz", "1", "1", "1"}}}), 2, "XYZ");
    expect(asset(directory, {{"illum", {"10"}}}), 2, "shadow-catcher");
    expect(asset(directory, {{"illum", {"7"}}, {"Tf", {".5", "1", "1"}}}), 2, "Colored Tf");
    expect(asset(directory, {{"aniso", {".5"}}}), 2, "Unsupported MTL property");
    expect(asset(directory, {{"map_Pr", {"-imfchan", "m", "rough.exr"}}}), 2,
           "retained image alpha");
    expect(asset(directory, {{"norm", {"-s", "2", "2", "rough.exr"}}}), 2, "identity repeat");
    expect(asset(directory, {{"map_Ke", {"-clamp", "on", "rough.exr"}}}), 2, "identity repeat");
    expect(asset(directory, {{"map_Kd", {"-blendu", "off", "color.exr"}}}), 2,
           "Unsupported MTL map option");
    auto a = asset(directory, {{"map_Kd", {"color.exr"}}});
    a.polygons.push_back({{}, {}, "", {}, 0, {a.entry.string(), 20, 1}});
    a.parts.push_back({{}, 0, {0}, {0}, false});
    expect(a, 1, "authored UV");
    a.parts[0].complete_uvs = true;
    success(a);
    a.parts[0].material = 99;
    expect(a, 1, "material index");
    obj::Limits limits;
    limits.output_bytes = 20;
    require(obj::prepare_materials(asset(directory, {}), limits).exit_code() == 1, "output budget");
    limits = {};
    limits.input.total_values = 2;
    require(
        obj::prepare_materials(asset(directory, {{"map_Pr", {"rough.exr"}}}), limits).exit_code() ==
            1,
        "texel work budget");
    limits = {};
    limits.input.diagnostics = 1;
    require(obj::prepare_materials(asset(directory, {{"Ka", {".5"}}, {"illum", {"10"}}}), limits)
                    .exit_code() == 2,
            "diagnostic cap retains rejection");
    require(obj::prepare_materials(asset(directory, {{"Ka", {".5"}}}), limits).exit_code() == 1,
            "warning budget exhaustion must reject incomplete provenance");
}
} // namespace
int main() {
    try {
        Temporary temporary;
        constants(temporary.path);
        images(temporary.path);
        rejected(temporary.path);
        closure_identity(temporary.path);
        geometry_domain(temporary.path);
        std::cout << "OBJ material conversion checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
