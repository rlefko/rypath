// SPDX-License-Identifier: GPL-3.0-only
#include "pbrt_render.hpp"
#include "pbrt_spectra.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace rypath;
using namespace rypath::pbrt;
namespace fs = std::filesystem;
void require(bool value, std::string_view message) {
    if (!value)
        throw std::runtime_error(std::string(message));
}
constexpr std::string_view options = R"(Camera "perspective"
Film "rgb" "integer xresolution" 8 "integer yresolution" 8
PixelFilter "box"
Sampler "independent" "integer pixelsamples" 4
Integrator "volpath"
WorldBegin
)";
constexpr std::string_view triangle = R"(Shape "trianglemesh" "point3 P" [-1 -1 3 1 -1 3 0 1 3]
)";
struct Fixture {
    fs::path directory;
    unsigned index = 0;
    RenderTranslation translate(std::string_view source) {
        const auto path = directory / (std::to_string(index++) + ".pbrt");
        {
            std::ofstream file(path);
            file << source;
        }
        const auto checked = check(path);
        if (checked.exit_code() != 0)
            throw std::runtime_error(report(checked));
        return rypath::pbrt::translate(checked, Location{path.string()});
    }
    // Scene text precedes the expected diagnostic at every call site.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void reject(std::string_view source, std::string_view expected) {
        const auto path = directory / (std::to_string(index++) + ".pbrt");
        {
            std::ofstream file(path);
            file << source;
        }
        const auto checked = check(path);
        std::string diagnostics = report(checked);
        int status = checked.exit_code();
        if (!status) {
            const auto result = rypath::pbrt::translate(checked, Location{path.string()});
            status = result.exit_code();
            for (const auto &d : result.diagnostics)
                diagnostics += format_diagnostic(d);
        }
        require(status != 0, "Expected medium rejection");
        require(diagnostics.find(expected) != std::string::npos, diagnostics);
    }
};
void success(const RenderTranslation &result) {
    if (result.exit_code()) {
        std::string diagnostic;
        for (const auto &d : result.diagnostics)
            diagnostic += format_diagnostic(d);
        throw std::runtime_error(diagnostic);
    }
    require(result.scene.has_value(), "Missing translated scene");
}
const RenderScene &checked_scene(const RenderTranslation &result) {
    if (!result.scene)
        throw std::runtime_error("Missing translated scene");
    return *result.scene;
}
void tests(Fixture &f) {
    auto result = f.translate(options);
    success(result);
    require(checked_scene(result).volume_transport && checked_scene(result).media.empty(),
            "Empty volpath");
    result = f.translate(std::string(options) + R"(MakeNamedMedium "fog" "string type" "homogeneous"
MediumInterface "fog"
Material "interface"
)" + std::string(triangle));
    success(result);
    require(checked_scene(result).media[0].sigma_a[100] == 1 &&
                checked_scene(result).media[0].sigma_s[100] == 1,
            "Pinned coefficient defaults");
    require(checked_scene(result).instance_media.empty(),
            "Equal-side interfaces must compact to retain the incoming medium");
    require(
        checked_scene(result).materials[checked_scene(result).instance_materials[0]].scattering ==
            ScatteringKind::null_interface,
        "Null boundary became a BSDF");
    result = f.translate(R"(MakeNamedMedium "a" "string type" "homogeneous" "rgb sigma_a" [0 0 0]
MediumInterface "" "a"
)" + std::string(options) +
                         R"(MakeNamedMedium "b" "string type" "homogeneous"
MediumInterface "b" ""
AttributeBegin
MediumInterface "a" "b"
Material "none"
)" + std::string(triangle) +
                         "AttributeEnd\n" + std::string(triangle));
    success(result);
    require(checked_scene(result).camera_medium == 0,
            "Camera medium captured after its declaration");
    require(checked_scene(result).instance_media[0].inside == 0 &&
                checked_scene(result).instance_media[0].outside == 1 &&
                checked_scene(result).instance_media[1].inside == 1 &&
                checked_scene(result).instance_media[1].outside == no_medium,
            "Scoped interface was not restored");
    result = f.translate(std::string(options) + R"(Attribute "medium" "rgb sigma_a" [0 0 0]
MakeNamedMedium "fog" "string type" "homogeneous" "spectrum sigma_s" [360 2 830 2]
 "float scale" 3 "float g" 2 "spectrum Le" [360 1 830 1] "float Lescale" 2
)");
    success(result);
    const auto &m = checked_scene(result).media[0];
    require(m.sigma_a[0] == 0 && m.sigma_s[100] == 6 && m.g == .99f && m.emission[100] > 0,
            "Medium inheritance/scales/emission/clamp");
    result = f.translate(std::string(options) + R"(MakeNamedMedium "fog" "string type" "homogeneous"
 "string preset" "not-a-preset" "rgb sigma_a" [0 0 0]
)");
    success(result);
    require(checked_scene(result).media[0].sigma_a[20] == 0 &&
                checked_scene(result).media[0].sigma_s[20] == 1 && !result.diagnostics.empty(),
            "Unknown preset must warn and use independent defaults");

    // Numeric and file spectra retain zero outside their declared interval.
    {
        std::ofstream file(f.directory / "medium.spd");
        file << "450 2\n650 2\n";
    }
    result = f.translate(std::string(options) + R"(MakeNamedMedium "fog" "string type" "homogeneous"
 "spectrum sigma_a" [450 2 650 2] "spectrum sigma_s" "medium.spd"
 "spectrum Le" "medium.spd" "float Lescale" 3
)");
    success(result);
    require(checked_scene(result).media[0].sigma_a[0] == 0 &&
                checked_scene(result).media[0].sigma_a[200] == 2 &&
                checked_scene(result).media[0].sigma_s[0] == 0 &&
                checked_scene(result).media[0].sigma_s[200] == 2 &&
                checked_scene(result).media[0].emission[0] == 0 &&
                checked_scene(result).media[0].emission[200] > 0,
            "Medium spectrum file and numeric support differ");
    result = f.translate(std::string(options) + R"(MakeNamedMedium "fog" "string type" "homogeneous"
 "spectrum sigma_a" "stdillum-D65" "blackbody sigma_s" 6500 "blackbody Le" 3000
)");
    success(result);
    require(checked_scene(result).media[0].sigma_a == *pbrt_named_spectrum("stdillum-D65") &&
                checked_scene(result).media[0].sigma_s[100] > 0 &&
                checked_scene(result).media[0].emission[100] > 0,
            "Named and blackbody medium spectra failed");
    result = f.translate(std::string(options) + R"(MakeNamedMedium "fog" "string type" "homogeneous"
 "rgb Le" [1 0.5 0.25] "float scale" 0 "float Lescale" 2
)");
    success(result);
    const auto emission = checked_scene(result).media[0].emission;
    require(checked_scene(result).media[0].sigma_s[100] == 0 && emission[100] > 0,
            "Extinction scale incorrectly scaled emission");
    result = f.translate(std::string(options) + R"(MakeNamedMedium "fog" "string type" "homogeneous"
 "rgb Le" [1 0.5 0.25] "float scale" 8 "float Lescale" 1
)");
    success(result);
    require(std::abs(emission[100] - 2 * checked_scene(result).media[0].emission[100]) < 1e-8,
            "Independent medium emission scaling failed");
    result = f.translate(std::string(options) +
                         R"(MakeNamedMedium "fog" "string type" "homogeneous" "rgb Le" [0 0 0]
)");
    success(result);
    require(checked_scene(result).media[0].emission[100] == 0,
            "Zero emission normalized into nonzero");
    result = f.translate(std::string(options) + R"(MakeNamedMedium "a" "string type" "homogeneous"
MakeNamedMedium "b" "string type" "homogeneous"
AttributeBegin
MediumInterface "a" "b"
ObjectBegin "boundary"
Material "interface"
)" + std::string(triangle) +
                         R"(ObjectEnd
