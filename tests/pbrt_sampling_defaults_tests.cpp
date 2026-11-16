// SPDX-License-Identifier: GPL-3.0-only
#include "pbrt_render.hpp"
#include "render_identity.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace rypath;
using namespace rypath::pbrt;
void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}
constexpr std::array declarations{"Camera \"perspective\"\n", "Film \"rgb\"\n",
                                  "PixelFilter \"gaussian\"\n", "Sampler \"zsobol\"\n",
                                  "Integrator \"volpath\"\n"};
constexpr std::string_view world =
    "WorldBegin\nLightSource \"infinite\"\n"
    "Shape \"trianglemesh\" \"point3 P\" [0 0 2 1 0 2 0 1 2] \"integer indices\" [0 1 2]\n";

struct AcceptedScene {
    RenderScene scene;
    RenderSettings settings;
    std::vector<Diagnostic> diagnostics;
};

struct Fixture {
    std::filesystem::path path;
    Result checked(std::string_view source) const {
        std::ofstream out(path);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        out << source;
        out.close();
        return check(path);
    }
    RenderTranslation translated(std::string_view source) const {
        const auto result = checked(source);
        require(result.exit_code() == 0, report(result));
        return translate(result, Location{path.string()});
    }
    AcceptedScene accepted(std::string_view source) const {
        auto result = translated(source);
        if (result.exit_code() != 0 || !result.scene) {
            for (const auto &diagnostic : result.diagnostics)
                std::cerr << diagnostic.message << '\n';
            throw std::runtime_error("expected accepted defaults fixture");
        }
        return {std::move(result.scene.value()), result.settings, std::move(result.diagnostics)};
    }
};

void omissions(const Fixture &fixture) {
    const auto implicit = fixture.accepted(world);
    require(implicit.settings.width == 1280 && implicit.settings.height == 720 &&
                implicit.settings.samples == 16 && implicit.settings.seed == 0 &&
                implicit.settings.sampler.kind == SamplerKind::zsobol &&
                implicit.settings.sampler.randomization == SobolRandomization::fastowen &&
                implicit.scene.volume_transport && implicit.scene.max_depth == 5 &&
                implicit.scene.filter.kind == PixelFilterKind::gaussian &&
                implicit.scene.filter.x_radius == 1.5f && implicit.scene.filter.y_radius == 1.5f &&
                implicit.scene.filter.sigma == .5f,
            "pinned implicit scene defaults");
    const auto identity = render_identity(implicit.scene, implicit.settings);
    for (unsigned mask = 0; mask != 32; ++mask) {
        std::string source;
        for (unsigned i = 0; i != declarations.size(); ++i)
            if (mask & (1u << i))
                source += declarations[i];
        source += world;
        const auto value = fixture.accepted(source);
        require(render_identity(value.scene, value.settings) == identity,
                "every explicit/default descriptor combination is equivalent");
    }
    const auto preflight = fixture.checked(world);
    require(preflight.scene && preflight.scene->options.empty(),
            "translation must not fabricate authored descriptors");
    const auto camera_state = fixture.accepted(
        std::string("Translate 9 8 7\nMediumInterface \"\" \"unused\"\nWorldBegin\n"
                    "MakeNamedMedium \"unused\" \"string type\" \"homogeneous\"\n"
                    "MediumInterface \"\" \"\"\n") +
        std::string(world.substr(world.find("LightSource"))));
    if (!camera_state.scene.raster_camera)
        throw std::runtime_error("omitted camera did not produce raster coefficients");
    const auto default_origin = camera_state.scene.raster_camera->origin;
    require(default_origin.x == 0 && default_origin.y == 0 && default_origin.z == 0 &&
                camera_state.scene.camera_medium == no_medium,
            "omitted camera ignores option transform and medium");
    const auto color_state =
        fixture.accepted(std::string("ColorSpace \"rec2020\"\nWorldBegin\n") +
                         "Shape \"trianglemesh\" \"point3 P\" [0 0 2 1 0 2 0 1 2] "
                         "\"integer indices\" [0 1 2]\n");
    require(color_state.scene.exposure == 1, "implicit RGB film retains initialized sRGB");
    require(fixture.checked("WorldBegin\nCoordSysTransform \"camera\"\n").exit_code() == 1,
            "omitted camera must not create named camera coordinates");
    const auto captured = fixture.accepted(
        std::string("Translate -1 -2 -3\nCamera \"perspective\"\nTranslate 7 8 9\n") +
        std::string(world));
    if (!captured.scene.raster_camera)
        throw std::runtime_error("explicit camera did not produce raster coefficients");
    const auto origin = captured.scene.raster_camera->origin;
    require(origin.x == 1 && origin.y == 2 && origin.z == 3,
            "explicit camera snapshots its own transform");
    for (auto unsupported :
         {"Camera \"orthographic\"\n", "Film \"gbuffer\"\n", "PixelFilter \"mitchell\"\n",
          "Sampler \"pmj02bn\"\n", "Integrator \"bdpt\"\n"}) {
        require(fixture.checked(std::string(unsupported) + std::string(world)).exit_code() == 2,
                "unsupported explicit descriptor must not select a default");
    }
}

