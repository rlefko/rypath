// SPDX-License-Identifier: GPL-3.0-only

#include "metal_geometry.hpp"

#include "analytic.hpp"
#include "geometry_wire.h"
#include "metal_execution.hpp"
#include "metal_geometry_access.hpp"
#include "metal_geometry_limits.hpp"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

namespace rypath {
namespace {

using geometry_wire::Closest;
using geometry_wire::Patch;
using geometry_wire::ProceduralRecord;
using geometry_wire::Ray;
using geometry_wire::Word;
constexpr auto max_wire_index = std::numeric_limits<Word>::max();

static_assert(std::is_trivially_copyable_v<GeometryPoint>);
static_assert(sizeof(GeometryPoint) == 12 && offsetof(GeometryPoint, y) == 4 &&
              offsetof(GeometryPoint, z) == 8);
static_assert(sizeof(std::array<Word, 3>) == 12);
static_assert(sizeof(MTLPackedFloat4x3) == 48);
static_assert(sizeof(MTLAxisAlignedBoundingBox) == 24 &&
              std::is_trivially_copyable_v<MTLAxisAlignedBoundingBox>);
static_assert(sizeof(MTLAccelerationStructureInstanceDescriptor) == 64 &&
              std::is_trivially_copyable_v<MTLAccelerationStructureInstanceDescriptor>);
static_assert(std::is_trivially_copyable_v<Ray> && std::is_trivially_copyable_v<Closest> &&
              std::is_trivially_copyable_v<Patch>);
static_assert(static_cast<Word>(PrimitiveKind::triangle) == geometry_wire::triangle &&
              static_cast<Word>(PrimitiveKind::bilinear_patch) == geometry_wire::bilinear_patch);

template <class T> bool fits_buffer(id<MTLDevice> device, std::size_t count) {
    return count <= device.maxBufferLength / sizeof(T);
}

template <class T> id<MTLBuffer> upload(id<MTLDevice> device, std::span<const T> data) {
    return [device newBufferWithBytes:data.data()
                               length:data.size_bytes()
                              options:MTLResourceStorageModeShared];
}

id<MTLAccelerationStructure> build_structure(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                             MTLAccelerationStructureDescriptor *descriptor,
                                             NSString *label, std::string &diagnostic) {
    const auto sizes = [device accelerationStructureSizesWithDescriptor:descriptor];
    if (sizes.accelerationStructureSize == 0 ||
        sizes.buildScratchBufferSize > device.maxBufferLength) {
        diagnostic = "Metal acceleration structure sizing failed";
        return nil;
    }
    id<MTLAccelerationStructure> structure =
        [device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
    const auto scratch_size = std::max(sizes.buildScratchBufferSize, NSUInteger{1});
    id<MTLBuffer> scratch = [device newBufferWithLength:scratch_size
                                                options:MTLResourceStorageModePrivate];
    if (structure == nil || scratch == nil) {
        diagnostic = "Metal acceleration structure or scratch allocation failed";
        return nil;
    }
    structure.label = label;

    id<MTLCommandBuffer> command = [queue commandBuffer];
    if (command == nil) {
        diagnostic = "Metal acceleration structure command buffer creation failed";
        return nil;
    }
    id<MTLAccelerationStructureCommandEncoder> encoder =
        [command accelerationStructureCommandEncoder];
    if (encoder == nil) {
        diagnostic = "Metal acceleration structure encoder creation failed";
        return nil;
    }
    [encoder buildAccelerationStructure:structure
                             descriptor:descriptor
                          scratchBuffer:scratch
                    scratchBufferOffset:0];
    [encoder endEncoding];
    diagnostic = metal::complete(command, "Metal acceleration structure build");
    return diagnostic.empty() ? structure : nil;
}

MTLAccelerationStructureTriangleGeometryDescriptor *
triangle_descriptor(id<MTLDevice> device, const GeometryMesh &mesh, std::string &diagnostic) {
    if (!fits_buffer<GeometryPoint>(device, mesh.positions.size()) ||
        !fits_buffer<std::array<Word, 3>>(device, mesh.triangles.size())) {
        diagnostic = "Metal vertex or index buffer exceeds device size limit";
        return nil;
    }
    id<MTLBuffer> vertices = upload(device, std::span{mesh.positions});
    // Keep every native primitive slot. A provably zero-measure canonical face
    // is represented by one repeated point, rather than relying on cancellation
    // in Metal's collinear triangle solve. Original indices remain in GeometryData.
    const bool has_zero = std::ranges::any_of(mesh.triangles, [&](const auto &t) {
        return !triangle_has_area(mesh.positions[t[0]], mesh.positions[t[1]], mesh.positions[t[2]]);
    });
    id<MTLBuffer> indices = nil;
    if (has_zero) {
        auto native_indices = mesh.triangles;
        for (auto &t : native_indices) {
            if (!triangle_has_area(mesh.positions[t[0]], mesh.positions[t[1]],
                                   mesh.positions[t[2]]))
                t = {t[0], t[0], t[0]};
        }
        indices = upload(device, std::span<const std::array<Word, 3>>{native_indices});
    } else {
        indices = upload(device, std::span{mesh.triangles});
    }
    if (vertices == nil || indices == nil) {
        diagnostic = "Metal vertex or index buffer allocation failed";
        return nil;
    }
    MTLAccelerationStructureTriangleGeometryDescriptor *triangle =
        [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
    triangle.vertexBuffer = vertices;
    triangle.vertexFormat = MTLAttributeFormatFloat3;
    triangle.vertexStride = sizeof(GeometryPoint);
    triangle.indexBuffer = indices;
    triangle.indexType = MTLIndexTypeUInt32;
    triangle.triangleCount = mesh.triangles.size();
    triangle.opaque = YES;
    return triangle;
}

MTLAccelerationStructureBoundingBoxGeometryDescriptor *
patch_descriptor(id<MTLDevice> device, const GeometryMesh &mesh, std::string &diagnostic) {
    if (!fits_buffer<MTLAxisAlignedBoundingBox>(device, mesh.patches.size()) ||
        !fits_buffer<ProceduralRecord>(device, mesh.patches.size())) {
        diagnostic = "Metal patch bounds or corner buffer exceeds device size limit";
        return nil;
    }
    std::vector<MTLAxisAlignedBoundingBox> bounds;
    std::vector<ProceduralRecord> corners;
    bounds.reserve(mesh.patches.size());
    corners.reserve(mesh.patches.size());
    for (const auto &indices : mesh.patches) {
        Patch patch{};
        for (std::size_t i = 0; i < indices.size(); ++i) {
            const auto &p = mesh.positions[indices[i]];
            patch.corners[i][0] = p.x;
            patch.corners[i][1] = p.y;
            patch.corners[i][2] = p.z;
        }
        ProceduralRecord record{};
        record.kind = geometry_wire::bilinear_patch;
        record.patch = patch;
        corners.push_back(record);

        // A bilinear patch lies in the convex hull of its four corners.
        // Expand planar bounds too, using normal floats if an axis lies at zero.
        MTLPackedFloat3 lower{}, upper{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto [lo, hi] = std::minmax({patch.corners[0][axis], patch.corners[1][axis],
                                               patch.corners[2][axis], patch.corners[3][axis]});
            const auto max_float = std::numeric_limits<float>::max();
            const auto min_normal = std::numeric_limits<float>::min();
            lower[axis] = std::nextafter(lo, -max_float);
            upper[axis] = std::nextafter(hi, max_float);
            if (std::abs(lower[axis]) < min_normal) {
                lower[axis] = -min_normal;
            }
            if (std::abs(upper[axis]) < min_normal) {
                upper[axis] = min_normal;
            }
        }
        bounds.emplace_back(lower, upper);
    }
    id<MTLBuffer> bounds_buffer =
        upload(device, std::span<const MTLAxisAlignedBoundingBox>{bounds});
    id<MTLBuffer> corner_buffer = upload(device, std::span<const ProceduralRecord>{corners});
    if (bounds_buffer == nil || corner_buffer == nil) {
        diagnostic = "Metal patch bounds or corner buffer allocation failed";
        return nil;
    }
    MTLAccelerationStructureBoundingBoxGeometryDescriptor *box =
        [MTLAccelerationStructureBoundingBoxGeometryDescriptor descriptor];
    box.boundingBoxBuffer = bounds_buffer;
    box.boundingBoxStride = sizeof(MTLAxisAlignedBoundingBox);
    box.boundingBoxCount = mesh.patches.size();
    box.primitiveDataBuffer = corner_buffer;
    box.primitiveDataStride = sizeof(ProceduralRecord);
    box.primitiveDataElementSize = sizeof(ProceduralRecord);
    box.opaque = NO;
    return box;
}

MTLAccelerationStructureBoundingBoxGeometryDescriptor *
analytic_descriptor(id<MTLDevice> device, const GeometryMesh &mesh, std::string &diagnostic) {
    if (!fits_buffer<MTLAxisAlignedBoundingBox>(device, mesh.analytics.size()) ||
        !fits_buffer<ProceduralRecord>(device, mesh.analytics.size())) {
        diagnostic = "Metal analytic bounds or primitive buffer exceeds device size limit";
        return nil;
    }
    std::vector<MTLAxisAlignedBoundingBox> bounds;
    std::vector<ProceduralRecord> records;
    for (const auto &shape : mesh.analytics) {
        const auto b = analytic_bounds(shape);
        MTLPackedFloat3 lo{}, hi{};
        const std::array<float, 3> lower{b[0].x, b[0].y, b[0].z}, upper{b[1].x, b[1].y, b[1].z};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            lo[axis] = std::nextafter(lower[axis], -std::numeric_limits<float>::max());
            hi[axis] = std::nextafter(upper[axis], std::numeric_limits<float>::max());
            if (std::abs(lo[axis]) < std::numeric_limits<float>::min())
                lo[axis] = -std::numeric_limits<float>::min();
            if (std::abs(hi[axis]) < std::numeric_limits<float>::min())
                hi[axis] = std::numeric_limits<float>::min();
        }
        bounds.emplace_back(lo, hi);
        ProceduralRecord record{};
        record.kind = geometry_wire::analytic;
        record.analytic = pack_analytic(shape);
        records.push_back(record);
    }
    id<MTLBuffer> bounds_buffer =
        upload(device, std::span<const MTLAxisAlignedBoundingBox>{bounds});
    id<MTLBuffer> record_buffer = upload(device, std::span<const ProceduralRecord>{records});
    if (bounds_buffer == nil || record_buffer == nil) {
        diagnostic = "Metal analytic bounds or primitive buffer allocation failed";
        return nil;
    }
    auto *box = [MTLAccelerationStructureBoundingBoxGeometryDescriptor descriptor];
    box.boundingBoxBuffer = bounds_buffer;
    box.boundingBoxStride = sizeof(MTLAxisAlignedBoundingBox);
    box.boundingBoxCount = mesh.analytics.size();
    box.primitiveDataBuffer = record_buffer;
    box.primitiveDataStride = sizeof(ProceduralRecord);
    box.primitiveDataElementSize = sizeof(ProceduralRecord);
    box.opaque = NO;
    return box;
}

constexpr std::array primitive_kinds{PrimitiveKind::triangle, PrimitiveKind::bilinear_patch,
                                     PrimitiveKind::analytic};

std::size_t kind_index(PrimitiveKind kind) {
    return kind == PrimitiveKind::triangle ? 0 : kind == PrimitiveKind::bilinear_patch ? 1 : 2;
}

std::size_t primitive_count(const GeometryMesh &mesh, PrimitiveKind kind) {
    return kind == PrimitiveKind::triangle         ? mesh.triangles.size()
           : kind == PrimitiveKind::bilinear_patch ? mesh.patches.size()
                                                   : mesh.analytics.size();
}

// Metal requires all geometry descriptors in a BLAS to have the same type.
id<MTLAccelerationStructure> build_mesh(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                        const GeometryMesh &mesh, PrimitiveKind kind,
                                        std::string &diagnostic) {
    const bool triangle = kind == PrimitiveKind::triangle;
    MTLAccelerationStructureGeometryDescriptor *geometry =
        triangle                                ? triangle_descriptor(device, mesh, diagnostic)
        : kind == PrimitiveKind::bilinear_patch ? patch_descriptor(device, mesh, diagnostic)
                                                : analytic_descriptor(device, mesh, diagnostic);
    if (geometry == nil) {
        return nil;
    }
    MTLPrimitiveAccelerationStructureDescriptor *descriptor =
        [MTLPrimitiveAccelerationStructureDescriptor descriptor];
    descriptor.geometryDescriptors = @[ geometry ];
    return build_structure(device, queue, descriptor,
                           triangle                                ? @"rypath triangles"
                           : kind == PrimitiveKind::bilinear_patch ? @"rypath bilinear patches"
                                                                   : @"rypath analytic shapes",
                           diagnostic);
}

struct InstanceIndices {
    Word source_index;
    Word mesh_index;
    PrimitiveKind kind;
    std::size_t primitive_count;
};

constexpr std::string_view moved_from = "Metal geometry scene is moved-from";

} // namespace

struct MetalGeometryScene::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLComputePipelineState> closest = nil;
    id<MTLComputePipelineState> occlusion = nil;
    id<MTLIntersectionFunctionTable> closest_table = nil;
    id<MTLIntersectionFunctionTable> occlusion_table = nil;
    NSArray<id<MTLAccelerationStructure>> *meshes = nil;
    id<MTLAccelerationStructure> instances = nil;
    std::vector<InstanceIndices> instance_indices;

