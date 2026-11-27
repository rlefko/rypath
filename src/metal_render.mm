// SPDX-License-Identifier: GPL-3.0-only

#include "metal_render.hpp"
#include "analytic.hpp"
#include "area_evaluation_context.hpp"
#include "area_pair_atlas.hpp"
#include "area_stencil_shared.h"
#include "area_wire.h"
#include "bsdf_table.hpp"
#include "checkpoint_contract.h"
#include "diffuse_event_eligibility.hpp"
#include "enhanced_duplication_wire.h"
#include "enhanced_motion_host.hpp"
#include "enhanced_wire.h"
#include "frame_session.hpp"
#include "frame_session_test_access.hpp"
#include "frame_wire.h"
#include "metal_gpu_diagnostics.hpp"
#include "native_hint_budget.hpp"
#include "render_timings.hpp"
#include "wavefront_wire.h"

#include "environment_sampling.hpp"
#include "gaussian_filter.hpp"
#include "light_sampling.hpp"
#include "metal_acceleration_build.hpp"
#include "metal_execution.hpp"
#include "metal_frame_access.hpp"
#include "metal_geometry_access.hpp"
#include "opaque_shadow_capability.hpp"
#include "packet_dispatch.hpp"
#include "packet_spectrum_shared.h"
#include "packet_wire.h"
#include "reconstruction_continuity.hpp"
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
#include <condition_variable>
#include <cstring>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <numbers>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace rypath {
namespace {

namespace wire = render_wire;
namespace edw = enhanced_duplication_wire;
static_assert(static_cast<unsigned>(FrameAreaDuplication::off) == edw::shared::off &&
              static_cast<unsigned>(FrameAreaDuplication::initial_seed) ==
                  edw::shared::initial_seed &&
              static_cast<unsigned>(FrameAreaDuplication::initial_atom) ==
                  edw::shared::initial_atom &&
              static_cast<unsigned>(FrameAreaDuplication::current) == edw::shared::current);
bool frame_duplication_enabled(const FrameConfig &c) {
    return c.area_duplication != FrameAreaDuplication::off;
}
// Enhanced cross-target cells serialize a FLOAT mantissa and a signed exponent.
constexpr std::size_t enhanced_cross_target_bytes = sizeof(float) + sizeof(std::int32_t);
static_assert(enhanced_cross_target_bytes == 8);
constexpr std::size_t area_cross_target_pixel_bytes(FrameAreaPathDomain domain) {
    return 4 *
           (domain == FrameAreaPathDomain::enhanced ? enhanced_cross_target_bytes : sizeof(float));
}
bool frame_suffix_cache_enabled(const FrameConfig &config) {
    return config.estimator == FrameEstimator::area_replay &&
           config.area_path_domain == FrameAreaPathDomain::enhanced &&
           config.contribution_replay == FrameContributionReplay::prefix &&
           config.enhanced_suffix_cache;
}
bool frame_pair_compaction_enabled(const FrameConfig &config) {
    return config.execution == FrameExecution::wavefront &&
           config.estimator == FrameEstimator::area_replay &&
           config.area_path_domain == FrameAreaPathDomain::enhanced &&
           config.compact_enhanced_pairs;
}
bool frame_motion_enabled(const FrameConfig &config) {
    return config.execution == FrameExecution::wavefront &&
           config.estimator == FrameEstimator::area_replay &&
           config.area_path_domain == FrameAreaPathDomain::enhanced &&
           config.enhanced_temporal_reprojection;
}
namespace motion_wire = enhanced_motion_wire;
namespace paired = rypath_area_pair_atlas;
bool frame_paired_atlas_enabled(const FrameConfig &config) {
    return config.execution == FrameExecution::wavefront &&
           config.estimator == FrameEstimator::area_replay &&
           config.area_stencil == FrameAreaStencil::paired_gaussian16;
}
static_assert(sizeof(paired::Entry) == 2 && sizeof(paired::Config) == 112 &&
              paired::tile_count == 4 && paired::total_entries * sizeof(paired::Entry) == 398304);
static_assert(area_wire::paired_atlas_id == 15 && area_wire::paired_config_id == 16);
static_assert(frame_area_canonical_version(FrameAreaPathDomain::enhanced) ==
                  contribution_wire::enhanced_canonical_law &&
              frame_area_discovery_version(FrameAreaPathDomain::enhanced) ==
                  contribution_wire::enhanced_discovery_law);
static_assert(wavefront_wire::enhanced_records_id == 20 &&
                  wavefront_wire::enhanced_evaluation_id == 21 &&
                  area_wire::enhanced_records_id == 11 && area_wire::enhanced_evaluation_id == 12 &&
                  wavefront_wire::enhanced_suffix_id == 22 && area_wire::enhanced_suffix_id == 13 &&
                  wavefront_wire::work_list_id == 23 && wavefront_wire::donors_id == 24 &&
                  area_wire::donors_id == 14,
              "Enhanced owned-record argument bindings");

// Execute the contract in the actual loaded library. A matching entry-point
// name alone cannot establish compatibility with a stale/custom metallib.
bool verify_shader_contract(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLLibrary> library,
                            std::string &diagnostic, bool native = false) {
    MTLFunctionConstantValues *constants = nil;
    if (native) {
        constants = [[MTLFunctionConstantValues alloc] init];
        [constants setConstantValue:&native type:MTLDataTypeBool atIndex:21];
    }
    auto pipeline = metal::load_pipeline(device, library,
                                         native ? @"rypath_native_intersection_contract_v2"
                                                : @"rypath_checkpoint_contract",
                                         diagnostic, nil, constants);
    if (pipeline == nil) {
        diagnostic =
            "Metal shader compatibility handshake unavailable; rebuild the metallib: " + diagnostic;
        return false;
    }
    const std::array<std::uint32_t, 4> expected =
        native ? std::array<std::uint32_t, 4>{geometry_wire::native_metadata_contract, 1u,
                                              sizeof(geometry_wire::NativeTriangleTag),
                                              sizeof(geometry_wire::ProceduralRecord)}
               : std::array<std::uint32_t, 4>{
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
    result.packet_transport = static_cast<wire::Word>(scene.spectral_transport);
    result.hero_wavelengths = static_cast<wire::Word>(scene.hero_wavelengths);
    result.bidirectional = scene.integrator == IntegratorKind::bdpt;
    result.packet_lane_bytes = static_cast<wire::Word>(packet_dispatch::lane_bytes(scene));
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
                                                          std::string &diagnostic,
                                                          AccelerationBuildInfo &info) {
    const auto start = std::chrono::steady_clock::now();
    auto descriptor = [MTLInstanceAccelerationStructureDescriptor descriptor];
    const bool fast = acceleration_fast_tlas(info.requested);
    if (!metal::prefer_fast_intersection(descriptor, fast)) {
        diagnostic = "Fast acceleration builds require the macOS 26 SDK and runtime";
        return nil;
    }
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
    if (diagnostic.empty() &&
        !metal::record_acceleration_build(info, structure, false, fast, start))
        diagnostic = "Acceleration structure byte count overflow";
    return diagnostic.empty() ? structure : nil;
}

} // namespace

struct FramePairAtlasAsset {
    id<MTLBuffer> entries = nil;
    FrameAreaAtlasInfo identity;
};

struct MetalRenderScene::Impl {
    std::unique_ptr<MetalGeometryScene> geometry;
    id<MTLAccelerationStructure> empty_packet_structure = nil;
    AccelerationBuildInfo acceleration_build;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLComputePipelineState> pipeline = nil;
    MTLFunctionConstantValues *function_constants = nil;
    id<MTLIntersectionFunctionTable> intersection_table = nil;
    id<MTLBuffer> instances = nil;
    id<MTLBuffer> positions = nil;
    id<MTLBuffer> triangles = nil;
    id<MTLBuffer> reconstruction_groups = nil;
    std::mutex reconstruction_groups_mutex;
    std::mutex paired_atlas_mutex;
    std::shared_ptr<const FramePairAtlasAsset> paired_atlas;
    std::uint64_t reconstruction_preparation_limit = 8ull << 30;
    std::uint32_t native_hint_mode = 0;
    bool opaque_shadows = false;
    std::size_t packet_payload_bytes = packet_wire::workspace_bytes;
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
    bool diffuse_event_proof = false;

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
        reconstruction_preparation_limit =
            std::min<std::uint64_t>(scene.preparation_byte_limit, 8ull << 30);
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