void samplers(const Fixture &fixture) {
    for (auto type : {"halton", "zsobol"}) {
        const std::string prefix = std::string("Sampler \"") + type + "\" ";
        auto defaults = fixture.accepted(prefix + '\n' + std::string(world));
        const bool halton = std::string_view(type) == "halton";
        require(defaults.settings.samples == 16 &&
                    defaults.settings.sampler.randomization ==
                        (halton ? SobolRandomization::permutedigits : SobolRandomization::fastowen),
                "sampler-specific defaults");
        for (auto mode : {"none", "permutedigits", "fastowen", "owen"}) {
            const auto source = prefix +
                                "\"integer seed\" -2147483648 "
                                "\"integer pixelsamples\" 7 \"string randomization\" \"" +
                                mode + "\"\n" + std::string(world);
            if (halton && std::string_view(mode) == "fastowen") {
                require(fixture.checked(source).exit_code() == 1, "Halton rejects fastowen");
                continue;
            }
            const auto result = fixture.accepted(source);
            require(result.settings.samples == (halton ? 7 : 4) &&
                        sobol_signed_seed(result.settings.seed) == INT32_MIN,
                    "effective sample count and signed seed");
            require(halton ? result.diagnostics.empty()
                           : result.diagnostics.size() == 1 &&
                                 result.diagnostics.front().severity == Severity::warning &&
                                 result.diagnostics.front().location.line == 1,
                    "ZSobol rounding diagnostic points to authored count");
        }
        const unsigned max_depth = halton ? 124 : 127;
        const auto accepted_depth =
            fixture.accepted(prefix +
                             "\nIntegrator \"path\" "
                             "\"integer maxdepth\" " +
                             std::to_string(max_depth) + '\n' + std::string(world));
        require(accepted_depth.scene.max_depth == max_depth, "inclusive dimension depth limit");
        const auto rejected_depth =
            fixture.translated(prefix +
                               "\nIntegrator \"path\" "
                               "\"integer maxdepth\" " +
                               std::to_string(max_depth + 1) + '\n' + std::string(world));
        require(rejected_depth.exit_code() == 2, "reject dimension exhaustion");
        const auto dielectric_depth = halton ? 110u : 113u;
        for (unsigned extra = 0; extra != 2; ++extra) {
            const auto source = prefix + "\nIntegrator \"path\" \"integer maxdepth\" " +
                                std::to_string(dielectric_depth + extra) +
                                "\nWorldBegin\nMaterial \"dielectric\"\n" +
                                std::string(world.substr(world.find("Shape")));
            const auto result = fixture.translated(source);
            require(result.exit_code() == (extra ? 2 : 0), "dielectric dimension boundary");
        }
    }
}

void gaussian(const Fixture &fixture) {
    for (auto values : {"\"float xradius\" .02", "\"float sigma\" 1e-40",
                        "\"float xradius\" .0625 \"float yradius\" .0625 \"float sigma\" .001",
                        "\"float xradius\" 129", "\"float xradius\" 128 \"float yradius\" 128"}) {
        const auto result = fixture.translated(std::string("PixelFilter \"gaussian\" ") + values +
                                               '\n' + std::string(world));
        require(result.exit_code() == 1 && !result.scene && !result.diagnostics.empty() &&
                    result.diagnostics.front().location.line == 1,
                "invalid Gaussian domain fails at the filter declaration");
    }
    const auto tiny = fixture.accepted("PixelFilter \"gaussian\" \"float xradius\" .0625 "
                                       "\"float yradius\" .0625 \"float sigma\" .01\n" +
                                       std::string(world));
    require(tiny.scene.filter.sigma == .01f, "positive two-cell Gaussian retained");
    auto changed = tiny.scene;
    changed.filter.sigma = .02f;
    require(render_identity(changed, tiny.settings) != render_identity(tiny.scene, tiny.settings),
            "Gaussian sigma participates in checkpoint identity");
}
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 2, "expected scratch directory");
        std::filesystem::create_directories(argv[1]);
        Fixture fixture{std::filesystem::path(argv[1]) / "defaults.pbrt"};
        omissions(fixture);
        samplers(fixture);
        gaussian(fixture);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
