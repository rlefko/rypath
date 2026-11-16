// SPDX-License-Identifier: GPL-3.0-only
#include "pbrt_render.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace rypath;
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct Fixture {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("rypath-packet-pbrt-" + std::to_string(getpid()));
    Fixture() { std::filesystem::create_directories(dir); }
    ~Fixture() { std::filesystem::remove_all(dir); }
    pbrt::RenderTranslation translate(const std::string &text) {
        const auto path = dir / "scene.pbrt";
        std::ofstream(path) << text;
        auto parsed = pbrt::check(path);
        require(parsed.exit_code() == 0, pbrt::report(parsed));
        return pbrt::translate(parsed, {path.string()});
    }
};
std::string diagnostic(const pbrt::RenderTranslation &result) {
    std::string text;
    for (const auto &item : result.diagnostics)
        text += item.message + "\n";
    return text;
}
std::string provenance(const pbrt::RenderTranslation &result) {
    std::string text;
    for (const auto &item : result.provenance)
        text += item + "\n";
    return text;
}
const RenderScene &success(const pbrt::RenderTranslation &result) {
    if (result.exit_code() != 0 || !result.scene.has_value())
        throw std::runtime_error(diagnostic(result));
    return *result.scene;
}
std::string options(const std::string &film, const std::string &integrator) {
    return "Film \"rgb\" \"integer xresolution\" [4] \"integer yresolution\" [3] " + film +
           "\nCamera \"perspective\"\nSampler \"halton\" \"integer pixelsamples\" [16]\n" +
           integrator + "\nWorldBegin\n";
}
void run() {
    Fixture fixture;
    const std::string world = "LightSource \"infinite\" \"rgb L\" [.1 .2 .3]\n";
    auto result = fixture.translate(
        options("\"float maxcomponentvalue\" [50] \"float iso\" [500]", "") + world);
    const auto &day = success(result);
    require(day.integrator == IntegratorKind::volpath && day.volume_transport &&
                day.spectral_transport == SpectralTransport::packet &&
                day.max_component_value == 50 && day.exposure == 5,
            "Authored clamp and omitted volpath must select packet transport");
    require(provenance(result).find("table-integral camera sample weight") != std::string::npos &&
                provenance(result).find("unit sample weight") == std::string::npos &&
                provenance(result).find("fixed dimension allocation [0,920)") !=
                    std::string::npos &&
                provenance(result).find("keyed material-local Philox streams") != std::string::npos,
            "Packet Gaussian and sampler provenance must describe packet allocation and weights");
    result = fixture.translate(options("", "Integrator \"bdpt\"") + world);
    const auto &night = success(result);
    require(night.integrator == IntegratorKind::bdpt && !night.volume_transport &&
                night.spectral_transport == SpectralTransport::packet && night.max_depth == 5 &&
                !std::isfinite(night.max_component_value),
            "BDPT defaults and unclamped packet mode");
    for (unsigned depth : {0u, 64u}) {
        result = fixture.translate(options("", "Integrator \"bdpt\" \"integer maxdepth\" [" +
                                                   std::to_string(depth) + "]") +
                                   world);
        require(success(result).max_depth == depth, "BDPT finite depth boundary");
    }
    result =
        fixture.translate(options("", "Integrator \"bdpt\" \"integer maxdepth\" [65]") + world);
    require(result.exit_code() == 2 && diagnostic(result).find("64") != std::string::npos,
            "BDPT depth overflow must diagnose, never truncate");
    result = fixture.translate(options("", "") + world);
    require(success(result).spectral_transport == SpectralTransport::scalar,
            "Unclamped default transport must preserve scalar mode");
    require(provenance(result).find("normalized PBRT midpoint table; unit sample weight") !=
                    std::string::npos &&
                provenance(result).find("Packet sampling:") == std::string::npos,
            "Scalar Gaussian provenance must retain legacy unit sample weights");
    const std::string lens_prefix =
        "Film \"rgb\" \"integer xresolution\" [4] \"integer yresolution\" [3]\n"
        "PixelFilter \"gaussian\"\nCamera \"perspective\" \"float lensradius\" [.05] \"float "
        "focaldistance\" [2]\n"
        "Sampler \"halton\" \"integer pixelsamples\" [16]\n";
    result = fixture.translate(lens_prefix + "Integrator \"bdpt\"\nWorldBegin\n" + world);
    require(
        success(result).perspective_lens.has_value() &&
            provenance(result).find("fixed packet header block 1") != std::string::npos &&
            provenance(result).find("lens-v1 sampling") == std::string::npos &&
            provenance(result).find("table-integral camera sample weight") != std::string::npos,
        "Packet lens and Gaussian provenance must not claim legacy lens-v1 or unit filter weights");
    result = fixture.translate(lens_prefix + "Integrator \"path\"\nWorldBegin\n" + world);
    require(success(result).spectral_transport == SpectralTransport::scalar &&
                provenance(result).find("lens-v1 sampling") != std::string::npos &&
                provenance(result).find("fixed packet header block 1") == std::string::npos,
            "Scalar lens provenance must retain legacy lens-v1 sampling");
    const std::string lights = world + "LightSource \"point\"\nLightSource \"distant\"\n"
                                       "Shape \"trianglemesh\" \"point3 P\" [0 0 1 1 0 1 0 1 1]\n";
    result = fixture.translate(options("", "Integrator \"bdpt\"") + lights);
    require(success(result).point_lights.size() == 1 &&
                success(result).distant_lights.size() == 1 &&
                provenance(result).find("Point lights: 1 (uniform combined light selection; "
                                        "delta-position endpoint PDFs)") != std::string::npos &&
                provenance(result).find("Distant lights: 1 (uniform combined light selection; "
                                        "delta-direction endpoint PDFs;") != std::string::npos &&
                provenance(result).find("all-point direct evaluation") == std::string::npos &&
                provenance(result).find("all-distant direct evaluation") == std::string::npos,
            "Packet point/distant provenance must describe uniform combined light selection");
    result = fixture.translate(options("", "Integrator \"path\"") + lights);
    success(result);
    require(provenance(result).find("all-point direct evaluation; delta-position weights") !=
                    std::string::npos &&
                provenance(result).find("all-distant direct evaluation; delta-direction weights") !=
                    std::string::npos &&
                provenance(result).find("uniform combined light selection") == std::string::npos,
            "Scalar point/distant provenance must retain all-light direct evaluation");
    const std::string fog = "MakeNamedMedium \"fog\" \"string type\" \"homogeneous\"\n";
    const std::string vacuum = "MakeNamedMedium \"vacuum\" \"string type\" \"homogeneous\" "
                               "\"rgb sigma_a\" [0 0 0] \"rgb sigma_s\" [0 0 0]\n";
    const auto packet = options("", "Integrator \"bdpt\"");
    success(fixture.translate(packet + fog + world));
    success(fixture.translate(vacuum + "MediumInterface \"vacuum\"\n" + packet + world));
    for (const auto &scene :
         {fog + "MediumInterface \"fog\"\n" + packet + world,
          packet + fog + "MediumInterface \"fog\"\nShape \"sphere\"\n",
          packet + fog + "MediumInterface \"\" \"fog\"\nLightSource \"point\"\n"}) {
        result = fixture.translate(scene);
        require(result.exit_code() == 2 && diagnostic(result).find("vacuum") != std::string::npos,
                "Active camera, equal-side placed, and light-endpoint media must diagnose");
    }
}
} // namespace
int main() {
    try {
        run();
        std::cout << "PBRT packet, BDPT, clamp, and vacuum profile tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