AttributeEnd
MediumInterface "" ""
Scale -1 1 1
ObjectInstance "boundary"
)");
    success(result);
    require(checked_scene(result).instance_media[0].inside == 0 &&
                checked_scene(result).instance_media[0].outside == 1,
            "Instance placement replaced object medium binding");
    {
        std::ofstream file(f.directory / "medium.inc");
        file << "MediumInterface \"a\" \"\"\n";
    }
    result = f.translate(std::string(options) + R"(Include "medium.inc"
Material "interface"
)" + std::string(triangle) +
                         R"(MakeNamedMedium "a" "string type" "homogeneous"
)");
    success(result);
    require(checked_scene(result).instance_media[0].inside == 0,
            "Included forward medium reference failed");
    f.reject(std::string(options) +
                 R"(MakeNamedMedium "fog" "string type" "homogeneous" "float scale" -1
)",
             "nonnegative");
    f.reject(std::string(options) +
                 R"(MakeNamedMedium "fog" "string type" "homogeneous" "rgb sigma_a" [3e38 3e38 3e38]
)",
             "overflows");
    f.reject(std::string(options) +
                 R"(MakeNamedMedium "fog" "string type" "homogeneous" "texture sigma_a" "t"
)",
             "Unexpected type");
    result = f.translate(std::string(options) +
                         R"(Attribute "medium" "spectrum sigma_a" "missing-ignored.spd"
MakeNamedMedium "fog" "string type" "homogeneous" "string preset" "Apple"
)");
    success(result);
    f.reject(std::string(options) + "Material \"interface\" \"float displacement\" 0.5\n" +
                 std::string(triangle),
             "Unknown parameter");
    f.reject(std::string(options) + "Material \"none\" \"string normalmap\" \"missing.png\"\n" +
                 std::string(triangle),
             "Unknown parameter");
    f.reject(std::string(options) +
                 R"(MakeNamedMedium "fog" "string type" "homogeneous" "spectrum Le" [1000 1 2000 1]
)",
             "photometric normalization");
    f.reject(
        std::string(options) +
            R"(MakeNamedMedium "fog" "string type" "homogeneous" "spectrum Le" [500.2 1 500.3 1]
)",
        "photometric normalization");
    f.reject(
        std::string(options) +
            R"(MakeNamedMedium "fog" "string type" "homogeneous" "spectrum Le" [1000 1 2000 1] "float Lescale" 0
)",
        "photometric normalization");
    {
        std::ofstream file(f.directory / "invisible.spd");
        file << "1000 1\n2000 1\n";
    }
    f.reject(std::string(options) +
                 R"(MakeNamedMedium "fog" "string type" "homogeneous" "spectrum Le" "invisible.spd"
)",
             "photometric normalization");
    for (const auto temperature : {"100", "1e20"}) {
        result = f.translate(
            std::string(options) +
            "MakeNamedMedium \"fog\" \"string type\" \"homogeneous\" \"blackbody Le\" " +
            temperature + "\n");
        success(result);
        require(std::ranges::any_of(checked_scene(result).media[0].emission,
                                    [](float v) { return v > 0; }),
                "Positive blackbody emission silently became zero");
    }
    f.reject(std::string(options) + R"(MakeNamedMedium "fog" "string type" "homogeneous"
 "string preset" "Apple" "spectrum sigma_a" "missing.spd"
)",
             "conflicts");
    f.reject(std::string(options) +
                 R"(MakeNamedMedium "fog" "string type" "homogeneous" "rgb sigma_a" [-1 0 0]
)",
             "nonnegative");
    f.reject(std::string(options) + "MediumInterface \"absent\" \"\"\n", "Undefined medium");
    f.reject(std::string(options) + R"(MakeNamedMedium "fog" "string type" "homogeneous"
MediumInterface "fog" ""
Material "interface"
)" + std::string(triangle) +
                 " \"float alpha\" 0.5\n",
             "opaque alpha");
    f.reject(std::string(options) + R"(Material "interface"
AreaLightSource "diffuse"
)" + std::string(triangle),
             "cannot emit");
    f.reject(std::string(options) + R"(MakeNamedMaterial "null" "string type" "interface"
MakeNamedMaterial "d" "string type" "diffuse"
Material "mix" "string materials" ["null" "d"]
)" + std::string(triangle),
             "mix child");
    std::string path_options(options);
    path_options.replace(path_options.find("volpath"), 7, "path");
    result =
        f.translate(path_options + "MakeNamedMedium \"unused\" \"string type\" \"homogeneous\"\n" +
                    std::string(triangle));
    success(result);
    f.reject(path_options + R"(MakeNamedMedium "fog" "string type" "homogeneous"
MediumInterface "fog" ""
)" + std::string(triangle),
             "volpath");
    struct Preset {
        std::string_view name;
        std::array<float, 3> scattering, absorption;
    };
    constexpr Preset presets[]{
#include "../third_party/pbrt/medium_presets.inc"
    };
    for (const auto &preset : presets) {
        result = f.translate(
            std::string(options) +
            "MakeNamedMedium \"m\" \"string type\" \"homogeneous\" \"string preset\" \"" +
            std::string(preset.name) + "\"\n");
        success(result);
        require(checked_scene(result).media.size() == 1 &&
                    checked_scene(result).media[0].sigma_s[200] >= 0,
                "Preset translation failed");
    }
}
void dump_presets(Fixture &f) {
    struct Preset {
        std::string_view name;
        std::array<float, 3> scattering, absorption;
    };
    constexpr Preset presets[]{
#include "../third_party/pbrt/medium_presets.inc"
    };
    for (const auto &p : presets) {
        const auto result = f.translate(
            std::string(options) +
            "MakeNamedMedium \"m\" \"string type\" \"homogeneous\" \"string preset\" \"" +
            std::string(p.name) + "\"\n");
        success(result);
        std::cout << p.name;
        for (const auto *values :
             {&checked_scene(result).media[0].sigma_a, &checked_scene(result).media[0].sigma_s})
            for (float v : *values)
                std::cout << ' ' << std::hex << std::setfill('0') << std::setw(8)
                          << std::bit_cast<std::uint32_t>(v);
        std::cout << '\n';
    }
}

