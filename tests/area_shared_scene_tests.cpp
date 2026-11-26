// SPDX-License-Identifier: GPL-3.0-only
#include "area_evaluation_context.hpp"
#include "frame_session_test_access.hpp"
#include "pbrt_render.hpp"
#include "render_identity.hpp"
#include "resource_limits.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef RYPATH_BUILD_DESCRIPTION
#define RYPATH_BUILD_DESCRIPTION "unknown"
#endif

namespace {
using namespace rypath;
namespace fs = std::filesystem;
void need(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Options {
    fs::path scene, library, output;
    unsigned width = 7, height = 3, frames = 8, warmup = 0;
    std::uint64_t seed = 61002;
    std::int32_t material_seed = 0;
    IntersectionProfile intersections = IntersectionProfile::native;
    ResourceProfile resource_profile = ResourceProfile::standard;
    bool off_off = false;
};
constexpr std::string_view usage =
    "Shared-AS Area correctness diagnostic (not a performance benchmark)\n"
    "  [--scene SCENE.pbrt] [--library FILE.metallib] [--output FRESH.json]\n"
    "  [--width 7] [--height 3] [--frames 8] [--warmup 0] [--seed 61002]\n"
    "  [--material-seed 0] [--intersections native|compensated] [--pair off-on|off-off]\n"
    "  [--resource-profile standard|large (default: standard; Pavilion requires large)]\n"
    "frames includes warmup; every frame is compared; first mismatch returns failure\n";
template <typename T> T number(std::string_view value) {
    T result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    need(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(),
         "Invalid integer: " + std::string(value));
    return result;
}
Options parse(int argc, char **argv) {
    Options o;
    std::set<std::string> seen;
    for (int n = 1; n < argc; n += 2) {
        const std::string key = argv[n];
        need(n + 1 < argc && seen.insert(key).second, "Missing or duplicate option: " + key);
        const std::string_view value = argv[n + 1];
        if (key == "--scene")
            o.scene = value;
        else if (key == "--library")
            o.library = value;
        else if (key == "--output")
            o.output = value;
        else if (key == "--width")
            o.width = number<unsigned>(value);
        else if (key == "--height")
            o.height = number<unsigned>(value);
        else if (key == "--frames")
            o.frames = number<unsigned>(value);
        else if (key == "--warmup")
            o.warmup = number<unsigned>(value);
        else if (key == "--seed")
            o.seed = number<std::uint64_t>(value);
        else if (key == "--material-seed")
            o.material_seed = number<std::int32_t>(value);
        else if (key == "--intersections") {
            need(value == "native" || value == "compensated", "Invalid intersection profile");
            o.intersections =
                value == "native" ? IntersectionProfile::native : IntersectionProfile::compensated;
        } else if (key == "--resource-profile") {
            const auto profile = parse_resource_profile(value);
            if (!profile)
                throw std::runtime_error("Invalid resource profile: " + std::string(value));
            o.resource_profile = *profile;
        } else if (key == "--pair") {
            need(value == "off-on" || value == "off-off", "Invalid shared-scene pair");
            o.off_off = value == "off-off";
        } else
            throw std::runtime_error("Unknown option: " + key);
    }
    need(o.width && o.height && o.width <= 16384 && o.height <= 16384 && o.frames &&
             o.frames <= 4096 && o.warmup < o.frames,
         "Dimensions must be 1..16384; frames 1..4096 with warmup < frames");
    need(2ull * o.width * o.height * sizeof(std::array<float, 3>) <= (512ull << 20),
         "Paired raw readback exceeds the 512 MiB diagnostic limit");
    return o;
}
std::string json_string(std::string_view value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\')
            out << '\\' << char(c);
        else if (c < 32)
            out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c)
                << std::dec;
        else
            out << char(c);
    }
    out << '"';
    return out.str();
}
// Match frame_benchmark's pre-translation film/sampler overrides. The authored
// camera, materials, geometry and maximum path depth are retained.
void parameter(pbrt::Descriptor &d, std::string name, std::optional<std::int32_t> value) {
    std::vector<pbrt::Parameter> effective;
    for (auto index : d.effective_authored)
        effective.push_back(d.authored.parameters[index]);
    d.authored.parameters = std::move(effective);
    std::erase_if(d.authored.parameters, [&](const auto &p) { return p.name == name; });
    std::erase_if(d.inherited, [&](const auto &p) { return p.parameter->name == name; });
    if (value) {
        pbrt::Parameter p;
        p.type = "integer";
        p.name = std::move(name);
        pbrt::Value v;
        v.kind = pbrt::ValueKind::integer;
        v.integer = *value;
        v.number = *value;
        v.text = std::to_string(*value);
        p.values.push_back(std::move(v));
        d.authored.parameters.push_back(std::move(p));
    }
    d.effective_authored.clear();
    for (std::size_t n = 0; n < d.authored.parameters.size(); ++n)
        d.effective_authored.push_back(n);
}
RenderScene fixture() {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.spectral_transport = SpectralTransport::hero;
    scene.max_depth = 4;
    scene.raster_camera = RasterCamera{{}, {-.05f, -.05f, 1}, {.1f, 0, 0}, {0, .1f, 0}};
    SpectralMaterial material;
    material.kind = SpectralMaterialKind::constant;
    material.constant = .6f;
    scene.materials.push_back(material);
    GeometryMesh mesh;
    mesh.positions = {{-8, -8, 3}, {8, -8, 3}, {8, 8, 3}, {-8, 8, 3}};
    mesh.triangles = {{0, 2, 1}, {0, 3, 2}};
    scene.geometry.meshes.push_back(std::move(mesh));
    scene.geometry.instances.push_back({0, {}});
    scene.instance_materials.push_back(0);
    scene.instance_emissions.push_back(no_area_emission);
    scene.instance_surface_transforms.emplace_back();
    scene.environment.fill(.2f);
    PointLight light;
    light.position = {.75f, .5f, 1};
    light.intensity.fill(4);
    scene.point_lights.push_back(light);
    return scene;
}
RenderScene load(const Options &o) {
    if (o.scene.empty())
        return fixture();
    // Use the same bounded preset for preflight and translation as the frame
    // benchmark. Pavilion's runner explicitly selects the existing large preset;
    // increasing only image bytes would still leave mesh/dimension limits wrong.
    const auto limits = resource_limits(o.resource_profile).pbrt;
    auto checked = pbrt::check(o.scene, limits);
    need(checked.exit_code() == 0 && checked.scene.has_value(), pbrt::report(checked));
    for (const auto &[name, type] : {std::pair{"Sampler", "independent"}, std::pair{"Film", "rgb"}})
        if (std::ranges::none_of(checked.scene->options,
                                 [&](const auto &d) { return d.authored.name == name; })) {
            pbrt::Descriptor d;
            d.authored.name = name;
            pbrt::Value v;
            v.kind = pbrt::ValueKind::string;
            v.text = type;
            d.authored.arguments.push_back(std::move(v));
            checked.scene->options.push_back(std::move(d));
        }
    for (auto &d : checked.scene->options) {
        if (d.authored.name == "Integrator" && d.authored.arguments.front().text == "bdpt")
            d.authored.arguments.front().text = "path";
        else if (d.authored.name == "Sampler") {
            d.authored.arguments.front().text = "independent";
            d.authored.parameters.clear();
            d.inherited.clear();
            d.effective_authored.clear();
            parameter(d, "pixelsamples", 1);
        } else if (d.authored.name == "Film") {
            parameter(d, "xresolution", std::int32_t(o.width));
            parameter(d, "yresolution", std::int32_t(o.height));
            parameter(d, "maxcomponentvalue", {});
        }
    }
    auto translated =
        pbrt::translate(checked, {o.scene.string()}, limits, SpectralTransportSelection::hero);
    for (const auto &d : translated.diagnostics)
        std::cerr << pbrt::format_diagnostic(d);
    need(translated.exit_code() == 0 && translated.scene.has_value(), "PBRT translation failed");
    return std::move(*translated.scene);
}
struct Difference {
    std::uint64_t count = 0, nonfinite = 0;
    std::array<std::uint64_t, 3> channels{};
    std::optional<std::size_t> pixel;
    unsigned channel = 0;
    std::array<std::uint32_t, 2> bits{};
    double maximum = 0;
};
Difference compare(const FrameCapture &a, const FrameCapture &b) {
    need(a.status == GeometryStatus::success && b.status == GeometryStatus::success,
         "Raw capture failed: " + a.diagnostic + " " + b.diagnostic);
    need(a.width == b.width && a.height == b.height && a.raw_xyz.size() == b.raw_xyz.size() &&
             a.raw_xyz.size() == std::uint64_t(a.width) * a.height,
         "Raw capture dimensions differ");
    Difference d;
    for (std::size_t pixel = 0; pixel < a.raw_xyz.size(); ++pixel)
        for (unsigned c = 0; c < 3; ++c) {
            const float x = a.raw_xyz[pixel][c], y = b.raw_xyz[pixel][c];
            const auto xb = std::bit_cast<std::uint32_t>(x), yb = std::bit_cast<std::uint32_t>(y);
            const bool nonfinite = !std::isfinite(x) || !std::isfinite(y);
            if (nonfinite)
                ++d.nonfinite;
            else
                d.maximum = std::max(d.maximum, std::abs(double(x) - double(y)));
            if (xb != yb) {
                ++d.count;
                ++d.channels[c];
            }
            if ((xb != yb || nonfinite) && !d.pixel) {
                d.pixel = pixel;
                d.channel = c;
                d.bits = {xb, yb};
            }
        }
    return d;
}
void accounting(const FrameInfo &i, const FrameConfig &c, unsigned ordinal) {
    const auto pixels = std::uint64_t(c.width) * c.height;
    const bool history = ordinal != 0;
    need(i.status == FrameStatus::complete, i.diagnostic);
    need(i.width == c.width && i.height == c.height && i.sample_index == ordinal && i.epoch == 0 &&
             i.area_serial == ordinal && i.history_generation == std::uint64_t(ordinal) + 1 &&
             i.history_used == history && i.mutation_policy == c.mutation_policy &&
             i.area_stencil == c.area_stencil && i.completed_samples == pixels &&
             i.fresh_candidates == pixels &&
             i.temporal_evaluation_cache_used == (history && c.cache_temporal_evaluations) &&
             valid_area_evaluation_accounting(
                 pixels, history, c.mutation_steps, i.temporal_evaluation_cache_used,
                 i.completed_evaluations, i.cached_evaluations, i.logical_evaluations) &&
             i.mutation_attempts == pixels * c.mutation_steps &&
             i.mutation_accepts <= i.mutation_attempts,
         "Shared-scene frame identity/history/cache accounting failed");
}
void info(std::ostream &out, const FrameInfo &i) {
    out << "{\"serial\":" << i.area_serial << ",\"generation\":" << i.history_generation
        << ",\"history\":" << (i.history_used ? "true" : "false")
        << ",\"cache\":" << (i.temporal_evaluation_cache_used ? "true" : "false")
        << ",\"physical_evaluations\":" << i.completed_evaluations
        << ",\"cached_evaluations\":" << i.cached_evaluations
        << ",\"logical_evaluations\":" << i.logical_evaluations
        << ",\"mutation_attempts\":" << i.mutation_attempts
        << ",\"mutation_accepts\":" << i.mutation_accepts << '}';
}
int run(const Options &o) {
    std::ofstream file;
    if (!o.output.empty()) {
        need(!fs::exists(o.output) && !fs::is_symlink(o.output), "Output must be fresh");
        file.open(o.output);
        need(bool(file), "Cannot open diagnostic output");
    }
    std::ostream &out = file.is_open() ? static_cast<std::ostream &>(file) : std::cout;
    out << std::setprecision(17);
    std::string error;
    bool shared = false, independent = false, exact = false;
    unsigned completed = 0;
    out << "{\"schema\":1,\"space\":\"raw XYZ float32\",\"scope\":\"same-AS raw XYZ correctness "
           "diagnostic; no performance "
           "claim\",\"pair\":"
        << json_string(o.off_off ? "off-off" : "off-on")
        << ",\"scene\":" << json_string(o.scene.empty() ? "builtin-diffuse" : o.scene.string())
        << ",\"build\":" << json_string(RYPATH_BUILD_DESCRIPTION)
        << ",\"library_argument\":" << json_string(o.library.string()) << ",\"width\":" << o.width
        << ",\"height\":" << o.height << ",\"seed\":" << o.seed
        << ",\"material_seed\":" << o.material_seed << ",\"frames_requested\":" << o.frames
        << ",\"raw_readback_bytes_per_pair\":"
        << 2ull * o.width * o.height * sizeof(std::array<float, 3>)
        << ",\"submission_order\":\"off then other; both complete before readback and next frame\""
        << ",\"warmup\":" << o.warmup << ",\"frames_include_warmup\":true,\"intersections\":"
        << json_string(o.intersections == IntersectionProfile::native ? "native" : "compensated")
        << ",\"resource_profile\":" << json_string(resource_profile_name(o.resource_profile));
    int code = 1;
    bool rows_open = false;
    try {
        const auto device = query_metal_device();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
        if (device.status == MetalStatus::no_device) {
            code = 77;
            throw std::runtime_error("No Metal device");
        }
#endif
        need(device.status == MetalStatus::success, device.diagnostic);
#ifdef RYPATH_ALLOW_NO_METAL_RAY_TRACING
        if (!device.device.compute_ray_tracing) {
            code = 77;
            throw std::runtime_error("No compute ray tracing capability");
        }
#endif
        need(device.device.compute_ray_tracing, "Shared-AS test requires compute ray tracing");
        auto scene = load(o);
        scene.integrator = scene.volume_transport ? IntegratorKind::volpath : IntegratorKind::path;
        scene.max_component_value = std::numeric_limits<double>::infinity();
        std::string spectral;
        need(select_spectral_transport(scene, SpectralTransportSelection::hero, &spectral).empty(),
             "Scene cannot select hero transport");
        scene.hero_wavelengths = HeroWavelengthSampling::visible;
        need(validate_render_scene(scene).empty(), "Invalid shared-AS scene");
        RenderSettings identity;
        identity.width = o.width;
        identity.height = o.height;
        identity.seed = o.seed;
        identity.material_seed = o.material_seed;
        identity.samples = 1;
        identity.sampler.kind = SamplerKind::independent;
        out << ",\"source_identity\":" << json_string(digest_hex(render_identity(scene, identity)))
            << ",\"authored_depth\":" << scene.max_depth
            << ",\"device\":" << json_string(device.device.name)
            << ",\"settings\":{\"resource_profile\":"
            << json_string(resource_profile_name(o.resource_profile))
            << ",\"transport\":\"hero\",\"hero_wavelengths\":\"visible\","
               "\"estimator\":\"area-replay\",\"mutations\":1,\"mutation_policy\":\"spectral-"
               "refresh\","
               "\"stencil\":\"reciprocal16\",\"area_output\":\"conditional-mean\","
               "\"target_scale\":1,\"footprint_samples\":1,\"shadow_traversal\":\"query\","
               "\"packet_layout\":\"packed\",\"packet_events\":1,\"requested_capacity\":65536,"
               "\"effective_capacity\":"
            << std::min<std::uint64_t>(65536, std::uint64_t(o.width) * o.height)
            << ",\"quantum\":8,\"group_width\":32,\"reconstruction\":\"none\"}";
        FrameConfig a;
        a.width = o.width;
        a.height = o.height;
        a.seed = o.seed;
        a.material_seed = o.material_seed;
        a.intersection_profile = o.intersections;
        a.estimator = FrameEstimator::area_replay;
        a.mutation_policy = FrameMutationPolicy::spectral_refresh;
        a.area_stencil = FrameAreaStencil::reciprocal16;
        a.cache_temporal_evaluations = false;
        auto b = a;
        b.cache_temporal_evaluations = !o.off_off;
        auto different = b;
        ++different.seed;
        const auto rejected =
            FrameSessionTestAccess::build_shared_scene(scene, a, different, o.library);
        need(rejected.status == GeometryStatus::failure && !rejected.sessions[0] &&
                 !rejected.sessions[1],
             "Shared factory accepted a non-cache configuration change");
        auto invalid = a;
        invalid.capacity = 0;
        const auto validation =
            FrameSessionTestAccess::build_shared_scene(scene, invalid, invalid, o.library);
        need(validation.status == GeometryStatus::failure && !validation.sessions[0],
             "Shared factory bypassed ordinary configuration validation");
        auto pair = FrameSessionTestAccess::build_shared_scene(scene, a, b, o.library);
        need(pair.status == GeometryStatus::success && pair.sessions[0] && pair.sessions[1],
             pair.diagnostic);
        shared = pair.shared_renderer;
        independent = pair.independent_history;
        need(shared && independent,
             "Sessions need one renderer/AS owner and independent histories");
        out << ",\"frames\":[";
        rows_open = true;
        for (unsigned ordinal = 0; ordinal < o.frames; ++ordinal) {
            auto first = pair.sessions[0]->submit({.sample_index = ordinal, .epoch = 0});
            need(first.status == GeometryStatus::success && first.frame, first.diagnostic);
            const auto ai = first.frame->wait();
            accounting(ai, a, ordinal);
            auto second = pair.sessions[1]->submit({.sample_index = ordinal, .epoch = 0});
            need(second.status == GeometryStatus::success && second.frame, second.diagnostic);
            const auto bi = second.frame->wait();
            accounting(bi, b, ordinal);
            need(ai.area_stencil_offsets == bi.area_stencil_offsets &&
                     ai.logical_evaluations == bi.logical_evaluations,
                 "Paired selection identities or logical work differ");
            // Retain both frames until readbacks complete. Only this pair is
            // resident on the CPU; no EXR or per-frame image files are written.
            const auto x = first.frame->capture();
            const auto y = second.frame->capture();
            const auto d = compare(x, y);
            if (o.scene.empty())
                need(std::ranges::any_of(x.raw_xyz, [](const auto &xyz) { return xyz[1] > 0; }),
                     "Built-in diffuse/direct fixture must produce positive radiance");
            if (ordinal)
                out << ',';
            out << "{\"index\":" << ordinal
                << ",\"warmup\":" << (ordinal < o.warmup ? "true" : "false") << ",\"off\":";
            info(out, ai);
            out << ",\"other\":";
            info(out, bi);
            out << ",\"differing_floats\":" << d.count << ",\"nonfinite_floats\":" << d.nonfinite
                << ",\"differing_XYZ\":[" << d.channels[0] << ',' << d.channels[1] << ','
                << d.channels[2] << ']' << ",\"maximum_absolute_difference\":" << d.maximum
                << ",\"first_difference\":";
            if (d.pixel)
                out << "{\"pixel\":" << *d.pixel << ",\"x\":" << *d.pixel % o.width
                    << ",\"y\":" << *d.pixel / o.width << ",\"channel\":" << d.channel
                    << ",\"off_bits\":" << d.bits[0] << ",\"other_bits\":" << d.bits[1] << '}';
            else
                out << "null";
            out << '}';
            out.flush();
            ++completed;
            if (d.count || d.nonfinite) {
                std::ostringstream diagnostic;
                diagnostic << "Raw XYZ differs at frame " << ordinal << ", pixel " << *d.pixel
                           << ", channel " << d.channel << ", maximum absolute difference "
                           << std::setprecision(17) << d.maximum;
                throw std::runtime_error(diagnostic.str());
            }
            need(ai.mutation_accepts == bi.mutation_accepts,
                 "Mutation decisions differ despite matching raw output");
        }
        exact = true;
        code = 0;
    } catch (const std::exception &e) {
        error = e.what();
    }
    if (rows_open)
        out << ']';
    out << ",\"shared_renderer_and_AS_owner\":" << (shared ? "true" : "false")
        << ",\"independent_history_banks\":" << (independent ? "true" : "false")
        << ",\"history_payloads_compared\":false"
        << ",\"compared_frames\":" << completed
        << ",\"complete\":" << (exact && completed == o.frames ? "true" : "false")
        << ",\"all_exact\":" << (exact ? "true" : "false") << ",\"exit_code\":" << code
        << ",\"diagnostic\":" << json_string(error) << "}\n";
    out.flush();
    need(bool(out), "Diagnostic output write failed");
    if (!error.empty())
        std::cerr << error << '\n';
    return code;
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << usage;
            return 0;
        }
        return run(parse(argc, argv));
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n' << usage;
        return 1;
    }
}
