// SPDX-License-Identifier: GPL-3.0-only

#include "metal_render.hpp"
#include "analytic.hpp"
#include "bsdf_table.hpp"
#include "checkpoint_contract.h"
#include "native_hint_budget.hpp"

#include "environment_sampling.hpp"
#include "gaussian_filter.hpp"
#include "light_sampling.hpp"
#include "metal_execution.hpp"
#include "metal_geometry_access.hpp"
#include "packet_spectrum_shared.h"
#include "packet_wire.h"
#include "render_wire.h"
#include "rgb_spectrum.hpp"
#include "sobol_shared.h"
#include "triangle_attributes.hpp"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <bit>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <numbers>
#include <ranges>
#include <tuple>
#include <type_traits>

namespace rypath {
namespace {

namespace wire = render_wire;

// Execute the contract in the actual loaded library. A matching entry-point
// name alone cannot establish compatibility with a stale/custom metallib.
bool verify_shader_contract(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLLibrary> library,
                            std::string &diagnostic) {
    auto pipeline =
        metal::load_pipeline(device, library, @"rypath_checkpoint_contract", diagnostic);
    if (pipeline == nil) {
        diagnostic =
            "Metal shader compatibility handshake unavailable; rebuild the metallib: " + diagnostic;
        return false;
    }
    constexpr std::array<std::uint32_t, 4> expected{
        checkpoint_contract::renderer, checkpoint_contract::sampling,
        checkpoint_contract::accumulation, checkpoint_contract::identity};
    auto buffer = [device newBufferWithLength:sizeof(expected)
                                      options:MTLResourceStorageModeShared];
    if (buffer == nil) {
        diagnostic = "Metal shader compatibility handshake buffer allocation failed";
        return false;
    }
    std::memset(buffer.contents, 0, sizeof(expected));
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    if (command == nil || encoder == nil) {
        diagnostic = "Metal shader compatibility handshake command creation failed";
        return false;
    }
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:buffer offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    diagnostic = metal::complete(command, "shader compatibility handshake");
    if (!diagnostic.empty())
        return false;
    std::array<std::uint32_t, 4> actual{};
    std::memcpy(actual.data(), buffer.contents, sizeof(actual));
    if (actual != expected) {
        diagnostic = "Metal shader contracts mismatch (renderer/sampling/accumulation/identity); "
                     "rebuild the metallib for this renderer";
        return false;
    }
    return true;
}

static_assert(std::is_trivially_copyable_v<wire::Parameters> &&
              std::is_trivially_copyable_v<wire::Sample>);
constexpr auto max_index = std::numeric_limits<std::uint32_t>::max();

bool gltf_visibility(const SpectralMaterial &m) {
    return m.scattering == ScatteringKind::gltf && (!m.gltf.double_sided || m.gltf.alpha_mode != 0);
}
bool filtered_visibility(const RenderScene &scene) {
    return std::ranges::any_of(scene.instance_alpha_roots,
                               [](auto root) { return root != max_index; }) ||
           std::ranges::any_of(scene.materials, gltf_visibility);
}
using Vec = std::array<double, 3>;

Vec cross(Vec a, Vec b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Vec normalize(Vec v) {
    const double length = std::hypot(v[0], v[1], v[2]);
    return {v[0] / length, v[1] / length, v[2] / length};
}

wire::Point pack(Vec v) {
    return {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
}

wire::Point pack(GeometryPoint point) { return {point.x, point.y, point.z}; }

GeometryPoint unpack(wire::Point point) { return {point.x, point.y, point.z}; }

wire::Parameters camera_parameters(const RenderScene &scene) {
    wire::Parameters result{};
    result.packet_transport = scene.spectral_transport == SpectralTransport::packet;
    result.bidirectional = scene.integrator == IntegratorKind::bdpt;
    result.packet_lane_bytes = packet_wire::lane_bytes(scene.max_depth);
    result.packet_filter_weight = 1;
    result.filter_kind = static_cast<wire::Word>(scene.filter.kind);
    result.filter_sigma = scene.filter.sigma;
    result.filter_x_radius = scene.filter.x_radius;
    result.filter_y_radius = scene.filter.y_radius;
    if (scene.model == RenderModel::pbrt) {
        const auto &camera = *scene.raster_camera;
        result.origin = pack(camera.origin);
        if (scene.perspective_lens) {
            const auto &lens = *scene.perspective_lens;
            result.lens_radius = lens.radius;
            result.focal_distance = lens.focal_distance;
            result.lens_x = pack(lens.camera_x);
            result.lens_y = pack(lens.camera_y);
        }
        // Reuse the demo camera slots for the PBRT world-space raster basis.
        result.forward = pack(camera.corner);
        result.right = pack(camera.dx);
        result.up = pack(camera.dy);
        result.model = wire::pbrt_model;
        result.max_depth = scene.max_depth;
        const Vec dx{camera.dx.x, camera.dx.y, camera.dx.z};
        const Vec dy{camera.dy.x, camera.dy.y, camera.dy.z};
        const Vec corner{camera.corner.x, camera.corner.y, camera.corner.z};
        const auto area_vector = cross(dx, dy);
        auto normal = normalize(area_vector);
        double depth = 0;
        for (unsigned axis = 0; axis < 3; ++axis)
            depth += normal[axis] * corner[axis];
        if (depth < 0) {
            for (auto &value : normal)
                value = -value;
            depth = -depth;
        }
        result.camera_normal = pack(normal);
        result.camera_plane_depth = static_cast<float>(depth);
        result.camera_image_area = static_cast<float>(
            std::hypot(area_vector[0], area_vector[1], area_vector[2]) / (depth * depth));
        return result;
    }
    const auto &camera = scene.camera;
    const auto forward = normalize({double{camera.target.x} - camera.origin.x,
                                    double{camera.target.y} - camera.origin.y,
                                    double{camera.target.z} - camera.origin.z});
    const auto right = normalize(cross(forward, {camera.up.x, camera.up.y, camera.up.z}));
    const auto up = cross(right, forward);
    result.origin = pack(camera.origin);
    result.forward = pack(forward);
    result.right = pack(right);
    result.up = pack(up);
    result.film_scale = static_cast<float>(
        std::tan(camera.vertical_fov_degrees * (std::numbers::pi_v<double> / 360.0)));
    std::ranges::copy(spectral_knots, result.knots);
    return result;
}

bool finite(wire::Point p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

// Bound unique geometry once, then transform eight corners per placement.
// The environment emission disk only needs a conservative scene enclosure.
std::string prepare_packet_scene_sphere(const RenderScene &scene, wire::Parameters &parameters) {
    using Bounds = std::array<Vec, 2>;
    const double infinity = std::numeric_limits<double>::infinity();
    const Bounds empty{{{infinity, infinity, infinity}, {-infinity, -infinity, -infinity}}};
    std::vector<Bounds> bounds(scene.geometry.meshes.size(), empty);
    const auto include = [](Bounds &box, Vec point) {
        for (unsigned axis = 0; axis < 3; ++axis) {
            box[0][axis] = std::min(box[0][axis], point[axis]);
            box[1][axis] = std::max(box[1][axis], point[axis]);
        }
    };
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        const auto &mesh = scene.geometry.meshes[i];
        for (const auto &p : mesh.positions)
            include(bounds[i], {p.x, p.y, p.z});
        for (const auto &shape : mesh.analytics) {
            const double low = shape.kind == AnalyticKind::disk ? shape.height : shape.z_min;
            const double high = shape.kind == AnalyticKind::disk ? shape.height : shape.z_max;
            include(bounds[i], {-shape.radius, -shape.radius, low});
            include(bounds[i], {shape.radius, shape.radius, high});
        }
    }
    Bounds world = empty;
    for (const auto &instance : scene.geometry.instances) {
        const auto &box = bounds[instance.mesh_index];
        if (box[0][0] > box[1][0])
            continue;
        const auto &m = instance.transform.values;
        for (unsigned corner = 0; corner < 8; ++corner) {
            const Vec p{box[corner & 1][0], box[(corner >> 1) & 1][1], box[(corner >> 2) & 1][2]};
            Vec q{};
            for (unsigned row = 0; row < 3; ++row) {
                q[row] = double(m[4 * row]) * p[0] + double(m[4 * row + 1]) * p[1] +
                         double(m[4 * row + 2]) * p[2] + m[4 * row + 3];
                if (!std::isfinite(q[row]) || std::abs(q[row]) > FLT_MAX)
                    return "Packet scene bounds exceed binary32 range";
            }
            include(world, q);
        }
    }
    if (world[0][0] > world[1][0]) {
        parameters.scene_center = {};
        parameters.scene_radius = 1;
        return {};
    }
    Vec center{}, extent{};
    for (unsigned axis = 0; axis < 3; ++axis) {
        center[axis] = static_cast<float>(world[0][axis] / 2 + world[1][axis] / 2);
        extent[axis] = std::max(std::abs(world[0][axis] - center[axis]),
                                std::abs(world[1][axis] - center[axis]));
        // Cover binary32 transformed-vertex and connection arithmetic as well.
        extent[axis] +=
            32 * FLT_EPSILON * std::max(std::abs(world[0][axis]), std::abs(world[1][axis]));
    }
    const double radius = std::hypot(extent[0], extent[1], extent[2]);
    if (!(radius > 0) || !std::isfinite(radius) || radius >= FLT_MAX)
        return "Packet scene sphere must have positive finite binary32 radius";
    parameters.scene_center = pack(center);
    parameters.scene_radius = std::nextafter(static_cast<float>(radius), INFINITY);
    if (!std::isfinite(parameters.scene_radius))
        return "Packet scene sphere radius overflows binary32";
    return {};
}

bool requires_world_space_emitter(const RenderScene &scene, std::size_t source) {
    const auto &instance = scene.geometry.instances[source];
    const auto &mesh = scene.geometry.meshes[instance.mesh_index];
    if (scene.instance_emissions.empty() || scene.instance_emissions[source] == no_area_emission ||
        instance.transform.values == GeometryTransform{}.values || !mesh.analytics.empty())
        return false;
    const bool retained_zero = std::ranges::any_of(mesh.triangles, [&](const auto &t) {
        return !triangle_has_area(mesh.positions[t[0]], mesh.positions[t[1]], mesh.positions[t[2]]);
    });
    return !retained_zero && (!mesh.triangles.empty() || !mesh.patches.empty());
}

std::string prepare_world_space_emitters(const RenderScene &scene, GeometryData &prepared) {
    std::vector<std::size_t> references(scene.geometry.meshes.size());
    for (const auto &instance : scene.geometry.instances) {
        ++references[instance.mesh_index];
    }
    std::size_t clone_count = 0;
    std::size_t vertex_count = 0;
    std::size_t triangle_count = 0;
    std::size_t patch_count = 0;
    const auto charge_mesh = [&](const GeometryMesh &mesh) {
        if (mesh.triangles.empty() && mesh.patches.empty()) {
            return true;
        }
        if (mesh.positions.size() > max_index - vertex_count ||
            mesh.triangles.size() > max_index - triangle_count ||
            mesh.patches.size() > max_index - patch_count) {
            return false;
        }
        vertex_count += mesh.positions.size();
        triangle_count += mesh.triangles.size();
        patch_count += mesh.patches.size();
        return true;
    };
    for (const auto &mesh : scene.geometry.meshes) {
        if (!charge_mesh(mesh)) {
            return "Metal world-space emitter geometry exceeds the shading index limits";
        }
    }
    for (std::size_t i = 0; i < scene.geometry.instances.size(); ++i) {
        const auto mesh_index = scene.geometry.instances[i].mesh_index;
        if (requires_world_space_emitter(scene, i) && references[mesh_index] > 1) {
            if (!charge_mesh(scene.geometry.meshes[mesh_index])) {
                return "Metal world-space emitter geometry exceeds the shading index limits";
            }
            ++clone_count;
        }
    }
    const auto mesh_limit = std::min<std::size_t>(max_index, prepared.meshes.max_size());
    if (clone_count > mesh_limit - scene.geometry.meshes.size()) {
        return "Metal world-space emitter mesh count exceeds the index limit";
    }
    prepared.meshes.reserve(scene.geometry.meshes.size() + clone_count);
    const auto copy_positions_and_triangles = [&](const GeometryMesh &mesh) {
        GeometryMesh copy;
        copy.positions = mesh.positions;
        copy.triangles = mesh.triangles;
        copy.patches = mesh.patches;
        copy.analytics = mesh.analytics;
        prepared.meshes.push_back(std::move(copy));
    };
    for (const auto &mesh : scene.geometry.meshes) {
        copy_positions_and_triangles(mesh);
    }
    prepared.instances = scene.geometry.instances;
    for (std::size_t source = 0; source < prepared.instances.size(); ++source) {
        if (!requires_world_space_emitter(scene, source)) {
            continue;
        }
        auto &instance = prepared.instances[source];
        if (references[instance.mesh_index] > 1) {
            copy_positions_and_triangles(scene.geometry.meshes[instance.mesh_index]);
            instance.mesh_index = static_cast<std::uint32_t>(prepared.meshes.size() - 1);
        }
        auto &mesh = prepared.meshes[instance.mesh_index];
        for (std::size_t vertex = 0; vertex < mesh.positions.size(); ++vertex) {
            auto &position = mesh.positions[vertex];
            const auto world = area_world_vertex(position, instance.transform);
            if (!world) {
                return "Metal world-space emitter instance " + std::to_string(source) +
                       " has an unrepresentable vertex " + std::to_string(vertex);
            }
            position = *world;
        }
        instance.transform = {};
        if (auto message = validate_area_geometry(mesh, instance.transform); !message.empty()) {
            return "Metal world-space emitter instance " + std::to_string(source) + ": " + message;
        }
    }
    return {};
}

template <class Container> id<MTLBuffer> upload(id<MTLDevice> device, const Container &data) {
    using T = typename Container::value_type;
    if (data.empty() || data.size() > device.maxBufferLength / sizeof(T)) {
        return nil;
    }
    return [device newBufferWithBytes:data.data()
                               length:data.size() * sizeof(T)
                              options:MTLResourceStorageModeShared];
}

std::string validate_request(const RenderRequest &request) {
    const auto pixels = std::uint64_t{request.width} * request.height;
    if (pixels == 0 || pixels > max_index || request.pixel_count == 0 ||
        std::uint64_t{request.pixel_begin} + request.pixel_count > pixels) {
        return "Invalid Metal render request image or pixel range";
    }
    if (request.sample_count == 0 ||
        request.sample_begin >
            std::numeric_limits<std::uint64_t>::max() - (request.sample_count - 1) ||
        std::uint64_t{request.pixel_count} * request.sample_count > render_tile_capacity) {
        return "Invalid Metal render request sample range or tile capacity";
    }
    if (request.emergency_limit == 0 || request.emergency_limit > render_emergency_limit) {
        return "Invalid Metal render emergency depth limit";
    }
    if (request.boundary_limit == 0 || request.boundary_limit > medium_boundary_limit)
        return "Invalid Metal medium boundary limit";
    return validate_sampler_range(request.sampler, request.seed, request.width, request.height,
                                  request.sample_begin, request.sample_count,
                                  request.total_samples);
}

std::string sample_error(wire::Word status) {
    switch (status) {
    case wire::emergency_depth:
        return "Metal render emergency depth reached";
    case wire::invalid_arithmetic:
        return "Metal render encountered non-finite or invalid path arithmetic";
    case wire::invalid_hit:
        return "Metal render encountered invalid native hit data";
    case wire::boundary_exhaustion:
        return "Metal render medium boundary or random-stream budget exhausted";
    default:
        return "Metal render sample status is invalid or unwritten";
    }
}

// A packet sky dispatch retains the native IAS argument even though its
// instance-count guard skips traversal. Bind a built empty IAS rather than nil.
id<MTLAccelerationStructure> build_empty_packet_structure(id<MTLDevice> device,
                                                          id<MTLCommandQueue> queue,
                                                          std::string &diagnostic) {
    auto descriptor = [MTLInstanceAccelerationStructureDescriptor descriptor];
    descriptor.instanceCount = 0;
    descriptor.instancedAccelerationStructures = @[];
    descriptor.instanceDescriptorStride = sizeof(MTLAccelerationStructureInstanceDescriptor);
    descriptor.instanceDescriptorBuffer =
        [device newBufferWithLength:sizeof(MTLAccelerationStructureInstanceDescriptor)
                            options:MTLResourceStorageModeShared];
    if (descriptor.instanceDescriptorBuffer == nil) {
        diagnostic = "Metal empty packet instance descriptor allocation failed";
        return nil;
    }
    const auto sizes = [device accelerationStructureSizesWithDescriptor:descriptor];
    if (sizes.accelerationStructureSize == 0 ||
        sizes.buildScratchBufferSize > device.maxBufferLength) {
        diagnostic = "Metal empty packet acceleration structure sizing failed";
        return nil;
    }
    auto structure = [device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
    auto scratch = [device newBufferWithLength:std::max(NSUInteger{1}, sizes.buildScratchBufferSize)
                                       options:MTLResourceStorageModePrivate];
    if (structure == nil || scratch == nil) {
        diagnostic = "Metal empty packet acceleration structure allocation failed";
        return nil;
    }
    auto command = [queue commandBuffer];
    auto encoder = [command accelerationStructureCommandEncoder];
    if (command == nil || encoder == nil) {
        diagnostic = "Metal empty packet acceleration structure command allocation failed";
        return nil;
    }
    structure.label = @"rypath empty packet instances";
    [encoder buildAccelerationStructure:structure
                             descriptor:descriptor
                          scratchBuffer:scratch
                    scratchBufferOffset:0];
    [encoder endEncoding];
    diagnostic = metal::complete(command, "Metal empty packet acceleration structure build");
    return diagnostic.empty() ? structure : nil;
}

} // namespace

struct MetalRenderScene::Impl {
    std::unique_ptr<MetalGeometryScene> geometry;
    id<MTLAccelerationStructure> empty_packet_structure = nil;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLComputePipelineState> pipeline = nil;
    id<MTLIntersectionFunctionTable> intersection_table = nil;
    id<MTLBuffer> instances = nil;
    id<MTLBuffer> positions = nil;
    id<MTLBuffer> triangles = nil;
    std::uint32_t native_hint_mode = 0;
    id<MTLBuffer> patches = nil;
    id<MTLBuffer> materials = nil;
    id<MTLBuffer> material_data = nil;
    id<MTLBuffer> environment = nil;
    id<MTLBuffer> knots = nil;
    id<MTLBuffer> area_emissions = nil;
    id<MTLBuffer> emitters = nil;
    id<MTLBuffer> normals = nil;
    id<MTLBuffer> sampling_tables = nil;
    id<MTLBuffer> texture_uvs = nil;
    id<MTLBuffer> texture_frames = nil;
    id<MTLBuffer> image_textures = nil;
    id<MTLBuffer> image_levels = nil;
    id<MTLBuffer> image_texels = nil;
    id<MTLBuffer> image_transfer = nil;
    id<MTLBuffer> image_spectra = nil;
    id<MTLBuffer> graph_nodes = nil;
    id<MTLBuffer> graph_knots = nil;
    id<MTLBuffer> surface_vertices = nil;
    id<MTLBuffer> surface_transforms = nil;
    id<MTLBuffer> environment_cdf = nil;
    id<MTLBuffer> illuminant = nil;
    wire::TextureGraphParameters graph_parameters{.environment_image = max_index};
    wire::TextureParameters texture_parameters{};
    std::uint32_t texture_width = 0;
    std::uint32_t texture_height = 0;
    id<MTLBuffer> output = nil;
    wire::Parameters parameters{};
    bool dielectrics = false;

    std::string append_light_tables(const RenderScene &scene, std::vector<wire::Word> &data,
                                    std::span<const LightSamplingInput> area) {
        std::vector<LightSamplingInput> input(area.begin(), area.end());
        const double pi = std::numbers::pi;
        const double radius = std::max(double(parameters.scene_radius), 1e-8);
        std::map<const ImagePyramid *, double> image_means;
        const auto image_mean = [&](std::uint32_t index) -> double {
            if (index >= scene.image_textures.size() || !scene.image_textures[index].image)
                return -1;
            auto *image = scene.image_textures[index].image.get();
            auto it = image_means.find(image);
            if (it != image_means.end())
                return it->second;
            const auto mean = light_image_power(*image);
            return mean ? image_means.emplace(image, *mean).first->second : -1;
        };
        const auto append_position = [&](GeometryPoint position, double power) {
            LightSamplingInput entry;
            entry.low = entry.high = {position.x, position.y, position.z};
            entry.power = power;
            entry.two_sided = false;
            input.push_back(entry);
            return std::ref(input.back());
        };
        for (const auto &light : scene.point_lights) {
            auto &entry =
                append_position(light.position, 4 * pi * light_spectrum_power(light.intensity))
                    .get();
            entry.bounds_power =
                4 * pi * *std::max_element(light.intensity.begin(), light.intensity.end());
        }
        for (const auto &light : scene.spot_lights) {
            auto &entry =
                append_position(light.position,
                                2 * pi *
                                    ((1 - light.cos_falloff_start) +
                                     (light.cos_falloff_start - light.cos_falloff_end) / 2) *
                                    light_spectrum_power(light.intensity))
                    .get();
            entry.bounds_power =
                4 * pi * *std::max_element(light.intensity.begin(), light.intensity.end());
            float cos_emission =
                static_cast<float>(std::cos(std::acos(double(light.cos_falloff_end)) -
                                            std::acos(double(light.cos_falloff_start))));
            if (cos_emission == 1)
                cos_emission = light.cos_falloff_end != light.cos_falloff_start
                                   ? .999f
                                   : std::nextafter(1.f, 0.f);
            light_positional_cone(entry, light.light_from_world, light.cos_falloff_start,
                                  cos_emission);
        }
        for (const auto &light : scene.goniometric_lights) {
            const auto &image = *scene.image_textures[light.image_texture].image;
            const auto &base = image.levels.front();
            double mean = 0;
            for (unsigned y = 0; y < base.height; ++y)
                for (unsigned x = 0; x < base.width; ++x) {
                    const auto rgb = image_base_texel_linear_rgb(image, x, y);
                    if (!rgb)
                        return "Invalid goniometric light sampling image";
                    mean += (*rgb)[0];
                }
            mean /= double(base.width) * base.height;
            auto &entry = append_position(light.position,
                                          4 * pi * mean * light_spectrum_power(light.intensity))
                              .get();
            entry.bounds_power =
                4 * pi * mean * *std::max_element(light.intensity.begin(), light.intensity.end());
        }
        for (const auto &light : scene.projection_lights) {
            const auto power = light_projection_power(
                *scene.image_textures[light.image_texture].image, light.inv_tan_half_fov,
                light.screen_half_width, light.screen_half_height);
            if (!power)
                return "Invalid projection light sampling image";
            const auto bound = light_image_bounds_power(
                *scene.image_textures[light.image_texture].image, {1, 1, 1}, true);
            if (!bound)
                return "Invalid projection light bounds image";
            auto &entry = append_position(light.position, *power * light.image_scale).get();
            entry.bounds_power = *bound * light.image_scale;
            const double x = double(light.screen_half_width) / light.inv_tan_half_fov;
            const double y = double(light.screen_half_height) / light.inv_tan_half_fov;
            light_positional_cone(entry, light.light_from_world, 1,
                                  static_cast<float>(1 / std::sqrt(1 + x * x + y * y)));
        }
        for (const auto &light : scene.distant_lights) {
            LightSamplingInput entry;
            entry.infinite = true;
            entry.power = pi * radius * radius * light_spectrum_power(light.radiance);
            input.push_back(entry);
        }
        parameters.environment_light = UINT32_MAX;
        if (scene.environment_image != UINT32_MAX ||
            std::ranges::any_of(scene.environment, [](float v) { return v > 0; })) {
            LightSamplingInput entry;
            entry.infinite = true;
            const double mean =
                scene.environment_image == UINT32_MAX
                    ? light_spectrum_power(scene.environment)
                    : image_mean(scene.environment_image) * scene.environment_image_scale;
            if (mean < 0)
                return "Invalid environment light sampling image";
            entry.power = 4 * pi * pi * radius * radius * mean;
            parameters.environment_light = static_cast<wire::Word>(input.size());
            input.push_back(entry);
        }
        std::string error;
        const auto tables = build_light_sampling(input, scene.preparation_byte_limit, error);
        if (!tables)
            return error;
        parameters.light_sampler = scene.integrator == IntegratorKind::bdpt
                                       ? 1
                                       : static_cast<wire::Word>(scene.light_sampler);
        parameters.light_count = static_cast<wire::Word>(input.size());
        const auto append = [&]<class T>(const std::vector<T> &items, wire::Word &offset) -> bool {
            constexpr auto stride = sizeof(T) / sizeof(wire::Word);
            const auto limit = std::min<std::size_t>(128 * 1024 * 1024,
                                                     device.maxBufferLength / sizeof(wire::Word));
            if (data.size() > limit || items.size() > (limit - data.size()) / stride)
                return false;
            offset = static_cast<wire::Word>(data.size());
            for (const auto &item : items) {
                auto words = std::bit_cast<std::array<wire::Word, stride>>(item);
                data.insert(data.end(), words.begin(), words.end());
            }
            return true;
        };
        if (!append(tables->alias, parameters.light_alias_begin) ||
            !append(tables->records, parameters.light_records_begin) ||
            !append(tables->nodes, parameters.light_nodes_begin) ||
            !append(tables->infinite, parameters.infinite_lights_begin))
            return "Light sampling tables exceed the shared GPU data budget";
        parameters.light_node_count = static_cast<wire::Word>(tables->nodes.size());
        parameters.infinite_light_count = static_cast<wire::Word>(tables->infinite.size());
        return {};
    }

    std::string upload_auxiliary_data(const RenderScene &scene, std::vector<wire::Word> &data,
                                      std::span<const LightSamplingInput> area = {}) {
        constexpr std::size_t words = sizeof(wire::Medium) / sizeof(wire::Word);
        const auto limit =
            std::min<std::size_t>(128 * 1024 * 1024, device.maxBufferLength / sizeof(wire::Word));
        if (data.size() > limit || scene.media.size() > (limit - data.size()) / words)
            return "Homogeneous media exceed the shared GPU data budget";
        parameters.medium_begin = static_cast<wire::Word>(data.size());
        parameters.medium_count = static_cast<wire::Word>(scene.media.size());
        parameters.camera_medium = scene.camera_medium;
        for (const auto &source : scene.media) {
            wire::Medium medium{};
            std::ranges::copy(source.sigma_a, medium.sigma_a);
            std::ranges::copy(source.sigma_s, medium.sigma_s);
            std::ranges::copy(source.emission, medium.emission);
            medium.g = std::clamp(source.g, -.99f, .99f);
            const auto encoded = std::bit_cast<std::array<wire::Word, words>>(medium);
            data.insert(data.end(), encoded.begin(), encoded.end());
        }
        constexpr std::size_t light_words = sizeof(wire::PositionalLight) / sizeof(wire::Word);
        const auto count = scene.point_lights.size() + scene.spot_lights.size() +
                           scene.goniometric_lights.size() + scene.projection_lights.size();
        if (data.size() > limit || count > (limit - data.size()) / light_words)
            return "Positional lights exceed the shared GPU data budget";
        parameters.positional_begin = static_cast<wire::Word>(data.size());
        parameters.positional_count = static_cast<wire::Word>(count);
        std::vector<wire::PositionalLight> packed_positional;
        using DirectionKey = std::tuple<const ImagePyramid *, wire::Word, float, float, float>;
        std::map<DirectionKey, std::array<wire::Word, 3>> direction_tables;
        const auto append = [&](const auto &source, wire::Word kind) {
            wire::PositionalLight light{};
            light.kind = kind;
            light.position = {source.position.x, source.position.y, source.position.z};
            if constexpr (std::is_same_v<std::decay_t<decltype(source)>, ProjectionLight>) {
                // A projection has image radiance, never a separate dense I spectrum.
                std::ranges::fill(light.intensity, source.image_scale == 0 ? 0.f : 1.f);
                std::ranges::copy(source.light_from_world, light.light_from_world);
                light.image_texture = source.image_texture;
                light.image_scale = source.image_scale;
                light.inv_tan_half_fov = source.inv_tan_half_fov;
                light.screen_half_width = source.screen_half_width;
                light.screen_half_height = source.screen_half_height;
            } else {
                std::ranges::copy(source.intensity, light.intensity);
            }
            if constexpr (std::is_same_v<std::decay_t<decltype(source)>, SpotLight>) {
                // Common positive scaling leaves the normalized direction unchanged
                // and prevents finite large direct-API matrices overflowing in MSL.
                float scale = 0;
                for (float v : source.light_from_world)
                    scale = std::max(scale, std::abs(v));
                for (std::size_t i = 0; i < 9; ++i)
                    light.light_from_world[i] = source.light_from_world[i] / scale;
                light.cos_falloff_start = source.cos_falloff_start;
                light.cos_falloff_end = source.cos_falloff_end;
            }
            if constexpr (std::is_same_v<std::decay_t<decltype(source)>, GoniometricLight>) {
                std::ranges::copy(source.light_from_world, light.light_from_world);
                light.image_texture = source.image_texture;
            }
            if constexpr (std::is_same_v<std::decay_t<decltype(source)>, GoniometricLight> ||
                          std::is_same_v<std::decay_t<decltype(source)>, ProjectionLight>) {
                const auto &image = *scene.image_textures[source.image_texture].image;
                const DirectionKey key{
                    &image, light.kind,
                    light.kind == wire::projection_light ? light.inv_tan_half_fov : 0,
                    light.kind == wire::projection_light ? light.screen_half_width : 1,
                    light.kind == wire::projection_light ? light.screen_half_height : 1};
                auto found = direction_tables.find(key);
                if (found == direction_tables.end()) {
                    std::string error;
                    const auto distribution = build_light_direction_sampling(
                        image,
                        std::min<std::size_t>(scene.preparation_byte_limit,
                                              (limit - data.size()) * sizeof(wire::Word)),
                        error, std::get<2>(key), std::get<3>(key), std::get<4>(key));
                    if (!distribution)
                        throw std::runtime_error(error);
                    const std::array<wire::Word, 3> metadata{static_cast<wire::Word>(data.size()),
                                                             distribution->width,
                                                             distribution->height};
                    for (float value : distribution->cdf)
                        data.push_back(std::bit_cast<wire::Word>(value));
                    found = direction_tables.emplace(key, metadata).first;
                }
                light.sampling_begin = found->second[0];
                light.sampling_width = found->second[1];
                light.sampling_height = found->second[2];
            }
            packed_positional.push_back(light);
        };
        try {
            for (const auto &source : scene.point_lights)
                append(source, wire::point_light);
            for (const auto &source : scene.spot_lights)
                append(source, wire::spot_light);
            for (const auto &source : scene.goniometric_lights)
                append(source, wire::goniometric_light);
            for (const auto &source : scene.projection_lights)
                append(source, wire::projection_light);
        } catch (const std::runtime_error &error) {
            return error.what();
        }
        if (data.size() > limit || packed_positional.size() > (limit - data.size()) / light_words)
            return "Positional light records exceed the GPU data budget";
        parameters.positional_begin = static_cast<wire::Word>(data.size());
        for (const auto &light : packed_positional) {
            const auto encoded = std::bit_cast<std::array<wire::Word, light_words>>(light);
            data.insert(data.end(), encoded.begin(), encoded.end());
        }
        constexpr std::size_t distant_words = sizeof(wire::DistantLight) / sizeof(wire::Word);
        if (data.size() > limit ||
            scene.distant_lights.size() > (limit - data.size()) / distant_words)
            return "Distant lights exceed the shared GPU data budget";
        parameters.distant_begin = static_cast<wire::Word>(data.size());
        parameters.distant_count = static_cast<wire::Word>(scene.distant_lights.size());
        parameters.distant_scene_radius = scene.distant_scene_radius;
        for (const auto &source : scene.distant_lights) {
            wire::DistantLight light{};
            light.direction = {source.direction.x, source.direction.y, source.direction.z};
            std::ranges::copy(source.radiance, light.radiance);
            const auto encoded = std::bit_cast<std::array<wire::Word, distant_words>>(light);
            data.insert(data.end(), encoded.begin(), encoded.end());
        }
        if (auto error = append_light_tables(scene, data, area); !error.empty())
            return error;
        parameters.material_data_words = static_cast<wire::Word>(data.size());
        if (data.empty())
            data.push_back(0);
        material_data = upload(device, data);
        return material_data == nil ? "Metal auxiliary data allocation failed" : "";
    }

    std::string upload_textures(const RenderScene &scene,
                                std::vector<wire::Instance> &packed_instances) {
        std::vector<wire::Point2> uvs;
        std::vector<wire::TextureFrame> frames;
        std::map<std::uint32_t, wire::Word> uv_offsets;
        std::map<std::uint32_t, wire::Word> frame_offsets;
        const auto uv_limit =
            std::min<std::size_t>(max_index, device.maxBufferLength / sizeof(wire::Point2));
        const auto frame_limit =
            std::min<std::size_t>(max_index, device.maxBufferLength / sizeof(wire::TextureFrame));
        for (std::size_t ordinal = 0; ordinal < packed_instances.size(); ++ordinal) {
            auto &packed = packed_instances[ordinal];
            packed.uv_begin = packed.frame_begin = max_index;
            const auto source = MetalGeometryAccess::source_instance(*geometry, ordinal);
            if (!instance_uses_texture_context(scene, source)) {
                continue;
            }
            const auto mesh_index = scene.geometry.instances[source].mesh_index;
            const auto &mesh = scene.geometry.meshes[mesh_index];
            if (!mesh.uvs.empty()) {
                const auto [it, added] =
                    uv_offsets.try_emplace(mesh_index, static_cast<wire::Word>(uvs.size()));
                packed.uv_begin = it->second;
                if (added) {
                    if (mesh.uvs.size() > uv_limit - uvs.size()) {
                        return "Metal texture coordinate buffer exceeds device or index limits";
                    }
                    for (const auto uv : mesh.uvs) {
                        uvs.push_back({uv[0], uv[1]});
                    }
                }
            }
            const auto &source_frames = scene.instance_texture_frames[source];
            if (!source_frames.empty()) {
                const auto [it, added] =
                    frame_offsets.try_emplace(source, static_cast<wire::Word>(frames.size()));
                packed.frame_begin = it->second;
                if (added) {
                    if (source_frames.size() > frame_limit - frames.size()) {
                        return "Metal texture frame buffer exceeds device or index limits";
                    }
                    for (const auto &frame : source_frames) {
                        frames.push_back({pack(frame.dpdu), pack(frame.dpdv)});
                    }
                }
            }
        }
        std::vector<wire::ImageTexture> textures;
        std::vector<wire::TextureLevel> levels;
        std::vector<wire::Word> texels;
        std::vector<float> transfer(512);
        std::ranges::copy(image_transfer_table(ImageEncoding::srgb), transfer.begin());
        std::ranges::copy(image_transfer_table(ImageEncoding::linear), transfer.begin() + 256);
        std::map<const std::array<float, 256> *, wire::Word> transfer_offsets;
        std::map<const ImagePyramid *, wire::Word> pyramid_offsets;
        const auto texel_limit =
            std::min<std::size_t>(max_index, device.maxBufferLength / sizeof(wire::Word));
        const auto level_limit =
            std::min<std::size_t>(max_index, device.maxBufferLength / sizeof(wire::TextureLevel));
        if (scene.image_textures.size() > device.maxBufferLength / sizeof(wire::ImageTexture) ||
            scene.image_textures.size() > max_index) {
            return "Metal image descriptor buffer exceeds device or index limits";
        }
        for (const auto &texture : scene.image_textures) {
            wire::Word filter;
            switch (texture.filter) {
            case ImageFilter::point:
                filter = wire::point_texture_filter;
                break;
            case ImageFilter::bilinear:
                filter = wire::bilinear_texture_filter;
                break;
            case ImageFilter::trilinear:
                filter = wire::trilinear_texture_filter;
                break;
            case ImageFilter::ewa:
                filter = wire::ewa_texture_filter;
                break;
            default:
                return "Invalid image texture filter";
            }
            const auto &image = *texture.image;
            const wire::Word texel_format =
                image.storage == ImageStorage::linear_float  ? wire::linear_float_texel_format
                : image.storage == ImageStorage::linear_half ? wire::linear_half_texel_format
                : image.encoding != ImageEncoding::srgb      ? wire::linear8_texel_format
                                                             : wire::srgb8_texel_format;
            wire::Word transfer_offset = image.encoding == ImageEncoding::srgb ? 0 : 256;
            if (image.custom_transfer) {
                const auto [found, fresh] = transfer_offsets.try_emplace(
                    image.custom_transfer.get(), static_cast<wire::Word>(transfer.size()));
                transfer_offset = found->second;
                if (fresh) {
                    if (transfer.size() >
                        std::min<std::size_t>(max_index, device.maxBufferLength / sizeof(float)) -
                            256) {
                        return "Metal image transfer tables exceed device or index limits";
                    }
                    transfer.insert(transfer.end(), image.custom_transfer->begin(),
                                    image.custom_transfer->end());
                }
            }
            const auto [it, added] =
                pyramid_offsets.try_emplace(&image, static_cast<wire::Word>(levels.size()));
            if (added) {
                if (image.texels.size() > texel_limit - texels.size() ||
                    image.levels.size() > level_limit - levels.size()) {
                    return "Metal image pyramid exceeds device or index limits";
                }
                const auto begin = static_cast<wire::Word>(texels.size());
                for (const auto &level : image.levels) {
                    levels.push_back({level.width, level.height, begin + level.offset});
                }
                texels.insert(texels.end(), image.texels.begin(), image.texels.end());
            }
            textures.push_back({it->second, static_cast<wire::Word>(image.levels.size()),
                                static_cast<wire::Word>(image.wrap), texel_format, texture.uscale,
                                texture.vscale, texture.udelta, texture.vdelta, texture.scale,
                                static_cast<wire::Word>(texture.invert), filter,
                                texture.max_anisotropy, transfer_offset, image.source_channels});
            if (texture.gltf_sampler) {
                auto &packed = textures.back();
                packed.gltf = 1;
                packed.wrap = texture.gltf_sampler->wrap_s;
                packed.wrap_t = texture.gltf_sampler->wrap_t;
                packed.min_filter = texture.gltf_sampler->min_filter;
                packed.mag_filter = texture.gltf_sampler->mag_filter;
            }
        }
        texture_parameters.texture_count = static_cast<wire::Word>(textures.size());
        texture_parameters.level_count = static_cast<wire::Word>(levels.size());
        texture_parameters.texel_word_count = static_cast<wire::Word>(texels.size());
        texture_parameters.uv_count = static_cast<wire::Word>(uvs.size());
        texture_parameters.frame_count = static_cast<wire::Word>(frames.size());
        texture_parameters.transfer_count = static_cast<wire::Word>(transfer.size());
        std::ranges::copy(scene.texture_camera.world_from_camera,
                          texture_parameters.world_from_camera);
        std::ranges::copy(scene.texture_camera.camera_from_world,
                          texture_parameters.camera_from_world);
        if (uvs.empty())
            uvs.emplace_back();
        if (frames.empty())
            frames.emplace_back();
        if (textures.empty())
            textures.emplace_back();
        if (levels.empty())
            levels.emplace_back();
        if (texels.empty())
            texels.emplace_back();
        texture_uvs = upload(device, uvs);
        texture_frames = upload(device, frames);
        image_textures = upload(device, textures);
        image_levels = upload(device, levels);
        image_texels = upload(device, texels);
        image_transfer = upload(device, transfer);
        if (texture_parameters.texture_count != 0 || !scene.texture_graph.nodes.empty() ||
            std::ranges::any_of(scene.materials, [](const auto &m) {
                return m.table != nullptr || m.scattering == ScatteringKind::gltf;
            })) {
            image_spectra = upload(device, srgb_reconstruction_table());
        } else {
            const std::array<float, 1> empty{};
            image_spectra = upload(device, empty);
        }
        if (texture_uvs == nil || texture_frames == nil || image_textures == nil ||
            image_levels == nil || image_texels == nil || image_transfer == nil ||
            image_spectra == nil) {
            return "Metal image texture buffer allocation failed";
        }
        return {};
    }

    std::string upload_graph(const RenderScene &scene) {
        auto nodes = scene.texture_graph.nodes;
        auto values = scene.texture_graph.knots;
        if (nodes.size() > max_index || values.size() > max_index) {
            return "Metal texture graph exceeds index limits";
        }
        graph_parameters.node_count = static_cast<wire::Word>(nodes.size());
        graph_parameters.knot_count = static_cast<wire::Word>(values.size());
        graph_parameters.alpha_enabled = filtered_visibility(scene);
        graph_parameters.environment_image = scene.environment_image;
        graph_parameters.environment_scale = scene.environment_image_scale;
        if (nodes.empty())
            nodes.emplace_back();
        if (values.empty())
            values.emplace_back();
        graph_nodes = upload(device, nodes);
        graph_knots = upload(device, values);
        illuminant = upload(device, pbrt_d65());
        std::vector<wire::SurfaceVertex> vertices;
        std::vector<wire::SurfaceTransform> transforms;
        const bool detail = graph_parameters.alpha_enabled ||
                            std::ranges::any_of(scene.materials, [](const SpectralMaterial &m) {
                                return (m.scattering != ScatteringKind::diffuse &&
                                        m.scattering != ScatteringKind::null_interface) ||
                                       m.bump_root != max_index || m.normal_map != max_index;
                            });
        std::map<std::uint32_t, wire::Word> offsets;
        const auto vertex_limit =
            std::min<std::size_t>(max_index, device.maxBufferLength / sizeof(wire::SurfaceVertex));
        const auto matrices = [](const std::array<double, 16> &m, float *linear, float *normal) {
            const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8],
                         h = m[9], i = m[10];
            const double cof[9] = {e * i - f * h, f * g - d * i, d * h - e * g,
                                   c * h - b * i, a * i - c * g, b * g - a * h,
                                   b * f - c * e, c * d - a * f, a * e - b * d};
            const double determinant = a * cof[0] + b * cof[1] + c * cof[2];
            if (determinant == 0 || !std::isfinite(determinant))
                return false;
            for (unsigned row = 0; row < 3; ++row) {
                for (unsigned col = 0; col < 3; ++col) {
                    linear[3 * row + col] = static_cast<float>(m[4 * row + col]);
                    normal[3 * row + col] = static_cast<float>(cof[3 * row + col] / determinant);
                    if (!std::isfinite(linear[3 * row + col]) ||
                        !std::isfinite(normal[3 * row + col]))
                        return false;
                }
            }
            return true;
        };
        if (detail) {
            for (std::size_t ordinal = 0; ordinal < parameters.instance_count; ++ordinal) {
                const auto source = MetalGeometryAccess::source_instance(*geometry, ordinal);
                const auto mesh_index = scene.geometry.instances[source].mesh_index;
                const auto &mesh = scene.geometry.meshes[mesh_index];
                const auto [entry, added] =
                    offsets.try_emplace(mesh_index, static_cast<wire::Word>(vertices.size()));
                if (added) {
                    if (mesh.positions.size() > vertex_limit - vertices.size())
                        return "Metal surface attributes exceed device or index limits";
                    for (std::size_t v = 0; v < mesh.positions.size(); ++v) {
                        vertices.push_back(
                            {pack(mesh.positions[v]),
                             pack(mesh.normals.empty() ? GeometryPoint{} : mesh.normals[v]),
                             pack(mesh.tangents.empty() ? GeometryPoint{} : mesh.tangents[v]),
                             mesh.tangent_signs.empty() ? 1 : mesh.tangent_signs[v]});
                    }
                }
                wire::SurfaceTransform packed{};
                packed.vertex_begin = entry->second;
                packed.vertex_count = static_cast<wire::Word>(mesh.positions.size());
                packed.has_normals = !mesh.normals.empty();
                packed.has_tangents = !mesh.tangents.empty();
                SurfaceTransforms fallback;
                std::ranges::copy(scene.geometry.instances[source].transform.values,
                                  fallback.shape.begin());
                const auto &input = scene.instance_surface_transforms.empty()
                                        ? fallback
                                        : scene.instance_surface_transforms[source];
                if (!matrices(input.shape, packed.shape, packed.shape_normal) ||
                    !matrices(input.placement, packed.placement, packed.placement_normal)) {
                    return "Surface frame transforms are not representable";
                }
                const auto &m = input.shape;
                const double determinant = m[0] * (m[5] * m[10] - m[6] * m[9]) -
                                           m[1] * (m[4] * m[10] - m[6] * m[8]) +
                                           m[2] * (m[4] * m[9] - m[5] * m[8]);
                packed.authored_reverse = input.authored_reverse;
                packed.shape_reverse_winding = input.authored_reverse != (determinant < 0);
                packed.definition_translation = {
                    static_cast<float>(input.shape[3] - parameters.origin.x),
                    static_cast<float>(input.shape[7] - parameters.origin.y),
                    static_cast<float>(input.shape[11] - parameters.origin.z)};
                if (!finite(packed.definition_translation))
                    return "Alpha definition position is not representable";
                transforms.push_back(packed);
            }
        }
        graph_parameters.surface_vertex_count = static_cast<wire::Word>(vertices.size());
        if (vertices.empty())
            vertices.emplace_back();
        if (transforms.empty())
            transforms.emplace_back();
        surface_vertices = upload(device, vertices);
        surface_transforms = upload(device, transforms);
        std::vector<float> cdf;
        if (scene.environment_image != max_index) {
            const auto &image = *scene.image_textures[scene.environment_image].image;
            std::string sampling_error;
            const auto distribution = build_environment_sampling(
                image, std::min<std::size_t>(scene.preparation_byte_limit, device.maxBufferLength),
                sampling_error);
            if (!distribution)
                return sampling_error;
            graph_parameters.environment_width = distribution->width;
            graph_parameters.environment_compensated_offset = distribution->compensated_offset;
            cdf = distribution->cdf;
            const auto &m = scene.environment_texture_from_world.values;
            for (unsigned row_index = 0; row_index < 3; ++row_index) {
                for (unsigned col = 0; col < 3; ++col) {
                    const auto value = m[4 * row_index + col];
                    graph_parameters.environment_from_world[3 * row_index + col] = value;
                    graph_parameters.world_from_environment[3 * col + row_index] = value;
                }
            }
        }
        graph_parameters.environment_cdf_count = static_cast<wire::Word>(cdf.size());
        if (cdf.empty())
            cdf.emplace_back();
        environment_cdf = upload(device, cdf);
        if (graph_nodes == nil || graph_knots == nil || surface_vertices == nil ||
            surface_transforms == nil || environment_cdf == nil || illuminant == nil) {
            return "Metal texture graph or consumer buffer allocation failed";
        }
        return {};
    }

    std::string upload_normals(const RenderScene &scene,
                               std::vector<wire::Instance> &packed_instances) {
        using Key = std::tuple<std::uint32_t, std::array<float, 9>, bool>;
        std::map<Key, wire::Word> offsets;
        struct Table {
            std::uint32_t source;
            bool reverse_winding;
        };
        std::vector<Table> tables;
        std::vector<wire::Point> packed_normals;
        std::vector<GeometryPoint> prepared;
        const auto limit = std::min<std::size_t>(
            {max_index, device.maxBufferLength / sizeof(wire::Point), packed_normals.max_size()});
        std::size_t count = 0;
        for (std::size_t ordinal = 0; ordinal < packed_instances.size(); ++ordinal) {
            auto &packed = packed_instances[ordinal];
            packed.normal_begin = wire::no_normals;
            const auto source = MetalGeometryAccess::source_instance(*geometry, ordinal);
            // Native emitter meshes may be baked and cloned. Always key and transform
            // the authored normals using the original mesh and instance.
            const auto &instance = scene.geometry.instances[source];
            const auto &mesh = scene.geometry.meshes[instance.mesh_index];
            if (mesh.normals.empty()) {
                continue;
            }
            const auto &m = instance.transform.values;
            const bool reverse_winding = packed.reverse_orientation != 0;
            const Key key{instance.mesh_index,
                          {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]},
                          reverse_winding};
            const auto [it, added] = offsets.try_emplace(key, static_cast<wire::Word>(count));
            packed.normal_begin = it->second;
            if (!added) {
                continue;
            }
            if (mesh.normals.size() > limit - count || mesh.normals.size() > prepared.max_size()) {
                return "Metal render normal buffer exceeds host, device or index limits";
            }
            count += mesh.normals.size();
            tables.push_back({source, reverse_winding});
        }
        packed_normals.reserve(std::max<std::size_t>(1, count));
        for (const auto [source, reverse_winding] : tables) {
            const auto &instance = scene.geometry.instances[source];
            const auto &mesh = scene.geometry.meshes[instance.mesh_index];
            if (auto message =
                    prepare_shading_normals(mesh, instance.transform, reverse_winding, prepared);
                !message.empty()) {
                return "Metal render instance " + std::to_string(source) + ": " + message;
            }
            for (const auto point : prepared) {
                packed_normals.push_back(pack(point));
            }
        }
        parameters.normal_count = static_cast<wire::Word>(count);
        if (packed_normals.empty()) {
            packed_normals.emplace_back();
        }
        normals = upload(device, packed_normals);
        return normals == nil ? "Metal render normal buffer allocation failed" : "";
    }

    std::string upload_area_lighting(const RenderScene &scene,
                                     std::vector<wire::Instance> &packed_instances) {
        static_assert(no_area_emission == wire::no_emission);
        if (scene.area_emissions.size() > max_index ||
            scene.area_emissions.size() > device.maxBufferLength / sizeof(wire::AreaEmission)) {
            return "Metal area emission buffer exceeds device or index limits";
        }
        parameters.emission_count = static_cast<wire::Word>(scene.area_emissions.size());
        const auto emitter_limit = device.maxBufferLength / sizeof(wire::Emitter);
        std::size_t emitter_count = 0;
        for (const auto &instance : packed_instances) {
            if (instance.emission != wire::no_emission) {
                const auto count = std::size_t{instance.triangle_count} + instance.patch_count +
                                   instance.analytic_count;
                if (count > emitter_limit - emitter_count) {
                    return "Metal area emitter buffer exceeds device size limit";
                }
                emitter_count += count;
            }
        }
        parameters.emitter_count = static_cast<wire::Word>(emitter_count);
        parameters.emitter_slots = std::bit_ceil(parameters.emitter_count);
        std::vector<wire::AreaEmission> packed_emissions(
            std::max<std::size_t>(1, parameters.emission_count));
        for (std::size_t i = 0; i < scene.area_emissions.size(); ++i) {
            const auto &source = scene.area_emissions[i];
            std::ranges::copy(source.radiance, packed_emissions[i].radiance);
            packed_emissions[i].two_sided = source.two_sided;
            packed_emissions[i].image_texture = source.image_texture;
            packed_emissions[i].image_scale = source.image_scale;
            packed_emissions[i].gltf_material = source.gltf_material;
        }
        std::vector<wire::Emitter> packed_emitters;
        packed_emitters.reserve(std::max<std::size_t>(1, emitter_count));
        for (wire::Word ordinal = 0; ordinal < parameters.instance_count; ++ordinal) {
            auto &instance = packed_instances[ordinal];
            if (instance.emission == wire::no_emission) {
                continue;
            }
            instance.emitter_begin = static_cast<wire::Word>(packed_emitters.size());
            for (wire::Word primitive = 0; primitive < instance.triangle_count; ++primitive) {
                packed_emitters.push_back({ordinal, primitive, wire::triangle_primitive});
            }
            for (wire::Word primitive = 0; primitive < instance.patch_count; ++primitive) {
                packed_emitters.push_back({ordinal, primitive, wire::patch_primitive});
            }
            for (wire::Word primitive = 0; primitive < instance.analytic_count; ++primitive) {
                packed_emitters.push_back({ordinal, primitive, wire::analytic_primitive});
            }
        }
        if (packed_emitters.empty()) {
            packed_emitters.emplace_back();
        }
        area_emissions = upload(device, packed_emissions);
        emitters = upload(device, packed_emitters);
        if (area_emissions == nil || emitters == nil) {
            return "Metal area lighting buffer allocation failed";
        }
        return {};
    }

    std::string upload_shading(const RenderScene &scene, const GeometryData &geometry_data) {
        const auto &meshes = geometry_data.meshes;
        std::vector<std::size_t> vertex_offsets(meshes.size());
        std::vector<std::size_t> triangle_offsets(meshes.size());
        std::vector<std::size_t> patch_offsets(meshes.size());
        std::size_t vertex_count = 0;
        std::size_t triangle_count = 0;
        std::size_t patch_count = 0;
        const auto vertex_limit =
            std::min<std::size_t>(max_index, device.maxBufferLength / sizeof(wire::Point));
        const auto triangle_limit =
            std::min<std::size_t>(max_index, device.maxBufferLength / sizeof(wire::Triangle));
        const auto patch_limit =
            std::min<std::size_t>(max_index, device.maxBufferLength / sizeof(wire::Patch));
        for (std::size_t index = 0; index < meshes.size(); ++index) {
            const auto &mesh = meshes[index];
            if (mesh.triangles.empty() && mesh.patches.empty() && mesh.analytics.empty()) {
                continue;
            }
            if (mesh.positions.size() > vertex_limit - vertex_count ||
                mesh.triangles.size() > triangle_limit - triangle_count ||
                mesh.patches.size() > patch_limit - patch_count) {
                return "Metal render shading geometry exceeds buffer or index limits";
            }
            vertex_offsets[index] = vertex_count;
            triangle_offsets[index] = triangle_count;
            patch_offsets[index] = patch_count;
            vertex_count += mesh.positions.size();
            triangle_count += mesh.triangles.size();
            patch_count += mesh.patches.size();
        }
        if (scene.materials.size() > max_index ||
            parameters.instance_count > device.maxBufferLength / sizeof(wire::Instance) ||
            scene.materials.size() > device.maxBufferLength / sizeof(wire::Material)) {
            return "Metal render instance or material buffer exceeds device size limit";
        }
        parameters.material_count = static_cast<wire::Word>(scene.materials.size());
        const auto knot_limit =
            std::min<std::size_t>(max_index, device.maxBufferLength / sizeof(wire::SpectralKnot));
        std::size_t knot_count = 0;
        if (scene.model == RenderModel::pbrt) {
            for (const auto &material : scene.materials) {
                for (const auto *curve : {&material.knots, &material.eta.knots, &material.k.knots,
                                          &material.surface.interface_eta.knots}) {
                    if (curve->size() > knot_limit - knot_count)
                        return "Metal render spectral knot buffer exceeds buffer or index limits";
                    knot_count += curve->size();
                }
            }
        }
        parameters.knot_count = static_cast<wire::Word>(knot_count);

        std::vector<wire::Point> packed_positions;
        packed_positions.reserve(vertex_count);
        std::vector<wire::Triangle> packed_triangles;
        packed_triangles.reserve(triangle_count);
        std::vector<wire::Patch> packed_patches;
        packed_patches.reserve(patch_count);
        for (const auto &mesh : meshes) {
            if (mesh.triangles.empty() && mesh.patches.empty() && mesh.analytics.empty()) {
                continue;
            }
            for (const auto point : mesh.positions) {
                packed_positions.push_back({point.x, point.y, point.z});
            }
            for (const auto &triangle : mesh.triangles) {
                // Match the native BLAS null slot. A rounded float cross product
                // can assign positive area to an exactly collinear canonical face.
                // Keep source geometry/identity and every primitive ordinal intact.
                if (!triangle_has_area(mesh.positions[triangle[0]], mesh.positions[triangle[1]],
                                       mesh.positions[triangle[2]]))
                    packed_triangles.push_back({{triangle[0], triangle[0], triangle[0]}});
                else
                    packed_triangles.push_back({{triangle[0], triangle[1], triangle[2]}});
            }
            for (const auto &patch : mesh.patches) {
                packed_patches.push_back({{patch[0], patch[1], patch[2], patch[3]}});
            }
        }
        if (packed_positions.empty())
            packed_positions.emplace_back();
        if (packed_triangles.empty()) {
            packed_triangles.emplace_back();
        }
        if (packed_patches.empty()) {
            packed_patches.emplace_back();
        }

        std::vector<wire::Instance> packed_instances;
        packed_instances.reserve(parameters.instance_count);
        for (std::size_t ordinal = 0; ordinal < parameters.instance_count; ++ordinal) {
            const auto source = MetalGeometryAccess::source_instance(*geometry, ordinal);
            const auto &instance = geometry_data.instances[source];
            const auto mesh = instance.mesh_index;
            wire::Instance packed{};
            std::ranges::copy(instance.transform.values, packed.matrix);
            packed.vertex_begin = static_cast<wire::Word>(vertex_offsets[mesh]);
            packed.triangle_begin = static_cast<wire::Word>(triangle_offsets[mesh]);
            const auto kind = MetalGeometryAccess::primitive_kind(*geometry, ordinal);
            packed.triangle_count = kind == PrimitiveKind::triangle
                                        ? static_cast<wire::Word>(meshes[mesh].triangles.size())
                                        : 0;
            packed.patch_begin = static_cast<wire::Word>(patch_offsets[mesh]);
            packed.patch_count = kind == PrimitiveKind::bilinear_patch
                                     ? static_cast<wire::Word>(meshes[mesh].patches.size())
                                     : 0;
            packed.analytic_count = kind == PrimitiveKind::analytic
                                        ? static_cast<wire::Word>(meshes[mesh].analytics.size())
                                        : 0;
            packed.material = scene.instance_materials[source];
            if (!scene.instance_reverse_orientation.empty()) {
                packed.reverse_orientation = scene.instance_reverse_orientation[source];
            } else if (kind == PrimitiveKind::analytic) {
                // Direct API callers with no override get outward analytic
                // normals, including reflected placements. PBRT translation
                // supplies the combined authored/handedness flag explicitly.
                const auto &m = instance.transform.values;
                const double determinant =
                    double(m[0]) * (double(m[5]) * m[10] - double(m[6]) * m[9]) -
                    double(m[1]) * (double(m[4]) * m[10] - double(m[6]) * m[8]) +
                    double(m[2]) * (double(m[4]) * m[9] - double(m[5]) * m[8]);
                packed.reverse_orientation = determinant < 0;
            }
            packed.emission = scene.instance_emissions.empty() ? wire::no_emission
                                                               : scene.instance_emissions[source];
            packed.alpha_root =
                scene.instance_alpha_roots.empty() ? max_index : scene.instance_alpha_roots[source];
            packed.invisible_emitter = scene.instance_invisible_emitters.empty()
                                           ? 0
                                           : scene.instance_invisible_emitters[source];
            if (!scene.instance_media.empty()) {
                packed.inside_medium = scene.instance_media[source].inside;
                packed.outside_medium = scene.instance_media[source].outside;
            }
            packed_instances.push_back(packed);
        }
        if (auto message = upload_normals(scene, packed_instances); !message.empty()) {
            return message;
        }
        if (auto message = upload_textures(scene, packed_instances); !message.empty()) {
            return message;
        }

        std::vector<wire::Material> packed_materials(scene.materials.size());
        std::vector<wire::Word> packed_data;
        std::map<const BsdfTable *, wire::Word> table_offsets;
        const auto data_limit =
            std::min<std::size_t>(128 * 1024 * 1024, device.maxBufferLength / sizeof(wire::Word));
        std::vector<wire::SpectralKnot> packed_knots;
        packed_knots.reserve(std::max<std::size_t>(1, knot_count));
        for (std::size_t index = 0; index < scene.materials.size(); ++index) {
            const auto &source = scene.materials[index];
            auto &packed = packed_materials[index];
            packed.texture_root = source.texture_root;
            packed.bump_root = source.bump_root;
            packed.normal_map = source.normal_map;
            packed.scattering = static_cast<wire::Word>(source.scattering);
            packed.eta_begin = static_cast<wire::Word>(packed_knots.size());
            packed.eta_count = static_cast<wire::Word>(source.eta.knots.size());
            for (const auto [wavelength, value] : source.eta.knots)
                packed_knots.push_back({wavelength, value});
            packed.k_begin = static_cast<wire::Word>(packed_knots.size());
            packed.k_count = static_cast<wire::Word>(source.k.knots.size());
            for (const auto [wavelength, value] : source.k.knots)
                packed_knots.push_back({wavelength, value});
            packed.optical_extensions = wire::Word(source.eta.extend_endpoints) |
                                        (wire::Word(source.k.extend_endpoints) << 1);
            packed.u_roughness = source.u_roughness;
            packed.v_roughness = source.v_roughness;
            packed.u_roughness_root = source.u_roughness_root;
            packed.v_roughness_root = source.v_roughness_root;
            packed.remap_roughness = source.remap_roughness;
            const auto &a = source.surface;
            const bool auxiliary = (source.scattering >= ScatteringKind::thin_dielectric &&
                                    source.scattering != ScatteringKind::null_interface) ||
                                   a.eta_root != max_index || a.k_root != max_index ||
                                   a.conductor_reflectance_root != max_index;
            if (auxiliary) {
                wire::Word table_offset = max_index;
                if (source.table) {
                    auto [it, added] = table_offsets.try_emplace(
                        source.table.get(), static_cast<wire::Word>(packed_data.size()));
                    if (added) {
                        if (source.table->words.size() > data_limit - packed_data.size())
                            return "Material tables exceed the GPU buffer budget";
                        packed_data.insert(packed_data.end(), source.table->words.begin(),
                                           source.table->words.end());
                    }
                    table_offset = it->second;
                }
                const auto interface_begin = static_cast<wire::Word>(packed_knots.size());
                for (const auto [wavelength, value] : a.interface_eta.knots)
                    packed_knots.push_back({wavelength, value});
                wire::MaterialAuxiliary aux{a.eta_root,
                                            a.k_root,
                                            a.conductor_reflectance_root,
                                            a.transmittance,
                                            a.transmittance_root,
                                            a.transmission_scale,
                                            a.transmission_scale_root,
                                            interface_begin,
                                            static_cast<wire::Word>(a.interface_eta.knots.size()),
                                            wire::Word(a.interface_eta.extend_endpoints),
                                            a.interface_eta_root,
                                            a.interface_u_roughness,
                                            a.interface_v_roughness,
                                            a.interface_u_roughness_root,
                                            a.interface_v_roughness_root,
                                            a.thickness,
                                            a.thickness_root,
                                            a.albedo,
                                            a.albedo_root,
                                            a.g,
                                            a.g_root,
                                            a.max_depth,
                                            a.samples,
                                            a.first_material,
                                            a.second_material,
                                            a.mix_amount,
                                            a.mix_amount_root,
                                            table_offset,
                                            {}};
                const auto &g = source.gltf;
                std::ranges::copy(g.base_color, aux.gltf.base_color);
                aux.gltf.metallic = g.metallic;
                aux.gltf.roughness = g.roughness;
                aux.gltf.normal_scale = g.normal_scale;
                aux.gltf.alpha_cutoff = g.alpha_cutoff;
                aux.gltf.alpha_mode = g.alpha_mode;
                aux.gltf.double_sided = g.double_sided;
                aux.gltf.base_color_texture = g.base_color_texture;
                aux.gltf.metallic_roughness_texture = g.metallic_roughness_texture;
                aux.gltf.normal_texture = g.normal_texture;
                aux.gltf.emissive_texture = g.emissive_texture;
                std::ranges::copy(g.emissive, aux.gltf.emissive);
                constexpr auto words = sizeof(aux) / sizeof(wire::Word);
                if (words > data_limit - packed_data.size())
                    return "Material parameters exceed the GPU buffer budget";
                packed.auxiliary = static_cast<wire::Word>(packed_data.size());
                const auto encoded = std::bit_cast<std::array<wire::Word, words>>(aux);
                packed_data.insert(packed_data.end(), encoded.begin(), encoded.end());
            }
            switch (source.kind) {
            case SpectralMaterialKind::sampled:
                packed.kind = wire::sampled_material;
                if (scene.model == RenderModel::demo) {
                    std::ranges::copy(source.reflectance, packed.reflectance);
                } else {
                    packed.knot_begin = static_cast<wire::Word>(packed_knots.size());
                    packed.knot_count = static_cast<wire::Word>(source.knots.size());
                    for (const auto [wavelength, value] : source.knots) {
                        packed_knots.push_back({wavelength, value});
                    }
                }
                break;
            case SpectralMaterialKind::constant:
                packed.kind = wire::constant_material;
                packed.scalar_value = source.constant;
                break;
            case SpectralMaterialKind::blackbody:
                packed.kind = wire::blackbody_material;
                packed.scalar_value = blackbody_exponent_factor(source.temperature_kelvin);
                break;
            case SpectralMaterialKind::rgb_sigmoid:
                packed.kind = wire::rgb_sigmoid_material;
                std::ranges::copy(source.coefficients, packed.coefficients);
                break;
            case SpectralMaterialKind::image:
                packed.kind = wire::image_material;
                packed.image_texture = source.image_texture;
                break;
            case SpectralMaterialKind::texture_graph:
                packed.kind = wire::graph_material;
                break;
            }
        }
        // The shared kernel always binds a knot buffer, even with no authored knots.
        if (packed_knots.empty()) {
            packed_knots.emplace_back();
        }

        // Geometry backends split triangle/patch instances; factors retain the
        // source placement and select the matching primitive range.
        std::map<std::uint32_t, wire::Word> analytic_offsets;
        constexpr std::size_t analytic_words = sizeof(geometry_wire::Analytic) / sizeof(wire::Word);
        for (std::size_t ordinal = 0; ordinal < packed_instances.size(); ++ordinal) {
            auto &packed = packed_instances[ordinal];
            if (packed.analytic_count == 0)
                continue;
            const auto source = MetalGeometryAccess::source_instance(*geometry, ordinal);
            const auto mesh_index = geometry_data.instances[source].mesh_index;
            const auto [entry, fresh] = analytic_offsets.try_emplace(
                mesh_index, static_cast<wire::Word>(packed_data.size()));
            packed.analytic_begin = entry->second;
            if (fresh) {
                if (packed_data.size() > data_limit ||
                    meshes[mesh_index].analytics.size() >
                        (data_limit - packed_data.size()) / analytic_words)
                    return "Analytic shading records exceed the shared GPU data budget";
                for (const auto &shape : meshes[mesh_index].analytics) {
                    const auto words =
                        std::bit_cast<std::array<wire::Word, analytic_words>>(pack_analytic(shape));
                    packed_data.insert(packed_data.end(), words.begin(), words.end());
                }
            }
        }
        std::vector<std::vector<float>> source_power_factors;
        std::size_t failed_power_instance;
        if (auto error = prepare_area_power(scene, source_power_factors, failed_power_instance);
            !error.empty())
            return error;
        const auto power_limit =
            std::min<std::size_t>(128 * 1024 * 1024, device.maxBufferLength / sizeof(wire::Word));
        for (std::size_t ordinal = 0; ordinal < packed_instances.size(); ++ordinal) {
            const auto source = MetalGeometryAccess::source_instance(*geometry, ordinal);
            if (scene.instance_emissions.empty())
                break;
            const auto &power_factors = source_power_factors[source];
            if (power_factors.empty())
                continue;
            auto &packed = packed_instances[ordinal];
            const auto &mesh = meshes[geometry_data.instances[source].mesh_index];
            const auto family = MetalGeometryAccess::primitive_kind(*geometry, ordinal);
            const auto begin = family == PrimitiveKind::triangle ? 0
                               : family == PrimitiveKind::bilinear_patch
                                   ? mesh.triangles.size()
                                   : mesh.triangles.size() + mesh.patches.size();
            const auto count =
                std::size_t{packed.triangle_count} + packed.patch_count + packed.analytic_count;
            if (packed_data.size() > power_limit || count > power_limit - packed_data.size())
                return "Area power factors exceed the shared GPU data budget";
            packed.area_power_begin = static_cast<wire::Word>(packed_data.size());
            for (std::size_t j = 0; j < count; ++j)
                packed_data.push_back(std::bit_cast<wire::Word>(power_factors[begin + j]));
        }
        if (auto error = upload_area_lighting(scene, packed_instances); !error.empty())
            return error;
        std::vector<LightSamplingInput> area_inputs;
        area_inputs.reserve(parameters.emitter_count);
        std::vector<double> emission_means(scene.area_emissions.size(), -1);
        std::vector<double> emission_bounds_means(scene.area_emissions.size(), -1);
        for (std::size_t ordinal = 0; ordinal < packed_instances.size(); ++ordinal) {
            const auto &instance = packed_instances[ordinal];
            if (instance.emission == wire::no_emission)
                continue;
            const auto &emission = scene.area_emissions[instance.emission];
            double &mean = emission_means[instance.emission];
            double &bound_mean = emission_bounds_means[instance.emission];
            if (mean < 0) {
                if (emission.image_texture != UINT32_MAX) {
                    const auto value =
                        light_image_power(*scene.image_textures[emission.image_texture].image);
                    if (!value)
                        return "Invalid area light sampling image";
                    mean = *value * emission.image_scale;
                    const auto bound = light_image_bounds_power(
                        *scene.image_textures[emission.image_texture].image);
                    if (!bound)
                        return "Invalid area light bounds image";
                    bound_mean = *bound * emission.image_scale;
                } else if (emission.gltf_material != UINT32_MAX) {
                    const auto &gltf = scene.materials[emission.gltf_material].gltf;
                    const auto value =
                        gltf.emissive_texture == UINT32_MAX
                            ? light_rgb_power(gltf.emissive)
                            : light_image_power(*scene.image_textures[gltf.emissive_texture].image,
                                                gltf.emissive);
                    if (!value)
                        return "Invalid glTF light sampling emission";
                    mean = *value * emission.image_scale;
                    if (gltf.emissive_texture == UINT32_MAX)
                        bound_mean =
                            (double(gltf.emissive[0]) + gltf.emissive[1] + gltf.emissive[2]) / 3 *
                            emission.image_scale;
                    else {
                        const auto bound = light_image_bounds_power(
                            *scene.image_textures[gltf.emissive_texture].image, gltf.emissive);
                        if (!bound)
                            return "Invalid glTF light bounds image";
                        bound_mean = *bound * emission.image_scale;
                    }
                } else {
                    mean = light_spectrum_power(emission.radiance);
                    bound_mean =
                        *std::max_element(emission.radiance.begin(), emission.radiance.end());
                }
            }
            const auto world = [&](Vec p) {
                Vec q{};
                for (unsigned a = 0; a < 3; ++a)
                    q[a] = instance.matrix[4 * a] * p[0] + instance.matrix[4 * a + 1] * p[1] +
                           instance.matrix[4 * a + 2] * p[2] + instance.matrix[4 * a + 3];
                return q;
            };
            const auto vertex = [&](wire::Word i) {
                const auto &p = packed_positions[instance.vertex_begin + i];
                return world({p.x, p.y, p.z});
            };
            const auto sub = [](Vec a, Vec b) {
                return Vec{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
            };
            const auto norm = [](Vec v) { return std::hypot(v[0], v[1], v[2]); };
            const auto append = [&](std::span<const Vec> corners, double area, Vec normal,
                                    std::size_t primitive, bool flat) {
                LightSamplingInput input;
                input.low = {INFINITY, INFINITY, INFINITY};
                input.high = {-INFINITY, -INFINITY, -INFINITY};
                for (const auto &p : corners)
                    for (unsigned a = 0; a < 3; ++a) {
                        input.low[a] = std::min(input.low[a], static_cast<float>(p[a]));
                        input.high[a] = std::max(input.high[a], static_cast<float>(p[a]));
                    }
                double factor = 1;
                if (instance.area_power_begin != UINT32_MAX)
                    factor =
                        std::bit_cast<float>(packed_data[instance.area_power_begin + primitive]);
                input.power =
                    std::numbers::pi * area * mean * factor * (emission.two_sided ? 2 : 1);
                // Bounds().phi intentionally omits Phi()'s two-sided factor.
                input.bounds_power = std::numbers::pi * area * bound_mean * factor;
                input.two_sided = emission.two_sided;
                const double length = norm(normal);
                if (flat && length > 0) {
                    for (unsigned a = 0; a < 3; ++a)
                        input.direction[a] = static_cast<float>(normal[a] / length);
                    input.cos_outer = std::nextafter(1.f, 0.f);
                    input.two_sided = emission.two_sided;
                }
                area_inputs.push_back(input);
                return std::ref(area_inputs.back());
            };
            for (wire::Word i = 0; i < instance.triangle_count; ++i) {
                const auto &t = packed_triangles[instance.triangle_begin + i];
                const std::array<Vec, 3> p{vertex(t.vertices[0]), vertex(t.vertices[1]),
                                           vertex(t.vertices[2])};
                auto n = cross(sub(p[1], p[0]), sub(p[2], p[0]));
                if (instance.reverse_orientation)
                    for (auto &v : n)
                        v = -v;
                auto &entry = append(p, .5 * norm(n), n, i, true).get();
                if (instance.normal_begin != wire::no_normals &&
                    emission.gltf_material == UINT32_MAX) {
                    const auto *values = static_cast<const wire::Point *>(normals.contents);
                    bool positive = false, negative = false;
                    for (unsigned j = 0; j < 3; ++j) {
                        const auto &v = values[instance.normal_begin + t.vertices[j]];
                        const double cosine = n[0] * v.x + n[1] * v.y + n[2] * v.z;
                        const double uncertainty =
                            8 * std::numeric_limits<float>::epsilon() * norm(n) *
                            std::hypot(double(v.x), double(v.y), double(v.z));
                        positive |= cosine >= -uncertainty;
                        negative |= cosine < uncertainty;
                    }
                    if (positive && negative)
                        entry.two_sided = true;
                    else if (negative)
                        for (auto &v : entry.direction)
                            v = -v;
                }
            }
            for (wire::Word i = 0; i < instance.patch_count; ++i) {
                const auto &patch = packed_patches[instance.patch_begin + i];
                std::array<Vec, 4> p;
                for (unsigned j = 0; j < 4; ++j)
                    p[j] = vertex(patch.vertices[j]);
                const double area = light_patch_area(p);
                auto &entry = append(p, area, {0, 0, 1}, instance.triangle_count + i, false).get();
                std::array<Vec, 4> ns{cross(sub(p[1], p[0]), sub(p[2], p[0])),
                                      cross(sub(p[1], p[0]), sub(p[3], p[1])),
                                      cross(sub(p[3], p[2]), sub(p[2], p[0])),
                                      cross(sub(p[3], p[2]), sub(p[3], p[1]))};
                if (instance.reverse_orientation)
                    for (auto &n : ns)
                        for (auto &v : n)
                            v = -v;
                // Authored normals can flip the geometric normal across the patch.
                // A two-sided cone of the underlying normals retains both orientations.
                if (instance.normal_begin != wire::no_normals &&
                    emission.gltf_material == UINT32_MAX) {
                    const auto *values = static_cast<const wire::Point *>(normals.contents);
                    bool positive = false, negative = false;
                    for (unsigned j = 0; j < 4; ++j) {
                        const auto &v = values[instance.normal_begin + patch.vertices[j]];
                        const double cosine = ns[j][0] * v.x + ns[j][1] * v.y + ns[j][2] * v.z;
                        const double uncertainty =
                            8 * std::numeric_limits<float>::epsilon() * norm(ns[j]) *
                            std::hypot(double(v.x), double(v.y), double(v.z));
                        positive |= cosine >= -uncertainty;
                        negative |= cosine < uncertainty;
                    }
                    if (positive && negative)
                        entry.two_sided = true;
                    else if (negative)
                        for (auto &n : ns)
                            for (auto &v : n)
                                v = -v;
                }
                std::array<Vec, 4> nonzero_normals;
                std::size_t count = 0;
                for (const auto &normal : ns)
                    if (norm(normal) > 0)
                        nonzero_normals[count++] = normal;
                light_normal_cone(entry, std::span(nonzero_normals).first(count));
            }

            const auto source = MetalGeometryAccess::source_instance(*geometry, ordinal);
            const auto &mesh = meshes[geometry_data.instances[source].mesh_index];
            for (wire::Word i = 0; i < instance.analytic_count; ++i) {
                const auto &shape = mesh.analytics[i];
                const auto bounds = analytic_bounds(shape);
                std::array<Vec, 8> corners;
                for (unsigned j = 0; j < 8; ++j)
                    corners[j] =
                        world({bounds[j & 1].x, bounds[(j >> 1) & 1].y, bounds[(j >> 2) & 1].z});
                double area = 0;
                // This integration is a proposal power estimate, never an emission normalization.
                for (unsigned y = 0; y < 16; ++y)
                    for (unsigned x = 0; x < 16; ++x) {
                        auto surface =
                            evaluate_analytic(shape, {float(x + .5f) / 16, float(y + .5f) / 16});
                        Vec du{surface.dpdu.x, surface.dpdu.y, surface.dpdu.z},
                            dv{surface.dpdv.x, surface.dpdv.y, surface.dpdv.z};
                        Vec wu{}, wv{};
                        for (unsigned a = 0; a < 3; ++a) {
                            wu[a] = instance.matrix[4 * a] * du[0] +
                                    instance.matrix[4 * a + 1] * du[1] +
                                    instance.matrix[4 * a + 2] * du[2];
                            wv[a] = instance.matrix[4 * a] * dv[0] +
                                    instance.matrix[4 * a + 1] * dv[1] +
                                    instance.matrix[4 * a + 2] * dv[2];
                        }
                        area += norm(cross(wu, wv)) / 256;
                    }
                GeometryTransform transform;
                for (unsigned j = 0; j < 12; ++j)
                    transform.values[j] = instance.matrix[j];
                if (const auto exact = analytic_world_area(shape, transform))
                    area = *exact;
                auto &entry = append(corners, area, {0, 0, 1},
                                     instance.triangle_count + instance.patch_count + i, false)
                                  .get();
                if (shape.kind == AnalyticKind::disk) {
                    Vec u{instance.matrix[0], instance.matrix[4], instance.matrix[8]};
                    Vec v{instance.matrix[1], instance.matrix[5], instance.matrix[9]};
                    auto normal = cross(u, v);
                    // Match analytic_surface_point: the cofactor already carries
                    // handedness, and the packed orientation supplies the only flip.
                    if (instance.reverse_orientation != 0)
                        for (auto &x : normal)
                            x = -x;
                    const std::array<Vec, 1> normals{normal};
                    light_normal_cone(entry, normals);
                }
                // Sphere/cylinder NormalBounds are full sphere in pinned PBRT too.
            }
        }
        if (auto error = upload_auxiliary_data(scene, packed_data, area_inputs); !error.empty())
            return error;
        instances = upload(device, packed_instances);
        positions = upload(device, packed_positions);
        triangles = upload(device, packed_triangles);
        patches = upload(device, packed_patches);
        materials = upload(device, packed_materials);
        knots = upload(device, packed_knots);
        if (instances == nil || positions == nil || triangles == nil || materials == nil ||
            knots == nil || patches == nil || material_data == nil) {
            return "Metal render shading buffer allocation failed";
        }
        return upload_graph(scene);
    }

    RenderSamplesResult sample(const RenderRequest &request) {
        RenderSamplesResult result;
        result.diagnostic = validate_request(request);
        if (!result.diagnostic.empty()) {
            return result;
        }
        if (parameters.packet_transport && parameters.bidirectional && parameters.max_depth > 0 &&
            request.sampler.kind == SamplerKind::halton) {
            result.diagnostic = validate_halton_light_range(
                request.width, request.height, request.sample_begin, request.sample_count);
            if (!result.diagnostic.empty())
                return result;
        }
        if (parameters.packet_transport && request.sample_count != 1) {
            result.diagnostic = "Packet requests require one absolute sample per dispatch";
            return result;
        }
        const auto depth_limit = sampler_max_depth(request.sampler, dielectrics);
        if (request.sampler.kind != SamplerKind::independent &&
            (parameters.model != wire::pbrt_model || parameters.max_depth > depth_limit)) {
            result.diagnostic = "QMC sampling requires PBRT rendering with maxdepth at most " +
                                std::to_string(depth_limit);
            return result;
        }
        if (parameters.filter_kind != wire::box_filter || parameters.filter_x_radius != 0.5f ||
            parameters.filter_y_radius != 0.5f) {
            const RasterCamera camera{unpack(parameters.origin), unpack(parameters.forward),
                                      unpack(parameters.right), unpack(parameters.up)};
            const PixelFilter filter{static_cast<PixelFilterKind>(parameters.filter_kind),
                                     parameters.filter_x_radius, parameters.filter_y_radius,
                                     parameters.filter_sigma};
            result.diagnostic =
                validate_raster_support(camera, filter, request.width, request.height);
            if (!result.diagnostic.empty()) {
                return result;
            }
        }
        if (parameters.lens_radius != 0) {
            const RasterCamera camera{unpack(parameters.origin), unpack(parameters.forward),
                                      unpack(parameters.right), unpack(parameters.up)};
            const PerspectiveLens lens{parameters.lens_radius, parameters.focal_distance,
                                       unpack(parameters.lens_x), unpack(parameters.lens_y)};
            const PixelFilter filter{static_cast<PixelFilterKind>(parameters.filter_kind),
                                     parameters.filter_x_radius, parameters.filter_y_radius,
                                     parameters.filter_sigma};
            result.diagnostic =
                validate_lens_support(camera, lens, filter, request.width, request.height);
            if (!result.diagnostic.empty()) {
                return result;
            }
        }
        const auto max_width = metal::group_width(device, pipeline);
        const auto group_width =
            request.threadgroup_width == 0 ? max_width : request.threadgroup_width;
        if (group_width == 0 || group_width > max_width) {
            result.diagnostic = "Metal render threadgroup width exceeds pipeline or device limit";
            return result;
        }

        auto p = parameters;
        auto texture_p = texture_parameters;
        if (texture_p.texture_count != 0 || graph_parameters.node_count != 0) {
            if (request.total_samples == 0 || request.sample_begin >= request.total_samples ||
                request.sample_count > request.total_samples - request.sample_begin) {
                result.diagnostic = "Image texture request requires a fixed total sample budget "
                                    "containing the sample range";
                return result;
            }
            if (texture_width != request.width || texture_height != request.height) {
                TextureCameraDifferentials differentials;
                result.diagnostic = prepare_texture_camera_differentials(
                    unpack(parameters.forward), unpack(parameters.right), unpack(parameters.up),
                    request.width, request.height, differentials);
                if (!result.diagnostic.empty())
                    return result;
                texture_parameters.minimum_dx = pack(differentials.minimum_dx);
                texture_parameters.minimum_dy = pack(differentials.minimum_dy);
                texture_width = request.width;
                texture_height = request.height;
            }
            texture_p = texture_parameters;
            texture_p.spp_scale = static_cast<float>(
                std::max(0.125, 1.0 / std::sqrt(static_cast<double>(request.total_samples))));
        }
        p.width = request.width;
        p.height = request.height;
        p.pixel_begin = request.pixel_begin;
        p.pixel_count = request.pixel_count;
        p.sample_low = static_cast<wire::Word>(request.sample_begin);
        p.sample_high = static_cast<wire::Word>(request.sample_begin >> 32);
        p.seed_low = static_cast<wire::Word>(request.seed);
        p.seed_high = static_cast<wire::Word>(request.seed >> 32);
        p.material_seed = request.material_seed;
        p.emergency_limit = request.emergency_limit;
        p.boundary_limit = request.boundary_limit;
        p.lane_count = request.pixel_count * request.sample_count;
        p.sampler_kind = static_cast<wire::Word>(request.sampler.kind);
        p.randomization = static_cast<wire::Word>(request.sampler.randomization);
        p.zsobol_log_spp = request.sampler.kind == SamplerKind::zsobol
                               ? static_cast<wire::Word>(std::bit_width(request.total_samples) - 1)
                               : 0;
        p.sobol_scale_exponent = (request.sampler.kind == SamplerKind::sobol ||
                                  request.sampler.kind == SamplerKind::zsobol)
                                     ? sobol_scale_exponent(request.width, request.height)
                                     : 0;

        const auto group_count = (p.lane_count + group_width - 1) / group_width;
        const std::size_t stride = p.packet_transport ? p.packet_lane_bytes : sizeof(wire::Sample);
        const std::size_t slots = p.packet_transport ? p.lane_count : group_count * group_width + 1;
        const std::size_t guard_bytes = p.packet_transport ? 64 : 0;
        if (p.packet_transport &&
            (stride == 0 || p.lane_count > packet_wire::workspace_bytes / stride)) {
            result.diagnostic = "Packet dispatch exceeds the 256 MiB workspace capacity";
            return result;
        }
        const auto bytes = slots * stride + guard_bytes;
        if (bytes > device.maxBufferLength) {
            result.diagnostic = "Metal render output exceeds device buffer limit";
            return result;
        }
        if (output.length < bytes) {
            output = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        }
        auto *output_bytes = static_cast<unsigned char *>(output.contents);
        if (output_bytes == nullptr) {
            result.diagnostic = "Metal render output buffer allocation failed";
            return result;
        }
        std::memset(output_bytes, 0xff, bytes);

        id<MTLCommandBuffer> command = [queue commandBuffer];
        if (command == nil) {
            result.diagnostic = "Metal render command buffer creation failed";
            return result;
        }
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        if (encoder == nil) {
            result.diagnostic = "Metal render compute encoder creation failed";
            return result;
        }
        [encoder setComputePipelineState:pipeline];
        [encoder setBytes:&p length:sizeof(p) atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        if (p.instance_count != 0 || p.packet_transport) {
            if (empty_packet_structure != nil)
                [encoder setAccelerationStructure:empty_packet_structure atBufferIndex:2];
            else
                MetalGeometryAccess::bind(*geometry, encoder, 2);
            [encoder setBuffer:instances offset:0 atIndex:3];
            [encoder setBuffer:positions offset:0 atIndex:4];
            [encoder setBuffer:triangles offset:0 atIndex:5];
            [encoder setBuffer:materials offset:0 atIndex:6];
            [encoder setBuffer:knots offset:0 atIndex:8];
            [encoder setBuffer:area_emissions offset:0 atIndex:9];
            [encoder setBuffer:emitters offset:0 atIndex:10];
            [encoder setBuffer:normals offset:0 atIndex:11];
            [encoder setBuffer:patches offset:0 atIndex:13];
            if (p.packet_transport) {
                if (empty_packet_structure != nil)
                    [encoder setAccelerationStructure:empty_packet_structure atBufferIndex:14];
                else
                    MetalGeometryAccess::bind_hint(*geometry, encoder, 14);
            } else
                [encoder setIntersectionFunctionTable:intersection_table atBufferIndex:14];
            [encoder setBuffer:texture_uvs offset:0 atIndex:15];
            [encoder setBuffer:texture_frames offset:0 atIndex:16];
            [encoder setBuffer:image_textures offset:0 atIndex:17];
            [encoder setBuffer:image_levels offset:0 atIndex:18];
            [encoder setBuffer:image_texels offset:0 atIndex:19];
            [encoder setBuffer:image_transfer offset:0 atIndex:20];
            [encoder setBuffer:image_spectra offset:0 atIndex:21];
            [encoder setBytes:&texture_p length:sizeof(texture_p) atIndex:22];
            [encoder setBuffer:graph_nodes offset:0 atIndex:23];
            [encoder setBuffer:graph_knots offset:0 atIndex:24];
            [encoder setBytes:&graph_parameters length:sizeof(graph_parameters) atIndex:25];
            [encoder setBuffer:surface_vertices offset:0 atIndex:26];
            [encoder setBuffer:surface_transforms offset:0 atIndex:27];
            [encoder setBuffer:environment_cdf offset:0 atIndex:28];
            [encoder setBuffer:illuminant offset:0 atIndex:29];
            [encoder setBuffer:material_data offset:0 atIndex:30];
        }
        if (p.instance_count == 0 && texture_p.texture_count != 0) {
            [encoder setBuffer:image_textures offset:0 atIndex:17];
            [encoder setBuffer:image_levels offset:0 atIndex:18];
            [encoder setBuffer:image_texels offset:0 atIndex:19];
            [encoder setBuffer:image_transfer offset:0 atIndex:20];
            [encoder setBuffer:image_spectra offset:0 atIndex:21];
            [encoder setBytes:&texture_p length:sizeof(texture_p) atIndex:22];
            [encoder setBytes:&graph_parameters length:sizeof(graph_parameters) atIndex:25];
            [encoder setBuffer:illuminant offset:0 atIndex:29];
        }
        [encoder setBuffer:environment offset:0 atIndex:7];
        [encoder setBuffer:sampling_tables offset:0 atIndex:12];
        // The sky pipeline may retain this argument even when runtime medium
        // and light counts are zero. upload_auxiliary_data supplies a sentinel.
        if (p.instance_count == 0)
            [encoder setBuffer:material_data offset:0 atIndex:30];
        [encoder dispatchThreadgroups:MTLSizeMake(group_count, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(group_width, 1, 1)];
        [encoder endEncoding];
        result.diagnostic = metal::complete(command, "Metal render sample dispatch");
        if (!result.diagnostic.empty()) {
            return result;
        }

        const std::span output_data{output_bytes, bytes};
        if (!std::ranges::all_of(output_data.subspan(p.lane_count * stride),
                                 [](unsigned char byte) { return byte == 0xff; })) {
            result.diagnostic = "Metal render output guard mismatch";
            return result;
        }
        if (p.packet_transport) {
            std::vector<PacketSample> samples;
            std::vector<PacketSplat> splats;
            samples.reserve(p.lane_count);
            for (std::size_t lane = 0; lane < p.lane_count; ++lane) {
                packet_wire::Result value{};
                const auto *record = output_bytes + lane * stride;
                std::memcpy(&value, record, sizeof(value));
                if (value.status != wire::success) {
                    result.diagnostic = sample_error(value.status) + " at pixel " +
                                        std::to_string(request.pixel_begin + lane) + " sample " +
                                        std::to_string(request.sample_begin) + " (packet lane " +
                                        std::to_string(lane) + ")";
                    return result;
                }
                const std::uint64_t sample =
                    (std::uint64_t{value.sample_high} << 32) | value.sample_low;
                if (value.pixel != request.pixel_begin + lane || sample != request.sample_begin ||
                    value.splat_count > p.max_depth || !(value.filter_weight > 0) ||
                    !std::isfinite(value.filter_weight)) {
                    result.diagnostic =
                        "Invalid packet sample identity, splat count or filter weight";
                    return result;
                }
                PacketSample sample_value{};
                sample_value.pixel = value.pixel;
                sample_value.sample = sample;
                sample_value.filter_weight = value.filter_weight;
                sample_value.splat_begin = static_cast<std::uint32_t>(splats.size());
                sample_value.splat_count = value.splat_count;
                for (unsigned i = 0; i < 4; ++i) {
                    if (!(value.wavelengths[i] >= 360 && value.wavelengths[i] <= 830) ||
                        !std::isfinite(value.wavelength_pdfs[i]) || value.wavelength_pdfs[i] < 0 ||
                        (i == 0 && value.wavelength_pdfs[i] == 0) ||
                        !std::isfinite(value.radiance[i]) || value.radiance[i] < 0) {
                        result.diagnostic =
                            "Packet spectral result is outside the finite domain at pixel " +
                            std::to_string(value.pixel) + " sample " + std::to_string(sample) +
                            " packet lane " + std::to_string(lane) + " spectral lane " +
                            std::to_string(i) + " wavelength " +
                            std::to_string(value.wavelengths[i]) + " (bits " +
                            std::to_string(std::bit_cast<std::uint32_t>(value.wavelengths[i])) +
                            ") pdf " + std::to_string(value.wavelength_pdfs[i]) + " radiance " +
                            std::to_string(value.radiance[i]);
                        return result;
                    }
                    sample_value.wavelengths[i] = value.wavelengths[i];
                    sample_value.wavelength_pdfs[i] = value.wavelength_pdfs[i];
                    sample_value.radiance[i] = value.radiance[i];
                }
                for (unsigned i = 0; i < value.splat_count; ++i) {
                    packet_wire::Splat value_splat{};
                    std::memcpy(&value_splat, record + sizeof(value) + i * sizeof(value_splat),
                                sizeof(value_splat));
                    PacketSplat splat{};
                    for (unsigned axis = 0; axis < 2; ++axis) {
                        if (!std::isfinite(value_splat.raster[axis])) {
                            result.diagnostic = "Packet splat raster position is nonfinite";
                            return result;
                        }
                        splat.raster[axis] = value_splat.raster[axis];
                    }
                    for (unsigned component = 0; component < 4; ++component) {
                        if (!std::isfinite(value_splat.radiance[component]) ||
                            value_splat.radiance[component] < 0) {
                            result.diagnostic =
                                "Packet splat radiance is outside the finite domain";
                            return result;
                        }
                        splat.radiance[component] = value_splat.radiance[component];
                    }
                    splat.strategy = value_splat.strategy;
                    splats.push_back(splat);
                }
                samples.push_back(sample_value);
            }
            result.packet_samples = std::move(samples);
            result.packet_splats = std::move(splats);
            result.guard_count = 1;
            result.status = GeometryStatus::success;
            return result;
        }
        std::vector<RenderSample> samples;
        samples.reserve(p.lane_count);
        for (std::size_t lane = 0; lane < p.lane_count; ++lane) {
            wire::Sample value{};
            std::memcpy(&value, output_bytes + lane * sizeof(value), sizeof(value));
            if (value.status != wire::success) {
                result.diagnostic = sample_error(value.status) + " at lane " + std::to_string(lane);
                return result;
            }
            const std::uint64_t sample =
                (std::uint64_t{value.sample_high} << 32) | value.sample_low;
            if (value.pixel != request.pixel_begin + lane % request.pixel_count ||
                sample != request.sample_begin + lane / request.pixel_count) {
                result.diagnostic =
                    "Metal render sample identity mismatch at lane " + std::to_string(lane);
                return result;
            }
            if (!std::isfinite(value.wavelength) || value.wavelength < spectral_knots.front() ||
                value.wavelength > spectral_knots.back() || !std::isfinite(value.radiance) ||
                value.radiance < 0 || value.scatterings > request.emergency_limit ||
                (p.model == wire::pbrt_model && value.scatterings > p.max_depth)) {
                result.diagnostic =
                    "Metal render sample values are invalid at lane " + std::to_string(lane);
                return result;
            }
            samples.push_back({value.pixel,
                               sample,
                               value.wavelength,
                               value.radiance,
                               value.scatterings,
                               {value.random_words[0], value.random_words[1], value.random_words[2],
                                value.random_words[3]}});
        }
        result.samples = std::move(samples);
        result.guard_count = slots - p.lane_count;
        result.status = GeometryStatus::success;
        return result;
    }
};

MetalRenderScene::MetalRenderScene(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MetalRenderScene::~MetalRenderScene() = default;
MetalRenderScene::MetalRenderScene(MetalRenderScene &&) noexcept = default;
MetalRenderScene &MetalRenderScene::operator=(MetalRenderScene &&) noexcept = default;

RenderSamplesResult MetalRenderScene::sample(const RenderRequest &request) {
    @autoreleasepool {
        if (!impl_) {
            return {.diagnostic = "Metal render scene is moved-from", .samples = {}};
        }
        return impl_->sample(request);
    }
}

namespace {
// Scan all materials, including unused entries and mix children. Prepared
// native placements copy the validated per-source invisible-emitter flags.
bool packet_visibility_requires_closest(const RenderScene &scene) {
    return std::ranges::any_of(scene.materials,
                               [](const auto &material) {
                                   return material.scattering == ScatteringKind::null_interface;
                               }) ||
           std::ranges::any_of(scene.instance_invisible_emitters,
                               [](std::uint32_t invisible) { return invisible != 0; });
}
} // namespace

RenderBuildResult build_metal_renderer(const RenderScene &scene,
                                       const std::filesystem::path &library_path) {
    @autoreleasepool {
        RenderBuildResult result;
        try {
            result.diagnostic = validate_render_scene(scene);
        } catch (const std::bad_alloc &) {
            result.diagnostic = "Metal render validation allocation failed";
        }
        if (!result.diagnostic.empty()) {
            return result;
        }
        auto parameters = camera_parameters(scene);
        if (parameters.packet_transport) {
            if (scene.max_depth > packet_wire::max_depth || !(parameters.camera_image_area > 0) ||
                !std::isfinite(parameters.camera_image_area) || !finite(parameters.camera_normal) ||
                !(parameters.camera_plane_depth > 0) ||
                !std::isfinite(parameters.camera_plane_depth) ||
                !(parameters.packet_filter_weight > 0) ||
                !std::isfinite(parameters.packet_filter_weight)) {
                result.diagnostic =
                    "Packet camera, filter or depth exceeds supported numerical range";
                return result;
            }
            result.diagnostic = prepare_packet_scene_sphere(scene, parameters);
            if (!result.diagnostic.empty())
                return result;
        }
        if (!parameters.packet_transport) {
            result.diagnostic = prepare_packet_scene_sphere(scene, parameters);
            if (!result.diagnostic.empty())
                return result;
        }
        if (scene.model == RenderModel::demo &&
            (!(parameters.film_scale > 0.0f) || !std::isfinite(parameters.film_scale) ||
             !finite(parameters.forward) || !finite(parameters.right) || !finite(parameters.up))) {
            result.diagnostic = "Metal render camera basis or field of view is not representable";
            return result;
        }

        if (scene.preparation_byte_limit != std::numeric_limits<std::size_t>::max()) {
            std::size_t estimate = 0;
            result.diagnostic = validate_render_preparation_budget(scene, &estimate);
            if (!result.diagnostic.empty())
                return result;
            id<MTLDevice> budget_device = MTLCreateSystemDefaultDevice();
            if (budget_device != nil && budget_device.recommendedMaxWorkingSetSize > 0 &&
                estimate > budget_device.recommendedMaxWorkingSetSize) {
                result.diagnostic = "Estimated shading preparation exceeds the device recommended "
                                    "working-set budget";
                return result;
            }
        }

        // A transformed emitter's native intersection can be ill-conditioned
        // even when its world triangle and connection ray are well-conditioned.
        // Use the same baked positions for both traversal and light sampling.
        GeometryData prepared;
        const auto emission_ordinals =
            std::views::iota(std::size_t{0}, scene.instance_emissions.size());
        if (std::ranges::any_of(emission_ordinals, [&](std::size_t source) {
                return requires_world_space_emitter(scene, source);
            })) {
            try {
                result.diagnostic = prepare_world_space_emitters(scene, prepared);
            } catch (const std::bad_alloc &) {
                result.diagnostic = "Metal world-space emitter preparation allocation failed";
            }
            if (!result.diagnostic.empty()) {
                return result;
            }
        }
        const bool alpha = filtered_visibility(scene);
        if (alpha) {
            if (prepared.instances.empty()) {
                prepared = scene.geometry;
            }
            for (std::size_t i = 0; i < prepared.instances.size(); ++i) {
                prepared.instances[i].non_opaque =
                    prepared.instances[i].non_opaque || scene.geometry.instances[i].non_opaque ||
                    (!scene.instance_alpha_roots.empty() &&
                     scene.instance_alpha_roots[i] != max_index) ||
                    gltf_visibility(scene.materials[scene.instance_materials[i]]);
            }
        }
        const auto &geometry_data = prepared.instances.empty() ? scene.geometry : prepared;
        // Global exclusions are deliberately conservative, including unused/mix children.
        // volume_transport by itself does not imply a medium interface (vacuum is eligible).
        const bool hint_passthrough = packet_visibility_requires_closest(scene);
        const bool hint_complex_material = std::ranges::any_of(scene.materials, [](const auto &m) {
            return m.scattering == ScatteringKind::mix || m.scattering == ScatteringKind::gltf;
        });
        const bool hint_medium_boundary = std::ranges::any_of(
            scene.instance_media, [](const auto &m) { return m.inside != m.outside; });
        std::size_t opaque_triangle_sources = 0;
        for (const auto &instance : geometry_data.instances) {
            if (!geometry_data.meshes[instance.mesh_index].triangles.empty()) {
                if (!instance.non_opaque)
                    ++opaque_triangle_sources;
            }
        }
        const bool hint_eligible = parameters.packet_transport && !hint_passthrough &&
                                   !hint_complex_material && !hint_medium_boundary &&
                                   opaque_triangle_sources != 0;
        if (hint_eligible) {
            std::size_t base_estimate = 0;
            result.diagnostic = validate_render_preparation_budget(scene, &base_estimate);
            if (!result.diagnostic.empty())
                return result;
            const auto budget =
                native_hint_budget(geometry_data, base_estimate, scene.preparation_byte_limit);
            if (!budget.valid) {
                result.diagnostic = "Native hint inputs exceed configured preparation byte budget "
                                    "(AS/driver scratch/pipelines excluded)";
                return result;
            }
            auto budget_device = MTLCreateSystemDefaultDevice();
            if (budget_device != nil && budget_device.recommendedMaxWorkingSetSize > 0 &&
                budget.combined > budget_device.recommendedMaxWorkingSetSize) {
                result.diagnostic =
                    "Native hint preparation estimate exceeds device recommended working-set size";
                return result;
            }
        }
        auto built = build_metal_geometry_with_hints(geometry_data, library_path, hint_eligible);
        result.device = std::move(built.device);
        if (built.status != GeometryStatus::success) {
            result.status = built.status;
            result.diagnostic = std::move(built.diagnostic);
            return result;
        }

        auto impl = std::make_unique<MetalRenderScene::Impl>();
        impl->geometry = std::move(built.scene);
        if (hint_eligible && !MetalGeometryAccess::has_hint(*impl->geometry)) {
            result.diagnostic = "Required native triangle hint acceleration structure is missing";
            return result;
        }
        impl->native_hint_mode = hint_eligible ? 3u : 0u;
        impl->device = MetalGeometryAccess::device(*impl->geometry);
        impl->queue = MetalGeometryAccess::queue(*impl->geometry);
        if (impl->queue == nil) {
            impl->queue = [impl->device newCommandQueue];
        }
        if (impl->queue == nil) {
            result.diagnostic = "Metal render command queue creation failed";
            return result;
        }
        impl->parameters = parameters;
        impl->dielectrics = scene_has_dielectrics(scene);
        impl->parameters.instance_count =
            static_cast<wire::Word>(MetalGeometryAccess::instance_count(*impl->geometry));

        id<MTLLibrary> library = metal::load_library(impl->device, library_path, result.diagnostic);
        if (library == nil) {
            return result;
        }
        if (!verify_shader_contract(impl->device, impl->queue, library, result.diagnostic)) {
            return result;
        }
        NSString *entry = impl->parameters.packet_transport
                              ? @"rypath_render_packets"
                              : (impl->parameters.instance_count == 0 ? @"rypath_render_sky"
                                                                      : @"rypath_render_paths");
        id<MTLFunction> intersection = nil;
        if (impl->parameters.instance_count != 0 || impl->parameters.packet_transport) {
            intersection = metal::load_patch_intersection(library, result.diagnostic);
            if (intersection == nil) {
                return result;
            }
        }
        const bool features[] = {
            !scene.texture_graph.nodes.empty(),
            filtered_visibility(scene),
            std::ranges::any_of(scene.materials,
                                [](const auto &m) {
                                    return m.bump_root != max_index || m.normal_map != max_index ||
                                           m.scattering == ScatteringKind::gltf;
                                }),
            scene.environment_image != max_index,
            std::ranges::any_of(scene.area_emissions,
                                [](const auto &e) { return e.image_texture != max_index; }),
            std::ranges::any_of(
                scene.materials,
                [](const auto &m) { return m.scattering == ScatteringKind::conductor; }),
            impl->dielectrics,
            std::ranges::any_of(scene.materials,
                                [](const auto &m) {
                                    return m.scattering >= ScatteringKind::thin_dielectric &&
                                           m.scattering != ScatteringKind::null_interface &&
                                           m.scattering != ScatteringKind::gltf;
                                }),
            scene.volume_transport || std::ranges::any_of(scene.materials,
                                                          [](const auto &m) {
                                                              return m.scattering ==
                                                                     ScatteringKind::null_interface;
                                                          }),
            std::ranges::any_of(scene.materials,
                                [](const auto &m) { return m.scattering == ScatteringKind::gltf; }),
            !scene.point_lights.empty() || !scene.spot_lights.empty() ||
                !scene.goniometric_lights.empty() || !scene.projection_lights.empty(),
            !scene.distant_lights.empty(),
            !scene.goniometric_lights.empty() || !scene.projection_lights.empty()};
        MTLFunctionConstantValues *constants = [[MTLFunctionConstantValues alloc] init];
        for (NSUInteger i = 0; i < std::size(features); ++i)
            [constants setConstantValue:&features[i] type:MTLDataTypeBool atIndex:i];
        if (impl->parameters.packet_transport) {
            // Scan every material, including children reached through mix nodes.
            const ScatteringKind families[] = {ScatteringKind::thin_dielectric,
                                               ScatteringKind::diffuse_transmission,
                                               ScatteringKind::measured, ScatteringKind::fourier};
            for (NSUInteger i = 0; i < std::size(families); ++i) {
                const bool enabled = std::ranges::any_of(
                    scene.materials, [&](const auto &m) { return m.scattering == families[i]; });
                [constants setConstantValue:&enabled type:MTLDataTypeBool atIndex:13 + i];
            }
            const bool coated = std::ranges::any_of(scene.materials, [](const auto &m) {
                return m.scattering == ScatteringKind::coated_diffuse ||
                       m.scattering == ScatteringKind::coated_conductor;
            });
            [constants setConstantValue:&coated type:MTLDataTypeBool atIndex:17];
            const bool passthrough = packet_visibility_requires_closest(scene);
            [constants setConstantValue:&passthrough type:MTLDataTypeBool atIndex:18];
            [constants setConstantValue:&impl->native_hint_mode type:MTLDataTypeUInt atIndex:19];
        }
        const auto pipeline_start = std::chrono::steady_clock::now();
        impl->pipeline = metal::load_pipeline(impl->device, library, entry, result.diagnostic,
                                              intersection, constants);
        if (impl->pipeline == nil) {
            return result;
        }
        if (intersection != nil) {
            impl->intersection_table =
                metal::make_intersection_table(impl->pipeline, intersection, result.diagnostic);
            if (impl->intersection_table == nil) {
                return result;
            }
        }
        result.pipeline_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - pipeline_start)
                .count();
        impl->environment = upload(impl->device, scene.environment);
        if (impl->environment == nil) {
            result.diagnostic = "Metal render environment buffer allocation failed";
            return result;
        }
        const auto &tables = sobol_tables();
        std::vector<wire::Word> sampling_words(sizeof(tables) / sizeof(wire::Word));
        std::memcpy(sampling_words.data(), &tables, sizeof(tables));
        if (scene.filter.kind == PixelFilterKind::gaussian) {
            auto compiled = compile_gaussian_filter(scene.filter);
            if (!compiled.diagnostic.empty()) {
                result.diagnostic = compiled.diagnostic;
                return result;
            }
            const auto &distribution = compiled.distribution;
            impl->parameters.gaussian_width = distribution.width;
            impl->parameters.gaussian_height = distribution.height;
            if (impl->parameters.packet_transport)
                impl->parameters.packet_filter_weight = distribution.integral;
            for (const auto value : distribution.conditional_cdf)
                sampling_words.push_back(std::bit_cast<wire::Word>(value));
            for (const auto value : distribution.marginal_cdf)
                sampling_words.push_back(std::bit_cast<wire::Word>(value));
        }
        if (sampling_words.size() > impl->device.maxBufferLength / sizeof(wire::Word)) {
            result.diagnostic = "Metal sampling tables exceed device buffer limit";
            return result;
        }
        impl->sampling_tables = upload(impl->device, sampling_words);
        if (impl->sampling_tables == nil) {
            result.diagnostic = "Metal Sobol table buffer allocation failed";
            return result;
        }
        if (impl->parameters.instance_count == 0) {
            std::vector<wire::Word> data;
            result.diagnostic = impl->upload_auxiliary_data(scene, data);
            if (!result.diagnostic.empty())
                return result;
        }
        if (impl->parameters.instance_count != 0 || !scene.image_textures.empty() ||
            impl->parameters.packet_transport) {
            try {
                if (impl->parameters.instance_count != 0)
                    result.diagnostic = impl->upload_shading(scene, geometry_data);
                else {
                    std::vector<wire::Instance> empty_instances;
                    result.diagnostic = impl->upload_textures(scene, empty_instances);
                    if (result.diagnostic.empty())
                        result.diagnostic = impl->upload_graph(scene);
                }
            } catch (const std::bad_alloc &) {
                result.diagnostic = "Metal render shading preparation allocation failed";
            }
            if (!result.diagnostic.empty()) {
                return result;
            }
        }

        if (impl->parameters.packet_transport && impl->parameters.instance_count == 0) {
            impl->empty_packet_structure =
                build_empty_packet_structure(impl->device, impl->queue, result.diagnostic);
            if (impl->empty_packet_structure == nil)
                return result;
            // The generic packet kernel retains the full binding layout even
            // when its empty-scene branch never traverses geometry. Supply valid
            // read-only storage for every buffer argument; no scene data is invented.
            const auto sentinel = [impl->device newBufferWithLength:2048
                                                            options:MTLResourceStorageModeShared];
            if (sentinel == nil) {
                result.diagnostic = "Packet empty-scene sentinel allocation failed";
                return result;
            }
            std::memset(sentinel.contents, 0, sentinel.length);
            for (auto *buffer :
                 {&impl->instances,        &impl->positions,          &impl->triangles,
                  &impl->patches,          &impl->materials,          &impl->knots,
                  &impl->area_emissions,   &impl->emitters,           &impl->normals,
                  &impl->texture_uvs,      &impl->texture_frames,     &impl->image_textures,
                  &impl->image_levels,     &impl->image_texels,       &impl->image_transfer,
                  &impl->image_spectra,    &impl->graph_nodes,        &impl->graph_knots,
                  &impl->surface_vertices, &impl->surface_transforms, &impl->environment_cdf,
                  &impl->illuminant,       &impl->material_data}) {
                if (*buffer == nil)
                    *buffer = sentinel;
            }
        }
        result.scene.reset(new MetalRenderScene(std::move(impl)));
        result.status = GeometryStatus::success;
        return result;
    }
}

ShadingNormalProbeResult
probe_shading_normal(const std::vector<shading_normal_probe::Query> &queries,
                     const std::filesystem::path &library_path) {
    ShadingNormalProbeResult result;
    if (queries.empty() || queries.size() > 65536) {
        result.diagnostic = "Shading normal probe requires 1..65536 inputs";
        return result;
    }
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            result.status = MetalStatus::no_device;
            result.diagnostic = "No Metal device";
            return result;
        }
        auto library = metal::load_library(device, library_path, result.diagnostic);
        if (library == nil)
            return result;
        auto pipeline =
            metal::load_pipeline(device, library, @"shading_normal_probe", result.diagnostic);
        if (pipeline == nil)
            return result;
        auto queue = [device newCommandQueue];
        constexpr std::size_t guard_bytes = 64;
        const auto payload = queries.size() * sizeof(shading_normal_probe::Result);
        auto input = [device newBufferWithBytes:queries.data()
                                         length:queries.size() * sizeof(queries[0])
                                        options:MTLResourceStorageModeShared];
        auto output = [device newBufferWithLength:payload + 2 * guard_bytes
                                          options:MTLResourceStorageModeShared];
        if (queue == nil || input == nil || output == nil) {
            result.diagnostic = "Shading normal probe resource allocation failed";
            return result;
        }
        std::memset(output.contents, 0xa5, payload + 2 * guard_bytes);
        const auto count = static_cast<std::uint32_t>(queries.size());
        auto command = [queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        if (command == nil || encoder == nil) {
            result.diagnostic = "Shading normal probe encoder creation failed";
            return result;
        }
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:output offset:guard_bytes atIndex:1];
        [encoder setBytes:&count length:sizeof(count) atIndex:2];
        // Include inactive lanes to exercise the kernel's bounds check.
        const auto width = metal::group_width(device, pipeline);
        const auto groups = (count + width - 1) / width;
        [encoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        [encoder endEncoding];
        result.diagnostic = metal::complete(command, "shading normal probe");
        if (!result.diagnostic.empty())
            return result;
        const auto *bytes = static_cast<const unsigned char *>(output.contents);
        for (std::size_t i = 0; i < guard_bytes; ++i) {
            if (bytes[i] != 0xa5 || bytes[guard_bytes + payload + i] != 0xa5) {
                result.diagnostic = "Shading normal probe output guard overwritten";
                return result;
            }
        }
        result.values.resize(count);
        std::memcpy(result.values.data(), bytes + guard_bytes, payload);
        result.guard_count = 2 * guard_bytes;
        result.status = MetalStatus::success;
        return result;
    }
}

} // namespace rypath
