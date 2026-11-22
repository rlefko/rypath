// SPDX-License-Identifier: GPL-3.0-only
#include "bsdf_table.hpp"
#include "metal_render.hpp"
#include "render_identity.hpp"

#include <bit>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
using namespace rypath;
void require(bool ok, std::string_view message) {
    if (!ok)
        throw std::runtime_error(std::string(message));
}

void identity_checks() {
    RenderScene scene;
    RenderSettings settings;
    const auto baseline = render_identity(scene, settings);
    // Independently serialized with Python struct '<I', '<Q', '<f', hashlib;
    // the 3766-byte empty-scene fixture includes field tags and all defaults.
    require(digest_hex(baseline) ==
                "a88e9d4b30d6c7e0fbbb9e1bdcc0cb9f80f00a85619a376d1e1fa35b22e064ce",
            "canonical empty-scene identity golden");
    require(baseline == render_identity(scene, settings), "deterministic identity");
    {
        // Independently serialized fixture, retaining nested counts, shared
        // content and exact nonfinite/-zero bits across the8192-byte buffer.
        RenderScene populated;
        auto image = std::make_shared<ImagePyramid>();
        image->encoding = ImageEncoding{};
        image->wrap = ImageWrap{};
        image->levels = {{65, 65, 0}};
        image->texels.resize(4097);
        for (std::size_t i = 0; i < image->texels.size(); ++i)
            image->texels[i] = static_cast<std::uint32_t>(i * 1664525 + 1013904223);
        auto transfer = std::make_shared<std::array<float, 256>>();
        for (std::size_t i = 0; i < transfer->size(); ++i)
            (*transfer)[i] = std::bit_cast<float>(static_cast<std::uint32_t>(i * 1234567));
        (*transfer)[0] = std::bit_cast<float>(std::uint32_t{0x80000000});
        (*transfer)[1] = std::bit_cast<float>(std::uint32_t{0x7fc12345});
        (*transfer)[2] = std::bit_cast<float>(std::uint32_t{0x7f800000});
        image->custom_transfer = transfer;
        populated.image_textures.resize(3);
        for (auto &texture : populated.image_textures)
            texture.image = image;
        populated.geometry.meshes.resize(1);
        auto &positions = populated.geometry.meshes[0].positions;
        positions.resize(257);
        for (std::size_t i = 0; i < positions.size(); ++i)
            positions[i] = {static_cast<float>(i), -static_cast<float>(i),
                            std::bit_cast<float>(std::uint32_t{0x80000000})};
        const auto expected = render_identity(populated, settings);
        require(digest_hex(expected) ==
                    "70aff95f7f052128f5958479cc1825a0a59f32133b0f0a7242a993d18bb3c9ba",
                "canonical populated identity golden");
        populated.image_textures[1].image = std::make_shared<ImagePyramid>(*image);
        require(render_identity(populated, settings) == expected,
                "canonical content independent of sharing topology");
        (*transfer)[0] = 0;
        require(render_identity(populated, settings) != expected,
                "canonical primitive bulk stream preserves negative zero");
    }
    scene.exposure = 42;
    settings.batch_size = 19;
    require(baseline == render_identity(scene, settings), "presentation and scheduling excluded");
    auto changed = [&](auto mutate, std::string_view name) {
        auto copy = scene;
        mutate(copy);
        require(render_identity(copy, settings) != baseline, name);
    };
    changed([](auto &s) { s.integrator = IntegratorKind::bdpt; }, "integrator identity");
    changed([](auto &s) { s.light_sampler = LightSamplerKind::power; },
            "power light selection identity");
    changed([](auto &s) { s.light_sampler = LightSamplerKind::uniform; },
            "uniform light selection identity");
    changed([](auto &s) { s.spectral_transport = SpectralTransport::packet; },
            "spectral transport identity");
    changed([](auto &s) { s.max_component_value = 50; }, "film clamp identity");
    auto clamped = scene;
    clamped.spectral_transport = SpectralTransport::packet;
    clamped.max_component_value = 50;
    const auto clamped_identity = render_identity(clamped, settings);
    clamped.exposure *= 2;
    require(render_identity(clamped, settings) != clamped_identity,
            "clamped sensor exposure identity");
    auto resource = scene;
    resource.image_limits.max_dimension = 8192;
    resource.triangle_frame_limit = 128'000'000;
    resource.preparation_byte_limit = 1024;
    require(render_identity(resource, settings) == baseline, "resource acceptance policy excluded");
    changed([](auto &s) { s.geometry.meshes.push_back({}); }, "geometry mesh count");
    changed([](auto &s) { s.geometry.instances.push_back({}); }, "geometry instance count");
    changed([](auto &s) { s.materials.push_back({}); }, "materials");
    changed([](auto &s) { s.instance_materials.push_back(0); }, "material bindings");
    changed([](auto &s) { s.camera.origin.x += 1; }, "camera");
    changed([](auto &s) { s.model = RenderModel::pbrt; }, "render model");
    changed([](auto &s) { s.raster_camera = RasterCamera{}; }, "raster camera presence");
    changed([](auto &s) { s.perspective_lens = PerspectiveLens{}; }, "lens presence");
    RenderScene lens_scene;
    lens_scene.perspective_lens = PerspectiveLens{.25f, 4, {1, 0, 0}, {0, 1, 0}};
    const auto lens_identity = render_identity(lens_scene, settings);
    const auto lens_changed = [&](auto change) {
        auto copy = lens_scene;
        change(*copy.perspective_lens);
        require(render_identity(copy, settings) != lens_identity, "lens identity payload");
    };
    lens_changed([](auto &lens) { lens.radius += .1f; });
    lens_changed([](auto &lens) { lens.focal_distance += 1; });
    for (auto axis : {&PerspectiveLens::camera_x, &PerspectiveLens::camera_y})
        for (auto component : {&GeometryPoint::x, &GeometryPoint::y, &GeometryPoint::z})
            lens_changed([=](auto &lens) { (lens.*axis).*component += .1f; });

    changed([](auto &s) { s.environment[470] = 1; }, "environment spectrum");
    changed([](auto &s) { ++s.max_depth; }, "integrator depth");
    changed([](auto &s) { s.instance_reverse_orientation.push_back(1); }, "orientation");
    changed([](auto &s) { s.area_emissions.push_back({}); }, "area emission");
    changed([](auto &s) { s.point_lights.push_back({}); }, "point light count");
    changed([](auto &s) { s.spot_lights.push_back({}); }, "spot light count");
    changed([](auto &s) { s.goniometric_lights.push_back({}); }, "goniometric light count");
    changed([](auto &s) { s.projection_lights.push_back({}); }, "projection light count");
    changed([](auto &s) { s.distant_lights.push_back({}); }, "distant light count");
    changed([](auto &s) { s.distant_scene_radius = 4; }, "distant scene radius");
    changed([](auto &s) { s.instance_emissions.push_back(0); }, "emission bindings");
    changed([](auto &s) { s.filter.kind = PixelFilterKind::triangle; }, "pixel filter kind");
    changed([](auto &s) { s.filter.x_radius += 1; }, "pixel filter x radius");
    changed([](auto &s) { s.filter.y_radius += 1; }, "pixel filter y radius");
    changed([](auto &s) { s.image_textures.push_back({}); }, "texture list");
    changed([](auto &s) { s.texture_camera.world_from_camera[8] = 2; }, "texture camera");
    changed([](auto &s) { s.instance_texture_frames.push_back({}); }, "texture frames");
    changed([](auto &s) { s.texture_graph.nodes.push_back({}); }, "texture graph nodes");
    changed([](auto &s) { s.texture_graph.knots.push_back({}); }, "texture graph spectra");
    changed([](auto &s) { s.instance_alpha_roots.push_back(0); }, "alpha");
    changed([](auto &s) { s.instance_invisible_emitters.push_back(1); }, "invisible emitters");
    changed([](auto &s) { s.environment_image = 0; }, "environment image");
    changed([](auto &s) { s.environment_image_scale += 1; }, "environment image scale");
    changed([](auto &s) { s.environment_texture_from_world.values[0] += 1; },
            "environment transform");
    changed([](auto &s) { s.instance_surface_transforms.push_back({}); }, "surface transforms");
    changed([](auto &s) { s.volume_transport = true; }, "volume integrator");
    changed([](auto &s) { s.media.push_back({}); }, "media");
    changed([](auto &s) { s.camera_medium = 0; }, "camera medium");
    changed([](auto &s) { s.instance_media.push_back({}); }, "medium interfaces");
    auto setting_changed = [&](auto mutate, std::string_view name) {
        auto copy = settings;
        mutate(copy);
        require(render_identity(scene, copy) != baseline, name);
    };
    setting_changed([](auto &s) { ++s.width; }, "film width");
    setting_changed([](auto &s) { ++s.height; }, "film height");
    setting_changed([](auto &s) { ++s.samples; }, "fixed target spp");
    setting_changed([](auto &s) { ++s.seed; }, "sampling seed");
    setting_changed([](auto &s) { s.material_seed = INT32_MIN; },
                    "negative stochastic material seed");
    setting_changed([](auto &s) { s.material_seed = INT32_MAX; },
                    "positive stochastic material seed");
    setting_changed([](auto &s) { s.sampler.kind = SamplerKind::sobol; }, "sampler");
    setting_changed([](auto &s) { s.sampler.randomization = SobolRandomization::none; },
                    "randomizer");

    RenderScene spots;
    spots.spot_lights.resize(2);
    spots.spot_lights[1].position.x = 1;
    const auto spot_identity = render_identity(spots, settings);
    const auto spot_changed = [&](auto change) {
        auto copy = spots;
        change(copy.spot_lights);
        require(render_identity(copy, settings) != spot_identity, "spot identity payload");
    };
    spot_changed([](auto &v) { std::swap(v[0], v[1]); });
    for (auto field : {&GeometryPoint::x, &GeometryPoint::y, &GeometryPoint::z})
        spot_changed([=](auto &v) { v[0].position.*field += 1; });
    for (std::size_t i = 0; i < 471; ++i)
        spot_changed([=](auto &v) { v[0].intensity[i] += 1; });
    for (std::size_t i = 0; i < 9; ++i)
        spot_changed([=](auto &v) { v[0].light_from_world[i] += 1; });
    spot_changed([](auto &v) { v[0].cos_falloff_start -= .1f; });
    spot_changed([](auto &v) { v[0].cos_falloff_end -= .1f; });

    RenderScene goniometric;
    goniometric.goniometric_lights.resize(2);
    goniometric.goniometric_lights[1].position.x = 1;
    auto scalar = std::make_shared<ImagePyramid>();
    scalar->encoding = ImageEncoding::linear;
    scalar->wrap = ImageWrap::clamp;
    scalar->source_channels = 1;
    scalar->native_base = true;
    scalar->levels = {{1, 1, 0}};
    scalar->texels = {0x00404040};
    goniometric.image_textures.push_back({scalar});
    goniometric.goniometric_lights[0].image_texture = 0;
    const auto goniometric_identity = render_identity(goniometric, settings);
    const auto goniometric_changed = [&](auto change) {
        auto copy = goniometric;
        change(copy);
        require(render_identity(copy, settings) != goniometric_identity,
                "goniometric identity payload");
    };
    goniometric_changed([](auto &s) { s.goniometric_lights.pop_back(); });
    goniometric_changed(
        [](auto &s) { std::swap(s.goniometric_lights[0], s.goniometric_lights[1]); });
    for (std::size_t light = 0; light < 2; ++light) {
        for (auto field : {&GeometryPoint::x, &GeometryPoint::y, &GeometryPoint::z})
            goniometric_changed([=](auto &s) { s.goniometric_lights[light].position.*field += 1; });
        for (std::size_t i = 0; i < 471; ++i)
            goniometric_changed([=](auto &s) { s.goniometric_lights[light].intensity[i] += 1; });
        for (std::size_t i = 0; i < 9; ++i)
            goniometric_changed(
                [=](auto &s) { s.goniometric_lights[light].light_from_world[i] += 1; });
        goniometric_changed([=](auto &s) { s.goniometric_lights[light].image_texture ^= 1; });
    }
    goniometric_changed([](auto &s) {
        auto copy = std::make_shared<ImagePyramid>(*s.image_textures[0].image);
        copy->texels[0] ^= 1;
        s.image_textures[0].image = copy;
    });

    RenderScene projection;
    projection.projection_lights.resize(2);
    projection.projection_lights[1].position.x = 1;
    auto projected_image = std::make_shared<ImagePyramid>(*scalar);
    projected_image->source_channels = 3;
    projection.image_textures.push_back({projected_image});
    projection.projection_lights[0].image_texture = 0;
    const auto projection_identity = render_identity(projection, settings);
    const auto projection_changed = [&](auto change) {
        auto copy = projection;
        change(copy);
        require(render_identity(copy, settings) != projection_identity,
                "projection identity payload");
    };
    projection_changed([](auto &s) { s.projection_lights.pop_back(); });
    projection_changed([](auto &s) { std::swap(s.projection_lights[0], s.projection_lights[1]); });
    for (std::size_t light = 0; light < 2; ++light) {
        for (auto field : {&GeometryPoint::x, &GeometryPoint::y, &GeometryPoint::z})
            projection_changed([=](auto &s) { s.projection_lights[light].position.*field += 1; });
        for (std::size_t i = 0; i < 9; ++i)
            projection_changed(
                [=](auto &s) { s.projection_lights[light].light_from_world[i] += 1; });
        projection_changed([=](auto &s) { s.projection_lights[light].image_texture ^= 1; });
        for (auto field :
             {&ProjectionLight::image_scale, &ProjectionLight::inv_tan_half_fov,
              &ProjectionLight::screen_half_width, &ProjectionLight::screen_half_height})
            projection_changed([=](auto &s) { s.projection_lights[light].*field += 1; });
    }
    projection_changed([](auto &s) {
        auto copy = std::make_shared<ImagePyramid>(*s.image_textures[0].image);
        copy->texels[0] ^= 1;
        s.image_textures[0].image = copy;
    });

    RenderScene distant;
    distant.distant_scene_radius = 4;
    distant.distant_lights.resize(2);
    distant.distant_lights[0].direction = {0, 0, -1};
    distant.distant_lights[1].direction = {0, 1, 0};
    const auto distant_identity = render_identity(distant, settings);
    const auto distant_changed = [&](auto change, std::string_view name) {
        auto copy = distant;
        change(copy);
        require(render_identity(copy, settings) != distant_identity, name);
    };
    distant_changed([](auto &s) { s.distant_lights.pop_back(); }, "populated distant count");
    distant_changed([](auto &s) { std::swap(s.distant_lights[0], s.distant_lights[1]); },
                    "distant list order");
    distant_changed([](auto &s) { s.distant_scene_radius = 8; }, "populated distant radius");
    for (std::size_t light = 0; light < distant.distant_lights.size(); ++light) {
        for (auto coordinate : {&GeometryPoint::x, &GeometryPoint::y, &GeometryPoint::z})
            distant_changed([=](auto &s) { s.distant_lights[light].direction.*coordinate += .25f; },
                            "distant direction component");
        for (std::size_t wavelength = 0; wavelength < 471; ++wavelength)
            distant_changed([=](auto &s) { s.distant_lights[light].radiance[wavelength] = 1; },
                            "distant spectral radiance bin");
    }

    // Populate every deep asset family, then change leaf payloads without
    // changing vector sizes. Hashing only descriptors or addresses fails these.
    scene.geometry.meshes.push_back(
        {{{1, 2, 3}}, {{0, 0, 0}}, {{0, 1, 0}}, {{0, 0}}, {{0, 0, 0, 0}}, {{1, 0, 0}}});
    scene.geometry.instances.push_back({});
    scene.materials.push_back({});
    scene.materials[0].knots.push_back({400, .5f});
    scene.materials[0].eta.knots.push_back({400, 1.5f});
    scene.materials[0].surface.interface_eta.knots.push_back({400, 1.2f});
    auto table = std::make_shared<BsdfTable>();
    table->kind = BsdfTableKind::fourier;
    table->words = {1, 2};
    scene.materials[0].table = table;
    auto image = std::make_shared<ImagePyramid>();
    image->encoding = ImageEncoding::gamma;
    image->wrap = ImageWrap::clamp;
    image->levels = {{1, 1, 0}};
    image->texels = {0x12345678};
    image->custom_transfer = std::make_shared<std::array<float, 256>>();
    scene.image_textures.push_back({image});
    scene.instance_texture_frames = {{{{1, 0, 0}, {0, 1, 0}}}};
    scene.texture_graph.nodes.push_back({});
    scene.media.push_back({});
    scene.instance_surface_transforms.push_back({});
    scene.area_emissions.push_back({});
    scene.point_lights = {{{1, 2, 3}, {}}, {{4, 5, 6}, {}}};
    const auto populated = render_identity(scene, settings);
    auto deep = [&](auto mutate, std::string_view name) {
        auto copy = scene;
        mutate(copy);
        require(render_identity(copy, settings) != populated, name);
    };
    deep([](auto &s) { s.geometry.meshes.front().analytics.push_back({}); },
         "analytic geometry identity");
    auto analytic = scene;
    analytic.geometry.meshes.front().analytics.push_back({});
    const auto analytic_identity = render_identity(analytic, settings);
    for (unsigned field = 0; field < 7; ++field) {
        auto copy = analytic;
        auto &shape = copy.geometry.meshes.front().analytics.front();
        switch (field) {
        case 0:
            shape.kind = AnalyticKind::disk;
            break;
        case 1:
            shape.radius = 2;
            break;
        case 2:
            shape.z_min = -.5f;
            break;
        case 3:
            shape.z_max = .5f;
            break;
        case 4:
            shape.phi_max = 3;
            break;
        case 5:
            shape.inner_radius = .25f;
            break;
        case 6:
            shape.height = 1;
            break;
        default:
            throw std::logic_error("invalid analytic identity test field");
        }
        require(render_identity(copy, settings) != analytic_identity,
                "analytic shape parameter identity");
    }
    // Every point and wavelength contributes to identity, including dark bins.
    for (std::size_t light = 0; light < scene.point_lights.size(); ++light) {
        for (auto coordinate : {&GeometryPoint::x, &GeometryPoint::y, &GeometryPoint::z})
            deep([=](auto &s) { s.point_lights[light].position.*coordinate += 1; },
                 "point position component");
        for (std::size_t wavelength = 0; wavelength < scene.point_lights[light].intensity.size();
             ++wavelength)
            deep([=](auto &s) { s.point_lights[light].intensity[wavelength] = 1; },
                 "point spectral intensity bin");
    }
    deep([](auto &s) { s.point_lights.pop_back(); }, "populated point count");
    deep([](auto &s) { std::swap(s.point_lights[0], s.point_lights[1]); }, "point order");
    deep([](auto &s) { s.area_emissions[0].power = 3; }, "area power");
    deep([](auto &s) { s.area_emissions[0].gltf_material = 0; }, "glTF emitter binding");
    deep(
        [](auto &s) {
            auto i = std::make_shared<ImagePyramid>(*s.image_textures[0].image);
            i->gltf = true;
            s.image_textures[0].image = i;
        },
        "glTF image policy");
    deep([](auto &s) { s.geometry.meshes[0].positions[0].z += 1; }, "vertex payload");
    deep([](auto &s) { s.geometry.meshes[0].tangent_signs = {-1}; }, "tangent handedness");
    deep([](auto &s) { s.materials[0].gltf.base_color[0] = .4f; }, "glTF base color");
    deep([](auto &s) { s.materials[0].gltf.base_color[3] = .4f; }, "glTF opacity");
    deep([](auto &s) { s.materials[0].gltf.metallic = .3f; }, "glTF metallic");
    deep([](auto &s) { s.materials[0].gltf.roughness = .3f; }, "glTF roughness");
    deep([](auto &s) { s.materials[0].gltf.normal_scale = .3f; }, "glTF normal scale");
    deep([](auto &s) { s.materials[0].gltf.alpha_mode = 1; }, "glTF alpha mode");
    deep([](auto &s) { s.materials[0].gltf.alpha_cutoff = .3f; }, "glTF alpha cutoff");
    deep([](auto &s) { s.materials[0].gltf.double_sided = true; }, "glTF sidedness");
    deep([](auto &s) { s.materials[0].gltf.base_color_texture = 0; }, "glTF base image");
    deep([](auto &s) { s.materials[0].gltf.metallic_roughness_texture = 0; }, "glTF data image");
    deep([](auto &s) { s.materials[0].gltf.normal_texture = 0; }, "glTF normal image");
    deep([](auto &s) { s.materials[0].gltf.emissive_texture = 0; }, "glTF emissive image");
    deep([](auto &s) { s.materials[0].gltf.emissive[1] = .3f; }, "glTF emission");
    deep([](auto &s) { s.image_textures[0].gltf_sampler = GltfSampler{}; },
         "glTF sampler presence");
    for (auto field : {&GltfSampler::wrap_s, &GltfSampler::wrap_t, &GltfSampler::min_filter,
                       &GltfSampler::mag_filter}) {
        auto a = scene, b = scene;
        a.image_textures[0].gltf_sampler = GltfSampler{};
        GltfSampler sampler{};
        ++(sampler.*field);
        b.image_textures[0].gltf_sampler = sampler;
        require(render_identity(a, settings) != render_identity(b, settings),
                "glTF sampler payload");
    }

    deep([](auto &s) { s.geometry.meshes[0].normals[0].y += 1; }, "normal payload");
    deep([](auto &s) { s.geometry.meshes[0].uvs[0][1] += 1; }, "UV payload");
    deep([](auto &s) { ++s.geometry.meshes[0].triangles[0][2]; }, "triangle payload");
    deep([](auto &s) { ++s.geometry.meshes[0].patches[0][3]; }, "patch payload");
    deep([](auto &s) { s.geometry.meshes[0].tangents[0].x += 1; }, "tangent payload");
    deep([](auto &s) { s.geometry.instances[0].transform.values[3] += 1; }, "instance transform");
    deep([](auto &s) { s.geometry.instances[0].non_opaque = !s.geometry.instances[0].non_opaque; },
         "native triangle hint opacity eligibility");
    deep([](auto &s) { s.materials[0].knots[0].value += 1; }, "material spectrum");
    deep([](auto &s) { s.materials[0].eta.knots[0].value += 1; }, "optical spectrum");
    deep([](auto &s) { s.materials[0].surface.interface_eta.knots[0].value += 1; },
         "coating spectrum");
    deep([](auto &s) { s.materials[0].surface.mix_amount += 1; }, "extended material");
    deep(
        [](auto &s) {
            auto t = std::make_shared<BsdfTable>(*s.materials[0].table);
            ++t->words[1];
            s.materials[0].table = t;
        },
        "BSDF table payload");
    deep(
        [](auto &s) {
            auto i = std::make_shared<ImagePyramid>(*s.image_textures[0].image);
            ++i->texels[0];
            s.image_textures[0].image = i;
        },
        "mip texel payload");
    deep(
        [](auto &s) {
            auto i = std::make_shared<ImagePyramid>(*s.image_textures[0].image);
            auto transfer = std::make_shared<std::array<float, 256>>(*i->custom_transfer);
            (*transfer)[255] = 1;
            i->custom_transfer = transfer;
            s.image_textures[0].image = i;
        },
        "custom transfer payload");
    deep([](auto &s) { s.instance_texture_frames[0][0].dpdu.z = 1; }, "texture frame payload");
    deep([](auto &s) { s.texture_graph.nodes[0].mapping.texture_from_render[11] = 1; },
         "graph mapping");
    deep([](auto &s) { s.media[0].sigma_a[470] = 1; }, "medium spectrum");
    deep([](auto &s) { s.instance_surface_transforms[0].shape[15] = 2; }, "double transform");
    auto copy = scene;
    auto table_copy = std::make_shared<BsdfTable>(*table);
    table_copy->source_bytes = 999;
    table_copy->working_bytes = 888;
    copy.materials[0].table = table_copy;
    auto image_copy = std::make_shared<ImagePyramid>(*image);
    image_copy->custom_transfer = std::make_shared<std::array<float, 256>>(*image->custom_transfer);
    copy.image_textures[0].image = image_copy;
    require(render_identity(copy, settings) == populated, "asset values and accounting exclusion");
    scene.materials.push_back(scene.materials[0]);
    copy.materials.push_back(copy.materials[0]);
    copy.materials[1].table = std::make_shared<BsdfTable>(*table);
    require(render_identity(copy, settings) == render_identity(scene, settings),
            "sharing topology excluded");
    for (auto kind : {SamplerKind::sobol, SamplerKind::halton, SamplerKind::zsobol}) {
        settings.sampler.kind = kind;
        settings.seed = UINT64_MAX;
        auto equivalent = settings;
        equivalent.seed = UINT32_MAX;
        require(render_identity(scene, settings) == render_identity(scene, equivalent),
                "QMC signed seed canonicalization");
    }
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--shader-contract") {
            require(argc >= 4, "--shader-contract PATH mismatch|unavailable [availability flags]");
            const std::string_view expected = argv[3];
            require(expected == "mismatch" || expected == "unavailable",
                    "invalid contract expectation");
            bool allow_no_device = false;
            bool allow_no_ray_tracing = false;
            for (int i = 4; i < argc; ++i) {
                const std::string_view flag = argv[i];
                if (flag == "--allow-no-device")
                    allow_no_device = true;
                else if (flag == "--allow-no-ray-tracing")
                    allow_no_ray_tracing = true;
                else
                    require(false, "invalid availability flag");
            }
            // Empty geometry reaches the render-library handshake without
            // requiring any geometry/render entry points in these tiny fixtures.
            const auto built = build_metal_renderer({}, argv[2]);
            if ((built.status == GeometryStatus::no_device && allow_no_device) ||
                (built.status == GeometryStatus::no_ray_tracing && allow_no_ray_tracing)) {
                std::cout << "SKIP Metal shader contract. " << built.diagnostic << '\n';
                return 77;
            }
            require(built.status == GeometryStatus::failure && !built.scene,
                    "incompatible metallib must fail renderer build");
            const auto text = expected == "mismatch" ? "shader contracts mismatch"
                                                     : "shader compatibility handshake unavailable";
            require(built.diagnostic.find(text) != std::string::npos, built.diagnostic);
            require(built.diagnostic.find("rebuild the metallib") != std::string::npos,
                    "incompatible metallib must suggest rebuilding");
            std::cout << "Shader contract rejection passed: " << built.diagnostic << '\n';
            return 0;
        }
        require(argc == 1, "unexpected identity test arguments");
        identity_checks();
        std::cout << "Render identity checks passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
