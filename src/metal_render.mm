// SPDX-License-Identifier: GPL-3.0-only

#include "metal_render.hpp"

#include "metal_execution.hpp"
#include "metal_geometry_access.hpp"
#include "render_wire.h"
#include "sobol_shared.h"
#include "triangle_attributes.hpp"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <bit>
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

static_assert(std::is_trivially_copyable_v<wire::Parameters> &&
              std::is_trivially_copyable_v<wire::Sample>);
constexpr auto max_index = std::numeric_limits<std::uint32_t>::max();

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
    if (scene.model == RenderModel::pbrt) {
        const auto &camera = *scene.raster_camera;
        result.origin = pack(camera.origin);
        // Reuse the demo camera slots for the PBRT world-space raster basis.
        result.forward = pack(camera.corner);
        result.right = pack(camera.dx);
        result.up = pack(camera.dy);
        result.model = wire::pbrt_model;
        result.max_depth = scene.max_depth;
        result.filter_kind = scene.filter.kind == PixelFilterKind::triangle ? wire::triangle_filter
                                                                            : wire::box_filter;
        result.filter_x_radius = scene.filter.x_radius;
        result.filter_y_radius = scene.filter.y_radius;
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

bool requires_world_space_emitter(const RenderScene &scene, std::size_t source) {
    const auto &instance = scene.geometry.instances[source];
    return !scene.instance_emissions.empty() &&
           scene.instance_emissions[source] != no_area_emission &&
           instance.transform.values != GeometryTransform{}.values &&
           !scene.geometry.meshes[instance.mesh_index].triangles.empty();
}

std::string prepare_world_space_emitters(const RenderScene &scene, GeometryData &prepared) {
    std::vector<std::size_t> references(scene.geometry.meshes.size());
    for (const auto &instance : scene.geometry.instances) {
        ++references[instance.mesh_index];
    }
    std::size_t clone_count = 0;
    std::size_t vertex_count = 0;
    std::size_t triangle_count = 0;
    const auto charge_mesh = [&](const GeometryMesh &mesh) {
        if (mesh.triangles.empty()) {
            return true;
        }
        if (mesh.positions.size() > max_index - vertex_count ||
            mesh.triangles.size() > max_index - triangle_count) {
            return false;
        }
        vertex_count += mesh.positions.size();
        triangle_count += mesh.triangles.size();
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
    return validate_sampler_range(request.sampler, request.seed, request.width, request.height,
                                  request.sample_begin, request.sample_count);
}

std::string sample_error(wire::Word status) {
    switch (status) {
    case wire::emergency_depth:
        return "Metal render emergency depth reached";
    case wire::invalid_arithmetic:
        return "Metal render encountered non-finite or invalid path arithmetic";
    case wire::invalid_hit:
        return "Metal render encountered invalid native hit data";
    default:
        return "Metal render sample status is invalid or unwritten";
    }
}

} // namespace

struct MetalRenderScene::Impl {
    std::unique_ptr<MetalGeometryScene> geometry;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLComputePipelineState> pipeline = nil;
    id<MTLBuffer> instances = nil;
    id<MTLBuffer> positions = nil;
    id<MTLBuffer> triangles = nil;
    id<MTLBuffer> materials = nil;
    id<MTLBuffer> environment = nil;
    id<MTLBuffer> knots = nil;
    id<MTLBuffer> area_emissions = nil;
    id<MTLBuffer> emitters = nil;
    id<MTLBuffer> normals = nil;
    id<MTLBuffer> sampling_tables = nil;
    id<MTLBuffer> output = nil;
    wire::Parameters parameters{};

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
                                     const std::vector<wire::Instance> &packed_instances) {
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
                if (instance.triangle_count > emitter_limit - emitter_count) {
                    return "Metal area emitter buffer exceeds device size limit";
                }
                emitter_count += instance.triangle_count;
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
        }
        std::vector<wire::Emitter> packed_emitters;
        packed_emitters.reserve(std::max<std::size_t>(1, emitter_count));
        for (wire::Word ordinal = 0; ordinal < parameters.instance_count; ++ordinal) {
            const auto &instance = packed_instances[ordinal];
            if (instance.emission == wire::no_emission) {
                continue;
            }
            for (wire::Word primitive = 0; primitive < instance.triangle_count; ++primitive) {
                packed_emitters.push_back({ordinal, primitive});
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
        std::size_t vertex_count = 0;
        std::size_t triangle_count = 0;
        const auto vertex_limit =
            std::min<std::size_t>(max_index, device.maxBufferLength / sizeof(wire::Point));
        const auto triangle_limit =
            std::min<std::size_t>(max_index, device.maxBufferLength / sizeof(wire::Triangle));
        for (std::size_t index = 0; index < meshes.size(); ++index) {
            const auto &mesh = meshes[index];
            if (mesh.triangles.empty()) {
                continue;
            }
            if (mesh.positions.size() > vertex_limit - vertex_count ||
                mesh.triangles.size() > triangle_limit - triangle_count) {
                return "Metal render shading geometry exceeds buffer or index limits";
            }
            vertex_offsets[index] = vertex_count;
            triangle_offsets[index] = triangle_count;
            vertex_count += mesh.positions.size();
            triangle_count += mesh.triangles.size();
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
                if (material.knots.size() > knot_limit - knot_count) {
                    return "Metal render spectral knot buffer exceeds buffer or index limits";
                }
                knot_count += material.knots.size();
            }
        }
        parameters.knot_count = static_cast<wire::Word>(knot_count);

        std::vector<wire::Point> packed_positions;
        packed_positions.reserve(vertex_count);
        std::vector<wire::Triangle> packed_triangles;
        packed_triangles.reserve(triangle_count);
        for (const auto &mesh : meshes) {
            if (mesh.triangles.empty()) {
                continue;
            }
            for (const auto point : mesh.positions) {
                packed_positions.push_back({point.x, point.y, point.z});
            }
            for (const auto &triangle : mesh.triangles) {
                packed_triangles.push_back({{triangle[0], triangle[1], triangle[2]}});
            }
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
            packed.triangle_count = static_cast<wire::Word>(meshes[mesh].triangles.size());
            packed.material = scene.instance_materials[source];
            if (!scene.instance_reverse_orientation.empty()) {
                packed.reverse_orientation = scene.instance_reverse_orientation[source];
            }
            packed.emission = scene.instance_emissions.empty() ? wire::no_emission
                                                               : scene.instance_emissions[source];
            packed_instances.push_back(packed);
        }
        if (auto message = upload_normals(scene, packed_instances); !message.empty()) {
            return message;
        }

        std::vector<wire::Material> packed_materials(scene.materials.size());
        std::vector<wire::SpectralKnot> packed_knots;
        packed_knots.reserve(std::max<std::size_t>(1, knot_count));
        for (std::size_t index = 0; index < scene.materials.size(); ++index) {
            const auto &source = scene.materials[index];
            auto &packed = packed_materials[index];
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
                packed.constant_value = source.constant;
                break;
            case SpectralMaterialKind::rgb_sigmoid:
                packed.kind = wire::rgb_sigmoid_material;
                std::ranges::copy(source.coefficients, packed.coefficients);
                break;
            }
        }
        // The shared kernel always binds a knot buffer, even with no authored knots.
        if (packed_knots.empty()) {
            packed_knots.emplace_back();
        }

        instances = upload(device, packed_instances);
        positions = upload(device, packed_positions);
        triangles = upload(device, packed_triangles);
        materials = upload(device, packed_materials);
        knots = upload(device, packed_knots);
        if (instances == nil || positions == nil || triangles == nil || materials == nil ||
            knots == nil) {
            return "Metal render shading buffer allocation failed";
        }
        return upload_area_lighting(scene, packed_instances);
    }

    RenderSamplesResult sample(const RenderRequest &request) {
        RenderSamplesResult result;
        result.diagnostic = validate_request(request);
        if (!result.diagnostic.empty()) {
            return result;
        }
        if (request.sampler.kind == SamplerKind::sobol &&
            (parameters.model != wire::pbrt_model || parameters.max_depth > sobol_max_depth)) {
            result.diagnostic = "Sobol requires PBRT rendering with maxdepth at most 127";
            return result;
        }
        if (parameters.filter_kind == wire::triangle_filter) {
            const RasterCamera camera{unpack(parameters.origin), unpack(parameters.forward),
                                      unpack(parameters.right), unpack(parameters.up)};
            const PixelFilter filter{PixelFilterKind::triangle, parameters.filter_x_radius,
                                     parameters.filter_y_radius};
            result.diagnostic =
                validate_raster_support(camera, filter, request.width, request.height);
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
        p.width = request.width;
        p.height = request.height;
        p.pixel_begin = request.pixel_begin;
        p.pixel_count = request.pixel_count;
        p.sample_low = static_cast<wire::Word>(request.sample_begin);
        p.sample_high = static_cast<wire::Word>(request.sample_begin >> 32);
        p.seed_low = static_cast<wire::Word>(request.seed);
        p.seed_high = static_cast<wire::Word>(request.seed >> 32);
        p.emergency_limit = request.emergency_limit;
        p.lane_count = request.pixel_count * request.sample_count;
        p.sampler_kind = static_cast<wire::Word>(request.sampler.kind);
        p.randomization = static_cast<wire::Word>(request.sampler.randomization);
        p.sobol_scale_exponent = request.sampler.kind == SamplerKind::sobol
                                     ? sobol_scale_exponent(request.width, request.height)
                                     : 0;

        const auto group_count = (p.lane_count + group_width - 1) / group_width;
        const auto slots = group_count * group_width + 1;
        const auto bytes = slots * sizeof(wire::Sample);
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
        if (p.instance_count != 0) {
            MetalGeometryAccess::bind(*geometry, encoder, 2);
            [encoder setBuffer:instances offset:0 atIndex:3];
            [encoder setBuffer:positions offset:0 atIndex:4];
            [encoder setBuffer:triangles offset:0 atIndex:5];
            [encoder setBuffer:materials offset:0 atIndex:6];
            [encoder setBuffer:knots offset:0 atIndex:8];
            [encoder setBuffer:area_emissions offset:0 atIndex:9];
            [encoder setBuffer:emitters offset:0 atIndex:10];
            [encoder setBuffer:normals offset:0 atIndex:11];
        }
        [encoder setBuffer:environment offset:0 atIndex:7];
        [encoder setBuffer:sampling_tables offset:0 atIndex:12];
        [encoder dispatchThreadgroups:MTLSizeMake(group_count, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(group_width, 1, 1)];
        [encoder endEncoding];
        result.diagnostic = metal::complete(command, "Metal render sample dispatch");
        if (!result.diagnostic.empty()) {
            return result;
        }

        const std::span output_data{output_bytes, bytes};
        if (!std::ranges::all_of(output_data.subspan(p.lane_count * sizeof(wire::Sample)),
                                 [](unsigned char byte) { return byte == 0xff; })) {
            result.diagnostic = "Metal render output guard mismatch";
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
        const auto parameters = camera_parameters(scene);
        if (scene.model == RenderModel::demo &&
            (!(parameters.film_scale > 0.0f) || !std::isfinite(parameters.film_scale) ||
             !finite(parameters.forward) || !finite(parameters.right) || !finite(parameters.up))) {
            result.diagnostic = "Metal render camera basis or field of view is not representable";
            return result;
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
        const auto &geometry_data = prepared.instances.empty() ? scene.geometry : prepared;
        auto built = build_metal_geometry(geometry_data, library_path);
        result.device = std::move(built.device);
        if (built.status != GeometryStatus::success) {
            result.status = built.status;
            result.diagnostic = std::move(built.diagnostic);
            return result;
        }

        auto impl = std::make_unique<MetalRenderScene::Impl>();
        impl->geometry = std::move(built.scene);
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
        impl->parameters.instance_count =
            static_cast<wire::Word>(MetalGeometryAccess::instance_count(*impl->geometry));

        id<MTLLibrary> library = metal::load_library(impl->device, library_path, result.diagnostic);
        if (library == nil) {
            return result;
        }
        NSString *entry =
            impl->parameters.instance_count == 0 ? @"rypath_render_sky" : @"rypath_render_paths";
        impl->pipeline = metal::load_pipeline(impl->device, library, entry, result.diagnostic);
        if (impl->pipeline == nil) {
            return result;
        }
        impl->environment = upload(impl->device, scene.environment);
        if (impl->environment == nil) {
            result.diagnostic = "Metal render environment buffer allocation failed";
            return result;
        }
        const auto &tables = sobol_tables();
        if (sizeof(tables) > impl->device.maxBufferLength) {
            result.diagnostic = "Metal Sobol tables exceed device buffer limit";
            return result;
        }
        impl->sampling_tables = [impl->device newBufferWithBytes:&tables
                                                          length:sizeof(tables)
                                                         options:MTLResourceStorageModeShared];
        if (impl->sampling_tables == nil) {
            result.diagnostic = "Metal Sobol table buffer allocation failed";
            return result;
        }
        if (impl->parameters.instance_count != 0) {
            try {
                result.diagnostic = impl->upload_shading(scene, geometry_data);
            } catch (const std::bad_alloc &) {
                result.diagnostic = "Metal render shading preparation allocation failed";
            }
            if (!result.diagnostic.empty()) {
                return result;
            }
        }

        result.scene.reset(new MetalRenderScene(std::move(impl)));
        result.status = GeometryStatus::success;
        return result;
    }
}

} // namespace rypath