void dump_spectra(Fixture &f) {
    const std::pair<std::string_view, std::string_view> cases[]{
        {"rgb",
         R"("rgb sigma_a" [0.2 0.6 1.1] "rgb sigma_s" [0.8 0.3 0.05] "rgb Le" [2 0.5 1] "float scale" 0.7 "float Lescale" 1.3)"},
        {"blackbody3000",
         R"("blackbody sigma_a" 3000 "blackbody sigma_s" 3000 "blackbody Le" 3000 "float scale" 0.7 "float Lescale" 1.3)"},
        {"blackbody6500",
         R"("blackbody sigma_a" 6500 "blackbody sigma_s" 6500 "blackbody Le" 6500 "float scale" 0.7 "float Lescale" 1.3)"}};
    for (const auto &[name, parameters] : cases) {
        const auto result = f.translate(std::string(options) +
                                        "MakeNamedMedium \"m\" \"string type\" \"homogeneous\" " +
                                        std::string(parameters) + "\n");
        success(result);
        std::cout << name;
        for (const auto *values :
             {&checked_scene(result).media[0].sigma_a, &checked_scene(result).media[0].sigma_s,
              &checked_scene(result).media[0].emission})
            for (float v : *values)
                std::cout << ' ' << std::hex << std::setfill('0') << std::setw(8)
                          << std::bit_cast<std::uint32_t>(v);
        std::cout << '\n';
    }
}
} // namespace
int main(int argc, char **argv) try {
    const bool dump = argc == 3 && (std::string_view(argv[1]) == "--dump-presets" ||
                                    std::string_view(argv[1]) == "--dump-spectra");
    require(argc == 2 || dump, "Expected test directory, optionally preceded by --dump-presets");
    const fs::path directory = argv[dump ? 2 : 1];
    fs::create_directories(directory);
    Fixture fixture{directory};
    if (dump) {
        if (std::string_view(argv[1]) == "--dump-presets")
            dump_presets(fixture);
        else
            dump_spectra(fixture);
        return 0;
    }
    tests(fixture);
    std::cout << "Homogeneous medium scene checks passed\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