    void bind(id<MTLComputeCommandEncoder> encoder, NSUInteger slot) const {
        [encoder setAccelerationStructure:instances atBufferIndex:slot];
        // Binding the TLAS does not declare the primitive structures it references.
        for (id<MTLAccelerationStructure> mesh in meshes) {
            [encoder useResource:mesh usage:MTLResourceUsageRead];
        }
    }

    template <class Output>
    id<MTLBuffer> dispatch(id<MTLComputePipelineState> pipeline,
                           id<MTLIntersectionFunctionTable> table,
                           std::span<const GeometryRay> rays, std::size_t &guard_count,
                           std::string &diagnostic) const {
        const auto width = metal::group_width(device, pipeline);
        if (width == 0 || rays.size() > max_wire_index) {
            diagnostic = "Metal ray dispatch exceeds threadgroup or ray index limits";
            return nil;
        }
        const auto groups = (rays.size() + width - 1) / width;
        const auto slots = groups * width + 1;
        if (slots - 1 > max_wire_index || !fits_buffer<Ray>(device, slots) ||
            !fits_buffer<Output>(device, slots)) {
            diagnostic = "Metal ray dispatch exceeds padded grid or buffer limits";
            return nil;
        }
        id<MTLBuffer> input = [device newBufferWithLength:slots * sizeof(Ray)
                                                  options:MTLResourceStorageModeShared];
        [[clang::objc_precise_lifetime]] id<MTLBuffer> output =
            [device newBufferWithLength:slots * sizeof(Output)
                                options:MTLResourceStorageModeShared];
        auto *input_data = static_cast<unsigned char *>(input.contents);
        auto *output_data = static_cast<unsigned char *>(output.contents);
        if (input_data == nullptr || output_data == nullptr) {
            diagnostic = "Metal ray input or output allocation failed";
            return nil;
        }
        // Valid inputs in the padded lanes keep a missing shader bounds check diagnosable.
        for (std::size_t index = 0; index < slots; ++index) {
            const auto &r = rays[index < rays.size() ? index : 0];
            const Ray wire{{r.origin.x, r.origin.y, r.origin.z},
                           {r.direction.x, r.direction.y, r.direction.z},
                           r.t_min,
                           r.t_max};
            std::memcpy(input_data + index * sizeof(wire), &wire, sizeof(wire));
        }
        std::memset(output_data, 0xff, slots * sizeof(Output));

        id<MTLCommandBuffer> command = [queue commandBuffer];
        if (command == nil) {
            diagnostic = "Metal ray command buffer creation failed";
            return nil;
        }
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        if (encoder == nil) {
            diagnostic = "Metal ray compute encoder creation failed";
            return nil;
        }
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        const auto count = static_cast<Word>(rays.size());
        [encoder setBytes:&count length:sizeof(count) atIndex:2];
        bind(encoder, 3);
        [encoder setIntersectionFunctionTable:table atBufferIndex:4];
        [encoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        [encoder endEncoding];
        diagnostic = metal::complete(command, "Metal ray dispatch");
        if (!diagnostic.empty()) {
            return nil;
        }
        const std::span bytes{output_data, slots * sizeof(Output)};
        if (!std::ranges::all_of(bytes.subspan(rays.size() * sizeof(Output)),
                                 [](unsigned char byte) { return byte == 0xff; })) {
            diagnostic = "Metal ray output guard mismatch";
            return nil;
        }
        guard_count = slots - rays.size();
        return output;
    }
};

MetalGeometryScene::MetalGeometryScene(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MetalGeometryScene::~MetalGeometryScene() = default;
MetalGeometryScene::MetalGeometryScene(MetalGeometryScene &&) noexcept = default;
MetalGeometryScene &MetalGeometryScene::operator=(MetalGeometryScene &&) noexcept = default;

id<MTLDevice> MetalGeometryAccess::device(const MetalGeometryScene &scene) {
    return scene.impl_->device;
}

id<MTLCommandQueue> MetalGeometryAccess::queue(const MetalGeometryScene &scene) {
    return scene.impl_->queue;
}

std::size_t MetalGeometryAccess::instance_count(const MetalGeometryScene &scene) {
    return scene.impl_->instance_indices.size();
}

std::uint32_t MetalGeometryAccess::source_instance(const MetalGeometryScene &scene,
                                                   std::size_t ordinal) {
    return scene.impl_->instance_indices[ordinal].source_index;
}

PrimitiveKind MetalGeometryAccess::primitive_kind(const MetalGeometryScene &scene,
                                                  std::size_t ordinal) {
    return scene.impl_->instance_indices[ordinal].kind;
}

void MetalGeometryAccess::bind(const MetalGeometryScene &scene,
                               id<MTLComputeCommandEncoder> encoder, NSUInteger slot) {
    scene.impl_->bind(encoder, slot);
}

GeometryClosestResult MetalGeometryScene::closest_hits(std::span<const GeometryRay> rays) {
    @autoreleasepool {
        GeometryClosestResult result;
        if (!impl_) {
            result.diagnostic = moved_from;
            return result;
        }
        result.diagnostic = validate_geometry_rays(rays);
        if (!result.diagnostic.empty()) {
            return result;
        }
        if (rays.empty() || impl_->instances == nil) {
            result.hits.resize(rays.size());
            result.status = GeometryStatus::success;
            return result;
        }

        std::size_t guards = 0;
        [[clang::objc_precise_lifetime]] id<MTLBuffer> output = impl_->dispatch<Closest>(
            impl_->closest, impl_->closest_table, rays, guards, result.diagnostic);
        if (output == nil) {
            return result;
        }
        const auto *output_data = static_cast<const unsigned char *>(output.contents);
        std::vector<GeometryHit> hits;
        hits.reserve(rays.size());
        for (std::size_t index = 0; index < rays.size(); ++index) {
            Closest hit{};
            std::memcpy(&hit, output_data + index * sizeof(hit), sizeof(hit));
            if (hit.kind == geometry_wire::miss) {
                hits.emplace_back();
                continue;
            }
            const bool triangle = hit.kind == geometry_wire::triangle;
            const bool patch = hit.kind == geometry_wire::bilinear_patch;
            const bool analytic = hit.kind == geometry_wire::analytic;
            const auto &ray = rays[index];
            if ((!triangle && !patch && !analytic) ||
                hit.instance >= impl_->instance_indices.size() || !std::isfinite(hit.t) ||
                hit.t < ray.t_min || hit.t > ray.t_max || !std::isfinite(hit.u) ||
                !std::isfinite(hit.v)) {
                result.diagnostic =
                    "Metal closest-hit output is invalid at ray " + std::to_string(index);
                return result;
            }
            const auto instance = impl_->instance_indices[hit.instance];
            if (hit.geometry != 0 || hit.kind != static_cast<Word>(instance.kind) ||
                hit.primitive >= instance.primitive_count) {
                result.diagnostic =
                    "Metal closest-hit primitive index is invalid at ray " + std::to_string(index);
                return result;
            }
            GeometryHit decoded{true, instance.mesh_index, instance.source_index, hit.primitive,
                                hit.t};
            if (triangle) {
                const float w = 1.0f - (hit.u + hit.v);
                if (!std::isfinite(w)) {
                    result.diagnostic = "Metal closest-hit barycentrics are invalid at ray " +
                                        std::to_string(index);
                    return result;
                }
                decoded.barycentric = {w, hit.u, hit.v};
            } else {
                if (hit.u < 0 || hit.u > 1 || hit.v < 0 || hit.v > 1) {
                    result.diagnostic = "Metal closest-hit patch coordinates are invalid at ray " +
                                        std::to_string(index);
                    return result;
                }
                decoded.kind = instance.kind;
                if (patch)
                    decoded.patch_uv = {hit.u, hit.v};
                else
                    decoded.analytic_uv = {hit.u, hit.v};
            }
            hits.push_back(decoded);
        }
        result.hits = std::move(hits);
        result.guard_count = guards;
        result.status = GeometryStatus::success;
        return result;
    }
}

GeometryOcclusionResult MetalGeometryScene::occlusion(std::span<const GeometryRay> rays) {
    @autoreleasepool {
        GeometryOcclusionResult result;
        if (!impl_) {
            result.diagnostic = moved_from;
            return result;
        }
        result.diagnostic = validate_geometry_rays(rays);
        if (!result.diagnostic.empty()) {
            return result;
        }
        if (rays.empty() || impl_->instances == nil) {
            result.occluded.resize(rays.size());
            result.status = GeometryStatus::success;
            return result;
        }

        std::size_t guards = 0;
        [[clang::objc_precise_lifetime]] id<MTLBuffer> output = impl_->dispatch<Word>(
            impl_->occlusion, impl_->occlusion_table, rays, guards, result.diagnostic);
        if (output == nil) {
            return result;
        }
        const auto *output_data = static_cast<const unsigned char *>(output.contents);
        std::vector<std::uint8_t> occluded;
        occluded.reserve(rays.size());
        for (std::size_t index = 0; index < rays.size(); ++index) {
            Word hit = 0;
            std::memcpy(&hit, output_data + index * sizeof(hit), sizeof(hit));
            if (hit > 1) {
                result.diagnostic =
                    "Metal occlusion output is invalid at ray " + std::to_string(index);
                return result;
            }
            occluded.push_back(static_cast<std::uint8_t>(hit));
        }
        result.occluded = std::move(occluded);
        result.guard_count = guards;
        result.status = GeometryStatus::success;
        return result;
    }
}

GeometryBuildResult build_metal_geometry(const GeometryData &geometry,
                                         const std::filesystem::path &library_path) {
    @autoreleasepool {
        GeometryBuildResult result;
        result.diagnostic = validate_geometry(geometry);
        if (!result.diagnostic.empty()) {
            return result;
        }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            result.status = GeometryStatus::no_device;
            result.diagnostic = "No system-default Metal device";
            return result;
        }
        result.device = metal::inspect_device(device);
        if (!result.device.compute_ray_tracing) {
            result.status = GeometryStatus::no_ray_tracing;
            result.diagnostic = "System-default Metal device does not support compute ray tracing";
            return result;
        }

        std::size_t active_instances = 0;
        for (const auto &instance : geometry.instances) {
            const auto &mesh = geometry.meshes[instance.mesh_index];
            active_instances += !mesh.triangles.empty();
            active_instances += !mesh.patches.empty();
            active_instances += !mesh.analytics.empty();
        }
        if (!metal::fits_instance_limit(active_instances)) {
            result.diagnostic =
                "Metal instance count exceeds the standard acceleration structure limit";
            return result;
        }
        if (!fits_buffer<MTLAccelerationStructureInstanceDescriptor>(device, active_instances)) {
            result.diagnostic = "Metal instance descriptor buffer exceeds device size limit";
            return result;
        }
        if (active_instances != 0) {
            for (std::size_t index = 0; index < geometry.meshes.size(); ++index) {
                const auto &mesh = geometry.meshes[index];
                if (!metal::fits_primitive_limit(mesh.triangles.size()) ||
                    !metal::fits_primitive_limit(mesh.patches.size()) ||
                    !metal::fits_primitive_limit(mesh.analytics.size())) {
                    result.diagnostic = "Metal primitive count exceeds the standard acceleration "
                                        "structure limit (mesh " +
                                        std::to_string(index) + ")";
                    return result;
                }
            }
        }

        auto impl = std::make_unique<MetalGeometryScene::Impl>();
        impl->device = device;
        impl->instance_indices.reserve(active_instances);
        for (std::size_t index = 0; index < geometry.instances.size(); ++index) {
            const auto mesh_index = geometry.instances[index].mesh_index;
            for (const auto kind : primitive_kinds) {
                const auto count = primitive_count(geometry.meshes[mesh_index], kind);
                if (count != 0) {
                    impl->instance_indices.push_back(
                        {static_cast<Word>(index), mesh_index, kind, count});
                }
            }
        }
        if (!impl->instance_indices.empty()) {
            id<MTLLibrary> library = metal::load_library(device, library_path, result.diagnostic);
            if (library == nil) {
                return result;
            }
            id<MTLFunction> intersection =
                metal::load_patch_intersection(library, result.diagnostic);
            if (intersection == nil) {
                return result;
            }
            impl->closest = metal::load_pipeline(device, library, @"rypath_closest_hits",
                                                 result.diagnostic, intersection);
            if (impl->closest == nil) {
                return result;
            }
            impl->closest_table =
                metal::make_intersection_table(impl->closest, intersection, result.diagnostic);
            if (impl->closest_table == nil) {
                return result;
            }
            impl->occlusion = metal::load_pipeline(device, library, @"rypath_occlusion",
                                                   result.diagnostic, intersection);
            if (impl->occlusion == nil) {
                return result;
            }
            impl->occlusion_table =
                metal::make_intersection_table(impl->occlusion, intersection, result.diagnostic);
            if (impl->occlusion_table == nil) {
                return result;
            }
            impl->queue = [device newCommandQueue];
            if (impl->queue == nil) {
                result.diagnostic = "Metal geometry command queue creation failed";
                return result;
            }

            NSMutableArray<id<MTLAccelerationStructure>> *structures = [NSMutableArray array];
            std::vector<std::array<Word, primitive_kinds.size()>> mesh_to_structure(
                geometry.meshes.size());
            for (std::size_t index = 0; index < geometry.meshes.size(); ++index) {
                const auto &mesh = geometry.meshes[index];
                for (const auto kind : primitive_kinds) {
                    if (primitive_count(mesh, kind) == 0) {
                        continue;
                    }
                    // Release each completed build's descriptors and command resources.
                    @autoreleasepool {
                        id<MTLAccelerationStructure> structure =
                            build_mesh(device, impl->queue, mesh, kind, result.diagnostic);
                        if (structure == nil) {
                            result.diagnostic += " (mesh " + std::to_string(index) + ")";
                            return result;
                        }
                        mesh_to_structure[index][kind_index(kind)] =
                            static_cast<Word>(structures.count);
                        [structures addObject:structure];
                    }
                }
            }
            impl->meshes = [structures copy];

            std::vector<MTLAccelerationStructureInstanceDescriptor> native_instances;
            native_instances.reserve(impl->instance_indices.size());
            for (const auto &indices : impl->instance_indices) {
                const auto &m = geometry.instances[indices.source_index].transform.values;
                MTLAccelerationStructureInstanceDescriptor instance{};
                for (std::size_t column = 0; column < 4; ++column) {
                    instance.transformationMatrix.columns[column] =
                        MTLPackedFloat3Make(m[column], m[column + 4], m[column + 8]);
                }
                instance.mask = geometry_wire::all_visible;
                if (geometry.instances[indices.source_index].non_opaque) {
                    instance.options = MTLAccelerationStructureInstanceOptionNonOpaque;
                }
                instance.accelerationStructureIndex =
                    mesh_to_structure[indices.mesh_index][kind_index(indices.kind)];
                native_instances.push_back(instance);
            }
            id<MTLBuffer> buffer = upload(
                device,
                std::span<const MTLAccelerationStructureInstanceDescriptor>{native_instances});
            if (buffer == nil) {
                result.diagnostic = "Metal instance descriptor buffer allocation failed";
                return result;
            }
            MTLInstanceAccelerationStructureDescriptor *descriptor =
                [MTLInstanceAccelerationStructureDescriptor descriptor];
            descriptor.instancedAccelerationStructures = impl->meshes;
            descriptor.instanceDescriptorBuffer = buffer;
            descriptor.instanceDescriptorStride =
                sizeof(MTLAccelerationStructureInstanceDescriptor);
            descriptor.instanceCount = native_instances.size();
            impl->instances = build_structure(device, impl->queue, descriptor, @"rypath instances",
                                              result.diagnostic);
            if (impl->instances == nil) {
                result.diagnostic += " (instances)";
                return result;
            }
        }

        result.scene.reset(new MetalGeometryScene(std::move(impl)));
        result.status = GeometryStatus::success;
        return result;
    }
}

} // namespace rypath
