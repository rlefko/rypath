// SPDX-License-Identifier: GPL-3.0-only
#include "diffuse_event_eligibility.hpp"
#include "frame_session_test_access.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace {
using namespace rypath;
void need(bool ok, const std::string &message) {
    if (!ok)
        throw std::runtime_error(message);
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
RenderScene cavity() {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.spectral_transport = SpectralTransport::hero;
    scene.max_depth = 3;
    scene.filter.kind = PixelFilterKind::box;
    scene.raster_camera = RasterCamera{{}, {-.05f, -.05f, 1}, {.1f, 0, 0}, {0, .1f, 0}};
    SpectralMaterial material;
    material.kind = SpectralMaterialKind::constant;
    material.constant = .5f;
    scene.materials.push_back(material);
    AreaEmission emission;
    emission.radiance.fill(.125f);
    emission.two_sided = true;
    scene.area_emissions.push_back(emission);
    for (unsigned axis = 0; axis < 3; ++axis)
        for (int sign : {-1, 1}) {
            GeometryMesh mesh;
            constexpr int corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (const auto &corner : corners) {
                std::array<float, 3> v{};
                v[axis] = float(sign) * 4;
                v[(axis + 1) % 3] = float(corner[0]) * 4;
                v[(axis + 2) % 3] = float(corner[1]) * 4;
                mesh.positions.push_back({v[0], v[1], v[2]});
            }
            mesh.triangles = sign < 0 ? std::vector<std::array<unsigned, 3>>{{0, 1, 2}, {0, 2, 3}}
                                      : std::vector<std::array<unsigned, 3>>{{0, 2, 1}, {0, 3, 2}};
            const auto index = unsigned(scene.geometry.meshes.size());
            scene.geometry.meshes.push_back(std::move(mesh));
            scene.geometry.instances.push_back({index, {}});
            scene.instance_materials.push_back(0);
            scene.instance_emissions.push_back(0);
            scene.instance_surface_transforms.emplace_back();
        }
    PointLight point;
    point.position = {.25f, -.375f, .125f};
    point.intensity.fill(8);
    scene.point_lights.push_back(point);
    need(validate_render_scene(scene).empty(), "Invalid diffuse cavity");
    return scene;
}
void representation(RenderScene &scene, SpectralMaterialKind kind) {
    auto &m = scene.materials[0];
    m = {};
    m.kind = kind;
    if (kind == SpectralMaterialKind::constant)
        m.constant = .6f;
    else if (kind == SpectralMaterialKind::sampled)
        m.knots = {{360, .2f}, {500, .7f}, {830, .4f}};
    else if (kind == SpectralMaterialKind::rgb_sigmoid)
        m.coefficients = {0, 0, 0};
}
void proof_controls() {
    auto scene = fixture();
    need(scene_proves_diffuse_events(scene), "Constant diffuse proof");
    for (auto kind : {SpectralMaterialKind::sampled, SpectralMaterialKind::rgb_sigmoid}) {
        auto s = scene;
        representation(s, kind);
        need(validate_render_scene(s).empty(), "Invalid positive spectral fixture");
        need(scene_proves_diffuse_events(s), "Supported diffuse spectral representation");
    }
    for (auto family :
         {ScatteringKind::conductor, ScatteringKind::dielectric, ScatteringKind::thin_dielectric,
          ScatteringKind::diffuse_transmission, ScatteringKind::coated_diffuse,
          ScatteringKind::coated_conductor, ScatteringKind::measured, ScatteringKind::fourier,
          ScatteringKind::mix, ScatteringKind::null_interface, ScatteringKind::gltf}) {
        auto s = scene;
        // Even an unused material must defeat the proof.
        auto m = s.materials[0];
        m.scattering = family;
        s.materials.push_back(m);
        need(!scene_proves_diffuse_events(s), "Wrong-family unused material admitted");
    }
    for (auto kind : {SpectralMaterialKind::image, SpectralMaterialKind::blackbody,
                      SpectralMaterialKind::texture_graph}) {
        auto s = scene;
        s.materials[0].kind = kind;
        need(!scene_proves_diffuse_events(s), "Excluded representation admitted");
    }
    for (unsigned control = 0; control < 12; ++control) {
        auto s = scene;
        switch (control) {
        case 0:
            s.materials[0].texture_root = 0;
            break;
        case 1:
            s.materials[0].bump_root = 0;
            break;
        case 2:
            s.materials[0].normal_map = 0;
            break;
        case 3:
            s.volume_transport = true;
            break;
        case 4:
            s.camera_medium = 0;
            break;
        case 5:
            s.instance_media.emplace_back();
            break;
        case 6:
            s.instance_alpha_roots.push_back(0);
            break;
        case 7:
            s.materials.clear();
            break;
        case 8:
            s.image_textures.emplace_back();
            break;
        case 9:
            s.texture_graph.nodes.emplace_back();
            break;
        case 10:
            s.media.emplace_back();
            break;
        case 11:
            s.environment_image = 0;
            break;
        }
        need(!scene_proves_diffuse_events(s), "Appearance/media proof negative admitted");
    }
    std::cout << "Diffuse scene-proof positive/negative controls passed\n";
}
void exact_capture(const FrameCapture &a, const FrameCapture &b) {
    need(a.status == GeometryStatus::success && b.status == GeometryStatus::success,
         "Capture failed: " + a.diagnostic + " " + b.diagnostic);
    need(a.width == b.width && a.height == b.height && a.raw_xyz.size() == b.raw_xyz.size(),
         "Capture population differs");
    for (std::size_t pixel = 0; pixel < a.raw_xyz.size(); ++pixel)
        for (unsigned channel = 0; channel < 3; ++channel) {
            const auto x = a.raw_xyz[pixel][channel], y = b.raw_xyz[pixel][channel];
            need(std::isfinite(x) && std::isfinite(y) &&
                     std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y),
                 "First raw mismatch pixel=" + std::to_string(pixel) +
                     " channel=" + std::to_string(channel) +
                     " generic_bits=" + std::to_string(std::bit_cast<std::uint32_t>(x)) +
                     " typed_bits=" + std::to_string(std::bit_cast<std::uint32_t>(y)));
        }
}
void selected_history(const EnhancedSessionSnapshot &a, const EnhancedSessionSnapshot &b) {
    need(a.status == GeometryStatus::success && b.status == GeometryStatus::success,
         "Enhanced snapshot failed: " + a.diagnostic + " " + b.diagnostic);
    need(a.history.size() == b.history.size() && a.front_bank == b.front_bank &&
             a.serial == b.serial && a.generation == b.generation,
         "History publication mismatch");
    const auto bits = [](float x) { return std::bit_cast<std::uint32_t>(x); };
    for (std::size_t pixel = 0; pixel < a.history.size(); ++pixel) {
        const auto &x = a.history[pixel], &y = b.history[pixel];
        need(x.valid == y.valid, "Selected validity mismatch");
        if (!x.valid)
            continue;
        need(x.selected_slot == y.selected_slot && x.confidence == y.confidence &&
                 x.record.bank == y.record.bank && x.record.pixel == y.record.pixel &&
                 bits(x.normalizer) == bits(y.normalizer) && bits(x.target) == bits(y.target) &&
                 bits(x.target_h_fraction) == bits(y.target_h_fraction) &&
                 x.target_h_exponent == y.target_h_exponent &&
                 x.normalizer_exponent == y.normalizer_exponent &&
                 x.target_exponent == y.target_exponent && bits(x.xyz.x) == bits(y.xyz.x) &&
                 bits(x.xyz.y) == bits(y.xyz.y) && bits(x.xyz.z) == bits(y.xyz.z),
             "Selected reservoir scalar mismatch");
        need(x.payload.version == y.payload.version &&
                 x.payload.wavelength == y.payload.wavelength &&
                 std::equal(std::begin(x.payload.tail_key), std::end(x.payload.tail_key),
                            std::begin(y.payload.tail_key)) &&
                 std::equal(std::begin(x.payload.film), std::end(x.payload.film),
                            std::begin(y.payload.film)) &&
                 std::equal(std::begin(x.payload.lens), std::end(x.payload.lens),
                            std::begin(y.payload.lens)),
             "Selected replay payload mismatch");
        if (enhanced_wire::valid(x.record, static_cast<std::uint32_t>(a.history.size()))) {
            const auto index = x.record.bank * a.history.size() + x.record.pixel;
            const auto &r = a.records.at(index), &t = b.records.at(index);
            need(r.version == t.version && r.tag == t.tag && r.light_id == t.light_id &&
                     r.event_ordinal == t.event_ordinal && r.instance == t.instance &&
                     r.primitive == t.primitive && r.kind == t.kind && r.reserved == t.reserved &&
                     std::equal(std::begin(r.coordinate_bits), std::end(r.coordinate_bits),
                                std::begin(t.coordinate_bits)),
                 "Selected owned light record mismatch");
        }
    }
}
void paired(RenderScene scene, FrameConfig config) {
    auto oracle = config;
    oracle.specialize_diffuse_events = false;
    auto pair = FrameSessionTestAccess::build_shared_scene(scene, oracle, config);
    need(pair.status == GeometryStatus::success && pair.sessions[0] && pair.sessions[1],
         pair.diagnostic);
    need(pair.shared_renderer, "Differential sessions must share renderer/AS");
    if (config.estimator == FrameEstimator::area_replay)
        need(pair.independent_history && pair.independent_owned_records &&
                 pair.independent_suffix_storage,
             "Enhanced arenas must be independent");
    for (unsigned sample = 0; sample < 5; ++sample) {
        auto a = pair.sessions[0]->submit({.sample_index = sample});
        need(a.status == GeometryStatus::success && a.frame, a.diagnostic);
        const auto ai = a.frame->wait();
        need(ai.status == FrameStatus::complete, ai.diagnostic);
        auto b = pair.sessions[1]->submit({.sample_index = sample});
        need(b.status == GeometryStatus::success && b.frame, b.diagnostic);
        const auto bi = b.frame->wait();
        need(bi.status == FrameStatus::complete, bi.diagnostic);
        need(!ai.diffuse_events && bi.diffuse_events, "Actual generic/typed dispatch not observed");
        need(ai.completed_samples == std::uint64_t(config.width) * config.height &&
                 bi.completed_samples == ai.completed_samples &&
                 ai.logical_evaluations == bi.logical_evaluations &&
                 ai.completed_evaluations == bi.completed_evaluations &&
                 ai.cached_evaluations == bi.cached_evaluations &&
                 ai.analytic_null_evaluations == bi.analytic_null_evaluations &&
                 ai.enhanced_suffix_hits == bi.enhanced_suffix_hits &&
                 ai.enhanced_suffix_misses == bi.enhanced_suffix_misses &&
                 ai.area_stencil_offsets == bi.area_stencil_offsets,
             "Differential accounting/suffix/selection mismatch");
        exact_capture(a.frame->capture(), b.frame->capture());
        if (config.estimator == FrameEstimator::area_replay)
            selected_history(FrameSessionTestAccess::snapshot_enhanced(*pair.sessions[0], sample),
                             FrameSessionTestAccess::snapshot_enhanced(*pair.sessions[1], sample));
    }
}
void gpu_controls() {
    FrameConfig c;
    c.specialize_diffuse_events = true;
    c.width = 7;
    c.height = 3;
    c.seed = 61002;
    // Capacity below image population exercises refill; one round/command exercises resume.
    c.capacity = 5;
    c.quantum = 1;
    for (auto kind : {SpectralMaterialKind::constant, SpectralMaterialKind::sampled,
                      SpectralMaterialKind::rgb_sigmoid}) {
        auto s = fixture();
        representation(s, kind);
        need(validate_render_scene(s).empty(), "Invalid production spectral control");
        paired(s, c);
        auto e = c;
        e.estimator = FrameEstimator::area_replay;
        e.area_path_domain = FrameAreaPathDomain::enhanced;
        e.contribution_replay = FrameContributionReplay::prefix;
        e.mutation_steps = 1;
        for (bool suffix : {false, true}) {
            e.enhanced_suffix_cache = suffix;
            paired(s, e);
        }
        e.contribution_replay = FrameContributionReplay::full;
        paired(s, e);
    }
    // Closed diffuse cavity continues to interior vertices and the finite-depth boundary.
    auto interior = cavity();
    paired(interior, c);
    auto interior_config = c;
    interior_config.estimator = FrameEstimator::area_replay;
    interior_config.area_path_domain = FrameAreaPathDomain::enhanced;
    interior_config.contribution_replay = FrameContributionReplay::prefix;
    paired(interior, interior_config);
    // Exact black exercises zero targets and initialized-complete/selected-null paths.
    auto black = fixture();
    black.materials[0].constant = 0;
    black.environment.fill(0);
    auto e = c;
    e.estimator = FrameEstimator::area_replay;
    e.area_path_domain = FrameAreaPathDomain::enhanced;
    e.contribution_replay = FrameContributionReplay::prefix;
    paired(black, e);
    // Config exclusions retain the actual generic entry on a proven diffuse scene.
    for (unsigned control = 0; control < 3; ++control) {
        auto fallback = c;
        if (control == 0)
            fallback.packet_events = 2;
        if (control == 1)
            fallback.packet_layout = FramePacketLayout::hot_cold;
        auto s = fixture();
        if (control == 2)
            s.spectral_transport = SpectralTransport::packet;
        auto b = build_frame_session(s, fallback);
        need(b.status == GeometryStatus::success && b.session, b.diagnostic);
        auto f = b.session->submit({});
        need(f.frame != nullptr, f.diagnostic);
        const auto i = f.frame->wait();
        need(i.status == FrameStatus::complete && !i.diffuse_events, "Config fallback dispatch");
    }
    std::cout << "Production diffuse shared-AS differential controls passed\n";
}
} // namespace
int main(int argc, char **argv) {
    try {
        proof_controls();
        if (argc == 2 && std::string_view(argv[1]) == "--proof-only")
            return 0;
        need(argc == 1, "Only --proof-only is accepted");
        gpu_controls();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