    void bind(id<MTLComputeCommandEncoder> encoder, const wire::Parameters &p,
              const wire::TextureParameters &texture_p, id<MTLBuffer> output,
              id<MTLBuffer> frame_graph_nodes = nil, id<MTLBuffer> frame_surface_transforms = nil,
              bool full_geometry = false,
              id<MTLIntersectionFunctionTable> frame_intersection_table = nil) {
        [encoder setBytes:&p length:sizeof(p) atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        if (p.instance_count != 0 || p.packet_transport || full_geometry) {
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
                [encoder setIntersectionFunctionTable:frame_intersection_table != nil
                                                          ? frame_intersection_table
                                                          : intersection_table
                                        atBufferIndex:14];
            [encoder setBuffer:texture_uvs offset:0 atIndex:15];
            [encoder setBuffer:texture_frames offset:0 atIndex:16];
            [encoder setBuffer:image_textures offset:0 atIndex:17];
            [encoder setBuffer:image_levels offset:0 atIndex:18];
            [encoder setBuffer:image_texels offset:0 atIndex:19];
            [encoder setBuffer:image_transfer offset:0 atIndex:20];
            [encoder setBuffer:image_spectra offset:0 atIndex:21];
            [encoder setBytes:&texture_p length:sizeof(texture_p) atIndex:22];
            [encoder setBuffer:frame_graph_nodes != nil ? frame_graph_nodes : graph_nodes
                        offset:0
                       atIndex:23];
            [encoder setBuffer:graph_knots offset:0 atIndex:24];
            [encoder setBytes:&graph_parameters length:sizeof(graph_parameters) atIndex:25];
            [encoder setBuffer:surface_vertices offset:0 atIndex:26];
            [encoder setBuffer:frame_surface_transforms != nil ? frame_surface_transforms
                                                               : surface_transforms
                        offset:0
                       atIndex:27];
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
    }
    void encode(id<MTLComputeCommandEncoder> encoder, const wire::Parameters &p,
                const wire::TextureParameters &texture_p, id<MTLBuffer> output,
                NSUInteger group_width, id<MTLBuffer> frame_graph_nodes = nil,
                id<MTLBuffer> frame_surface_transforms = nil) {
        [encoder setComputePipelineState:pipeline];
        bind(encoder, p, texture_p, output, frame_graph_nodes, frame_surface_transforms);
        const auto group_count = (p.lane_count + group_width - 1) / group_width;
        [encoder dispatchThreadgroups:MTLSizeMake(group_count, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(group_width, 1, 1)];
    }

    RenderSamplesResult sample(const RenderRequest &request, RenderTimings *timings = nullptr) {
        RenderTimingScope setup(timings, RenderTimingPhase::dispatch_setup);
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
        if (p.packet_transport && (stride == 0 || p.lane_count > packet_payload_bytes / stride)) {
            result.diagnostic = "Packet dispatch exceeds the configured workspace capacity";
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
        if (p.packet_transport) {
            // Preserve missing-output sentinels; path vertices are initialized by
            // the shader before their counts expose them to other path operations.
            // Unused internal Vertex storage is intentionally not poisoned.
            const auto prefix_bytes = p.packet_transport == 2
                                          ? sizeof(packet_wire::Result)
                                          : packet_wire::vertex_offset(p.max_depth);
            for (std::size_t lane = 0; lane < p.lane_count; ++lane)
                std::memset(output_bytes + lane * stride, 0xff, prefix_bytes);
            std::memset(output_bytes + slots * stride, 0xff, guard_bytes);
        } else {
            std::memset(output_bytes, 0xff, bytes);
        }

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
        encode(encoder, p, texture_p, output, group_width);
        [encoder endEncoding];
        setup.close();
        result.diagnostic = render_timed(timings, RenderTimingPhase::dispatch_wait, [&] {
            return metal::complete(command, "Metal render sample dispatch");
        });
        if (timings)
            timings->gpu_command(command.status == MTLCommandBufferStatusCompleted,
                                 command.GPUStartTime, command.GPUEndTime);
        RenderTimingScope decode(timings, RenderTimingPhase::dispatch_decode);
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
                    value.splat_count > p.max_depth ||
                    (p.packet_transport == 2 && value.splat_count != 0) ||
                    !(value.filter_weight > 0) || !std::isfinite(value.filter_weight)) {
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
RenderSamplesResult MetalRenderScene::sample(const RenderRequest &request, RenderTimings &timings) {
    @autoreleasepool {
        if (!impl_)
            return {.diagnostic = "Metal render scene is moved-from", .samples = {}};
        return impl_->sample(request, &timings);
    }
}

struct FrameWavefrontBuffers {
    id<MTLBuffer> config = nil, scalar_state = nil, packet_state = nil;
    id<MTLBuffer> packet_hot = nil, packet_cold = nil, packet_hit = nil;
    id<MTLBuffer> hero_state = nil, hero_cold = nil;
    id<MTLBuffer> contribution_evaluation = nil, contribution_state = nil;
    id<MTLBuffer> enhanced_records = nil, enhanced_evaluation = nil, enhanced_suffix = nil;
    id<MTLBuffer> queue[2]{nil, nil};
    id<MTLBuffer> free_slots = nil, counters = nil, done = nil, work_list = nil, donors = nil;
    id<MTLBuffer> current_dispatch = nil, refill_dispatch = nil;
    id<MTLBuffer> arguments[2]{nil, nil};
    id<MTLBuffer> replay = nil;
};
struct FrameWavefrontPrograms {
    id<MTLComputePipelineState> reset = nil, initialize = nil, trace = nil, shade = nil;
    id<MTLComputePipelineState> prepare_refill = nil, refill = nil, advance = nil;
    id<MTLComputePipelineState> admit_all_pixels = nil, finalize_admission = nil;
    id<MTLIntersectionFunctionTable> trace_table = nil, shade_table = nil;
    id<MTLArgumentEncoder> argument_encoder = nil;
    wavefront_wire::Contract contract{};
};
struct FrameMotionPrograms {
    id<MTLComputePipelineState> reset = nil, geometry = nil, donors = nil;
    id<MTLIntersectionFunctionTable> table = nil;
    id<MTLArgumentEncoder> argument_encoder = nil;
};
struct FrameDuplicationPrograms {
    id<MTLComputePipelineState> reset = nil, count = nil;
    id<MTLArgumentEncoder> argument_encoder = nil;
};
struct FrameAreaBuffers {
    id<MTLBuffer> duplication_config = nil, duplication_arguments = nil, duplication_counters = nil;
    id<MTLBuffer> replay = nil, leaf = nil, accumulators = nil, temporal = nil, failure = nil;
    id<MTLBuffer> cross_targets = nil;
    id<MTLBuffer> contribution_evaluation = nil;
    id<MTLBuffer> enhanced_records = nil, enhanced_evaluation = nil, enhanced_suffix = nil;
    id<MTLBuffer> donors = nil, motion_config = nil, motion_counters = nil, motion_arguments = nil;
    id<MTLBuffer> paired_atlas = nil, paired_config = nil;
    id<MTLBuffer> config[area_wire::config_count]{};
    id<MTLBuffer> arguments[area_wire::config_count]{};
};
struct FrameAreaPrograms {
    id<MTLComputePipelineState> reset = nil, prepare = nil, merge = nil, cache_evaluation = nil;
    id<MTLComputePipelineState> temporal_finish = nil, spatial_begin = nil, finalize = nil;
    id<MTLComputePipelineState> mutation_prepare = nil, mutation_finish = nil;
    id<MTLArgumentEncoder> argument_encoder = nil;
    id<MTLBuffer> paired_atlas = nil;
};
// Independent of frame slots: the published reservoir must retain the exact
// physical evaluation context used to compute its stored source target.
struct FrameAreaContext {
    std::optional<wire::Point> copied_origin;
    wire::Parameters parameters{};
    wire::TextureParameters texture{};
    AreaPathDomainContext domain{};
    std::uint64_t suffix_context = 0;
    id<MTLBuffer> graph_nodes = nil, surface_transforms = nil;
};
// History never crosses sessions: scene geometry, materials, spectra, image
// data, environment, CIE/sampling tables and specialized pipelines are immutable
// on this renderer. Compare the complete mutable evaluation context by content.
static bool same_area_evaluation_context(const FrameAreaContext &a, const FrameAreaContext &b) {
    const auto same_buffer = [](id<MTLBuffer> x, id<MTLBuffer> y) {
        if (x == nil || y == nil)
            return x == y;
        return same_area_evaluation_bytes(x.contents, x.length, y.contents, y.length);
    };
    return same_area_path_domain_context(a.domain, b.domain) &&
           same_area_evaluation_parameters(a.parameters, b.parameters, a.texture, b.texture) &&
           same_buffer(a.graph_nodes, b.graph_nodes) &&
           same_buffer(a.surface_transforms, b.surface_transforms);
}
struct FrameAreaHistory {
    std::mutex mutex;
    id<MTLBuffer> banks[2]{nil, nil};
    // One pending Area frame exclusively owns scratch banks 2/3. Slots borrow
    // this arena; history banks 0/1 publish under the same completion token.
    id<MTLBuffer> enhanced_records = nil, enhanced_evaluation = nil, enhanced_suffix = nil;
    id<MTLBuffer> duplication_labels[2]{nil, nil}, duplication_temporal = nil;
    id<MTLBuffer> duplication_counts[2]{nil, nil};
    bool fail_next_duplication_publication = false;
    FrameAreaContext contexts[2];
    id<MTLBuffer> geometry[2]{nil, nil}, donors = nil;
    unsigned geometry_front = 0;
    bool geometry_valid = false, fail_next_motion_publication = false;
    std::uint64_t geometry_generation = 0;
    bool latest_motion_generated = false;
    bool pending = false, valid = false, epoch_known = false, serial_exhausted = false;
    std::uint64_t epoch = 0, generation = 0, next_serial = 0, pending_serial = 0;
    // Last successful publication, independent of the latest accepted attempt.
    std::optional<std::uint64_t> committed_serial;
    std::uint64_t next_suffix_context = 1;
    bool suffix_context_exhausted = false, suffix_context_seeded = false;
    unsigned front = 0;
};
struct Frame::Impl {
    // The completion block retains this lease without retaining the command
    // buffer itself, so completion safety does not depend on breaking a cycle.
    struct Retention {
        std::shared_ptr<MetalRenderScene> renderer;
        id<MTLBuffer> raw = nil, status = nil, scratch = nil;
        id<MTLBuffer> graph_nodes = nil, surface_transforms = nil;
        std::shared_ptr<FrameWavefrontBuffers> wavefront;
        std::shared_ptr<FrameAreaBuffers> area;
    };
    std::shared_ptr<Retention> retention;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLBuffer> raw = nil, status = nil, scratch = nil;
    FrameInfo info;
    // Resolved once after validation, before submission; consumers never mutate these.
    wire::Parameters resolved_parameters{};
    wire::TextureParameters resolved_texture_parameters{};
    FrameCamera resolved_camera;
    IntersectionProfile intersection_profile = IntersectionProfile::compensated;
    id<MTLLibrary> library = nil;
    mutable std::mutex info_mutex, capture_mutex;
    mutable std::condition_variable completed;
};
struct MetalFrameLease::Impl {
    std::shared_ptr<Frame::Impl> frame;
    FrameInfo info;
};
struct MetalFramePipeline::Impl {
    // Pipelines are scene-specialized, not camera-specialized. A weak owner
    // permits stream-wide caching without pinning a completed frame or slot.
    std::weak_ptr<MetalRenderScene> renderer;
    id<MTLDevice> device = nil;
    IntersectionProfile profile = IntersectionProfile::compensated;
    id<MTLComputePipelineState> pipeline = nil;
    id<MTLFunction> function = nil;
    id<MTLIntersectionFunctionTable> table = nil;
};
MetalFrameLease::MetalFrameLease(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MetalFrameLease::~MetalFrameLease() = default;
MetalFrameLease::MetalFrameLease(MetalFrameLease &&) noexcept = default;
MetalFrameLease &MetalFrameLease::operator=(MetalFrameLease &&) noexcept = default;
MetalFramePipeline::MetalFramePipeline(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MetalFramePipeline::~MetalFramePipeline() = default;
MetalFramePipeline::MetalFramePipeline(MetalFramePipeline &&) noexcept = default;
MetalFramePipeline &MetalFramePipeline::operator=(MetalFramePipeline &&) noexcept = default;
id<MTLComputePipelineState> MetalFramePipeline::pipeline() const {
    return impl_ ? impl_->pipeline : nil;
}
id<MTLIntersectionFunctionTable> MetalFramePipeline::intersection_table() const {
    return impl_ ? impl_->table : nil;
}
id<MTLArgumentEncoder> MetalFramePipeline::argument_encoder() const {
    return impl_ ? [impl_->function newArgumentEncoderWithBufferIndex:1] : nil;
}
id<MTLDevice> MetalFrameLease::device() const { return impl_ ? impl_->frame->device : nil; }
id<MTLCommandQueue> MetalFrameLease::queue() const { return impl_ ? impl_->frame->queue : nil; }
id<MTLBuffer> MetalFrameLease::raw_xyz() const { return impl_ ? impl_->frame->raw : nil; }
const FrameInfo &MetalFrameLease::info() const {
    if (!impl_)
        throw std::logic_error("Metal frame lease is moved-from");
    return impl_->info;
}
const render_wire::Parameters &MetalFrameLease::parameters() const {
    if (!impl_)
        throw std::logic_error("Metal frame lease is moved-from");
    return impl_->frame->resolved_parameters;
}
const render_wire::TextureParameters &MetalFrameLease::texture_parameters() const {
    if (!impl_)
        throw std::logic_error("Metal frame lease is moved-from");
    return impl_->frame->resolved_texture_parameters;
}
const FrameCamera &MetalFrameLease::camera() const {
    if (!impl_)
        throw std::logic_error("Metal frame lease is moved-from");
    return impl_->frame->resolved_camera;
}
IntersectionProfile MetalFrameLease::intersection_profile() const {
    if (!impl_)
        throw std::logic_error("Metal frame lease is moved-from");
    return impl_->frame->intersection_profile;
}
MetalFrameAcquireResult MetalFrameAccess::acquire(const Frame &frame) {
    MetalFrameAcquireResult result;
    if (!frame.impl_) {
        result.info.status = FrameStatus::failed;
        result.info.diagnostic = result.diagnostic = "Frame is moved-from";
        return result;
    }
    {
        std::lock_guard lock(frame.impl_->info_mutex);
        result.info = frame.impl_->info;
    }
    result.status = result.info.status;
    result.diagnostic = result.info.diagnostic;
    if (result.status != FrameStatus::complete)
        return result;
    const auto pixels = std::uint64_t(result.info.width) * result.info.height;
    if (!pixels || result.info.completed_samples != pixels || !frame.impl_->retention ||
        frame.impl_->device == nil || frame.impl_->queue == nil || frame.impl_->library == nil ||
        frame.impl_->raw == nil || frame.impl_->raw.storageMode != MTLStorageModePrivate ||
        frame.impl_->raw.length != pixels * sizeof(wire::Point)) {
        result.status = result.info.status = FrameStatus::failed;
        result.info.diagnostic = result.diagnostic =
            "Completed Metal frame resources are inconsistent";
        return result;
    }
    auto impl = std::make_unique<MetalFrameLease::Impl>();
    impl->frame = frame.impl_;
    impl->info = result.info;
    result.lease = std::unique_ptr<MetalFrameLease>(new MetalFrameLease(std::move(impl)));
    return result;
}
MetalFramePipelineResult MetalFrameLease::create_guide_pipeline(NSString *entry) const {
    @autoreleasepool {
        MetalFramePipelineResult result;
        if (!impl_ || entry == nil || entry.length == 0) {
            result.diagnostic = "A live Metal frame lease and guide entry point are required";
            return result;
        }
        auto &frame = *impl_->frame;
        const auto owner = frame.retention->renderer;
        auto &renderer = *owner->impl_;
        if (renderer.function_constants == nil) {
            result.diagnostic = "Metal frame scene specialization constants are unavailable";
            return result;
        }
        MTLFunctionConstantValues *constants = [renderer.function_constants copy];
        if (constants == nil) {
            result.diagnostic = "Guide specialization constants allocation failed";
            return result;
        }
        const bool bidirectional = false;
        [constants setConstantValue:&bidirectional type:MTLDataTypeBool atIndex:20];
        const bool hero = false;
        [constants setConstantValue:&hero type:MTLDataTypeBool atIndex:25];
        auto intersection = metal::load_patch_intersection(frame.library, result.diagnostic);
        if (intersection == nil)
            return result;
        auto pipeline = metal::load_pipeline(frame.device, frame.library, entry, result.diagnostic,
                                             intersection, constants);
        if (pipeline == nil)
            return result;
        auto table = metal::make_intersection_table(pipeline, intersection, result.diagnostic);
        if (table == nil)
            return result;
        auto function = metal::load_function(frame.library, entry, result.diagnostic, constants);
        if (function == nil)
            return result;
        auto built = std::make_unique<MetalFramePipeline::Impl>();
        built->renderer = owner;
        built->device = frame.device;
        built->profile = frame.intersection_profile;
        built->pipeline = pipeline;
        built->function = function;
        built->table = table;
        result.pipeline =
            std::unique_ptr<MetalFramePipeline>(new MetalFramePipeline(std::move(built)));
        result.status = GeometryStatus::success;
        return result;
    }
}
render_wire::Parameters MetalFrameLease::guide_parameters() const {
    if (!impl_)
        throw std::logic_error("Metal frame lease is moved-from");
    const auto &frame = *impl_->frame;
    auto p = frame.resolved_parameters;
    p.packet_transport = 0;
    p.hero_wavelengths = 0;
    p.bidirectional = 0;
    p.lens_radius = 0;
    p.pixel_begin = 0;
    p.pixel_count = p.lane_count = frame.info.width * frame.info.height;
    return p;
}
std::string MetalFrameLease::prepare_reconstruction_groups() const {
    if (!impl_)
        return "Reconstruction continuity requires a live frame lease";
    auto &renderer = *impl_->frame->retention->renderer->impl_;
    std::lock_guard lock(renderer.reconstruction_groups_mutex);
    if (renderer.reconstruction_groups)
        return {};
    // Immutable shared resident geometry supplies topology on demand. Ordinary
    // offline scene construction does no continuity work and retains no copies.
    const auto face_count = renderer.triangles.length / sizeof(wire::Triangle);
    const auto vertex_count = renderer.positions.length / sizeof(wire::Point);
    const std::uint64_t output_bytes = face_count * sizeof(std::uint32_t);
    // Includes map nodes/edge vectors, face planes, adjacency, mesh copies,
    // queues, groups and simultaneous Metal upload. Conservative STL allowance.
    const std::uint64_t peak_bytes = 768ull * face_count + 64ull * vertex_count + 65536;
    const auto budget = renderer.device.recommendedMaxWorkingSetSize
                            ? std::min<std::uint64_t>(renderer.reconstruction_preparation_limit,
                                                      renderer.device.recommendedMaxWorkingSetSize)
                            : renderer.reconstruction_preparation_limit;
    if (output_bytes > renderer.device.maxBufferLength || peak_bytes > budget)
        return "Reconstruction continuity preparation exceeds resource budget";
    try {
        std::vector<std::uint32_t> groups(face_count, 0xffffffffu);
        const auto *instances = static_cast<const wire::Instance *>(renderer.instances.contents);
        const auto *triangles = static_cast<const wire::Triangle *>(renderer.triangles.contents);
        const auto *positions = static_cast<const wire::Point *>(renderer.positions.contents);
        std::map<std::uint32_t, bool> prepared;
        for (std::uint32_t i = 0; i < renderer.parameters.instance_count; ++i) {
            const auto &instance = instances[i];
            if (!instance.triangle_count || !prepared.emplace(instance.triangle_begin, true).second)
                continue;
            GeometryMesh mesh;
            mesh.triangles.reserve(instance.triangle_count);
            std::uint32_t local_vertices = 0;
            for (std::uint32_t t = 0; t < instance.triangle_count; ++t) {
                const auto &triangle = triangles[instance.triangle_begin + t];
                mesh.triangles.push_back(
                    {triangle.vertices[0], triangle.vertices[1], triangle.vertices[2]});
                for (auto vertex : triangle.vertices)
                    local_vertices = std::max(local_vertices, vertex + 1);
            }
            mesh.positions.reserve(local_vertices);
            for (std::uint32_t v = 0; v < local_vertices; ++v) {
                const auto p = positions[instance.vertex_begin + v];
                mesh.positions.push_back({p.x, p.y, p.z});
            }
            const auto local = reconstruction_continuity(mesh);
            std::copy(local.begin(), local.end(), groups.begin() + instance.triangle_begin);
        }
        auto buffer = upload(renderer.device, groups);
        if (!buffer || buffer.allocatedSize > output_bytes + 65536)
            return "Reconstruction continuity allocation failed or exceeded estimate";
        renderer.reconstruction_groups = buffer;
    } catch (const std::bad_alloc &) {
        return "Reconstruction continuity host allocation failed";
    }
    return {};
}
id<MTLBuffer> MetalFrameLease::reconstruction_groups() const {
    if (!impl_)
        return nil;
    auto &renderer = *impl_->frame->retention->renderer->impl_;
    std::lock_guard lock(renderer.reconstruction_groups_mutex);
    return renderer.reconstruction_groups;
}
std::string MetalFrameLease::bind_guides(id<MTLComputeCommandEncoder> encoder,
                                         const MetalFramePipeline &pipeline,
                                         id<MTLBuffer> arguments) const {
    if (!impl_ || !pipeline.impl_)
        return "Metal frame lease or guide pipeline is moved-from";
    auto &frame = *impl_->frame;
    const auto owner = pipeline.impl_->renderer.lock();
    if (!owner || owner != frame.retention->renderer ||
        pipeline.impl_->profile != frame.intersection_profile)
        return "Guide pipeline belongs to a different render session or numerical profile";
    if (encoder == nil || arguments == nil || !arguments.length || encoder.device != frame.device ||
        arguments.device != frame.device || pipeline.impl_->device != frame.device ||
        pipeline.impl_->pipeline == nil || pipeline.impl_->table == nil)
        return "Guide encoder, arguments, and pipeline must use the completed frame's device";
    auto &renderer = *owner->impl_;
    if (!renderer.parameters.instance_count && renderer.empty_packet_structure == nil)
        return "Completed frame is missing its empty-scene acceleration structure";
    const auto p = guide_parameters();
    [encoder setComputePipelineState:pipeline.impl_->pipeline];
    renderer.bind(encoder, p, frame.resolved_texture_parameters, arguments,
                  frame.retention->graph_nodes, frame.retention->surface_transforms, true,
                  pipeline.impl_->table);
    return {};
}
std::string MetalFrameLease::retain_until_completed(id<MTLCommandBuffer> command) const {
    if (!impl_)
        return "Metal frame lease is moved-from";
    if (command == nil || command.device != impl_->frame->device)
        return "Consumer command must use the completed frame's device";
    if (command.status != MTLCommandBufferStatusNotEnqueued &&
        command.status != MTLCommandBufferStatusEnqueued)
        return "Attach the completed-frame lease before committing the consumer command";
    const auto retained = impl_->frame;
    [command addCompletedHandler:^(id<MTLCommandBuffer>) {
      (void)retained;
    }];
    return {};
}

struct FrameSession::Impl {
    std::shared_ptr<MetalRenderScene> renderer;
    FrameConfig config;
    RenderScene camera_scene;
    bool supports_camera_medium = false;
    id<MTLComputePipelineState> initialize = nil, resolve = nil;
    id<MTLBuffer> cie = nil;
    id<MTLLibrary> frame_library = nil;
    std::shared_ptr<FrameWavefrontPrograms> wavefront;
    std::shared_ptr<FrameAreaPrograms> area;
    std::shared_ptr<FrameMotionPrograms> motion;
    std::shared_ptr<FrameDuplicationPrograms> duplication;
    std::shared_ptr<const FramePairAtlasAsset> paired_atlas;
    std::shared_ptr<FrameAreaHistory> history;
    std::size_t state_bytes = 0, slot_bytes = 0;
    AccelerationBuildInfo acceleration_build_info() const {
        auto info = renderer->impl_->acceleration_build;
        info.device_allocated_bytes = renderer->impl_->device.currentAllocatedSize;
        return info;
    }
    struct Slot {
        std::weak_ptr<Frame::Impl::Retention> active;
        id<MTLBuffer> raw = nil, scratch = nil, status = nil;
        id<MTLBuffer> graph_nodes = nil, surface_transforms = nil;
        std::optional<wire::Point> rebased_origin;
        std::shared_ptr<FrameWavefrontBuffers> wavefront;
        std::shared_ptr<FrameAreaBuffers> area;
    };
    std::array<Slot, 2> slots;
    std::size_t tile_count = 0, scratch_bytes = 0;

    EnhancedSuffixContextState suffix_context_state(std::optional<std::uint64_t> seed = {}) {
        EnhancedSuffixContextState result;
        if (!frame_suffix_cache_enabled(config) || !history) {
            result.diagnostic = "Suffix context access requires enabled Enhanced prefix caching";
            return result;
        }
        std::lock_guard lock(history->mutex);
        auto &h = *history;
        if (h.pending) {
            result.diagnostic = "Suffix context access requires no pending frame";
            return result;
        }
        if (seed) {
            if (!*seed || h.epoch_known || h.valid || h.generation || h.next_serial ||
                h.committed_serial || h.next_suffix_context != 1 || h.suffix_context_exhausted ||
                h.suffix_context_seeded || h.contexts[0].suffix_context ||
                h.contexts[1].suffix_context) {
                result.diagnostic =
                    "Suffix context seed requires a pristine session and nonzero seed";
                return result;
            }
            h.next_suffix_context = *seed;
            h.suffix_context_seeded = true;
        }
        result.status = GeometryStatus::success;
        result.contexts = {h.contexts[0].suffix_context, h.contexts[1].suffix_context};
        result.front = h.front;
        result.next_context = h.next_suffix_context;
        result.exhausted = h.suffix_context_exhausted;
        result.valid = h.valid;
        result.generation = h.generation;
        result.next_serial = h.next_serial;
        result.pending_serial = h.pending_serial;
        result.committed_serial = h.committed_serial;
        return result;
    }

    EnhancedSessionSnapshot snapshot_enhanced(std::uint64_t expected_serial, bool include_suffix,
                                              bool include_motion, bool include_duplication) const {
        EnhancedSessionSnapshot result;
        if (config.estimator != FrameEstimator::area_replay ||
            config.area_path_domain != FrameAreaPathDomain::enhanced || !history) {
            result.diagnostic = "Enhanced snapshot requires an Enhanced Area session";
            return result;
        }
        // Submission is externally serialized. Keep the publication locked until
        // copies complete; a retained Frame alone does not freeze shared arenas.
        std::lock_guard lock(history->mutex);
        if (history->pending || !history->valid || !history->generation ||
            !history->committed_serial || *history->committed_serial != expected_serial ||
            history->pending_serial != *history->committed_serial || history->front >= 2) {
            result.diagnostic = "Enhanced snapshot publication is pending, invalid or stale";
            return result;
        }
        std::shared_ptr<FrameAreaBuffers> latest;
        for (const auto &slot : slots) {
            if (!slot.area || slot.area->config[0] == nil ||
                slot.area->config[0].length != sizeof(area_wire::Config))
                continue;
            area_wire::Config submitted{};
            std::memcpy(&submitted, slot.area->config[0].contents, sizeof(submitted));
            const auto serial =
                std::uint64_t(submitted.serial_low) | (std::uint64_t(submitted.serial_high) << 32);
            if (serial == expected_serial && submitted.phase == area_wire::fresh &&
                submitted.width == config.width && submitted.height == config.height &&
                submitted.contribution_mode == contribution_wire::enhanced) {
                if (latest) {
                    result.diagnostic = "Enhanced snapshot has ambiguous slot identity";
                    return result;
                }
                latest = slot.area;
            }
        }
        if (!latest || latest->enhanced_records != history->enhanced_records ||
            latest->enhanced_suffix != history->enhanced_suffix ||
            latest->donors != history->donors) {
            result.diagnostic = "Enhanced snapshot completed slot is unavailable";
            return result;
        }
        const auto pixels = std::size_t(config.width) * config.height;
        if (!pixels ||
            pixels > std::numeric_limits<std::size_t>::max() /
                         (enhanced_wire::bank_count * sizeof(rypath_enhanced_suffix_tape::Entry))) {
            result.diagnostic = "Enhanced snapshot byte count overflow";
            return result;
        }
        const std::size_t history_bytes = pixels * sizeof(area_wire::History);
        const std::size_t temporal_bytes = pixels * sizeof(area_wire::Temporal);
        const std::size_t record_count = pixels * enhanced_wire::bank_count;
        const std::size_t record_bytes = record_count * sizeof(enhanced_wire::Record);
        const bool suffix_enabled = frame_suffix_cache_enabled(config);
        const bool copy_suffix = include_suffix && suffix_enabled;
        const std::size_t suffix_bytes =
            suffix_enabled ? record_count * sizeof(rypath_enhanced_suffix_tape::Entry) : 16;
        const auto source_history = history->banks[history->front];
        if (source_history == nil || source_history.length != history_bytes ||
            latest->temporal == nil || latest->temporal.length != temporal_bytes ||
            history->enhanced_records == nil || history->enhanced_records.length != record_bytes ||
            history->enhanced_suffix == nil || history->enhanced_suffix.length != suffix_bytes) {
            result.diagnostic = "Enhanced snapshot buffer ownership or size mismatch";
            return result;
        }
        const bool motion_enabled = frame_motion_enabled(config);
        const bool copy_geometry = include_motion && motion_enabled;
        const bool copy_donors = copy_geometry && history->latest_motion_generated;
        const std::size_t geometry_bytes = pixels * sizeof(motion_wire::Geometry);
        const std::size_t donor_bytes = pixels * sizeof(motion_wire::Donor);
        if (motion_enabled &&
            (!history->geometry_valid || history->geometry_front > 1 ||
             history->geometry_generation != history->generation ||
             history->geometry[history->geometry_front] == nil ||
             history->geometry[history->geometry_front].length != geometry_bytes ||
             history->donors == nil || history->donors.length != donor_bytes)) {
            result.diagnostic = "Enhanced motion snapshot publication identity/size mismatch";
            return result;
        }
        auto &r = *renderer->impl_;
        auto history_copy = [r.device newBufferWithLength:history_bytes
                                                  options:MTLResourceStorageModeShared];
        auto temporal_copy = [r.device newBufferWithLength:temporal_bytes
                                                   options:MTLResourceStorageModeShared];
        auto record_copy = [r.device newBufferWithLength:record_bytes
                                                 options:MTLResourceStorageModeShared];
        id<MTLBuffer> suffix_copy =
            copy_suffix
                ? [r.device newBufferWithLength:suffix_bytes options:MTLResourceStorageModeShared]
                : nil;
        id<MTLBuffer> geometry_copy =
            copy_geometry
                ? [r.device newBufferWithLength:geometry_bytes options:MTLResourceStorageModeShared]
                : nil;
        id<MTLBuffer> donor_copy = copy_donors
                                       ? [r.device newBufferWithLength:donor_bytes
                                                               options:MTLResourceStorageModeShared]
                                       : nil;
        if (history_copy == nil || temporal_copy == nil || record_copy == nil ||
            (copy_suffix && suffix_copy == nil) || (copy_geometry && geometry_copy == nil) ||
            (copy_donors && donor_copy == nil)) {
            result.diagnostic = "Enhanced snapshot staging allocation failed";
            return result;
        }
        auto command = [r.queue commandBuffer];
        auto encoder = [command blitCommandEncoder];
        if (command == nil || encoder == nil) {
            result.diagnostic = "Enhanced snapshot encoder creation failed";
            return result;
        }
        [encoder copyFromBuffer:source_history
                   sourceOffset:0
                       toBuffer:history_copy
              destinationOffset:0
                           size:history_bytes];
        [encoder copyFromBuffer:latest->temporal
                   sourceOffset:0
                       toBuffer:temporal_copy
              destinationOffset:0
                           size:temporal_bytes];
        [encoder copyFromBuffer:history->enhanced_records
                   sourceOffset:0
                       toBuffer:record_copy
              destinationOffset:0
                           size:record_bytes];
        if (copy_suffix)
            [encoder copyFromBuffer:history->enhanced_suffix
                       sourceOffset:0
                           toBuffer:suffix_copy
                  destinationOffset:0
                               size:suffix_bytes];
        if (copy_geometry)
            [encoder copyFromBuffer:history->geometry[history->geometry_front]
                       sourceOffset:0
                           toBuffer:geometry_copy
                  destinationOffset:0
                               size:geometry_bytes];
        if (copy_donors)
            [encoder copyFromBuffer:history->donors
                       sourceOffset:0
                           toBuffer:donor_copy
                  destinationOffset:0
                               size:donor_bytes];
        [encoder endEncoding];
        result.diagnostic = metal::complete(command, "Enhanced test snapshot");
        if (!result.diagnostic.empty())
            return result;
        result.history.resize(pixels);
        result.temporal.resize(pixels);
        result.records.resize(record_count);
        std::memcpy(result.history.data(), history_copy.contents, history_bytes);
        std::memcpy(result.temporal.data(), temporal_copy.contents, temporal_bytes);
        std::memcpy(result.records.data(), record_copy.contents, record_bytes);
        if (copy_suffix) {
            result.enhanced_suffix.resize(record_count);
            std::memcpy(result.enhanced_suffix.data(), suffix_copy.contents, suffix_bytes);
        }
        result.motion_enabled = motion_enabled;
        if (motion_enabled) {
            result.geometry_valid = history->geometry_valid;
            result.geometry_front = history->geometry_front;
            result.geometry_generation = history->geometry_generation;
            result.motion_map_generated = history->latest_motion_generated;
        }
        if (copy_geometry) {
            result.geometry.resize(pixels);
            std::memcpy(result.geometry.data(), geometry_copy.contents, geometry_bytes);
        }
        if (copy_donors) {
            result.donors.resize(pixels);
            std::memcpy(result.donors.data(), donor_copy.contents, donor_bytes);
        }
        if (frame_paired_atlas_enabled(config)) {
            if (latest->paired_atlas != area->paired_atlas || latest->paired_config == nil ||
                latest->paired_config.length != sizeof(paired::Config)) {
                result.diagnostic = "Enhanced atlas snapshot resource identity mismatch";
                return result;
            }
            paired::Config actual{};
            std::memcpy(&actual, latest->paired_config.contents, sizeof(actual));
            const auto expected =
                paired::frame_config(static_cast<std::uint32_t>(expected_serial),
                                     static_cast<std::uint32_t>(expected_serial >> 32),
                                     static_cast<std::uint32_t>(config.seed),
                                     static_cast<std::uint32_t>(config.seed >> 32));
            if (!paired::valid_config(actual) || expected.status != paired::Status::ok ||
                std::memcmp(&actual, &expected.config, sizeof(actual)) != 0) {
                result.diagnostic = "Enhanced atlas snapshot config is invalid";
                return result;
            }
            result.paired_config = actual;
        }
        result.duplication_mode = config.area_duplication;
        if (include_duplication && duplication) {
            edw::Config actual{};
            if (!latest->duplication_config ||
                latest->duplication_config.length != sizeof(actual)) {
                result.diagnostic = "Duplication snapshot config is unavailable";
                return result;
            }
            std::memcpy(&actual, latest->duplication_config.contents, sizeof(actual));
            if (!edw::valid_config(actual) || actual.width != config.width ||
                actual.height != config.height ||
                actual.mode != static_cast<std::uint32_t>(config.area_duplication) ||
                (std::uint64_t(actual.serial_low) | (std::uint64_t(actual.serial_high) << 32)) !=
                    expected_serial) {
                result.diagnostic = "Duplication snapshot publication config mismatch";
                return result;
            }
            const std::size_t label_bytes = pixels * sizeof(edw::Label),
                              count_bytes = pixels * sizeof(std::uint16_t);
            const auto read = [&](id<MTLBuffer> source, void *out,
                                  std::size_t bytes) -> std::string {
                if (!source || source.length != bytes)
                    return "Duplication snapshot ownership/size mismatch";
                auto staging = [r.device newBufferWithLength:bytes
                                                     options:MTLResourceStorageModeShared];
                if (!staging)
                    return "Duplication snapshot staging allocation failed";
                auto command = [r.queue commandBuffer];
                auto blit = [command blitCommandEncoder];
                if (!command || !blit)
                    return "Duplication snapshot encoder failed";
                [blit copyFromBuffer:source
                         sourceOffset:0
                             toBuffer:staging
                    destinationOffset:0
                                 size:bytes];
                [blit endEncoding];
                auto error = metal::complete(command, "duplication test snapshot");
                if (error.empty())
                    std::memcpy(out, staging.contents, bytes);
                return error;
            };
            result.duplication_labels.resize(pixels);
            result.duplication_temporal_labels.resize(pixels);
            result.duplication_counts.resize(pixels);
            for (auto error : {read(history->duplication_labels[history->front],
                                    result.duplication_labels.data(), label_bytes),
                               read(history->duplication_temporal,
                                    result.duplication_temporal_labels.data(), label_bytes),
                               read(history->duplication_counts[history->front],
                                    result.duplication_counts.data(), count_bytes)})
                if (!error.empty()) {
                    result.diagnostic = error;
                    return result;
                }
        }
        result.suffix_cache_enabled = suffix_enabled;
        result.suffix_context = history->contexts[history->front].suffix_context;
        result.width = config.width;
        result.height = config.height;
        result.front_bank = history->front;
        result.serial = expected_serial;
        result.generation = history->generation;
        result.epoch = history->epoch;
        result.status = GeometryStatus::success;
        return result;
    }

    std::string prepare(std::shared_ptr<MetalRenderScene> owner, const RenderScene &scene,
                        const FrameConfig &settings, const std::filesystem::path &library_path) {
        renderer = std::move(owner);
        config = settings;
        if (frame_duplication_enabled(config) &&
            (static_cast<unsigned>(config.area_duplication) > 3 ||
             config.execution != FrameExecution::wavefront ||
             config.estimator != FrameEstimator::area_replay ||
             config.area_path_domain != FrameAreaPathDomain::enhanced))
            return "Area duplication diagnostics require wavefront Enhanced Area";
        if (config.mutation_policy == FrameMutationPolicy::automatic)
            config.mutation_policy = FrameMutationPolicy::legacy;
        if (config.area_stencil == FrameAreaStencil::automatic)
            config.area_stencil = FrameAreaStencil::adjacent;
        if (config.estimator == FrameEstimator::independent)
            config.mutation_steps = 0;
        supports_camera_medium = scene.model == RenderModel::pbrt && scene.volume_transport &&
                                 scene.spectral_transport == SpectralTransport::scalar;
        camera_scene.model = scene.model;
        camera_scene.camera = scene.camera;
        camera_scene.raster_camera = scene.raster_camera;
        camera_scene.perspective_lens = scene.perspective_lens;
        camera_scene.filter = scene.filter;
        auto &r = *renderer->impl_;
        if ((config.packet_events != 1 || config.packet_layout != FramePacketLayout::packed) &&
            !r.parameters.packet_transport)
            return "Packet scheduling controls require effective packet transport";
        std::string error;
        auto library = metal::load_library(r.device, library_path, error);
        if (library == nil)
            return error;
        frame_library = library;
        initialize = metal::load_pipeline(r.device, library, @"rypath_frame_initialize", error);
        if (initialize == nil)
            return error;
        resolve = metal::load_pipeline(r.device, library, @"rypath_frame_resolve", error);
        if (resolve == nil)
            return error;
        auto handshake = metal::load_pipeline(r.device, library, @"rypath_frame_contract", error);
        if (handshake == nil)
            return error;
        auto check = [r.device newBufferWithLength:sizeof(std::uint32_t)
                                           options:MTLResourceStorageModeShared];
        if (check == nil)
            return "Frame contract allocation failed";
        *static_cast<std::uint32_t *>(check.contents) = 0;
        auto command = [r.queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        if (command == nil || encoder == nil)
            return "Frame contract encoder creation failed";
        [encoder setComputePipelineState:handshake];
        [encoder setBuffer:check offset:0 atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        error = metal::complete(command, "frame contract");
        if (!error.empty())
            return error;
        if (*static_cast<const std::uint32_t *>(check.contents) != frame_wire::contract)
            return "Metal frame shader contract mismatch; rebuild the metallib";
        std::vector<wire::Point> table;
        table.reserve(471);
        for (unsigned i = 0; i < 471; ++i)
            table.push_back(pack(scene.model == RenderModel::pbrt ? pbrt_cie_xyz(360 + i)
                                                                  : wavelength_xyz(360 + i)));
        cie = upload(r.device, table);
        if (cie == nil)
            return "Frame sensor table allocation failed";
        // Scalar sky previously avoided all geometry arguments. The generic
        // wavefront signatures require valid storage even on a synthetic miss.
        if (!r.parameters.instance_count && r.empty_packet_structure == nil) {
            r.empty_packet_structure =
                build_empty_packet_structure(r.device, r.queue, error, r.acceleration_build);
            if (r.empty_packet_structure == nil)
                return error;
        }
        if (!r.parameters.instance_count) {
            auto sentinel = [r.device newBufferWithLength:2048
                                                  options:MTLResourceStorageModeShared];
            if (sentinel == nil)
                return "Wavefront empty-scene sentinel allocation failed";
            std::memset(sentinel.contents, 0, sentinel.length);
            for (auto *buffer :
                 {&r.instances,       &r.positions,    &r.triangles,        &r.patches,
                  &r.materials,       &r.knots,        &r.area_emissions,   &r.emitters,
                  &r.normals,         &r.texture_uvs,  &r.texture_frames,   &r.image_textures,
                  &r.image_levels,    &r.image_texels, &r.image_transfer,   &r.image_spectra,
                  &r.graph_nodes,     &r.graph_knots,  &r.surface_vertices, &r.surface_transforms,
                  &r.environment_cdf, &r.illuminant,   &r.material_data})
                if (*buffer == nil)
                    *buffer = sentinel;
        }
        if (config.execution == FrameExecution::wavefront)
            return prepare_wavefront(library);
        const auto stride =
            r.parameters.packet_transport ? r.parameters.packet_lane_bytes : sizeof(wire::Sample);
        tile_count = std::min<std::size_t>(
            render_tile_capacity,
            r.parameters.packet_transport ? r.packet_payload_bytes / stride : render_tile_capacity);
        tile_count = std::min<std::size_t>(tile_count, std::uint64_t(config.width) * config.height);
        if (!tile_count)
            return "Frame transport workspace has no lanes";
        const auto group = metal::group_width(r.device, r.pipeline);
        // Include rounded dispatch lanes and one untouched scalar sentinel slot.
        scratch_bytes = r.parameters.packet_transport
                            ? tile_count * stride + 64
                            : ((tile_count + group - 1) / group * group + 1) * stride;
        const std::uint64_t raw_bytes =
            std::uint64_t(config.width) * config.height * sizeof(wire::Point);
        const auto budget = std::min<std::uint64_t>(config.transient_byte_limit, 8ull << 30);
        // Include two captures as well as both persistent output/scratch/status slots.
        const auto total = 2 * (2 * raw_bytes + scratch_bytes + sizeof(frame_wire::Status) +
                                r.graph_nodes.length + r.surface_transforms.length) +
                           cie.length;
        if (raw_bytes > r.device.maxBufferLength || scratch_bytes > r.device.maxBufferLength ||
            total > budget ||
            (r.device.recommendedMaxWorkingSetSize &&
             total > r.device.recommendedMaxWorkingSetSize))
            return "Frame slots exceed the transient or device memory budget";
        return {};
    }
    bool diffuse_events = false;
    std::string prepare_wavefront(id<MTLLibrary> library) {
        auto &r = *renderer->impl_;
        NSString *shade_entry = @"rypath_wavefront_shade_scalar";
        if (r.parameters.packet_transport) {
            if (config.packet_layout == FramePacketLayout::packed)
                shade_entry = config.packet_events == 1 ? @"rypath_wavefront_shade_packet"
                              : config.packet_events == 2
                                  ? @"rypath_wavefront_shade_packet_packed_2"
                                  : @"rypath_wavefront_shade_packet_packed_4";
            else if (config.packet_layout == FramePacketLayout::hot_cold)
                shade_entry =
                    config.packet_events == 1   ? @"rypath_wavefront_shade_packet_hot_cold_1"
                    : config.packet_events == 2 ? @"rypath_wavefront_shade_packet_hot_cold_2"
                                                : @"rypath_wavefront_shade_packet_hot_cold_4";
            else
                shade_entry = @"rypath_wavefront_shade_packet_split";
        }
        // No generic branch exists in this separate typed entry. Missing optional
        // entry falls back to generic without claiming effective specialization.
        diffuse_events = config.specialize_diffuse_events && r.diffuse_event_proof &&
                         r.parameters.packet_transport == 2 &&
                         config.packet_layout == FramePacketLayout::packed &&
                         config.packet_events == 1 &&
                         (config.estimator == FrameEstimator::independent ||
                          config.area_path_domain == FrameAreaPathDomain::enhanced);
        if (diffuse_events) {
            const bool available =
                [library.functionNames containsObject:@"rypath_wavefront_shade_diffuse"];
            diffuse_events = available;
            if (available)
                shade_entry = @"rypath_wavefront_shade_diffuse";
        }
        std::string error;
        if (r.opaque_shadows &&
            !metal::verify_opaque_shadow_capability(library, shade_entry, error))
            return error;
        wavefront = std::make_shared<FrameWavefrontPrograms>();
        auto handshake =
            metal::load_pipeline(r.device, library, @"rypath_wavefront_contract_v23", error);
        if (handshake == nil)
            return error;
        auto check = [r.device newBufferWithLength:sizeof(wavefront_wire::Contract)
                                           options:MTLResourceStorageModeShared];
        if (check == nil)
            return "Wavefront contract allocation failed";
        std::memset(check.contents, 0, check.length);
        auto command = [r.queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        if (command == nil || encoder == nil)
            return "Wavefront contract encoder creation failed";
        [encoder setComputePipelineState:handshake];
        [encoder setBuffer:check offset:0 atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        error = metal::complete(command, "wavefront contract");
        if (!error.empty())
            return error;
        auto &contract = wavefront->contract;
        std::memcpy(&contract, check.contents, sizeof(contract));
        const auto alignment_valid = [](std::uint32_t size, std::uint32_t alignment) {
            return size && alignment && alignment <= 16 && !(alignment & (alignment - 1)) &&
                   size % alignment == 0;
        };
        if (contract.version != wavefront_wire::contract ||
            contract.config_bytes != sizeof(wavefront_wire::Config) ||
            contract.counters_bytes != sizeof(wavefront_wire::Counters) ||
            contract.dispatch_bytes != sizeof(wavefront_wire::DispatchArguments) ||
            !alignment_valid(contract.scalar_bytes, contract.scalar_alignment) ||
            !alignment_valid(contract.packet_bytes, contract.packet_alignment) ||
            !alignment_valid(contract.hot_bytes, contract.hot_alignment) ||
            !alignment_valid(contract.cold_bytes, contract.cold_alignment) ||
            !alignment_valid(contract.hit_bytes, contract.hit_alignment) ||
            !alignment_valid(contract.hero_bytes, contract.hero_alignment) ||
            !alignment_valid(contract.hero_hot_bytes, contract.hero_hot_alignment) ||
            !alignment_valid(contract.hero_cold_bytes, contract.hero_cold_alignment) ||
            contract.hero_hot_bytes != contract.hot_bytes ||
            contract.hero_hot_alignment != contract.hot_alignment ||
            contract.hero_bytes >= contract.packet_bytes ||
            contract.hero_cold_bytes >= contract.cold_bytes ||
            !alignment_valid(contract.contribution_state_bytes,
                             contract.contribution_state_alignment) ||
            !alignment_valid(contract.enhanced_state_bytes, contract.enhanced_state_alignment) ||
            contract.enhanced_record_bytes != sizeof(enhanced_wire::Record) ||
            contract.enhanced_evaluation_bytes != sizeof(enhanced_wire::Evaluation) ||
            contract.suffix_entry_bytes != sizeof(rypath_enhanced_suffix_tape::Entry) ||
            contract.donor_bytes != sizeof(motion_wire::Donor))
            return "Metal wavefront shader contract mismatch; rebuild the metallib";
        // Verify the actual selected packet entry before reporting any policy
        // as effective. Older/unrelated libraries must not silently ignore the
        // requested specialization, even if their resource sizes happen to fit.
        if (r.parameters.packet_transport) {
            // Fixed shade entries do not reference policy constants. Producers
            // still specialize them and validate the immutable wire config.
            struct RequiredPolicyConstants {
                NSString *entry;
                bool events;
            };
            for (const auto &required : {
                     RequiredPolicyConstants{@"rypath_wavefront_reset", true},
                     RequiredPolicyConstants{@"rypath_wavefront_initialize", true},
                     RequiredPolicyConstants{@"rypath_wavefront_prepare_refill", true},
                     RequiredPolicyConstants{@"rypath_wavefront_refill", false},
                 }) {
                auto declaration = [library newFunctionWithName:required.entry];
                bool has_layout = false, has_events = false, has_contribution = false,
                     has_enhanced = false, has_count = false;
                for (MTLFunctionConstant *value in declaration.functionConstantsDictionary
                         .allValues) {
                    has_layout |= value.index == wavefront_wire::packet_layout_constant &&
                                  value.type == MTLDataTypeUInt;
                    has_events |= value.index == wavefront_wire::packet_events_constant &&
                                  value.type == MTLDataTypeUInt;
                    has_contribution |= value.index == wavefront_wire::contribution_mode_constant &&
                                        value.type == MTLDataTypeBool;
                    has_count |= value.index == wavefront_wire::queued_slots_constant &&
                                 value.type == MTLDataTypeBool;
                    has_enhanced |= value.index == wavefront_wire::enhanced_mode_constant &&
                                    value.type == MTLDataTypeBool;
                }
                if ((config.profile_stages &&
                     ([required.entry isEqualToString:@"rypath_wavefront_reset"] ||
                      [required.entry isEqualToString:@"rypath_wavefront_prepare_refill"]) &&
                     !has_count) ||
                    !has_layout || (required.events && !has_events) ||
                    (frame_area_contribution_family(config.area_path_domain) &&
                     !has_contribution) ||
                    (config.area_path_domain == FrameAreaPathDomain::enhanced && !has_enhanced))
                    return "Metal wavefront packet specialization capability missing; rebuild the "
                           "metallib";
            }
        }
        const bool contribution = frame_area_contribution_family(config.area_path_domain);
        const bool enhanced = config.area_path_domain == FrameAreaPathDomain::enhanced;
        MTLFunctionConstantValues *transport_constants = [r.function_constants copy];
        if (transport_constants == nil)
            return "Wavefront transport specialization constants allocation failed";
        [transport_constants setConstantValue:&contribution
                                         type:MTLDataTypeBool
                                      atIndex:wavefront_wire::contribution_mode_constant];
        [transport_constants setConstantValue:&enhanced
                                         type:MTLDataTypeBool
                                      atIndex:wavefront_wire::enhanced_mode_constant];
        if (contribution) {
            auto declaration = [library newFunctionWithName:shade_entry];
            bool declared = false, declared_enhanced = false;
            for (MTLFunctionConstant *value in declaration.functionConstantsDictionary.allValues) {
                declared |= value.index == wavefront_wire::contribution_mode_constant &&
                            value.type == MTLDataTypeBool;
                declared_enhanced |= value.index == wavefront_wire::enhanced_mode_constant &&
                                     value.type == MTLDataTypeBool;
            }
            if (!declared || (enhanced && !declared_enhanced))
                return "Metal wavefront contribution specialization missing; rebuild the metallib";
        }
        MTLFunctionConstantValues *policy_constants = [transport_constants copy];
        if (policy_constants == nil)
            return "Wavefront policy specialization constants allocation failed";
        const std::uint32_t packet_layout = static_cast<std::uint32_t>(config.packet_layout);
        [policy_constants setConstantValue:&packet_layout
                                      type:MTLDataTypeUInt
                                   atIndex:wavefront_wire::packet_layout_constant];
        [policy_constants setConstantValue:&config.packet_events
                                      type:MTLDataTypeUInt
                                   atIndex:wavefront_wire::packet_events_constant];
        const auto load = [&](NSString *name, id<MTLFunction> intersection = nil) {
            return metal::load_pipeline(r.device, library, name, error, intersection,
                                        policy_constants);
        };
        // Keep the count constant out of the shade pipeline specialization.
        MTLFunctionConstantValues *count_constants = [policy_constants copy];
        if (count_constants == nil)
            return "Wavefront count specialization constants allocation failed";
        const bool count_slots = config.profile_stages && r.parameters.packet_transport != 0 &&
                                 config.packet_layout == FramePacketLayout::packed &&
                                 config.packet_events == 1;
        [count_constants setConstantValue:&count_slots
                                     type:MTLDataTypeBool
                                  atIndex:wavefront_wire::queued_slots_constant];
        const auto load_count = [&](NSString *name) {
            return metal::load_pipeline(r.device, library, name, error, nil, count_constants);
        };
        wavefront->reset = load_count(@"rypath_wavefront_reset");
        if (wavefront->reset == nil)
            return error;
        if (frame_pair_compaction_enabled(config)) {
            wavefront->admit_all_pixels = load(@"rypath_wavefront_admit_all_pixels");
            if (wavefront->admit_all_pixels == nil)
                return error;
            wavefront->finalize_admission = load(@"rypath_wavefront_finalize_admission");
            if (wavefront->finalize_admission == nil)
                return error;
        }
        wavefront->initialize = load(@"rypath_wavefront_initialize");
        if (wavefront->initialize == nil)
            return error;
        wavefront->prepare_refill = load_count(@"rypath_wavefront_prepare_refill");
        if (wavefront->prepare_refill == nil)
            return error;
        wavefront->refill = load(@"rypath_wavefront_refill");
        if (wavefront->refill == nil)
            return error;
        wavefront->advance = load(@"rypath_wavefront_advance");
        if (wavefront->advance == nil)
            return error;
        auto intersection = metal::load_patch_intersection(library, error);
        if (intersection == nil)
            return error;
        if (!r.parameters.packet_transport ||
            config.packet_layout == FramePacketLayout::split_trace) {
            wavefront->trace =
                load(r.parameters.packet_transport ? @"rypath_wavefront_trace_packet"
                                                   : @"rypath_wavefront_trace_scalar",
                     intersection);
            if (wavefront->trace == nil)
                return error;
            if (!r.parameters.packet_transport) {
                wavefront->trace_table =
                    metal::make_intersection_table(wavefront->trace, intersection, error);
                if (wavefront->trace_table == nil)
                    return error;
            }
        }
        wavefront->shade = metal::load_pipeline(r.device, library, shade_entry, error, intersection,
                                                transport_constants);
        if (wavefront->shade == nil)
            return error;
        if (!r.parameters.packet_transport) {
            wavefront->shade_table =
                metal::make_intersection_table(wavefront->shade, intersection, error);
            if (wavefront->shade_table == nil)
                return error;
        }
        for (auto pipeline :
             {wavefront->reset, wavefront->initialize, wavefront->trace, wavefront->shade,
              wavefront->prepare_refill, wavefront->refill, wavefront->advance,
              wavefront->admit_all_pixels, wavefront->finalize_admission})
            if (pipeline != nil && config.group_width > metal::group_width(r.device, pipeline))
                return "Wavefront group width exceeds a pipeline or device limit";
        auto function =
            metal::load_function(library, @"rypath_wavefront_reset", error, policy_constants);
        if (function == nil)
            return error;
        wavefront->argument_encoder = [function newArgumentEncoderWithBufferIndex:1];
        if (wavefront->argument_encoder == nil)
            return "Wavefront argument encoder creation failed";
        config.capacity =
            std::min<std::uint64_t>(config.capacity, std::uint64_t(config.width) * config.height);
        const bool separated = config.packet_layout != FramePacketLayout::packed;
        const bool hero = r.parameters.packet_transport == 2;
        state_bytes = hero ? (separated ? contract.hero_hot_bytes : contract.hero_bytes)
                      : r.parameters.packet_transport
                          ? (separated ? contract.hot_bytes : contract.packet_bytes)
                          : contract.scalar_bytes;
        const std::uint64_t state_allocation = std::uint64_t(config.capacity) * state_bytes;
        const std::uint64_t cold_bytes =
            separated ? std::uint64_t(config.capacity) *
                            (hero ? contract.hero_cold_bytes : contract.cold_bytes)
                      : 16;
        const std::uint64_t hit_bytes = config.packet_layout == FramePacketLayout::split_trace
                                            ? std::uint64_t(config.capacity) * contract.hit_bytes
                                            : 16;
        const std::uint64_t contribution_state_bytes =
            contribution
                ? std::uint64_t(config.capacity) *
                      (enhanced ? contract.enhanced_state_bytes : contract.contribution_state_bytes)
                : 16;
        const std::uint64_t raw_bytes =
            std::uint64_t(config.width) * config.height * sizeof(wire::Point);
        const std::uint64_t done_bytes =
            std::uint64_t(config.width) * config.height * sizeof(std::uint32_t);
        const std::uint64_t work_list_bytes =
            frame_pair_compaction_enabled(config) ? done_bytes : 16;
        const auto argument_bytes = wavefront->argument_encoder.encodedLength;
        slot_bytes = 2 * raw_bytes + done_bytes + state_allocation + 16 +
                     3 * std::uint64_t(config.capacity) * sizeof(std::uint32_t) +
                     sizeof(wavefront_wire::Config) + sizeof(wavefront_wire::Counters) +
                     2 * sizeof(wavefront_wire::DispatchArguments) + 2 * argument_bytes +
                     r.graph_nodes.length + r.surface_transforms.length;
        slot_bytes += cold_bytes + hit_bytes + 16; // Alternative layout sentinels/pools.
        slot_bytes += 32;                          // Inactive hero/packet pool sentinels.
        slot_bytes += 32; // Independent replay sentinel; Area replaces it with full replay input.
        // Area shares its evaluation buffer with wavefront; its accounting
        // replaces this sentinel rather than charging the full buffer twice.
        slot_bytes += contribution_state_bytes + 16;
        // Area replaces these independent-mode sentinels with shared
        // records/evaluation/suffix allocations for the history, not each slot.
        if (slot_bytes > std::numeric_limits<std::uint64_t>::max() - 48)
            return "Wavefront buffer byte count overflow";
        slot_bytes += 48;
        if (work_list_bytes > std::numeric_limits<std::uint64_t>::max() - slot_bytes)
            return "Wavefront work-list byte count overflow";
        slot_bytes += work_list_bytes;
        if (slot_bytes > std::numeric_limits<std::uint64_t>::max() - sizeof(motion_wire::Donor))
            return "Wavefront donor sentinel byte count overflow";
        slot_bytes += sizeof(motion_wire::Donor);
        if (slot_bytes > (std::numeric_limits<std::uint64_t>::max() - cie.length) / 2)
            return "Wavefront slots byte count overflow";
        const std::uint64_t total = 2 * std::uint64_t(slot_bytes) + cie.length;
        if (cold_bytes > r.device.maxBufferLength || hit_bytes > r.device.maxBufferLength ||
            state_allocation > r.device.maxBufferLength || raw_bytes > r.device.maxBufferLength ||
            done_bytes > r.device.maxBufferLength || work_list_bytes > r.device.maxBufferLength ||
            argument_bytes > r.device.maxBufferLength ||
            contribution_state_bytes > r.device.maxBufferLength ||
            total > std::min<std::uint64_t>(config.transient_byte_limit, 8ull << 30) ||
            (r.device.recommendedMaxWorkingSetSize &&
             total > r.device.recommendedMaxWorkingSetSize))
            return "Wavefront slots exceed the transient or device memory budget";
        if (config.estimator == FrameEstimator::area_replay)
            return prepare_area(library);
        return {};
    }
    std::string prepare_motion(id<MTLLibrary> library) {
        if (!frame_motion_enabled(config))
            return {};
        auto &r = *renderer->impl_;
        std::string error;
        motion = std::make_shared<FrameMotionPrograms>();
        auto probe =
            metal::load_pipeline(r.device, library, @"rypath_enhanced_motion_contract_v1", error);
        if (probe == nil)
            return error;
        auto check = [r.device newBufferWithLength:sizeof(motion_wire::Contract)
                                           options:MTLResourceStorageModeShared];
        if (check == nil)
            return "Enhanced motion contract allocation failed";
        std::memset(check.contents, 0, check.length);
        auto command = [r.queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        if (command == nil || encoder == nil)
            return "Enhanced motion contract encoder failed";
        [encoder setComputePipelineState:probe];
        [encoder setBuffer:check offset:0 atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        error = metal::complete(command, "Enhanced motion contract");
        if (!error.empty())
            return error;
        motion_wire::Contract actual{};
        std::memcpy(&actual, check.contents, sizeof(actual));
        if (actual.version != motion_wire::contract ||
            actual.config_bytes != sizeof(motion_wire::Config) ||
            actual.geometry_bytes != sizeof(motion_wire::Geometry) ||
            actual.donor_bytes != sizeof(motion_wire::Donor) ||
            actual.counters_bytes != sizeof(motion_wire::Counters) ||
            actual.camera_bytes != sizeof(rypath_enhanced_motion::Camera))
            return "Enhanced motion shader contract mismatch; rebuild the metallib";
        motion->reset =
            metal::load_pipeline(r.device, library, @"rypath_enhanced_motion_reset", error);
        if (motion->reset == nil)
            return error;
        motion->donors =
            metal::load_pipeline(r.device, library, @"rypath_enhanced_motion_donors", error);
        if (motion->donors == nil)
            return error;
        MTLFunctionConstantValues *constants = [r.function_constants copy];
        if (constants == nil)
            return "Enhanced motion specialization allocation failed";
        const bool off = false;
        [constants setConstantValue:&off type:MTLDataTypeBool atIndex:20];
        [constants setConstantValue:&off type:MTLDataTypeBool atIndex:25];
        auto intersection = metal::load_patch_intersection(library, error);
        if (intersection == nil)
            return error;
        motion->geometry = metal::load_pipeline(
            r.device, library, @"rypath_enhanced_motion_geometry", error, intersection, constants);
        if (motion->geometry == nil)
            return error;
        motion->table = metal::make_intersection_table(motion->geometry, intersection, error);
        if (motion->table == nil)
            return error;
        auto function =
            metal::load_function(library, @"rypath_enhanced_motion_geometry", error, constants);
        if (function == nil)
            return error;
        motion->argument_encoder = [function newArgumentEncoderWithBufferIndex:1];
        if (motion->argument_encoder == nil)
            return "Enhanced motion argument encoder allocation failed";
        for (auto pipeline : {motion->reset, motion->geometry, motion->donors})
            if (config.group_width > metal::group_width(r.device, pipeline))
                return "Enhanced motion group width exceeds device or pipeline limit";
        return {};
    }
    std::string prepare_duplication(id<MTLLibrary> library) {
        if (!frame_duplication_enabled(config))
            return {};
        auto &r = *renderer->impl_;
        std::string error;
        auto probe = metal::load_pipeline(r.device, library,
                                          @"rypath_enhanced_duplication_contract_v1", error);
        if (!probe)
            return error;
        auto check = [r.device newBufferWithLength:sizeof(edw::Contract)
                                           options:MTLResourceStorageModeShared];
        if (!check)
            return "Duplication contract allocation failed";
        std::memset(check.contents, 0, check.length);
        auto command = [r.queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        if (!command || !encoder)
            return "Duplication contract encoder failed";
        [encoder setComputePipelineState:probe];
        [encoder setBuffer:check offset:0 atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        error = metal::complete(command, "duplication contract");
        if (!error.empty())
            return error;
        edw::Contract c{};
        std::memcpy(&c, check.contents, sizeof(c));
        if (c.version != edw::contract || c.config_bytes != sizeof(edw::Config) ||
            c.label_bytes != sizeof(edw::Label) || c.counters_bytes != sizeof(edw::Counters) ||
            c.count_bytes != sizeof(std::uint16_t) || c.histogram_bins != 291 ||
            c.tile_width != 8 || c.tile_height != 8)
            return "Metal duplication diagnostic contract mismatch";
        duplication = std::make_shared<FrameDuplicationPrograms>();
        duplication->reset =
            metal::load_pipeline(r.device, library, @"rypath_enhanced_duplication_reset", error);
        if (!duplication->reset)
            return error;
        duplication->count =
            metal::load_pipeline(r.device, library, @"rypath_enhanced_duplication_count_v1", error);
        if (!duplication->count)
            return error;
        if (duplication->count.maxTotalThreadsPerThreadgroup < 64 ||
            r.device.maxThreadsPerThreadgroup.width < 8 ||
            r.device.maxThreadsPerThreadgroup.height < 8 ||
            duplication->count.staticThreadgroupMemoryLength > r.device.maxThreadgroupMemoryLength)
            return "Duplication count tile exceeds device limits";
        auto fn = metal::load_function(library, @"rypath_enhanced_duplication_reset", error);
        if (!fn)
            return error;
        duplication->argument_encoder = [fn newArgumentEncoderWithBufferIndex:2];
        if (!duplication->argument_encoder)
            return "Duplication argument encoder creation failed";
        return {};
    }
    std::string prepare_paired_atlas() {
        auto &r = *renderer->impl_;
        if (!frame_paired_atlas_enabled(config)) {
            area->paired_atlas = [r.device newBufferWithLength:16
                                                       options:MTLResourceStorageModeShared];
            if (area->paired_atlas == nil)
                return "Area atlas sentinel allocation failed";
            std::memset(area->paired_atlas.contents, 0, area->paired_atlas.length);
            return {};
        }
        // One immutable asset per renderer/scene. Shared-scene sessions borrow
        // the same bytes; pose storage remains private to each pending slot.
        std::lock_guard lock(r.paired_atlas_mutex);
        if (!r.paired_atlas) {
            const auto generated = area_pair_atlas::generate();
            if (!generated)
                return "Area paired atlas generation failed: " + generated.diagnostic;
            if (generated.entries.size() != paired::total_entries)
                return "Area paired atlas generation returned an invalid entry count";
            auto built = std::make_shared<FramePairAtlasAsset>();
            built->entries =
                [r.device newBufferWithBytes:generated.entries.data()
                                      length:generated.entries.size() * sizeof(paired::Entry)
                                     options:MTLResourceStorageModeShared];
            if (built->entries == nil)
                return "Area paired atlas upload failed";
            auto &identity = built->identity;
            identity.version = paired::version;
            identity.entry_count = paired::total_entries;
            identity.asset_bytes = generated.entries.size() * sizeof(paired::Entry);
            identity.iterations = paired::shuffle_count;
            identity.sigma = std::bit_cast<float>(paired::sigma_bits);
            identity.seed = {paired::atlas_seed_low, paired::atlas_seed_high};
            for (unsigned k = 0; k < 4; ++k)
                identity.widths[k] = paired::tile_width(k);
            identity.sha256 = digest_hex(generated.digest);
            r.paired_atlas = std::move(built);
        }
        paired_atlas = r.paired_atlas;
        area->paired_atlas = paired_atlas->entries;
        return {};
    }
    std::string prepare_area(id<MTLLibrary> library) {
        auto &r = *renderer->impl_;
        area = std::make_shared<FrameAreaPrograms>();
        history = std::make_shared<FrameAreaHistory>();
        std::string error;
        auto probe = metal::load_pipeline(r.device, library, @"rypath_area_contract_v16", error);
        if (probe == nil)
            return error;
        auto check = [r.device newBufferWithLength:sizeof(area_wire::Contract)
                                           options:MTLResourceStorageModeShared];
        if (check == nil)
            return "Area contract allocation failed";
        std::memset(check.contents, 0, check.length);
        auto command = [r.queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        if (command == nil || encoder == nil)
            return "Area contract encoder creation failed";
        [encoder setComputePipelineState:probe];
        [encoder setBuffer:check offset:0 atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        error = metal::complete(command, "area contract");
        if (!error.empty())
            return error;
        area_wire::Contract contract{};
        std::memcpy(&contract, check.contents, sizeof(contract));
        if (contract.version != area_wire::contract ||
            contract.config_bytes != sizeof(area_wire::Config) ||
            contract.history_bytes != sizeof(area_wire::History) ||
            contract.temporal_bytes != sizeof(area_wire::Temporal) ||
            contract.accumulator_bytes != sizeof(area_wire::Accumulator) ||
            contract.replay_bytes != sizeof(rypath_replay::ReplayState) ||
            contract.counters_bytes != sizeof(area_wire::Counters) ||
            contract.contribution_evaluation_bytes != sizeof(contribution_wire::Evaluation) ||
            contract.suffix_entry_bytes != sizeof(rypath_enhanced_suffix_tape::Entry) ||
            contract.donor_bytes != sizeof(motion_wire::Donor) ||
            contract.paired_config_bytes != sizeof(paired::Config) ||
            contract.paired_entry_bytes != sizeof(paired::Entry))
            return "Metal Area shader contract mismatch; rebuild the metallib";
        const auto load = [&](NSString *name) {
            return metal::load_pipeline(r.device, library, name, error);
        };
        area->reset = load(@"rypath_area_reset");
        if (area->reset == nil)
            return error;
        area->prepare = load(@"rypath_area_prepare");
        if (area->prepare == nil)
            return error;
        area->cache_evaluation = load(@"rypath_area_cache_evaluation");
        if (area->cache_evaluation == nil)
            return error;
        area->merge = load(@"rypath_area_merge");
        if (area->merge == nil)
            return error;
        area->temporal_finish =
            load(frame_duplication_enabled(config) ? @"rypath_area_temporal_finish_duplication_v1"
                                                   : @"rypath_area_temporal_finish");
        if (area->temporal_finish == nil)
            return error;
        area->mutation_prepare = load(@"rypath_area_mutation_prepare");
        if (area->mutation_prepare == nil)
            return error;
        area->mutation_finish = load(@"rypath_area_mutation_finish");
        if (area->mutation_finish == nil)
            return error;
        area->spatial_begin = load(@"rypath_area_spatial_begin");
        if (area->spatial_begin == nil)
            return error;
        area->finalize =
            load(frame_duplication_enabled(config) ? @"rypath_area_finalize_duplication_v1"
                                                   : @"rypath_area_finalize");
        if (area->finalize == nil)
            return error;
        for (auto pipeline : {area->reset, area->prepare, area->merge, area->cache_evaluation,
                              area->temporal_finish, area->spatial_begin, area->finalize,
                              area->mutation_prepare, area->mutation_finish})
            if (config.group_width > metal::group_width(r.device, pipeline))
                return "Area group width exceeds a pipeline or device limit";
        auto function = metal::load_function(library, @"rypath_area_reset", error);
        if (function == nil)
            return error;
        area->argument_encoder = [function newArgumentEncoderWithBufferIndex:1];
        if (area->argument_encoder == nil)
            return "Area argument encoder creation failed";
        error = prepare_duplication(library);
        if (!error.empty())
            return error;
        error = prepare_motion(library);
        if (!error.empty())
            return error;
        const std::uint64_t pixels = std::uint64_t(config.width) * config.height;
        const std::uint64_t history_bytes = pixels * sizeof(area_wire::History);
        const std::uint64_t replay_bytes = pixels * sizeof(rypath_replay::ReplayState);
        const std::uint64_t leaf_bytes = pixels * sizeof(wire::Point);
        const std::uint64_t temporal_bytes = pixels * sizeof(area_wire::Temporal);
        const std::uint64_t accumulator_bytes = pixels * sizeof(area_wire::Accumulator);
        const std::uint64_t evaluation_bytes =
            frame_area_contribution_family(config.area_path_domain)
                ? pixels * sizeof(contribution_wire::Evaluation)
                : 16;
        const auto arguments_bytes = area->argument_encoder.encodedLength;
        const auto add_bytes = [](std::uint64_t &total, std::uint64_t bytes) {
            if (bytes > std::numeric_limits<std::uint64_t>::max() - total)
                return false;
            total += bytes;
            return true;
        };
        const auto product_bytes = [](std::uint64_t count, std::uint64_t stride,
                                      std::uint64_t &bytes) {
            if (stride && count > std::numeric_limits<std::uint64_t>::max() / stride)
                return false;
            bytes = count * stride;
            return true;
        };
        std::uint64_t cross_bytes = 0;
        if (!product_bytes(pixels, area_cross_target_pixel_bytes(config.area_path_domain),
                           cross_bytes))
            return "Area cross-target byte count overflow";
        const bool enhanced = config.area_path_domain == FrameAreaPathDomain::enhanced;
        std::uint64_t enhanced_record_bytes = 16, enhanced_evaluation_bytes = 16;
        std::uint64_t enhanced_suffix_bytes = 16;
        if (frame_suffix_cache_enabled(config) &&
            (!product_bytes(pixels, sizeof(rypath_enhanced_suffix_tape::Entry),
                            enhanced_suffix_bytes) ||
             !product_bytes(enhanced_suffix_bytes, enhanced_wire::bank_count,
                            enhanced_suffix_bytes)))
            return "Enhanced suffix byte count overflow";
        if (enhanced &&
            (!product_bytes(pixels, sizeof(enhanced_wire::Record), enhanced_record_bytes) ||
             !product_bytes(enhanced_record_bytes, enhanced_wire::bank_count,
                            enhanced_record_bytes) ||
             !product_bytes(pixels, sizeof(enhanced_wire::Evaluation), enhanced_evaluation_bytes)))
            return "Enhanced records/evaluation byte count overflow";
        std::uint64_t geometry_bytes = sizeof(motion_wire::Geometry),
                      donor_bytes = sizeof(motion_wire::Donor);
        if (frame_motion_enabled(config) &&
            (!product_bytes(pixels, sizeof(motion_wire::Geometry), geometry_bytes) ||
             !product_bytes(pixels, sizeof(motion_wire::Donor), donor_bytes)))
            return "Enhanced motion geometry/donor byte count overflow";
        std::uint64_t duplication_label_bytes = 0, duplication_count_bytes = 0;
        if (duplication && (!product_bytes(pixels, sizeof(edw::Label), duplication_label_bytes) ||
                            !product_bytes(pixels, sizeof(std::uint16_t), duplication_count_bytes)))
            return "Duplication byte count overflow";
        std::uint64_t paired_atlas_bytes = 16;
        if (frame_paired_atlas_enabled(config) &&
            !product_bytes(paired::total_entries, sizeof(paired::Entry), paired_atlas_bytes))
            return "Area paired atlas byte count overflow";
        std::uint64_t workspace_bytes = 0;
        for (const auto bytes :
             {replay_bytes, leaf_bytes, temporal_bytes, accumulator_bytes, cross_bytes,
              evaluation_bytes, std::uint64_t(sizeof(area_wire::Counters))})
            if (!add_bytes(workspace_bytes, bytes))
                return "Area workspace byte count overflow";
        if (motion && (!add_bytes(workspace_bytes, sizeof(motion_wire::Config)) ||
                       !add_bytes(workspace_bytes, sizeof(motion_wire::Counters)) ||
                       !add_bytes(workspace_bytes, motion->argument_encoder.encodedLength)))
            return "Enhanced motion per-slot byte count overflow";
        if (duplication &&
            (!add_bytes(workspace_bytes, sizeof(edw::Config)) ||
             !add_bytes(workspace_bytes, sizeof(edw::Counters)) ||
             !add_bytes(workspace_bytes, duplication->argument_encoder.encodedLength)))
            return "Duplication per-slot byte count overflow";
        // Reflected Config storage exists in old modes too, without any draw.
        if (!add_bytes(workspace_bytes, sizeof(paired::Config)))
            return "Area paired config byte count overflow";
        std::uint64_t argument_set_bytes = sizeof(area_wire::Config), all_arguments_bytes = 0;
        if (!add_bytes(argument_set_bytes, arguments_bytes) ||
            !product_bytes(area_wire::config_count, argument_set_bytes, all_arguments_bytes) ||
            !add_bytes(workspace_bytes, all_arguments_bytes))
            return "Area arguments byte count overflow";
        // Replace the per-slot replay(32), contribution evaluation(16), and
        // Enhanced records/evaluation/suffix(48) sentinels. The latter buffers belong
        // to the history and are charged once, including in legacy Area mode.
        constexpr std::uint64_t replaced_sentinels = 32 + 16 + 48 + sizeof(motion_wire::Donor);
        if (slot_bytes < replaced_sentinels)
            return "Area slot accounting underflow";
        std::uint64_t per_slot_bytes = slot_bytes - replaced_sentinels;
        std::uint64_t total = 0, bank_bytes = 0, context_bytes = 0;
        if (!add_bytes(per_slot_bytes, workspace_bytes) ||
            !product_bytes(2, per_slot_bytes, total) ||
            !product_bytes(2, history_bytes, bank_bytes) || !add_bytes(total, bank_bytes) ||
            !add_bytes(context_bytes, r.graph_nodes.length) ||
            !add_bytes(context_bytes, r.surface_transforms.length) ||
            !product_bytes(2, context_bytes, context_bytes) || !add_bytes(total, context_bytes) ||
            !add_bytes(total, enhanced_record_bytes) ||
            !add_bytes(total, enhanced_evaluation_bytes) ||
            !add_bytes(total, enhanced_suffix_bytes) || !add_bytes(total, geometry_bytes) ||
            !add_bytes(total, geometry_bytes) || !add_bytes(total, donor_bytes) ||
            !add_bytes(total, paired_atlas_bytes) || !add_bytes(total, cie.length))
            return "Area slots/history byte count overflow";
        for (unsigned i = 0; i < 3; ++i)
            if (!add_bytes(total, duplication_label_bytes))
                return "Duplication labels budget overflow";
        for (unsigned i = 0; i < 2; ++i)
            if (!add_bytes(total, duplication_count_bytes))
                return "Duplication maps budget overflow";
        for (const auto bytes :
             {history_bytes,
              duplication_label_bytes,
              duplication_count_bytes,
              std::uint64_t(duplication ? sizeof(edw::Config) : 0),
              std::uint64_t(duplication ? sizeof(edw::Counters) : 0),
              std::uint64_t(duplication ? duplication->argument_encoder.encodedLength : 0),
              replay_bytes,
              leaf_bytes,
              temporal_bytes,
              accumulator_bytes,
              cross_bytes,
              evaluation_bytes,
              enhanced_record_bytes,
              enhanced_evaluation_bytes,
              enhanced_suffix_bytes,
              geometry_bytes,
              donor_bytes,
              paired_atlas_bytes,
              std::uint64_t(sizeof(paired::Config)),
              std::uint64_t(motion ? sizeof(motion_wire::Config) : 0),
              std::uint64_t(motion ? sizeof(motion_wire::Counters) : 0),
              std::uint64_t(motion ? motion->argument_encoder.encodedLength : 0),
              std::uint64_t(arguments_bytes),
              std::uint64_t(r.graph_nodes.length),
              std::uint64_t(r.surface_transforms.length)})
            if (bytes > r.device.maxBufferLength)
                return "Area buffer exceeds the device limit";
        if (total > std::min<std::uint64_t>(config.transient_byte_limit, 8ull << 30) ||
            (r.device.recommendedMaxWorkingSetSize &&
             total > r.device.recommendedMaxWorkingSetSize))
            return "Area slots and history exceed the transient or device memory budget";
        error = prepare_paired_atlas();
        if (!error.empty())
            return error;
        if (duplication) {
            for (auto &b : history->duplication_labels) {
                b = [r.device newBufferWithLength:duplication_label_bytes
                                          options:MTLResourceStorageModePrivate];
                if (!b)
                    return "Duplication labels allocation failed";
            }
            history->duplication_temporal =
                [r.device newBufferWithLength:duplication_label_bytes
                                      options:MTLResourceStorageModePrivate];
            if (!history->duplication_temporal)
                return "Duplication temporal labels allocation failed";
            for (auto &b : history->duplication_counts) {
                b = [r.device newBufferWithLength:duplication_count_bytes
                                          options:MTLResourceStorageModePrivate];
                if (!b)
                    return "Duplication count map allocation failed";
            }
        }
        for (auto &bank : history->banks) {
            bank = [r.device newBufferWithLength:history_bytes
                                         options:MTLResourceStorageModePrivate];
            if (bank == nil)
                return "Area history allocation failed";
        }
        history->enhanced_records = [r.device newBufferWithLength:enhanced_record_bytes
                                                          options:MTLResourceStorageModePrivate];
        history->enhanced_evaluation = [r.device newBufferWithLength:enhanced_evaluation_bytes
                                                             options:MTLResourceStorageModePrivate];
        history->enhanced_suffix = [r.device newBufferWithLength:enhanced_suffix_bytes
                                                         options:MTLResourceStorageModePrivate];
        if (history->enhanced_records == nil || history->enhanced_evaluation == nil ||
            history->enhanced_suffix == nil)
            return "Enhanced owned-record storage allocation failed";
        for (auto &geometry : history->geometry) {
            geometry = [r.device newBufferWithLength:geometry_bytes
                                             options:MTLResourceStorageModePrivate];
            if (geometry == nil)
                return "Enhanced motion geometry allocation failed";
        }
        history->donors = [r.device newBufferWithLength:donor_bytes
                                                options:MTLResourceStorageModePrivate];
        if (history->donors == nil)
            return "Enhanced motion donor allocation failed";
        // Contents are produced/validated by Area prepare/finish, never cleared
        // here or during slot reuse. Front publication is the validity boundary.
        return {};
    }
    std::string allocate_area(Slot &slot) {
        auto &r = *renderer->impl_;
        if (!slot.area)
            slot.area = std::make_shared<FrameAreaBuffers>();
        auto &b = *slot.area;
        if (!history || history->enhanced_records == nil || history->enhanced_evaluation == nil ||
            history->enhanced_suffix == nil)
            return "Area owned-record storage is unavailable";
        b.enhanced_records = history->enhanced_records;
        b.enhanced_evaluation = history->enhanced_evaluation;
        b.enhanced_suffix = history->enhanced_suffix;
        b.donors = history->donors;
        b.paired_atlas = area->paired_atlas;
        const auto allocate = [&](id<MTLBuffer> __strong &buffer, std::size_t bytes,
                                  bool shared = false) {
            if (buffer == nil)
                buffer = [r.device newBufferWithLength:bytes
                                               options:shared ? MTLResourceStorageModeShared
                                                              : MTLResourceStorageModePrivate];
            return buffer != nil;
        };
        const auto pixels = std::size_t(config.width) * config.height;
        const auto cross_target_stride = area_cross_target_pixel_bytes(config.area_path_domain);
        if (pixels > std::numeric_limits<std::size_t>::max() / cross_target_stride)
            return "Area cross-target byte count overflow";
        const auto cross_bytes = pixels * cross_target_stride;
        if (!allocate(b.replay, pixels * sizeof(rypath_replay::ReplayState)) ||
            !allocate(b.leaf, pixels * sizeof(wire::Point)) ||
            !allocate(b.accumulators, pixels * sizeof(area_wire::Accumulator)) ||
            !allocate(b.temporal, pixels * sizeof(area_wire::Temporal)) ||
            !allocate(b.cross_targets, cross_bytes) ||
            !allocate(b.contribution_evaluation,
                      frame_area_contribution_family(config.area_path_domain)
                          ? pixels * sizeof(contribution_wire::Evaluation)
                          : 16) ||
            !allocate(b.failure, sizeof(area_wire::Counters), true))
            return "Area workspace allocation failed";
        if (duplication &&
            (!allocate(b.duplication_config, sizeof(edw::Config), true) ||
             !allocate(b.duplication_counters, sizeof(edw::Counters), true) ||
             !allocate(b.duplication_arguments, duplication->argument_encoder.encodedLength, true)))
            return "Duplication pending-frame allocation failed";
        if (!allocate(b.paired_config, sizeof(paired::Config), true))
            return "Area paired frame-config allocation failed";
        if (motion && (!allocate(b.motion_config, sizeof(motion_wire::Config), true) ||
                       !allocate(b.motion_counters, sizeof(motion_wire::Counters), true) ||
                       !allocate(b.motion_arguments, motion->argument_encoder.encodedLength, true)))
            return "Enhanced motion pending-frame allocation failed";
        for (unsigned index = 0; index < area_wire::config_count; ++index)
            if (!allocate(b.config[index], sizeof(area_wire::Config), true) ||
                !allocate(b.arguments[index], area->argument_encoder.encodedLength, true))
                return "Area argument allocation failed";
        return {};
    }
    void configure_area(Slot &slot, id<MTLBuffer> raw, std::uint64_t serial, bool history_used,
                        bool temporal_cache, id<MTLBuffer> source, id<MTLBuffer> destination,
                        rypath_area_stencil::Offsets offsets, unsigned history_source_bank,
                        std::uint64_t suffix_context, std::uint64_t source_suffix_context,
                        bool mapped_motion) {
        auto &b = *slot.area;
        if (duplication) {
            edw::Config c{edw::contract,
                          config.width,
                          config.height,
                          static_cast<std::uint32_t>(config.area_duplication),
                          static_cast<std::uint32_t>(serial),
                          static_cast<std::uint32_t>(serial >> 32),
                          0,
                          0};
            std::memcpy(b.duplication_config.contents, &c, sizeof(c));
            auto encoder = duplication->argument_encoder;
            [encoder setArgumentBuffer:b.duplication_arguments offset:0];
            const std::array<id<MTLBuffer>, 7> resources{
                b.duplication_config,
                history->duplication_labels[history_source_bank],
                history->duplication_labels[history_source_bank ^ 1],
                history->duplication_temporal,
                history->duplication_counts[history_source_bank ^ 1],
                b.duplication_counters,
                destination};
            for (NSUInteger i = 0; i < resources.size(); ++i)
                [encoder setBuffer:resources[i] offset:0 atIndex:i];
        }
        if (frame_area_contribution_family(config.area_path_domain)) {
            // The free frame slot owns this shared config until completion.
            // Decision entropy follows accepted submission identity, not the
            // potentially repeated sample index or a changing replay payload.
            auto *wavefront_config =
                static_cast<wavefront_wire::Config *>(slot.wavefront->config.contents);
            wavefront_config->contribution_serial_low = static_cast<std::uint32_t>(serial);
            wavefront_config->contribution_serial_high = static_cast<std::uint32_t>(serial >> 32);
        }
        std::memset(b.failure.contents, 0, b.failure.length);
        for (unsigned index = 0; index < area_wire::config_count; ++index) {
            const bool mutation = index >= area_wire::mutation_config_begin &&
                                  index < area_wire::temporal_reverse_config;
            const auto phase =
                index == 0                                    ? area_wire::fresh
                : index == 1                                  ? area_wire::temporal
                : index == area_wire::temporal_reverse_config ? area_wire::temporal_reverse
                : index == area_wire::spatial_finalize_config ? area_wire::spatial_final
                : mutation                                    ? area_wire::mutation
                                                              : area_wire::spatial;
            const auto candidate = mutation ? index - area_wire::mutation_config_begin
                                   : index >= area_wire::temporal_reverse_config ? 0u
                                   : index < 2                                   ? index
                                                                                 : index - 2;
            const auto actual_context =
                phase == area_wire::temporal_reverse ? source_suffix_context : suffix_context;
            const area_wire::Config cfg{
                config.width * config.height,
                config.width,
                config.height,
                phase,
                candidate,
                history_used ? 1u : 0u,
                static_cast<std::uint32_t>(serial),
                static_cast<std::uint32_t>(serial >> 32),
                static_cast<std::uint32_t>(config.seed),
                static_cast<std::uint32_t>(config.seed >> 32),
                config.target_scale,
                config.mutation_steps,
                config.area_output == FrameAreaOutput::conditional_mean
                    ? area_wire::conditional_mean_output
                    : area_wire::selected_output,
                config.mutation_policy == FrameMutationPolicy::spectral_refresh
                    ? area_wire::spectral_refresh_mutation
                    : area_wire::legacy_mutation,
                config.area_stencil == FrameAreaStencil::paired_gaussian16
                    ? area_wire::paired_gaussian16_stencil
                : config.area_stencil == FrameAreaStencil::reciprocal16
                    ? area_wire::reciprocal16_stencil
                    : area_wire::adjacent_stencil,
                temporal_cache ? 1u : 0u,
                {offsets.first.x, offsets.second.x},
                {offsets.first.y, offsets.second.y},
                static_cast<std::uint32_t>(config.area_path_domain),
                frame_area_contribution_family(config.area_path_domain)
                    ? renderer->impl_->parameters.max_depth
                    : 0u,
                config.area_path_domain == FrameAreaPathDomain::enhanced ? history_source_bank : 0u,
                static_cast<std::uint32_t>(actual_context),
                static_cast<std::uint32_t>(actual_context >> 32),
                mapped_motion ? 1u : 0u};
            std::memcpy(b.config[index].contents, &cfg, sizeof(cfg));
            auto encoder = area->argument_encoder;
            [encoder setArgumentBuffer:b.arguments[index] offset:0];
            const std::array<id<MTLBuffer>, 17> resources{b.config[index],
                                                          b.replay,
                                                          b.leaf,
                                                          b.accumulators,
                                                          b.temporal,
                                                          source,
                                                          destination,
                                                          raw,
                                                          b.failure,
                                                          b.cross_targets,
                                                          b.contribution_evaluation,
                                                          b.enhanced_records,
                                                          b.enhanced_evaluation,
                                                          b.enhanced_suffix,
                                                          b.donors,
                                                          b.paired_atlas,
                                                          b.paired_config};
            for (NSUInteger argument = 0; argument < resources.size(); ++argument)
                [encoder setBuffer:resources[argument] offset:0 atIndex:argument];
        }
    }
    std::string allocate_wavefront(Slot &slot, id<MTLBuffer> raw, id<MTLBuffer> replay = nil,
                                   id<MTLBuffer> contribution_evaluation = nil,
                                   id<MTLBuffer> enhanced_records = nil,
                                   id<MTLBuffer> enhanced_evaluation = nil,
                                   id<MTLBuffer> enhanced_suffix = nil,
                                   id<MTLBuffer> donors = nil) {
        auto &r = *renderer->impl_;
        if (!slot.wavefront)
            slot.wavefront = std::make_shared<FrameWavefrontBuffers>();
        auto &b = *slot.wavefront;
        const auto allocate = [&](id<MTLBuffer> __strong &buffer, std::size_t bytes,
                                  bool shared = false) {
            if (buffer == nil)
                buffer = [r.device newBufferWithLength:bytes
                                               options:shared ? MTLResourceStorageModeShared
                                                              : MTLResourceStorageModePrivate];
            return buffer != nil;
        };
        if (donors != nil)
            b.donors = donors;
        if (replay != nil)
            b.replay = replay;
        if (contribution_evaluation != nil)
            b.contribution_evaluation = contribution_evaluation;
        if ((enhanced_records == nil) != (enhanced_evaluation == nil) ||
            (enhanced_records == nil) != (enhanced_suffix == nil))
            return "Wavefront owned-record/evaluation/suffix bindings must be supplied together";
        if (enhanced_records != nil) {
            b.enhanced_records = enhanced_records;
            b.enhanced_evaluation = enhanced_evaluation;
            b.enhanced_suffix = enhanced_suffix;
        }
        if (config.area_path_domain == FrameAreaPathDomain::enhanced && enhanced_records == nil)
            return "Enhanced wavefront requires shared Area owned-record storage";
        const auto pixels = std::size_t(config.width) * config.height;
        if (frame_pair_compaction_enabled(config) &&
            pixels > std::numeric_limits<std::size_t>::max() / sizeof(wavefront_wire::Word))
            return "Wavefront work-list byte count overflow";
        const auto work_list_bytes =
            frame_pair_compaction_enabled(config) ? pixels * sizeof(wavefront_wire::Word) : 16;
        const auto state_allocation = std::size_t(config.capacity) * state_bytes;
        const bool separated = config.packet_layout != FramePacketLayout::packed;
        const bool hero = r.parameters.packet_transport == 2;
        const bool contribution = frame_area_contribution_family(config.area_path_domain);
        if (contribution && contribution_evaluation == nil)
            return "Contribution wavefront requires the shared Area evaluation buffer";
        if (!allocate(b.hero_state, hero && !separated ? state_allocation : 16) ||
            !allocate(b.hero_cold, hero && separated ? std::size_t(config.capacity) *
                                                           wavefront->contract.hero_cold_bytes
                                                     : 16) ||
            !allocate(b.packet_hot, separated ? state_allocation : 16) ||
            !allocate(b.packet_cold, separated && !hero ? std::size_t(config.capacity) *
                                                              wavefront->contract.cold_bytes
                                                        : 16) ||
            !allocate(b.packet_hit,
                      config.packet_layout == FramePacketLayout::split_trace
                          ? std::size_t(config.capacity) * wavefront->contract.hit_bytes
                          : 16) ||
            !allocate(b.replay, 32) || !allocate(b.contribution_evaluation, 16) ||
            !allocate(b.enhanced_records, 16) || !allocate(b.enhanced_evaluation, 16) ||
            !allocate(b.enhanced_suffix, 16) || !allocate(b.work_list, work_list_bytes) ||
            !allocate(b.donors, sizeof(motion_wire::Donor)) ||
            !allocate(b.contribution_state,
                      contribution ? std::size_t(config.capacity) *
                                         (config.area_path_domain == FrameAreaPathDomain::enhanced
                                              ? wavefront->contract.enhanced_state_bytes
                                              : wavefront->contract.contribution_state_bytes)
                                   : 16) ||
            !allocate(b.config, sizeof(wavefront_wire::Config), true) ||
            !allocate(b.scalar_state, r.parameters.packet_transport ? 16 : state_allocation) ||
            !allocate(b.packet_state, r.parameters.packet_transport && !hero && !separated
                                          ? state_allocation
                                          : 16) ||
            !allocate(b.queue[0], std::size_t(config.capacity) * 4) ||
            !allocate(b.queue[1], std::size_t(config.capacity) * 4) ||
            !allocate(b.free_slots, std::size_t(config.capacity) * 4) ||
            !allocate(b.counters, sizeof(wavefront_wire::Counters), true) ||
            !allocate(b.done, std::size_t(config.width) * config.height * 4) ||
            !allocate(b.current_dispatch, sizeof(wavefront_wire::DispatchArguments)) ||
            !allocate(b.refill_dispatch, sizeof(wavefront_wire::DispatchArguments)) ||
            !allocate(b.arguments[0], wavefront->argument_encoder.encodedLength, true) ||
            !allocate(b.arguments[1], wavefront->argument_encoder.encodedLength, true))
            return "Wavefront slot allocation failed";
        const wavefront_wire::Config cfg{
            config.width * config.height,
            config.capacity,
            r.parameters.packet_transport,
            r.parameters.model,
            config.group_width,
            config.estimator == FrameEstimator::area_replay ? 1u : 0u,
            config.packet_events,
            static_cast<std::uint32_t>(config.packet_layout),
            static_cast<std::uint32_t>(config.area_path_domain),
            contribution ? r.parameters.max_depth : 0u,
            contribution ? config.target_scale : 0.f,
            contribution ? static_cast<std::uint32_t>(config.contribution_replay) : 0u,
            0,
            0,
            frame_pair_compaction_enabled(config) ? 1u : 0u,
            0};
        std::memcpy(b.config.contents, &cfg, sizeof(cfg));
        std::memset(b.counters.contents, 0, b.counters.length);
        for (unsigned variant = 0; variant < 2; ++variant) {
            auto encoder = wavefront->argument_encoder;
            [encoder setArgumentBuffer:b.arguments[variant] offset:0];
            const std::array<id<MTLBuffer>, 25> resources{b.config,
                                                          b.scalar_state,
                                                          b.packet_state,
                                                          b.queue[variant],
                                                          b.queue[variant ^ 1],
                                                          b.free_slots,
                                                          b.counters,
                                                          raw,
                                                          cie,
                                                          b.done,
                                                          b.current_dispatch,
                                                          b.refill_dispatch,
                                                          b.packet_hot,
                                                          b.packet_cold,
                                                          b.packet_hit,
                                                          b.replay,
                                                          b.hero_state,
                                                          b.hero_cold,
                                                          b.contribution_evaluation,
                                                          b.contribution_state,
                                                          b.enhanced_records,
                                                          b.enhanced_evaluation,
                                                          b.enhanced_suffix,
                                                          b.work_list,
                                                          b.donors};
            for (NSUInteger index = 0; index < resources.size(); ++index)
                [encoder setBuffer:resources[index] offset:0 atIndex:index];
        }
        return {};
    }
    std::string rebase_camera(Slot &slot, wire::Point origin, std::uint64_t &uploaded) {
        auto &r = *renderer->impl_;
        if (slot.rebased_origin && slot.rebased_origin->x == origin.x &&
            slot.rebased_origin->y == origin.y && slot.rebased_origin->z == origin.z)
            return {};
        // A slot is available only after its previous frame and GPU lease are
        // gone. Rewrite that slot's shared upload buffers from immutable originals.
        if (r.graph_parameters.node_count && slot.graph_nodes == nil)
            slot.graph_nodes = [r.device newBufferWithLength:r.graph_nodes.length
                                                     options:MTLResourceStorageModeShared];
        if (r.surface_transforms != nil && slot.surface_transforms == nil)
            slot.surface_transforms = [r.device newBufferWithLength:r.surface_transforms.length
                                                            options:MTLResourceStorageModeShared];
        if ((r.graph_parameters.node_count && slot.graph_nodes == nil) ||
            (r.surface_transforms != nil && slot.surface_transforms == nil))
            return "Camera mapping rebase buffer allocation failed";
        const std::array<double, 3> delta{double(origin.x) - r.parameters.origin.x,
                                          double(origin.y) - r.parameters.origin.y,
                                          double(origin.z) - r.parameters.origin.z};
        // Do not retain the old cache key if validation fails after a partial write.
        slot.rebased_origin.reset();
        if (slot.graph_nodes != nil) {
            std::memcpy(slot.graph_nodes.contents, r.graph_nodes.contents, r.graph_nodes.length);
            auto *nodes = static_cast<texture_graph_wire::Node *>(slot.graph_nodes.contents);
            for (std::size_t n = 0; n < r.graph_parameters.node_count; ++n) {
                auto &m = nodes[n].mapping.texture_from_render;
                for (unsigned row = 0; row < 3; ++row) {
                    double t = m[row * 4 + 3];
                    for (unsigned axis = 0; axis < 3; ++axis)
                        t += double(m[row * 4 + axis]) * delta[axis];
                    m[row * 4 + 3] = static_cast<float>(t);
                    if (!std::isfinite(m[row * 4 + 3]))
                        return "Camera texture graph rebase exceeds the finite domain";
                }
            }
            uploaded += slot.graph_nodes.length;
        }
        if (slot.surface_transforms != nil) {
            std::memcpy(slot.surface_transforms.contents, r.surface_transforms.contents,
                        r.surface_transforms.length);
            auto *transforms =
                static_cast<wire::SurfaceTransform *>(slot.surface_transforms.contents);
            for (std::size_t n = 0;
                 n < r.surface_transforms.length / sizeof(wire::SurfaceTransform); ++n) {
                auto &t = transforms[n].definition_translation;
                t = {static_cast<float>(double(t.x) - delta[0]),
                     static_cast<float>(double(t.y) - delta[1]),
                     static_cast<float>(double(t.z) - delta[2])};
                if (!finite(t))
                    return "Camera surface translation rebase exceeds the finite domain";
            }
            uploaded += slot.surface_transforms.length;
        }
        slot.rebased_origin = origin;
        return {};
    }
    std::string snapshot_area_context(FrameAreaContext &context, const wire::Parameters &p,
                                      const wire::TextureParameters &texture, id<MTLBuffer> graph,
                                      id<MTLBuffer> surfaces, std::uint64_t &uploaded) {
        // Only the non-front bank is written while no Area frame is pending.
        context.suffix_context = 0; // No token belongs to an incomplete snapshot.
        // Cache by numerical origin, never by reusable slot-buffer identity.
        const bool same_origin = context.copied_origin && context.copied_origin->x == p.origin.x &&
                                 context.copied_origin->y == p.origin.y &&
                                 context.copied_origin->z == p.origin.z;
        if (graph == nil && surfaces == nil) {
            context.graph_nodes = nil;
            context.surface_transforms = nil;
            context.copied_origin.reset();
        } else if (!same_origin) {
            auto &r = *renderer->impl_;
            const auto copy = [&](id<MTLBuffer> source, id<MTLBuffer> __strong &destination) {
                if (source == nil) {
                    destination = nil;
                    return true;
                }
                if (destination == nil)
                    destination = [r.device newBufferWithLength:source.length
                                                        options:MTLResourceStorageModeShared];
                if (destination == nil)
                    return false;
                std::memcpy(destination.contents, source.contents, source.length);
                uploaded += source.length;
                return true;
            };
            context.copied_origin.reset();
            if (!copy(graph, context.graph_nodes) || !copy(surfaces, context.surface_transforms))
                return "Area source-context mapping allocation failed";
            context.copied_origin = p.origin;
        }
        context.parameters = p;
        context.texture = texture;
        context.domain = {static_cast<std::uint32_t>(config.area_path_domain),
                          frame_area_contribution_family(config.area_path_domain) ? p.max_depth
                                                                                  : 0u,
                          config.target_scale,
                          frame_area_contribution_family(config.area_path_domain)
                              ? static_cast<std::uint32_t>(config.contribution_replay)
                              : 0u,
                          frame_area_canonical_version(config.area_path_domain),
                          frame_area_discovery_version(config.area_path_domain)};
        return {};
    }
    std::string assign_suffix_context(unsigned destination_index) {
        auto &h = *history;
        auto &destination = h.contexts[destination_index];
        destination.suffix_context = 0;
        if (!frame_suffix_cache_enabled(config))
            return {};
        const auto &source = h.contexts[h.front];
        if (h.valid && source.suffix_context && same_area_evaluation_context(source, destination)) {
            destination.suffix_context = source.suffix_context;
            return {};
        }
        if (h.suffix_context_exhausted)
            return "Enhanced suffix context identity exhausted";
        destination.suffix_context = h.next_suffix_context;
        if (h.next_suffix_context == std::numeric_limits<std::uint64_t>::max())
            h.suffix_context_exhausted = true;
        else
            ++h.next_suffix_context;
        return {};
    }
    struct Runner : std::enable_shared_from_this<Runner> {
        std::shared_ptr<Frame::Impl> frame;
        std::shared_ptr<FrameWavefrontPrograms> programs;
        std::shared_ptr<FrameAreaPrograms> area_programs;
        std::shared_ptr<FrameMotionPrograms> motion_programs;
        std::shared_ptr<FrameDuplicationPrograms> duplication_programs;
        std::shared_ptr<FrameAreaHistory> history;
        bool motion_generate = false, motion_mapped = false, motion_validated = false;
        unsigned motion_destination = 0;
        wire::Parameters motion_parameters{};
        id<MTLBuffer> history_source = nil, history_destination = nil;
        unsigned history_destination_index = 0, area_leaf = 0;
        FrameAreaContext source_context;
        int area_merge = -1;
        bool area_reset = true, area_finalizing = false;
        std::uint32_t leaf_completed = 0, leaf_null_completed = 0;
        std::optional<std::uint32_t> leaf_admitted_work_count;
        std::uint64_t pending_cached_evaluations = 0, leaf_packet_step_invocations = 0;
        MetalRenderScene::Impl *renderer = nullptr; // retained by frame's resource lease
        FrameConfig config;
        wire::Parameters parameters{};
        wire::TextureParameters texture{};
        id<MTLBuffer> cie = nil;
        unsigned variant = 0;
        bool first = true, all_gpu_times_valid = true;
        double gpu_seconds = 0;
        double previous_gpu_end = 0, last_encode_seconds = 0;

        void record_work(id<MTLCommandBuffer> command, std::uint64_t completed,
                         std::uint32_t active, std::uint64_t packet_step_invocations = 0) {
            if (!config.profile_stages)
                return;
            const char *kind = !history ? "independent" : "fresh";
            std::uint32_t index = 0;
            if (history) {
                if (area_finalizing)
                    kind = "finalize";
                else if (area_leaf == area_wire::temporal_reverse_config)
                    kind = "temporal-reverse";
                else if (area_leaf == 1)
                    kind = "temporal-forward";
                else if (area_leaf >= area_wire::mutation_config_begin) {
                    kind = "mutation";
                    index = area_leaf - area_wire::mutation_config_begin;
                } else if (area_leaf >= 3) {
                    kind = "spatial";
                    index = area_leaf - 3;
                }
            }
            const double begin = command.GPUStartTime, end = command.GPUEndTime;
            const bool valid =
                all_gpu_times_valid && (previous_gpu_end == 0 || begin >= previous_gpu_end);
            const double gap = previous_gpu_end == 0 ? 0 : begin - previous_gpu_end;
            previous_gpu_end = end;
            std::lock_guard lock(frame->info_mutex);
            add_frame_work(frame->info.path_work, kind, index, area_finalizing ? 0 : config.quantum,
                           completed, active, end - begin, gap, last_encode_seconds, valid,
                           packet_step_invocations);
        }

        bool area_failed() const {
            if (!history)
                return false;
            std::uint32_t failure = 0;
            std::memcpy(&failure, frame->retention->area->failure.contents, sizeof(failure));
            return failure != 0;
        }
        std::string area_failure_diagnostic() const {
            area_wire::Counters counters{};
            std::memcpy(&counters, frame->retention->area->failure.contents, sizeof(counters));
            const auto &record = counters.first_failure;
            std::ostringstream out;
            out << "GPU Area reservoir validation failed: flags=0x" << std::hex << counters.failure
                << std::dec << " mutations=" << counters.mutations
                << " accepted=" << counters.accepted << " reserved=" << counters.reserved
                << " area_leaf=" << area_leaf << " area_merge=" << area_merge
                << " completed_evaluations=" << frame->info.completed_evaluations
                << " analytic_null_evaluations=" << frame->info.analytic_null_evaluations
                << " cached_evaluations=" << frame->info.cached_evaluations
                << " logical_evaluations=" << frame->info.logical_evaluations
                << " first={site=shaders/area_reuse.metal:" << record.source_line
                << " pixel=" << record.pixel << " phase=" << record.phase
                << " candidate=" << record.candidate
                << " serial=" << ((std::uint64_t(record.serial_high) << 32) | record.serial_low)
                << " flags=0x" << std::hex << record.flags << std::dec
                << " substatus=" << record.substatus << " words=[";
            for (unsigned i = 0; i < 16; ++i) {
                if (i)
                    out << ',';
                out << "0x" << std::hex << std::setw(8) << std::setfill('0') << record.operands[i];
            }
            out << "]}";
            return out.str();
        }
        void complete_area() {
            const auto pixels = std::uint64_t(config.width) * config.height;
            std::string error;
            area_wire::Counters mutation_counters{};
            std::memcpy(&mutation_counters, frame->retention->area->failure.contents,
                        sizeof(mutation_counters));
            const area_wire::FirstFailure empty_record{};
            if (std::memcmp(&mutation_counters.first_failure, &empty_record,
                            sizeof(empty_record)) != 0 &&
                mutation_counters.failure == 0) {
                fail("GPU Area first-failure record is nonzero without a failure");
                return;
            }
            if (mutation_counters.failure || mutation_counters.reserved ||
                mutation_counters.mutations != pixels * config.mutation_steps ||
                mutation_counters.accepted > mutation_counters.mutations) {
                fail("GPU Area mutation accounting mismatch");
                return;
            }
            FrameAreaDuplicationInfo duplication_info;
            if (duplication_programs) {
                try {
                    duplication_info.histogram.resize(291);
                } catch (const std::bad_alloc &) {
                    fail("Duplication CPU summary allocation failed");
                    return;
                }
                edw::Counters c{};
                std::memcpy(&c, frame->retention->area->duplication_counters.contents, sizeof(c));
                std::uint64_t total = 0, weighted = 0;
                for (unsigned i = 0; i < 291; ++i) {
                    duplication_info.histogram[i] = c.histogram[i];
                    total += c.histogram[i];
                    if (i <= 288) {
                        duplication_info.valid_pixels += c.histogram[i];
                        weighted += std::uint64_t(i) * c.histogram[i];
                    }
                }
                if (c.failure || c.temporal_completed != pixels || c.final_completed != pixels ||
                    c.counted_completed != pixels || total != pixels || c.histogram[290]) {
                    std::ostringstream out;
                    out << "Enhanced duplication diagnostic accounting mismatch: flags="
                        << c.failure << " temporal=" << c.temporal_completed
                        << " final=" << c.final_completed << " counted=" << c.counted_completed
                        << " histogram=" << total << " unknown=" << c.histogram[290]
                        << " expected=" << pixels;
                    fail(out.str());
                    return;
                }
                duplication_info.mode = config.area_duplication;
                duplication_info.version = edw::contract;
                duplication_info.denominator = 288;
                duplication_info.null_pixels = c.histogram[289];
                duplication_info.unknown_pixels = c.histogram[290];
                if (duplication_info.valid_pixels)
                    duplication_info.mean_d =
                        double(weighted) / (288.0 * double(duplication_info.valid_pixels));
            }
            {
                std::lock_guard history_lock(history->mutex);
                if (!history->pending || history->pending_serial != frame->info.area_serial ||
                    history->epoch != frame->info.epoch)
                    error = "Area history publication token mismatch";
                else if (history->generation == std::numeric_limits<std::uint64_t>::max())
                    error = "Area history generation exhausted";
                else if (motion_generate && !motion_validated)
                    error = "Enhanced motion geometry was not validated before publication";
                else {
                    std::lock_guard frame_lock(frame->info_mutex);
                    const bool accounting =
                        config.area_path_domain == FrameAreaPathDomain::enhanced
                            ? valid_enhanced_area_evaluation_accounting(
                                  pixels, frame->info.history_used, config.mutation_steps,
                                  frame->info.temporal_evaluation_cache_used,
                                  frame->info.completed_evaluations, frame->info.cached_evaluations,
                                  frame->info.analytic_null_evaluations,
                                  frame->info.logical_evaluations)
                            : frame->info.analytic_null_evaluations == 0 &&
                                  valid_area_evaluation_accounting(
                                      pixels, frame->info.history_used, config.mutation_steps,
                                      frame->info.temporal_evaluation_cache_used,
                                      frame->info.completed_evaluations,
                                      frame->info.cached_evaluations,
                                      frame->info.logical_evaluations);
                    if (!accounting ||
                        frame->info.compacted_null_evaluations >
                            frame->info.analytic_null_evaluations ||
                        (!frame_pair_compaction_enabled(config) &&
                         frame->info.compacted_null_evaluations))
                        error = "Area evaluation accounting mismatch";
                    else if (duplication_programs && history->fail_next_duplication_publication) {
                        history->fail_next_duplication_publication = false;
                        error = "Enhanced duplication test forced publication failure";
                    } else if (frame_motion_enabled(config) &&
                               history->fail_next_motion_publication) {
                        history->fail_next_motion_publication = false;
                        error = "Enhanced motion test forced publication failure";
                    } else {
                        frame->info.duplication = std::move(duplication_info);
                        history->front = history_destination_index;
                        history->valid = true;
                        history->committed_serial = frame->info.area_serial;
                        ++history->generation;
                        if (frame_motion_enabled(config)) {
                            if (motion_generate)
                                history->geometry_front = motion_destination;
                            history->geometry_valid = true;
                            history->geometry_generation = history->generation;
                            history->latest_motion_generated = motion_generate;
                        }
                        history->pending = false;
                        frame->info.history_generation = history->generation;
                        frame->info.completed_samples = pixels;
                        frame->info.mutation_attempts = mutation_counters.mutations;
                        frame->info.mutation_accepts = mutation_counters.accepted;
                        frame->info.status = FrameStatus::complete;
                        frame->info.gpu_time_valid = all_gpu_times_valid;
                        frame->info.gpu_seconds = all_gpu_times_valid ? gpu_seconds : 0;
                    }
                }
            }
            if (!error.empty()) {
                fail(std::move(error));
                return;
            }
            frame->completed.notify_all();
        }
        void fail(std::string diagnostic) {
            if (history) {
                std::lock_guard lock(history->mutex);
                if (history->pending && history->pending_serial == frame->info.area_serial)
                    history->pending = false;
            }
            {
                std::lock_guard lock(frame->info_mutex);
                frame->info.status = FrameStatus::failed;
                frame->info.diagnostic = std::move(diagnostic);
                frame->info.gpu_time_valid = false;
                invalidate_gpu_stages(frame->info.gpu_stages, "invalid: frame failed");
                invalidate_frame_work(frame->info.path_work, "invalid: frame failed");
            }
            frame->completed.notify_all();
        }
        void finished(id<MTLCommandBuffer> command) {
            if (command.status != MTLCommandBufferStatusCompleted) {
                std::string diagnostic = "Metal wavefront command failed";
                if (command.error)
                    diagnostic += ": " + std::string(command.error.localizedDescription.UTF8String);
                fail(std::move(diagnostic));
                return;
            }
            const bool time_valid = std::isfinite(command.GPUStartTime) &&
                                    std::isfinite(command.GPUEndTime) && command.GPUStartTime > 0 &&
                                    command.GPUEndTime >= command.GPUStartTime &&
                                    std::isfinite(command.GPUEndTime - command.GPUStartTime);
            all_gpu_times_valid &= time_valid;
            if (time_valid)
                gpu_seconds += command.GPUEndTime - command.GPUStartTime;
            all_gpu_times_valid &= std::isfinite(gpu_seconds);
            if (motion_generate && !motion_validated) {
                motion_wire::Counters c{};
                std::memcpy(&c, frame->retention->area->motion_counters.contents, sizeof(c));
                const auto n = std::uint64_t(config.width) * config.height;
                const auto geometry_total =
                    std::uint64_t(c.geometry_hits) + c.geometry_sky + c.geometry_unsupported;
                const auto route_total = std::uint64_t(c.cold) + c.ordinary + c.dual + c.fallback;
                if (c.failure || c.reserved0 || c.reserved1 || c.geometry_completed != n ||
                    geometry_total != n || c.donor_completed != n || route_total != n ||
                    (motion_mapped ? c.cold != 0
                                   : (c.cold != n || c.ordinary || c.dual || c.fallback))) {
                    fail("GPU Enhanced motion counter validation failed (flags " +
                         std::to_string(c.failure) + ")");
                    return;
                }
                motion_validated = true;
                std::lock_guard lock(frame->info_mutex);
                frame->info.enhanced_geometry_generated = c.geometry_completed;
                frame->info.enhanced_temporal_reprojection_used = motion_mapped;
                frame->info.enhanced_motion_routes = {c.cold, c.ordinary, c.dual, c.fallback};
            }
            if (area_failed()) {
                fail(area_failure_diagnostic());
                return;
            }
            if (history && area_finalizing) {
                record_work(command, 0, 0);
                complete_area();
                return;
            }
            wavefront_wire::Counters counters{};
            const auto &buffers = *frame->retention->wavefront;
            std::memcpy(&counters, buffers.counters.contents, sizeof(counters));
            const auto pixels = std::uint64_t(config.width) * config.height;
            const std::uint64_t suffix_attempts = std::uint64_t(counters.suffix_hits[0]) +
                                                  counters.suffix_hits[1] +
                                                  counters.suffix_hits[2] + counters.suffix_misses;
            if (suffix_attempts > pixels ||
                (!frame_suffix_cache_enabled(config) && suffix_attempts)) {
                fail("GPU suffix cache counter accounting mismatch");
                return;
            }
            const bool compact_pairs = frame_pair_compaction_enabled(config);
            if ((!compact_pairs && counters.work_count) ||
                (compact_pairs &&
                 (counters.work_count > pixels ||
                  std::uint64_t(counters.work_count) + counters.null_completed != pixels ||
                  counters.next_pixel < counters.null_completed ||
                  counters.current > counters.work_count ||
                  (leaf_admitted_work_count &&
                   *leaf_admitted_work_count != counters.work_count)))) {
                fail("GPU Enhanced pair admission counter accounting mismatch");
                return;
            }
            if (counters.failure || counters.null_completed > counters.completed ||
                (config.area_path_domain != FrameAreaPathDomain::enhanced &&
                 counters.null_completed) ||
                (history && area_leaf == 0 && counters.null_completed) ||
                counters.completed > pixels || counters.next_pixel > pixels ||
                counters.current > config.capacity || counters.out || counters.free ||
                counters.refill_begin || counters.next_pixel < counters.completed ||
                counters.current != counters.next_pixel - counters.completed) {
                fail("GPU wavefront validation failed: sticky failure or invalid queue/sample "
                     "accounting (flags " +
                     std::to_string(counters.failure) + ")");
                return;
            }
            if (compact_pairs)
                leaf_admitted_work_count = counters.work_count;
            if (counters.completed < leaf_completed ||
                counters.null_completed < leaf_null_completed ||
                counters.null_completed - leaf_null_completed >
                    counters.completed - leaf_completed) {
                fail("GPU replay completion counter regressed");
                return;
            }
            std::uint64_t packet_step_delta = 0;
            if (config.profile_stages && parameters.packet_transport != 0 &&
                config.packet_layout == FramePacketLayout::packed && config.packet_events == 1) {
                const auto queued =
                    (std::uint64_t(counters.queued_slots_hi) << 32) | counters.queued_slots_lo;
                if (!frame_packet_step_delta(queued, counters.null_completed, compact_pairs,
                                             leaf_packet_step_invocations, packet_step_delta)) {
                    fail("GPU packet step invocation counter regressed or underflowed");
                    return;
                }
            }
            record_work(command,
                        (counters.completed - leaf_completed) -
                            (counters.null_completed - leaf_null_completed),
                        counters.current, packet_step_delta);
            {
                std::lock_guard lock(frame->info_mutex);
                frame->info.completed_evaluations +=
                    (counters.completed - leaf_completed) -
                    (counters.null_completed - leaf_null_completed);
                frame->info.analytic_null_evaluations +=
                    counters.null_completed - leaf_null_completed;
                frame->info.cached_evaluations += pending_cached_evaluations;
                pending_cached_evaluations = 0;
                frame->info.logical_evaluations = frame->info.completed_evaluations +
                                                  frame->info.cached_evaluations +
                                                  frame->info.analytic_null_evaluations;
                if (!history)
                    frame->info.completed_samples = counters.completed;
                if (counters.completed == pixels) {
                    if (compact_pairs)
                        frame->info.compacted_null_evaluations += counters.null_completed;
                    for (unsigned op = 0; op < 3; ++op)
                        frame->info.enhanced_suffix_hits[op] += counters.suffix_hits[op];
                    frame->info.enhanced_suffix_misses += counters.suffix_misses;
                    if (!history || area_leaf == 0)
                        frame->info.fresh_candidates = pixels;
                    if (!history) {
                        frame->info.status = FrameStatus::complete;
                        frame->info.gpu_time_valid = all_gpu_times_valid;
                        frame->info.gpu_seconds = all_gpu_times_valid ? gpu_seconds : 0;
                    }
                }
            }
            leaf_completed = counters.completed;
            leaf_null_completed = counters.null_completed;
            if (counters.completed == pixels) {
                if (!history) {
                    frame->completed.notify_all();
                    return;
                }
                area_merge = static_cast<int>(area_leaf);
                if (area_leaf == 6)
                    area_finalizing = true;
                else if (area_leaf == 0 && frame->info.history_used)
                    area_leaf = area_wire::temporal_reverse_config;
                else if (area_leaf == area_wire::temporal_reverse_config)
                    area_leaf = 1;
                else if (area_leaf == 0 || area_leaf == 1)
                    area_leaf = config.mutation_steps ? area_wire::mutation_config_begin : 3;
                else if (area_leaf >= area_wire::mutation_config_begin &&
                         area_leaf < area_wire::temporal_reverse_config)
                    area_leaf =
                        area_leaf - area_wire::mutation_config_begin + 1 < config.mutation_steps
                            ? area_leaf + 1
                            : 3;
                else
                    ++area_leaf;
                first = true;
                variant = 0;
                leaf_completed = 0;
                leaf_null_completed = 0;
                leaf_packet_step_invocations = 0;
                leaf_admitted_work_count.reset();
                launch();
                return;
            }
            if (!counters.current) {
                fail("GPU wavefront made no progress with unfinished pixels");
                return;
            }
            launch();
        }
        void launch() {
            @autoreleasepool {
                const auto encode_start = config.profile_stages
                                              ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point{};
                auto &r = *renderer;
                auto &b = *frame->retention->wavefront;
                if (config.profile_stages) {
                    std::lock_guard lock(frame->info_mutex);
                    auto &work = frame->info.path_work;
                    if (!work.requested) {
                        work.requested = true;
                        work.packet_step_invocations_supported =
                            parameters.packet_transport != 0 &&
                            config.packet_layout == FramePacketLayout::packed &&
                            config.packet_events == 1;
                        work.status = "available: command-level path passes";
                        work.state_pool_bytes =
                            b.scalar_state.length + b.packet_state.length + b.packet_hot.length +
                            b.packet_cold.length + b.packet_hit.length + b.hero_state.length +
                            b.hero_cold.length + b.contribution_state.length +
                            b.enhanced_records.length + b.enhanced_evaluation.length +
                            b.enhanced_suffix.length + b.work_list.length + b.donors.length;
                    }
                }
                const bool reverse = history && area_leaf == area_wire::temporal_reverse_config &&
                                     !frame->info.temporal_evaluation_cache_used;
                // launch follows completion of the prior consuming command. No
                // in-flight command observes mutation of this slot-owned config.
                auto *wavefront_config = static_cast<wavefront_wire::Config *>(b.config.contents);
                wavefront_config->mapped_evaluation = motion_mapped && reverse ? 1u : 0u;
                const auto &evaluation_parameters =
                    reverse ? source_context.parameters : parameters;
                const auto &evaluation_texture = reverse ? source_context.texture : texture;
                const auto evaluation_graph =
                    reverse ? source_context.graph_nodes : frame->retention->graph_nodes;
                const auto evaluation_surfaces = reverse ? source_context.surface_transforms
                                                         : frame->retention->surface_transforms;
                auto command = [r.queue commandBuffer];
                command.label = @"rypath wavefront frame quantum";
                auto encoder = [command computeCommandEncoder];
                encoder.label = @"wavefront trace shade refill advance / Area replay";
                auto stages =
                    config.profile_stages ? std::make_shared<MetalGPUStages>(r.device) : nullptr;
                if (command == nil || encoder == nil) {
                    fail("Wavefront command creation failed");
                    return;
                }
                const auto use_resource = [&](id<MTLResource> resource, MTLResourceUsage usage) {
                    if (stages)
                        stages->use_resource(encoder, resource, usage);
                    else
                        [encoder useResource:resource usage:usage];
                };
                const auto read_write = MTLResourceUsageRead | MTLResourceUsageWrite;
                for (auto buffer : {b.scalar_state,
                                    b.packet_state,
                                    b.queue[0],
                                    b.queue[1],
                                    b.free_slots,
                                    b.counters,
                                    b.done,
                                    b.current_dispatch,
                                    b.refill_dispatch,
                                    frame->raw,
                                    b.replay,
                                    b.packet_hot,
                                    b.packet_cold,
                                    b.packet_hit,
                                    b.hero_state,
                                    b.hero_cold,
                                    b.contribution_evaluation,
                                    b.contribution_state,
                                    b.enhanced_records,
                                    b.enhanced_evaluation,
                                    b.enhanced_suffix,
                                    b.work_list})
                    use_resource(buffer, read_write);
                use_resource(b.config, MTLResourceUsageRead);
                use_resource(b.donors, MTLResourceUsageRead);
                use_resource(cie, MTLResourceUsageRead);
                const auto barrier = [&] {
                    [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
                };
                const auto generic = [&](id<MTLComputePipelineState> pipeline, NSUInteger count,
                                         bool indirect, const char *label) {
                    if (stages && !stages->begin(command, encoder, label))
                        return;
                    [encoder setComputePipelineState:pipeline];
                    [encoder setBytes:&evaluation_parameters
                               length:sizeof(evaluation_parameters)
                              atIndex:0];
                    [encoder setBuffer:b.arguments[variant] offset:0 atIndex:1];
                    [encoder setBuffer:r.environment offset:0 atIndex:7];
                    [encoder setBuffer:r.sampling_tables offset:0 atIndex:12];
                    if (indirect)
                        [encoder
                            dispatchThreadgroupsWithIndirectBuffer:b.refill_dispatch
                                              indirectBufferOffset:0
                                             threadsPerThreadgroup:MTLSizeMake(config.group_width,
                                                                               1, 1)];
                    else
                        [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
                            threadsPerThreadgroup:MTLSizeMake(count == 1 ? 1 : config.group_width,
                                                              1, 1)];
                    if (stages)
                        stages->end(encoder);
                    barrier();
                };
                const auto full = [&](id<MTLComputePipelineState> pipeline,
                                      id<MTLIntersectionFunctionTable> table, const char *label) {
                    if (stages && !stages->begin(command, encoder, label))
                        return;
                    [encoder setComputePipelineState:pipeline];
                    r.bind(encoder, evaluation_parameters, evaluation_texture, b.arguments[variant],
                           evaluation_graph, evaluation_surfaces, true, table);
                    [encoder dispatchThreadgroupsWithIndirectBuffer:b.current_dispatch
                                               indirectBufferOffset:0
                                              threadsPerThreadgroup:MTLSizeMake(config.group_width,
                                                                                1, 1)];
                    if (stages)
                        stages->end(encoder);
                    barrier();
                };
                if (history) {
                    auto &a = *frame->retention->area;
                    for (auto buffer :
                         {a.replay, a.leaf, a.accumulators, a.temporal, a.failure, a.cross_targets,
                          a.contribution_evaluation, a.enhanced_records, a.enhanced_evaluation,
                          a.enhanced_suffix, history_destination})
                        use_resource(buffer, read_write);
                    if (duplication_programs) {
                        use_resource(a.duplication_config, MTLResourceUsageRead);
                        use_resource(a.duplication_counters, read_write);
                        use_resource(history->duplication_labels[history_destination_index ^ 1],
                                     MTLResourceUsageRead);
                        use_resource(history->duplication_labels[history_destination_index],
                                     read_write);
                        use_resource(history->duplication_temporal, read_write);
                        use_resource(history->duplication_counts[history_destination_index],
                                     read_write);
                    }
                    use_resource(a.donors, MTLResourceUsageRead);
                    use_resource(a.paired_atlas, MTLResourceUsageRead);
                    use_resource(a.paired_config, MTLResourceUsageRead);
                    if (motion_generate) {
                        use_resource(history->geometry[motion_destination], read_write);
                        use_resource(history->geometry[history->geometry_front],
                                     MTLResourceUsageRead);
                        use_resource(history->donors, read_write);
                        use_resource(a.motion_counters, read_write);
                        use_resource(a.motion_config, MTLResourceUsageRead);
                    }
                    use_resource(history_source, MTLResourceUsageRead);
                    for (auto buffer : a.config)
                        use_resource(buffer, MTLResourceUsageRead);
                    const auto area_dispatch = [&](id<MTLComputePipelineState> pipeline,
                                                   unsigned argument, NSUInteger count,
                                                   const char *label) {
                        if (stages && !stages->begin(command, encoder, label))
                            return;
                        [encoder setComputePipelineState:pipeline];
                        [encoder setBytes:&parameters length:sizeof(parameters) atIndex:0];
                        [encoder setBuffer:a.arguments[argument] offset:0 atIndex:1];
                        if (duplication_programs)
                            [encoder setBuffer:a.duplication_arguments offset:0 atIndex:2];
                        [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
                            threadsPerThreadgroup:MTLSizeMake(count == 1 ? 1 : config.group_width,
                                                              1, 1)];
                        if (stages)
                            stages->end(encoder);
                        barrier();
                    };
                    const auto pixels = std::uint64_t(config.width) * config.height;
                    const auto duplication_dispatch = [&](id<MTLComputePipelineState> pipeline,
                                                          bool count) {
                        if (stages &&
                            !stages->begin(command, encoder,
                                           count ? "duplication.count" : "duplication.reset"))
                            return;
                        [encoder setComputePipelineState:pipeline];
                        [encoder setBuffer:a.duplication_arguments offset:0 atIndex:2];
                        if (count)
                            [encoder dispatchThreadgroups:MTLSizeMake((config.width + 7) / 8,
                                                                      (config.height + 7) / 8, 1)
                                    threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
                        else
                            [encoder dispatchThreads:MTLSizeMake(291, 1, 1)
                                threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                        if (stages)
                            stages->end(encoder);
                        barrier();
                    };
                    if (area_reset && duplication_programs)
                        duplication_dispatch(duplication_programs->reset, false);
                    if (area_reset && motion_generate) {
                        const auto dispatch_motion = [&](id<MTLComputePipelineState> pipeline,
                                                         NSUInteger count, const char *label,
                                                         bool geometry) {
                            if (stages && !stages->begin(command, encoder, label))
                                return;
                            [encoder setComputePipelineState:pipeline];
                            if (geometry)
                                r.bind(encoder, motion_parameters, texture, a.motion_arguments,
                                       frame->retention->graph_nodes,
                                       frame->retention->surface_transforms, true,
                                       motion_programs->table);
                            else
                                [encoder setBuffer:a.motion_arguments offset:0 atIndex:1];
                            [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
                                threadsPerThreadgroup:MTLSizeMake(count == 1 ? 1
                                                                             : config.group_width,
                                                                  1, 1)];
                            if (stages)
                                stages->end(encoder);
                            barrier();
                        };
                        dispatch_motion(motion_programs->reset, 1, "motion.reset", false);
                        dispatch_motion(motion_programs->geometry, pixels, "motion.geometry", true);
                        dispatch_motion(motion_programs->donors, pixels, "motion.donors", false);
                    }
                    if (area_reset) {
                        area_dispatch(area_programs->reset, 0, 1, "area.reset");
                        area_reset = false;
                    }
                    if (area_merge >= 0) {
                        const auto previous = static_cast<unsigned>(area_merge);
                        if (previous >= area_wire::mutation_config_begin &&
                            previous < area_wire::temporal_reverse_config) {
                            area_dispatch(area_programs->mutation_finish, previous, pixels,
                                          "area.mutation_finish");
                            if (previous - area_wire::mutation_config_begin + 1 ==
                                config.mutation_steps)
                                area_dispatch(area_programs->spatial_begin, 2, pixels,
                                              "area.spatial_begin");
                        } else {
                            area_dispatch(area_programs->merge, previous, pixels, "area.merge");
                            if (previous == 1 || (previous == 0 && !frame->info.history_used)) {
                                area_dispatch(area_programs->temporal_finish, previous, pixels,
                                              "area.temporal_finish");
                                if (config.mutation_steps == 0)
                                    area_dispatch(area_programs->spatial_begin, 2, pixels,
                                                  "area.spatial_begin");
                            }
                        }
                        area_merge = -1;
                        if (previous == 0 && frame->info.temporal_evaluation_cache_used) {
                            // Preserve the exact prepare/merge/finish order and RNG
                            // addresses. Only the identical-context leaf walks vanish.
                            for (unsigned cached :
                                 std::array<unsigned, 2>{area_wire::temporal_reverse_config, 1u}) {
                                area_dispatch(area_programs->prepare, cached, pixels,
                                              "area.prepare");
                                area_dispatch(area_programs->cache_evaluation, cached, pixels,
                                              "area.cache_evaluation");
                                area_dispatch(area_programs->merge, cached, pixels, "area.merge");
                            }
                            area_dispatch(area_programs->temporal_finish, 1, pixels,
                                          "area.temporal_finish");
                            if (config.mutation_steps == 0)
                                area_dispatch(area_programs->spatial_begin, 2, pixels,
                                              "area.spatial_begin");
                            pending_cached_evaluations += 2 * pixels;
                            area_leaf =
                                config.mutation_steps ? area_wire::mutation_config_begin : 3;
                        }
                    }
                    if (area_finalizing) {
                        area_dispatch(area_programs->finalize, area_wire::spatial_finalize_config,
                                      pixels, "area.finalize");
                        if (duplication_programs)
                            duplication_dispatch(duplication_programs->count, true);
                    } else if (first) {
                        const bool mutation = area_leaf >= area_wire::mutation_config_begin &&
                                              area_leaf < area_wire::temporal_reverse_config;
                        area_dispatch(
                            mutation ? area_programs->mutation_prepare : area_programs->prepare,
                            area_leaf, pixels, mutation ? "area.mutation_prepare" : "area.prepare");
                    }
                }
                if (!area_finalizing && first) {
                    generic(programs->reset, std::uint64_t(config.width) * config.height, false,
                            "wavefront.reset");
                    if (frame_pair_compaction_enabled(config)) {
                        generic(programs->admit_all_pixels,
                                std::uint64_t(config.width) * config.height, false,
                                "wavefront.admit_all_pixels");
                        generic(programs->finalize_admission, 1, false,
                                "wavefront.finalize_admission");
                    }
                    generic(programs->initialize, config.capacity, false, "wavefront.initialize");
                    first = false;
                }
                for (std::uint32_t round = 0; !area_finalizing && round < config.quantum; ++round) {
                    if (!parameters.packet_transport ||
                        config.packet_layout == FramePacketLayout::split_trace)
                        full(programs->trace, programs->trace_table,
                             parameters.packet_transport ? "wavefront.packet_trace"
                                                         : "wavefront.trace");
                    full(programs->shade, programs->shade_table, "wavefront.shade");
                    generic(programs->prepare_refill, 1, false, "wavefront.prepare_refill");
                    generic(programs->refill, 0, true, "wavefront.refill");
                    generic(programs->advance, 1, false, "wavefront.advance");
                    variant ^= 1;
                }
                [encoder endEncoding];
                if (stages && stages->encoding_failed()) {
                    fail("Profiling compute encoder allocation failed");
                    return;
                }
                {
                    std::lock_guard lock(frame->info_mutex);
                    ++frame->info.command_count;
                }
                // No command is retained by the runner or frame. The handler
                // owns the runner/lease until it resumes or completes the work.
                const auto self = shared_from_this();
                [command addCompletedHandler:^(id<MTLCommandBuffer> finished_command) {
                  @autoreleasepool {
                      if (stages) {
                          std::lock_guard lock(self->frame->info_mutex);
                          stages->resolve(finished_command, self->frame->info.gpu_stages);
                      }
                      self->finished(finished_command);
                  }
                }];
                if (config.profile_stages)
                    last_encode_seconds = std::chrono::duration<double>(
                                              std::chrono::steady_clock::now() - encode_start)
                                              .count();
                [command commit];
            }
        }
    };
};
Frame::Frame(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
Frame::~Frame() = default;
Frame::Frame(Frame &&) noexcept = default;
Frame &Frame::operator=(Frame &&) noexcept = default;
FrameInfo Frame::poll() const {
    if (!impl_)
        return {.status = FrameStatus::failed, .diagnostic = "Frame is moved-from"};
    std::lock_guard lock(impl_->info_mutex);
    return impl_->info;
}
FrameInfo Frame::wait() const {
    if (!impl_)
        return poll();
    std::unique_lock lock(impl_->info_mutex);
    impl_->completed.wait(lock, [&] { return impl_->info.status != FrameStatus::pending; });
    return impl_->info;
}
FrameCapture Frame::capture() const {
    @autoreleasepool {
        if (!impl_)
            return {.diagnostic = "Frame is moved-from"};
        std::lock_guard capture_lock(impl_->capture_mutex);
        const auto info = wait();
        FrameCapture result;
        result.width = info.width;
        result.height = info.height;
        if (info.status != FrameStatus::complete) {
            result.diagnostic = info.diagnostic;
            return result;
        }
        auto staging = [impl_->device newBufferWithLength:impl_->raw.length
                                                  options:MTLResourceStorageModeShared];
        if (staging == nil) {
            result.diagnostic = "Frame capture allocation failed";
            return result;
        }
        auto command = [impl_->queue commandBuffer];
        auto encoder = [command blitCommandEncoder];
        if (command == nil || encoder == nil) {
            result.diagnostic = "Frame capture encoder creation failed";
            return result;
        }
        [encoder copyFromBuffer:impl_->raw
                   sourceOffset:0
                       toBuffer:staging
              destinationOffset:0
                           size:impl_->raw.length];
        [encoder endEncoding];
        result.diagnostic = metal::complete(command, "frame capture");
        if (!result.diagnostic.empty())
            return result;
        result.raw_xyz.resize(std::size_t(info.width) * info.height);
        static_assert(sizeof(result.raw_xyz[0]) == sizeof(wire::Point));
        std::memcpy(result.raw_xyz.data(), staging.contents, staging.length);
        result.status = GeometryStatus::success;
        return result;
    }
}
FrameSession::FrameSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FrameSession::~FrameSession() = default;
FrameSession::FrameSession(FrameSession &&) noexcept = default;
FrameSession &FrameSession::operator=(FrameSession &&) noexcept = default;
FrameSubmitResult FrameSession::submit(const FrameRequest &request) {
    @autoreleasepool {
        FrameSubmitResult result;
        if (!impl_) {
            result.diagnostic = "Frame session is moved-from";
            return result;
        }
        if (impl_->history) {
            std::lock_guard lock(impl_->history->mutex);
            if (impl_->history->pending) {
                result.busy = true;
                result.diagnostic = "Area history frame is pending";
                return result;
            }
            if (impl_->history->serial_exhausted ||
                impl_->history->generation == std::numeric_limits<std::uint64_t>::max()) {
                result.diagnostic = "Area submission serial or history generation exhausted";
                return result;
            }
        }
        auto slot = std::find_if(impl_->slots.begin(), impl_->slots.end(),
                                 [](const auto &s) { return s.active.expired(); });
        if (slot == impl_->slots.end()) {
            result.busy = true;
            result.diagnostic = "Both frame slots are retained";
            return result;
        }
        auto &r = *impl_->renderer->impl_;
        auto p = r.parameters;
        auto texture = r.texture_parameters;
        if (request.camera && request.camera->camera_medium) {
            const auto medium = *request.camera->camera_medium;
            if (medium != no_medium && !impl_->supports_camera_medium) {
                result.diagnostic = "Camera medium overrides require a scalar PBRT volume session";
                return result;
            }
            if (medium != no_medium && medium >= p.medium_count) {
                result.diagnostic = "Frame camera medium index is outside the scene media";
                return result;
            }
            p.camera_medium = medium;
        }
        auto camera_scene = impl_->camera_scene;
        if (request.camera) {
            camera_scene.camera = request.camera->camera;
            camera_scene.raster_camera = request.camera->raster_camera;
            camera_scene.perspective_lens = request.camera->perspective_lens;
        }
        if (camera_scene.model == RenderModel::pbrt && !camera_scene.raster_camera) {
            result.diagnostic = "PBRT frame camera requires raster coefficients";
            return result;
        }
        if (camera_scene.model == RenderModel::demo &&
            (!(camera_scene.camera.vertical_fov_degrees > 0) ||
             !(camera_scene.camera.vertical_fov_degrees < 180))) {
            result.diagnostic =
                "Demo frame camera field of view must be between zero and 180 degrees";
            return result;
        }
        const auto camera = camera_parameters(camera_scene);
        if (!finite(camera.origin) || !finite(camera.forward) || !finite(camera.right) ||
            !finite(camera.up) || !std::isfinite(camera.film_scale) ||
            !std::isfinite(camera.lens_radius) || camera.lens_radius < 0) {
            result.diagnostic = "Invalid frame camera";
            return result;
        }
        if (p.packet_transport &&
            (!finite(camera.camera_normal) || !std::isfinite(camera.camera_image_area) ||
             !(camera.camera_image_area > 0) || !std::isfinite(camera.camera_plane_depth) ||
             !(camera.camera_plane_depth > 0))) {
            result.diagnostic = "Frame packet camera projection is invalid";
            return result;
        }
        p.origin = camera.origin;
        p.forward = camera.forward;
        p.right = camera.right;
        p.up = camera.up;
        p.film_scale = camera.film_scale;
        p.lens_radius = camera.lens_radius;
        p.focal_distance = camera.focal_distance;
        p.lens_x = camera.lens_x;
        p.lens_y = camera.lens_y;
        p.camera_normal = camera.camera_normal;
        p.camera_plane_depth = camera.camera_plane_depth;
        p.camera_image_area = camera.camera_image_area;
        const auto &c = impl_->config;
        if (camera_scene.model == RenderModel::pbrt) {
            result.diagnostic = validate_raster_support(*camera_scene.raster_camera,
                                                        camera_scene.filter, c.width, c.height);
            if (result.diagnostic.empty() && camera_scene.perspective_lens)
                result.diagnostic = validate_lens_support(*camera_scene.raster_camera,
                                                          *camera_scene.perspective_lens,
                                                          camera_scene.filter, c.width, c.height);
            if (!result.diagnostic.empty())
                return result;
        }
        if (texture.texture_count || r.graph_parameters.node_count) {
            if (request.camera) {
                if (!request.camera->texture_camera) {
                    result.diagnostic = "Textured camera snapshots require texture camera matrices";
                    return result;
                }
                const auto &tc = *request.camera->texture_camera;
                if (!std::ranges::all_of(tc.world_from_camera,
                                         [](float v) { return std::isfinite(v); }) ||
                    !std::ranges::all_of(tc.camera_from_world,
                                         [](float v) { return std::isfinite(v); })) {
                    result.diagnostic = "Texture camera matrices must be finite";
                    return result;
                }
                for (unsigned row = 0; row < 3; ++row)
                    for (unsigned col = 0; col < 3; ++col) {
                        double ab = 0, ba = 0;
                        for (unsigned k = 0; k < 3; ++k) {
                            ab += double(tc.world_from_camera[row * 3 + k]) *
                                  tc.camera_from_world[k * 3 + col];
                            ba += double(tc.camera_from_world[row * 3 + k]) *
                                  tc.world_from_camera[k * 3 + col];
                        }
                        if (std::abs(ab - (row == col ? 1 : 0)) > 32 * FLT_EPSILON ||
                            std::abs(ba - (row == col ? 1 : 0)) > 32 * FLT_EPSILON) {
                            result.diagnostic = "Texture camera matrices must be inverse pairs";
                            return result;
                        }
                    }
                std::ranges::copy(tc.world_from_camera, texture.world_from_camera);
                std::ranges::copy(tc.camera_from_world, texture.camera_from_world);
            }
            TextureCameraDifferentials differentials;
            result.diagnostic = prepare_texture_camera_differentials(
                unpack(p.forward), unpack(p.right), unpack(p.up), c.width, c.height, differentials);
            if (!result.diagnostic.empty())
                return result;
            texture.minimum_dx = pack(differentials.minimum_dx);
            texture.minimum_dy = pack(differentials.minimum_dy);
            texture.spp_scale =
                static_cast<float>(std::max(.125, 1. / std::sqrt(double(c.footprint_samples))));
        }
        id<MTLBuffer> frame_graph_nodes = nil, frame_surface_transforms = nil;
        std::uint64_t camera_upload_bytes = 0;
        if (p.origin.x != r.parameters.origin.x || p.origin.y != r.parameters.origin.y ||
            p.origin.z != r.parameters.origin.z) {
            result.diagnostic = impl_->rebase_camera(*slot, p.origin, camera_upload_bytes);
            if (!result.diagnostic.empty())
                return result;
            frame_graph_nodes = slot->graph_nodes;
            frame_surface_transforms = slot->surface_transforms;
        }
        p.width = c.width;
        p.height = c.height;
        p.sample_low = static_cast<wire::Word>(request.sample_index);
        p.sample_high = static_cast<wire::Word>(request.sample_index >> 32);
        p.seed_low = static_cast<wire::Word>(c.seed);
        p.seed_high = static_cast<wire::Word>(c.seed >> 32);
        p.material_seed = c.material_seed;
        p.sampler_kind = wire::independent_sampler;
        p.emergency_limit = render_emergency_limit;
        p.boundary_limit = medium_boundary_limit;
        auto frame = std::make_shared<Frame::Impl>();
        frame->device = r.device;
        frame->queue = r.queue;
        frame->resolved_parameters = p;
        frame->resolved_parameters.pixel_begin = 0;
        frame->resolved_parameters.pixel_count = frame->resolved_parameters.lane_count =
            c.width * c.height;
        frame->resolved_texture_parameters = texture;
        frame->resolved_camera = {.camera = camera_scene.camera,
                                  .camera_medium = p.camera_medium,
                                  .raster_camera = camera_scene.raster_camera,
                                  .perspective_lens = camera_scene.perspective_lens,
                                  .texture_camera = TextureCamera{}};
        std::ranges::copy(texture.world_from_camera,
                          frame->resolved_camera.texture_camera->world_from_camera.begin());
        std::ranges::copy(texture.camera_from_world,
                          frame->resolved_camera.texture_camera->camera_from_world.begin());
        frame->intersection_profile = c.intersection_profile;
        frame->library = impl_->frame_library;
        frame->info = {.width = c.width,
                       .height = c.height,
                       .sample_index = request.sample_index,
                       .epoch = request.epoch,
                       .camera_upload_bytes = camera_upload_bytes};
        frame->info.diffuse_events = impl_->diffuse_events;
        frame->info.area_path_domain = c.area_path_domain;
        frame->info.contribution_replay = frame_area_contribution_family(c.area_path_domain)
                                              ? c.contribution_replay
                                              : FrameContributionReplay::full;
        frame->info.canonical_transport_version = frame_area_canonical_version(c.area_path_domain);
        frame->info.fresh_discovery_version = frame_area_discovery_version(c.area_path_domain);
        frame->info.enhanced_suffix_cache = frame_suffix_cache_enabled(c);
        frame->info.enhanced_pair_compaction = frame_pair_compaction_enabled(c);
        frame->info.enhanced_temporal_reprojection_requested = c.enhanced_temporal_reprojection;
        frame->info.enhanced_temporal_reprojection = frame_motion_enabled(c);
        frame->info.contribution_depth =
            frame_area_contribution_family(c.area_path_domain) ? p.max_depth : 0;
        if (slot->raw == nil)
            slot->raw =
                [r.device newBufferWithLength:std::size_t(c.width) * c.height * sizeof(wire::Point)
                                      options:MTLResourceStorageModePrivate];
        if (c.execution == FrameExecution::megakernel && slot->scratch == nil)
            slot->scratch = [r.device newBufferWithLength:impl_->scratch_bytes
                                                  options:MTLResourceStorageModePrivate];
        if (c.execution == FrameExecution::megakernel && slot->status == nil)
            slot->status = [r.device newBufferWithLength:sizeof(frame_wire::Status)
                                                 options:MTLResourceStorageModeShared];
        frame->raw = slot->raw;
        frame->scratch = slot->scratch;
        frame->status = slot->status;
        if (frame->raw == nil || (c.execution == FrameExecution::megakernel &&
                                  (frame->scratch == nil || frame->status == nil))) {
            result.diagnostic = "Frame slot allocation failed";
            return result;
        }
        frame->retention = std::make_shared<Frame::Impl::Retention>();
        frame->retention->renderer = impl_->renderer;
        frame->retention->raw = frame->raw;
        frame->retention->status = frame->status;
        frame->retention->scratch = frame->scratch;
        frame->retention->graph_nodes = frame_graph_nodes;
        frame->retention->surface_transforms = frame_surface_transforms;
        if (c.execution == FrameExecution::wavefront) {
            if (impl_->history) {
                result.diagnostic = impl_->allocate_area(*slot);
                if (!result.diagnostic.empty())
                    return result;
                frame->retention->area = slot->area;
            }
            result.diagnostic = impl_->allocate_wavefront(
                *slot, impl_->history ? slot->area->leaf : frame->raw,
                impl_->history ? slot->area->replay : nil,
                impl_->history ? slot->area->contribution_evaluation : nil,
                impl_->history ? slot->area->enhanced_records : nil,
                impl_->history ? slot->area->enhanced_evaluation : nil,
                impl_->history ? slot->area->enhanced_suffix : nil,
                impl_->history ? slot->area->donors : nil);
            if (!result.diagnostic.empty())
                return result;
            frame->retention->wavefront = slot->wavefront;
            p.pixel_begin = 0;
            p.pixel_count = p.lane_count = c.width * c.height;
            auto runner = std::make_shared<Impl::Runner>();
            runner->frame = frame;
            runner->programs = impl_->wavefront;
            runner->renderer = &r;
            runner->config = c;
            runner->parameters = p;
            runner->texture = texture;
            runner->cie = impl_->cie;
            if (impl_->history) {
                auto &history = *impl_->history;
                std::lock_guard lock(history.mutex);
                if (history.pending) {
                    result.busy = true;
                    result.diagnostic = "Area history frame is pending";
                    return result;
                }
                if (history.serial_exhausted ||
                    history.generation == std::numeric_limits<std::uint64_t>::max()) {
                    result.diagnostic = "Area submission serial or history generation exhausted";
                    return result;
                }
                // Epoch changes invalidate eligibility permanently, including a
                // failed new-epoch submission and a later return to an old value.
                if (!history.epoch_known || history.epoch != request.epoch) {
                    history.epoch_known = true;
                    history.epoch = request.epoch;
                    history.valid = false;
                    history.geometry_valid = false;
                }
                const unsigned destination_index = history.front ^ 1;
                result.diagnostic = impl_->snapshot_area_context(
                    history.contexts[destination_index], p, texture, frame_graph_nodes,
                    frame_surface_transforms, camera_upload_bytes);
                if (!result.diagnostic.empty())
                    return result;
                if (history.valid &&
                    !same_area_path_domain_context(history.contexts[history.front].domain,
                                                   history.contexts[destination_index].domain)) {
                    history.valid = false;
                    history.geometry_valid = false;
                }
                result.diagnostic = impl_->assign_suffix_context(destination_index);
                if (!result.diagnostic.empty())
                    return result;
                frame->info.camera_upload_bytes = camera_upload_bytes;
                const auto serial = history.next_serial;
                if (frame_paired_atlas_enabled(c)) {
                    const auto frozen =
                        paired::frame_config(static_cast<std::uint32_t>(serial),
                                             static_cast<std::uint32_t>(serial >> 32),
                                             static_cast<std::uint32_t>(c.seed),
                                             static_cast<std::uint32_t>(c.seed >> 32));
                    if (frozen.status != paired::Status::ok ||
                        !paired::valid_config(frozen.config) || !impl_->paired_atlas) {
                        result.diagnostic = "Area paired atlas pose generation failed";
                        return result;
                    }
                    std::memcpy(slot->area->paired_config.contents, &frozen.config,
                                sizeof(frozen.config));
                    frame->info.paired_atlas = impl_->paired_atlas->identity;
                    for (unsigned k = 0; k < 4; ++k) {
                        const auto pose = frozen.config.poses[k];
                        frame->info.paired_atlas.poses[k] = {pose.transform, pose.offset_x,
                                                             pose.offset_y};
                    }
                } else {
                    std::memset(slot->area->paired_config.contents, 0,
                                slot->area->paired_config.length);
                }
                runner->duplication_programs = impl_->duplication;
                if (frame_motion_enabled(c)) {
                    if (history.geometry_front > 1 ||
                        (history.valid && (!history.geometry_valid ||
                                           history.geometry_generation != history.generation))) {
                        result.diagnostic =
                            "Enhanced motion published geometry/context identity mismatch";
                        return result;
                    }
                    const bool same =
                        history.valid && history.geometry_valid &&
                        same_area_evaluation_context(history.contexts[history.front],
                                                     history.contexts[destination_index]);
                    runner->motion_programs = impl_->motion;

                    runner->motion_generate = !same;
                    runner->motion_mapped = !same && history.valid;
                    runner->motion_destination = history.geometry_front ^ 1u;
                    if (same)
                        frame->info.enhanced_geometry_reused = std::uint64_t(c.width) * c.height;
                    else {
                        const auto packed = enhanced_motion_host::pack_config_from_parameters(
                            p, history.valid ? history.contexts[history.front].parameters : p,
                            history.valid, serial);
                        const auto geometry = enhanced_motion_host::center_geometry_parameters(p);
                        if (!packed.config || !geometry.parameters) {
                            result.diagnostic =
                                !packed.config ? packed.diagnostic : geometry.diagnostic;
                            return result;
                        }
                        runner->motion_parameters = *geometry.parameters;
                        auto &a = *slot->area;
                        std::memcpy(a.motion_config.contents, &*packed.config,
                                    sizeof(*packed.config));
                        std::memset(a.motion_counters.contents, 0, a.motion_counters.length);
                        auto encoder = impl_->motion->argument_encoder;
                        [encoder setArgumentBuffer:a.motion_arguments offset:0];
                        const std::array<id<MTLBuffer>, 5> resources{
                            a.motion_config, history.geometry[runner->motion_destination],
                            history.geometry[history.geometry_front], history.donors,
                            a.motion_counters};
                        for (NSUInteger index = 0; index < resources.size(); ++index)
                            [encoder setBuffer:resources[index] offset:0 atIndex:index];
                    }
                }
                if (serial == std::numeric_limits<std::uint64_t>::max())
                    history.serial_exhausted = true;
                else
                    ++history.next_serial;
                history.pending = true;
                history.pending_serial = serial;
                frame->info.area_serial = serial;
                frame->info.mutation_policy = c.mutation_policy;
                frame->info.area_stencil = c.area_stencil;
                const auto offsets =
                    c.area_stencil == FrameAreaStencil::reciprocal16
                        ? rypath_area_stencil::draw(static_cast<std::uint32_t>(serial),
                                                    static_cast<std::uint32_t>(serial >> 32),
                                                    static_cast<std::uint32_t>(c.seed),
                                                    static_cast<std::uint32_t>(c.seed >> 32))
                    : c.area_stencil == FrameAreaStencil::paired_gaussian16
                        ? rypath_area_stencil::Offsets{{0, 0}, {0, 0}}
                        : rypath_area_stencil::Offsets{{-1, 0}, {0, -1}};
                frame->info.area_stencil_offsets = {offsets.first.x, offsets.first.y,
                                                    offsets.second.x, offsets.second.y};
                frame->info.history_used = history.valid;
                frame->info.temporal_evaluation_cache_used =
                    history.valid && c.cache_temporal_evaluations && !runner->motion_mapped &&
                    same_area_evaluation_context(history.contexts[history.front],
                                                 history.contexts[destination_index]);
                frame->info.history_generation = history.generation;
                frame->info.temporal_candidates = history.valid ? 2 : 1;
                frame->info.spatial_candidates = area_wire::spatial_candidates;
                runner->history = impl_->history;
                runner->area_programs = impl_->area;
                runner->history_source = history.banks[history.front];
                runner->source_context = history.contexts[history.front];
                runner->history_destination_index = destination_index;
                runner->history_destination = history.banks[runner->history_destination_index];
                impl_->configure_area(
                    *slot, frame->raw, serial, history.valid,
                    frame->info.temporal_evaluation_cache_used, runner->history_source,
                    runner->history_destination, offsets, history.front,
                    history.contexts[destination_index].suffix_context,
                    history.valid ? history.contexts[history.front].suffix_context : 0,
                    runner->motion_mapped);
            }

            slot->active = frame->retention;
            runner->launch();
            {
                std::lock_guard lock(frame->info_mutex);
                if (!runner->history && !frame->info.command_count &&
                    frame->info.status == FrameStatus::failed) {
                    result.diagnostic = frame->info.diagnostic;
                    return result;
                }
            }
            result.frame = std::unique_ptr<Frame>(new Frame(std::move(frame)));
            result.status = GeometryStatus::success;
            return result;
        }
        std::memset(frame->status.contents, 0, sizeof(frame_wire::Status));
        auto command = [r.queue commandBuffer];
        command.label = @"rypath megakernel frame";
        auto encoder = [command computeCommandEncoder];
        encoder.label = @"megakernel render and resolve tiles";
        auto stages = c.profile_stages ? std::make_shared<MetalGPUStages>(r.device) : nullptr;
        if (command == nil || encoder == nil) {
            result.diagnostic = "Frame command creation failed";
            return result;
        }
        const auto pixels = std::uint64_t(c.width) * c.height;
        const auto group = metal::group_width(r.device, r.pipeline);
        for (std::uint64_t begin = 0; begin < pixels; begin += impl_->tile_count) {
            p.pixel_begin = static_cast<wire::Word>(begin);
            p.pixel_count = p.lane_count =
                static_cast<wire::Word>(std::min<std::uint64_t>(pixels - begin, impl_->tile_count));
            frame_wire::Parameters f{
                p.lane_count,
                p.pixel_begin,
                p.sample_low,
                p.sample_high,
                p.packet_transport ? p.packet_lane_bytes
                                   : static_cast<wire::Word>(sizeof(wire::Sample)),
                p.packet_transport,
                p.model,
                static_cast<wire::Word>(p.packet_transport
                                            ? p.lane_count * p.packet_lane_bytes + 64
                                            : ((p.lane_count + group - 1) / group * group + 1) *
                                                  sizeof(wire::Sample))};
            if (stages && !stages->begin(command, encoder, "megakernel.initialize")) {
                result.diagnostic = "Profiling compute encoder allocation failed";
                return result;
            }
            [encoder setComputePipelineState:impl_->initialize];
            [encoder setBytes:&f length:sizeof(f) atIndex:0];
            [encoder setBuffer:frame->scratch offset:0 atIndex:1];
            [encoder dispatchThreads:MTLSizeMake(p.lane_count, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            if (stages)
                stages->end(encoder);
            [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
            if (stages && !stages->begin(command, encoder, "megakernel.render")) {
                result.diagnostic = "Profiling compute encoder allocation failed";
                return result;
            }
            r.encode(encoder, p, texture, frame->scratch, group, frame_graph_nodes,
                     frame_surface_transforms);
            if (stages)
                stages->end(encoder);
            [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
            if (stages && !stages->begin(command, encoder, "megakernel.resolve")) {
                result.diagnostic = "Profiling compute encoder allocation failed";
                return result;
            }
            [encoder setComputePipelineState:impl_->resolve];
            [encoder setBytes:&f length:sizeof(f) atIndex:0];
            [encoder setBuffer:frame->scratch offset:0 atIndex:1];
            [encoder setBuffer:frame->raw offset:0 atIndex:2];
            [encoder setBuffer:frame->status offset:0 atIndex:3];
            [encoder setBuffer:impl_->cie offset:0 atIndex:4];
            [encoder dispatchThreads:MTLSizeMake(p.lane_count, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            if (stages)
                stages->end(encoder);
            [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
        }
        [encoder endEncoding];
        // The callback owns the frame/lease, and the frame owns no command.
        frame->info.command_count = 1;
        [command addCompletedHandler:^(id<MTLCommandBuffer> finished) {
          {
              std::lock_guard lock(frame->info_mutex);
              auto &info = frame->info;
              if (stages)
                  stages->resolve(finished, info.gpu_stages);
              if (finished.status != MTLCommandBufferStatusCompleted) {
                  info.status = FrameStatus::failed;
                  info.diagnostic = "Metal frame command failed";
                  if (finished.error)
                      info.diagnostic +=
                          ": " + std::string(finished.error.localizedDescription.UTF8String);
              } else {
                  frame_wire::Status status{};
                  std::memcpy(&status, frame->status.contents, sizeof(status));
                  info.completed_samples = status.completed;
                  info.gpu_time_valid =
                      std::isfinite(finished.GPUStartTime) && std::isfinite(finished.GPUEndTime) &&
                      finished.GPUStartTime > 0 && finished.GPUEndTime >= finished.GPUStartTime &&
                      std::isfinite(finished.GPUEndTime - finished.GPUStartTime);
                  if (info.gpu_time_valid)
                      info.gpu_seconds = finished.GPUEndTime - finished.GPUStartTime;
                  info.status =
                      status.failure || status.completed != std::uint64_t(info.width) * info.height
                          ? FrameStatus::failed
                          : FrameStatus::complete;
                  if (info.status == FrameStatus::complete)
                      info.logical_evaluations = info.completed_evaluations =
                          info.fresh_candidates = status.completed;
                  if (info.status == FrameStatus::failed) {
                      invalidate_gpu_stages(info.gpu_stages, "invalid: frame validation failed");
                      info.diagnostic =
                          "GPU frame validation failed: invalid, missing, or non-finite sample";
                  }
              }
          }
          frame->completed.notify_all();
        }];
        slot->active = frame->retention;
        [command commit];
        result.frame = std::unique_ptr<Frame>(new Frame(std::move(frame)));
        result.status = GeometryStatus::success;
        return result;
    }
}
FrameSessionBuildResult build_frame_session(const RenderScene &scene, const FrameConfig &config,
                                            const std::filesystem::path &library_path) {
    FrameSessionBuildResult result;
    result.shadow_traversal.requested = config.shadow_traversal;
    result.acceleration_build.requested = config.acceleration_build_policy;
    const auto pixels = std::uint64_t(config.width) * config.height;
    if (!pixels || pixels > max_index || !config.footprint_samples ||
        !config.transient_byte_limit) {
        result.diagnostic = "Invalid frame dimensions, footprint budget, or memory budget";
        return result;
    }
    if (frame_motion_enabled(config) &&
        !enhanced_motion_host::valid_dimensions(config.width, config.height)) {
        result.diagnostic = "Enhanced motion dimensions must be 1..2^23 with a 32-bit pixel count";
        return result;
    }
    if (config.area_duplication != FrameAreaDuplication::off &&
        (static_cast<unsigned>(config.area_duplication) > 3 ||
         config.execution != FrameExecution::wavefront ||
         config.estimator != FrameEstimator::area_replay ||
         config.area_path_domain != FrameAreaPathDomain::enhanced)) {
        result.diagnostic = "Area duplication diagnostics require wavefront Enhanced Area";
        return result;
    }
    if (config.mutation_policy != FrameMutationPolicy::automatic &&
        config.mutation_policy != FrameMutationPolicy::legacy &&
        config.mutation_policy != FrameMutationPolicy::spectral_refresh) {
        result.diagnostic = "Invalid Area mutation policy";
        return result;
    }
    if (config.area_stencil != FrameAreaStencil::automatic &&
        config.area_stencil != FrameAreaStencil::adjacent &&
        config.area_stencil != FrameAreaStencil::reciprocal16 &&
        config.area_stencil != FrameAreaStencil::paired_gaussian16) {
        result.diagnostic = "Invalid Area stencil";
        return result;
    }
    if (config.mutation_steps != 0 && config.mutation_steps != 1 && config.mutation_steps != 2 &&
        config.mutation_steps != 4) {
        result.diagnostic = "Area mutation steps must be 0, 1, 2, or 4";
        return result;
    }
    if (config.estimator == FrameEstimator::area_replay &&
        pixels * config.mutation_steps > std::numeric_limits<std::uint32_t>::max()) {
        result.diagnostic = "Area mutation attempt count exceeds the GPU counter domain";
        return result;
    }
    if (config.estimator != FrameEstimator::independent &&
        config.estimator != FrameEstimator::area_replay) {
        result.diagnostic = "Invalid frame estimator";
        return result;
    }
    if (config.area_path_domain != FrameAreaPathDomain::summed &&
        !frame_area_contribution_family(config.area_path_domain)) {
        result.diagnostic = "Invalid Area path domain";
        return result;
    }
    if (config.contribution_replay != FrameContributionReplay::full &&
        config.contribution_replay != FrameContributionReplay::prefix) {
        result.diagnostic = "Invalid contribution replay mode";
        return result;
    }
    if (config.contribution_replay == FrameContributionReplay::prefix &&
        !frame_area_contribution_family(config.area_path_domain)) {
        result.diagnostic = "Prefix replay requires contribution path domain";
        return result;
    }
    if (frame_area_contribution_family(config.area_path_domain)) {
        if (config.estimator != FrameEstimator::area_replay ||
            config.execution != FrameExecution::wavefront) {
            result.diagnostic = "Contribution path domain requires wavefront Area replay";
            return result;
        }
        if (scene.spectral_transport != SpectralTransport::hero ||
            scene.model != RenderModel::pbrt) {
            result.diagnostic = "Contribution path domain requires four-wavelength hero transport";
            return result;
        }
        if (scene.max_depth > 65) {
            result.diagnostic =
                "Contribution path domain requires finite authored depth from 0 to 65";
            return result;
        }
        if (config.packet_layout != FramePacketLayout::packed || config.packet_events != 1) {
            result.diagnostic =
                "Contribution path domain requires packed wavefront state and one event";
            return result;
        }
    }
    if (config.area_output != FrameAreaOutput::selected &&
        config.area_output != FrameAreaOutput::conditional_mean) {
        result.diagnostic = "Invalid Area output mode";
        return result;
    }
    if (!std::isnormal(config.target_scale) || !(config.target_scale > 0)) {
        result.diagnostic = "Area target scale must be positive and normal";
        return result;
    }
    if (config.estimator == FrameEstimator::area_replay &&
        config.execution != FrameExecution::wavefront) {
        result.diagnostic = "Area replay requires wavefront execution";
        return result;
    }
    if (config.execution != FrameExecution::megakernel &&
        config.execution != FrameExecution::wavefront) {
        result.diagnostic = "Invalid frame execution mode";
        return result;
    }
    if (config.execution == FrameExecution::wavefront &&
        (!config.capacity || config.capacity > wavefront_wire::default_capacity ||
         !config.quantum || config.quantum > 1024 || !config.group_width ||
         config.group_width > 256 ||
         (config.packet_events != 1 && config.packet_events != 2 && config.packet_events != 4))) {
        result.diagnostic = "Invalid wavefront capacity, quantum, group width, or packet events";
        return result;
    }
    if (config.packet_layout != FramePacketLayout::packed &&
        config.packet_layout != FramePacketLayout::hot_cold &&
        config.packet_layout != FramePacketLayout::split_trace) {
        result.diagnostic = "Invalid packet layout";
        return result;
    }
    if ((config.packet_events != 1 || config.packet_layout != FramePacketLayout::packed) &&
        config.execution != FrameExecution::wavefront) {
        result.diagnostic = "Packet scheduling controls require wavefront execution";
        return result;
    }
    if (config.packet_layout == FramePacketLayout::split_trace && config.packet_events != 1) {
        result.diagnostic = "Split packet trace requires packet_events=1";
        return result;
    }
    if (scene.integrator == IntegratorKind::bdpt || std::isfinite(scene.max_component_value)) {
        result.diagnostic =
            "Live frame sessions require explicit PT and unclamped raw scene overrides";
        return result;
    }
    auto built = build_metal_renderer(
        scene, library_path,
        RenderBuildOptions{.intersection_profile = config.intersection_profile,
                           .shadow_traversal = config.shadow_traversal,
                           .acceleration_build_policy = config.acceleration_build_policy});
    result.device = built.device;
    result.acceleration_build.supported = built.acceleration_build.supported;
    result.status = built.status;
    result.diagnostic = built.diagnostic;
    if (built.status != GeometryStatus::success) {
        result.acceleration_build = built.acceleration_build;
        return result;
    }
    auto impl = std::make_unique<FrameSession::Impl>();
    result.diagnostic = impl->prepare(std::shared_ptr<MetalRenderScene>(std::move(built.scene)),
                                      scene, config, library_path);
    if (!result.diagnostic.empty()) {
        result.status = GeometryStatus::failure;
        return result;
    }
    result.shadow_traversal = built.shadow_traversal;
    result.acceleration_build = impl->acceleration_build_info();
    result.session = std::unique_ptr<FrameSession>(new FrameSession(std::move(impl)));
    result.status = GeometryStatus::success;
    return result;
}

SharedSceneFrameSessions
FrameSessionTestAccess::build_shared_scene(const RenderScene &scene, const FrameConfig &a,
                                           const FrameConfig &b,
                                           const std::filesystem::path &library_path) {
    SharedSceneFrameSessions result;
    // Cache/compaction and duplication diagnostics are execution controls. Motion additionally
    // changes temporal source-domain selection: only static identity contrasts
    // imply exact equality; moving contrasts require matched raw mean tests.
    // Compare every other FrameConfig field, including unrelated seeds/policy.
    const auto same =
        a.profile_stages == b.profile_stages && a.width == b.width && a.height == b.height &&
        a.seed == b.seed && a.material_seed == b.material_seed &&
        a.footprint_samples == b.footprint_samples &&
        a.transient_byte_limit == b.transient_byte_limit &&
        a.intersection_profile == b.intersection_profile &&
        a.shadow_traversal == b.shadow_traversal &&
        a.acceleration_build_policy == b.acceleration_build_policy && a.execution == b.execution &&
        a.capacity == b.capacity && a.quantum == b.quantum && a.group_width == b.group_width &&
        a.packet_events == b.packet_events && a.packet_layout == b.packet_layout &&
        a.estimator == b.estimator && a.area_path_domain == b.area_path_domain &&
        a.contribution_replay == b.contribution_replay && a.target_scale == b.target_scale &&
        a.area_output == b.area_output && a.mutation_steps == b.mutation_steps &&
        a.mutation_policy == b.mutation_policy &&
        // Stencil changes spatial source domains, never an execution-only toggle.
        a.area_stencil == b.area_stencil;
    if (!same) {
        result.diagnostic = "Shared-scene validation configurations differ beyond temporal/suffix "
                            "caches, diffuse specialization, pair compaction, temporal "
                            "reprojection or duplication diagnostics";
        return result;
    }
    if (a.execution != FrameExecution::wavefront) {
        result.diagnostic = "Shared-scene validation requires wavefront execution";
        return result;
    }
    auto first = build_frame_session(scene, a, library_path);
    result.device = first.device;
    result.status = first.status;
    result.diagnostic = first.diagnostic;
    result.shadow_traversal = first.shadow_traversal;
    result.acceleration_build = first.acceleration_build;
    if (first.status != GeometryStatus::success)
        return result;
    if (!first.session) {
        result.status = GeometryStatus::failure;
        result.diagnostic = "Shared-scene validation did not build its first session";
        return result;
    }
    auto second = std::make_unique<FrameSession::Impl>();
    result.diagnostic = second->prepare(first.session->impl_->renderer, scene, b, library_path);
    if (!result.diagnostic.empty()) {
        result.status = GeometryStatus::failure;
        return result;
    }
    result.shared_renderer = first.session->impl_->renderer == second->renderer;
    if (frame_paired_atlas_enabled(a)) {
        const auto left = first.session->impl_->area->paired_atlas;
        const auto right = second->area->paired_atlas;
        result.paired_atlas_arenas = {reinterpret_cast<std::uintptr_t>((__bridge void *)left),
                                      reinterpret_cast<std::uintptr_t>((__bridge void *)right)};
        result.shared_paired_atlas_storage = left != nil && left == right;
        if (!result.shared_paired_atlas_storage) {
            result.status = GeometryStatus::failure;
            result.diagnostic =
                "Shared-scene atlas sessions require one renderer-owned immutable asset";
            return result;
        }
    }
    const auto &first_history = first.session->impl_->history;
    const auto &second_history = second->history;
    result.independent_history = first_history && second_history && first_history != second_history;
    if (result.independent_history)
        for (auto left : first_history->banks)
            for (auto right : second_history->banks)
                result.independent_history &= left != right;
    if (first_history && second_history) {
        result.duplication_enabled = {bool(first.session->impl_->duplication),
                                      bool(second->duplication)};
        result.independent_duplication_storage = true;
        for (auto left :
             {first_history->duplication_labels[0], first_history->duplication_labels[1],
              first_history->duplication_temporal, first_history->duplication_counts[0],
              first_history->duplication_counts[1]})
            for (auto right :
                 {second_history->duplication_labels[0], second_history->duplication_labels[1],
                  second_history->duplication_temporal, second_history->duplication_counts[0],
                  second_history->duplication_counts[1]})
                if (left && right && left == right)
                    result.independent_duplication_storage = false;
        result.independent_motion_storage = true;
        for (auto left :
             {first_history->geometry[0], first_history->geometry[1], first_history->donors})
            for (auto right :
                 {second_history->geometry[0], second_history->geometry[1], second_history->donors})
                result.independent_motion_storage &= left != nil && right != nil && left != right;
        result.enhanced_record_arenas = {
            reinterpret_cast<std::uintptr_t>((__bridge void *)first_history->enhanced_records),
            reinterpret_cast<std::uintptr_t>((__bridge void *)second_history->enhanced_records)};
        result.enhanced_evaluations = {
            reinterpret_cast<std::uintptr_t>((__bridge void *)first_history->enhanced_evaluation),
            reinterpret_cast<std::uintptr_t>((__bridge void *)second_history->enhanced_evaluation)};
        result.enhanced_suffix_arenas = {
            reinterpret_cast<std::uintptr_t>((__bridge void *)first_history->enhanced_suffix),
            reinterpret_cast<std::uintptr_t>((__bridge void *)second_history->enhanced_suffix)};
        result.independent_suffix_storage =
            first_history->enhanced_suffix != nil && second_history->enhanced_suffix != nil &&
            first_history->enhanced_suffix != second_history->enhanced_suffix;
        result.independent_owned_records =
            first_history->enhanced_records != nil && first_history->enhanced_evaluation != nil &&
            second_history->enhanced_records != nil && second_history->enhanced_evaluation != nil;
        for (auto left : {first_history->enhanced_records, first_history->enhanced_evaluation})
            for (auto right :
                 {second_history->enhanced_records, second_history->enhanced_evaluation})
                result.independent_owned_records &= left != right;
    }
    if (!result.shared_renderer ||
        (a.estimator == FrameEstimator::area_replay &&
         (!result.independent_history || !result.independent_owned_records ||
          !result.independent_suffix_storage)) ||
        ((frame_motion_enabled(a) || frame_motion_enabled(b)) &&
         !result.independent_motion_storage)) {
        result.status = GeometryStatus::failure;
        result.diagnostic = "Shared-scene validation requires one renderer/AS and independent "
                            "history/owned records";
        return result;
    }
    result.sessions[0] = std::move(first.session);
    result.sessions[1].reset(new FrameSession(std::move(second)));
    result.status = GeometryStatus::success;
    return result;
}

EnhancedMotionState FrameSessionTestAccess::motion_state(FrameSession &session) {
    EnhancedMotionState result;
    if (!session.impl_ || !frame_motion_enabled(session.impl_->config) || !session.impl_->history) {
        result.diagnostic = "Motion state requires an enabled Enhanced motion session";
        return result;
    }
    auto &h = *session.impl_->history;
    std::lock_guard lock(h.mutex);
    if (h.pending) {
        result.diagnostic = "Motion state requires no pending frame";
        return result;
    }
    result.status = GeometryStatus::success;
    result.geometry_valid = h.geometry_valid;
    result.history_valid = h.valid;
    result.geometry_front = h.geometry_front;
    result.geometry_generation = h.geometry_generation;
    result.history_generation = h.generation;
    result.next_serial = h.next_serial;
    result.pending_serial = h.pending_serial;
    result.committed_serial = h.committed_serial;
    return result;
}
EnhancedDuplicationState FrameSessionTestAccess::duplication_state(FrameSession &session) {
    EnhancedDuplicationState result;
    if (!session.impl_ || !session.impl_->history) {
        result.diagnostic = "Duplication state requires an Area session";
        return result;
    }
    auto &h = *session.impl_->history;
    std::lock_guard lock(h.mutex);
    if (h.pending) {
        result.diagnostic = "Duplication state requires no pending frame";
        return result;
    }
    result.status = GeometryStatus::success;
    result.mode = session.impl_->config.area_duplication;
    result.history_valid = h.valid;
    result.valid = h.valid && bool(session.impl_->duplication);
    result.front = result.history_front = h.front;
    result.generation = result.history_generation = h.generation;
    result.next_serial = h.next_serial;
    result.pending_serial = h.pending_serial;
    result.committed_serial = h.committed_serial;
    for (auto b : {h.duplication_labels[0], h.duplication_labels[1], h.duplication_temporal,
                   h.duplication_counts[0], h.duplication_counts[1]})
        if (b)
            result.storage_bytes += b.length;
    return result;
}
std::string
FrameSessionTestAccess::force_next_duplication_publication_failure(FrameSession &session) {
    if (!session.impl_ || !session.impl_->duplication || !session.impl_->history)
        return "Duplication failure control requires an enabled session";
    auto &h = *session.impl_->history;
    std::lock_guard lock(h.mutex);
    if (h.pending || h.fail_next_duplication_publication)
        return "Duplication failure control requires idle, unarmed history";
    h.fail_next_duplication_publication = true;
    return {};
}
std::string FrameSessionTestAccess::force_next_motion_publication_failure(FrameSession &session) {
    if (!session.impl_ || !frame_motion_enabled(session.impl_->config) || !session.impl_->history)
        return "Motion failure control requires an enabled Enhanced motion session";
    auto &h = *session.impl_->history;
    std::lock_guard lock(h.mutex);
    if (h.pending || h.fail_next_motion_publication)
        return "Motion failure control requires idle, unarmed history";
    h.fail_next_motion_publication = true;
    return {};
}

EnhancedSuffixContextState FrameSessionTestAccess::suffix_context_state(FrameSession &session) {
    if (!session.impl_)
        return {.diagnostic = "Suffix context session is moved-from"};
    return session.impl_->suffix_context_state();
}

EnhancedSuffixContextState
FrameSessionTestAccess::seed_next_suffix_context(FrameSession &session,
                                                 std::uint64_t next_context) {
    if (!session.impl_)
        return {.diagnostic = "Suffix context session is moved-from"};
    return session.impl_->suffix_context_state(next_context);
}

EnhancedSessionSnapshot FrameSessionTestAccess::snapshot_enhanced(FrameSession &session,
                                                                  std::uint64_t expected_serial,
                                                                  bool include_suffix,
                                                                  bool include_motion,
                                                                  bool include_duplication) {
    @autoreleasepool {
        if (!session.impl_)
            return {.diagnostic = "Enhanced snapshot session is moved-from"};
        try {
            return session.impl_->snapshot_enhanced(expected_serial, include_suffix, include_motion,
                                                    include_duplication);
        } catch (const std::bad_alloc &) {
            return {.diagnostic = "Enhanced snapshot CPU allocation failed"};
        }
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
    return build_metal_renderer(scene, library_path, nullptr);
}
RenderBuildResult build_metal_renderer(const RenderScene &scene,
                                       const std::filesystem::path &library_path,
                                       RenderTimings *timings) {
    return build_metal_renderer(scene, library_path, RenderBuildOptions{}, timings);
}
RenderBuildResult build_metal_renderer(const RenderScene &scene,
                                       const std::filesystem::path &library_path,
                                       RenderBuildOptions options, RenderTimings *timings) {
    @autoreleasepool {
        RenderBuildResult result;
        result.shadow_traversal.requested = options.shadow_traversal;
        result.acceleration_build.requested = options.acceleration_build_policy;
        if (!valid_acceleration_build_policy(options.acceleration_build_policy) ||
            (options.acceleration_build_policy != AccelerationBuildPolicy::default_build &&
             options.intersection_profile != IntersectionProfile::native)) {
            result.diagnostic = "Fast acceleration build policies require the native intersection "
                                "profile and a valid policy";
            result.acceleration_build.supported = false;
            result.acceleration_build.diagnostic = result.diagnostic;
            return result;
        }
        std::optional<std::size_t> growth_estimate;
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
            if (scene.max_depth >
                    (parameters.packet_transport == 2 ? 65u : packet_wire::max_depth) ||
                !(parameters.camera_image_area > 0) ||
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
            growth_estimate = estimate;
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
        if (options.shadow_traversal != ShadowTraversal::query &&
            options.shadow_traversal != ShadowTraversal::opaque_auto) {
            result.diagnostic = "Invalid shadow traversal mode";
            return result;
        }
        const auto shadow_selection = select_shadow_traversal(
            scene, geometry_data, options.intersection_profile, options.shadow_traversal);
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
        const bool hint_eligible =
            options.intersection_profile == IntersectionProfile::compensated &&
            parameters.packet_transport && !hint_passthrough && !hint_complex_material &&
            !hint_medium_boundary && opaque_triangle_sources != 0;
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
            growth_estimate = budget.combined;
            auto budget_device = MTLCreateSystemDefaultDevice();
            if (budget_device != nil && budget_device.recommendedMaxWorkingSetSize > 0 &&
                budget.combined > budget_device.recommendedMaxWorkingSetSize) {
                result.diagnostic =
                    "Native hint preparation estimate exceeds device recommended working-set size";
                return result;
            }
        }
        auto built = build_metal_geometry_with_hints(
            geometry_data, library_path, hint_eligible,
            {options.intersection_profile, options.acceleration_build_policy});
        result.device = std::move(built.device);
        result.acceleration_build.supported = built.acceleration_build.supported;
        if (built.status != GeometryStatus::success) {
            result.acceleration_build = built.acceleration_build;
            result.status = built.status;
            result.diagnostic = std::move(built.diagnostic);
            return result;
        }

        auto impl = std::make_unique<MetalRenderScene::Impl>();
        impl->geometry = std::move(built.scene);
        impl->acceleration_build = built.acceleration_build;
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
        impl->diffuse_event_proof = scene_proves_diffuse_events(scene);
        impl->parameters.instance_count =
            static_cast<wire::Word>(MetalGeometryAccess::instance_count(*impl->geometry));

        id<MTLLibrary> library = metal::load_library(impl->device, library_path, result.diagnostic);
        if (library == nil) {
            return result;
        }
        if (!verify_shader_contract(impl->device, impl->queue, library, result.diagnostic)) {
            return result;
        }
        if (options.intersection_profile == IntersectionProfile::native &&
            !verify_shader_contract(impl->device, impl->queue, library, result.diagnostic, true)) {
            return result;
        }
        NSString *entry = impl->parameters.packet_transport
                              ? @"rypath_render_packets"
                              : (impl->parameters.instance_count == 0 ? @"rypath_render_sky"
                                                                      : @"rypath_render_paths");
        impl->opaque_shadows = shadow_selection.effective == ShadowTraversal::opaque_auto;
        if (impl->opaque_shadows &&
            !metal::verify_opaque_shadow_capability(library, entry, result.diagnostic))
            return result;
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
        const bool native = options.intersection_profile == IntersectionProfile::native;
        if (native)
            [constants setConstantValue:&native type:MTLDataTypeBool atIndex:21];
        [constants setConstantValue:&impl->opaque_shadows type:MTLDataTypeBool atIndex:22];
        const bool hero = impl->parameters.packet_transport == 2;
        [constants setConstantValue:&hero type:MTLDataTypeBool atIndex:25];
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
            const bool bidirectional = impl->parameters.bidirectional != 0;
            [constants setConstantValue:&bidirectional type:MTLDataTypeBool atIndex:20];
        }
        impl->function_constants = [constants copy];
        RenderTimingScope pipeline_timing(timings, RenderTimingPhase::pipeline);
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
        pipeline_timing.close();
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
            impl->empty_packet_structure = build_empty_packet_structure(
                impl->device, impl->queue, result.diagnostic, impl->acceleration_build);
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
        if (impl->parameters.packet_transport) {
            const auto recommended = impl->device.recommendedMaxWorkingSetSize;
            if (!growth_estimate && recommended > 0) {
                // Unlimited scenes without hints were not previously estimated here.
                // Optional growth must not introduce a new preparation failure.
                try {
                    std::size_t estimate = 0;
                    if (validate_render_preparation_budget(scene, &estimate).empty())
                        growth_estimate = estimate;
                } catch (...) {
                    // Keep the existing baseline when optional estimation cannot complete.
                }
            }
            std::optional<std::size_t> finite_limit;
            if (scene.preparation_byte_limit != std::numeric_limits<std::size_t>::max())
                finite_limit = scene.preparation_byte_limit;
            impl->packet_payload_bytes = packet_dispatch::payload_bytes(
                growth_estimate, finite_limit, recommended, impl->device.maxBufferLength);
            result.packet_lane_capacity = packet_dispatch::lane_capacity(
                impl->packet_payload_bytes, impl->parameters.packet_lane_bytes,
                impl->device.maxBufferLength);
        }
        result.shadow_traversal = shadow_selection;
        result.acceleration_build = impl->acceleration_build;
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
