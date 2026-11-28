// SPDX-License-Identifier: GPL-3.0-only
#include "enhanced_motion_wire.h"
#include "metal_execution.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
namespace {
using namespace rypath;
namespace mw = enhanced_motion_wire;
namespace motion = rypath_enhanced_motion;
constexpr unsigned width = 64, height = 48, pixels = width * height, guard = 256;
constexpr unsigned destination = 24 * width + 32, ordinary_source = 24 * width + 36;
constexpr unsigned dual_source = 24 * width + 48;
void need(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
mw::Geometry sky() {
    mw::Geometry g{};
    g.status = mw::sky;
    g.instance = g.primitive = g.primitive_kind = g.material = g.group = mw::absent;
    g.camera_relative_position = {0, 0, 1};
    return g;
}
mw::Geometry finite(render_wire::Point position, unsigned instance = 7) {
    auto g = sky();
    g.status = mw::finite;
    g.instance = instance;
    g.primitive = 0;
    g.primitive_kind = render_wire::triangle_primitive;
    g.material = 3;
    g.camera_relative_position = position;
    g.distance =
        std::sqrt(position.x * position.x + position.y * position.y + position.z * position.z);
    g.pixel_footprint = .25f;
    g.geometric_normal = {0, 0, -1};
    return g;
}
motion::Camera camera(float origin_x = 0, float sx = 1) {
    motion::Camera c{};
    c.width = width;
    c.height = height;
    c.world_to_view[0] = c.world_to_view[5] = c.world_to_view[10] = c.world_to_view[15] = 1;
    c.world_to_view[12] = -origin_x;
    c.view_to_clip[0] = sx;
    c.view_to_clip[5] = c.view_to_clip[11] = 1;
    c.view_to_clip[14] = .001f;
    return c;
}
// Independent double pinhole oracle, with y-down raster and floor indexing.
std::array<double, 2> project(double x, double y, double z, double origin_x = 0, double sx = 1) {
    return {(sx * (x - origin_x) / z + 1) * width / 2., (1 - y / z) * height / 2.};
}
unsigned index(std::array<double, 2> p) {
    need(p[0] >= 0 && p[0] < width && p[1] >= 0 && p[1] < height, "Independent projection bounds");
    return unsigned(std::floor(p[1])) * width + unsigned(std::floor(p[0]));
}
int run(const std::filesystem::path &path) {
    auto device = MTLCreateSystemDefaultDevice();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
    if (!device)
        return 77;
#endif
    need(device != nil, "No Metal device for motion donor probe");
    std::string why;
    auto library = metal::load_library(device, path, why);
    need(library != nil, why.c_str());
    auto function = metal::load_function(library, @"rypath_enhanced_motion_donors", why);
    auto donors = metal::load_pipeline(device, library, @"rypath_enhanced_motion_donors", why);
    auto reset = metal::load_pipeline(device, library, @"rypath_enhanced_motion_reset", why);
    need(function && donors && reset, why.c_str());
    auto argument_encoder = [function newArgumentEncoderWithBufferIndex:1];
    need(argument_encoder != nil, "Motion argument encoder");
    auto arguments = [device newBufferWithLength:argument_encoder.encodedLength
                                         options:MTLResourceStorageModeShared];
    const std::array<std::size_t, 5> lengths{sizeof(mw::Config), pixels * sizeof(mw::Geometry),
                                             pixels * sizeof(mw::Geometry),
                                             pixels * sizeof(mw::Donor), sizeof(mw::Counters)};
    std::array<id<MTLBuffer>, 5> buffers{};
    [argument_encoder setArgumentBuffer:arguments offset:0];
    for (unsigned i = 0; i < buffers.size(); ++i) {
        buffers[i] = [device newBufferWithLength:lengths[i] + 2 * guard
                                         options:MTLResourceStorageModeShared];
        need(buffers[i] != nil, "Motion guarded allocation");
        [argument_encoder setBuffer:buffers[i] offset:guard atIndex:i];
    }
    const auto data = [&](unsigned id) {
        return static_cast<unsigned char *>(buffers[id].contents) + guard;
    };
    auto queue = [device newCommandQueue];
    auto contract_pipeline =
        metal::load_pipeline(device, library, @"rypath_enhanced_motion_contract_v1", why);
    need(contract_pipeline != nil, why.c_str());
    auto contract_buffer = [device newBufferWithLength:sizeof(mw::Contract) + 2 * guard
                                               options:MTLResourceStorageModeShared];
    need(queue && contract_buffer, "Motion contract resources");
    std::memset(contract_buffer.contents, 0xa5, contract_buffer.length);
    auto contract_command = [queue commandBuffer];
    auto contract_encoder = [contract_command computeCommandEncoder];
    [contract_encoder setComputePipelineState:contract_pipeline];
    [contract_encoder setBuffer:contract_buffer offset:guard atIndex:0];
    [contract_encoder dispatchThreads:MTLSizeMake(1, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [contract_encoder endEncoding];
    need(metal::complete(contract_command, "Motion ABI").empty(), "Motion ABI completion");
    mw::Contract contract{};
    std::memcpy(&contract, static_cast<const unsigned char *>(contract_buffer.contents) + guard,
                sizeof(contract));
    const mw::Contract expected_contract{1, 352, 64, 16, 48, 148};
    need(std::memcmp(&contract, &expected_contract, sizeof(contract)) == 0,
         "Actual motion shader ABI");
    const auto *contract_bytes = static_cast<const unsigned char *>(contract_buffer.contents);
    for (unsigned k = 0; k < guard; ++k)
        need(contract_bytes[k] == 0xa5 && contract_bytes[guard + sizeof(contract) + k] == 0xa5,
             "Motion contract guards");
    const auto dispatch = [&](id<MTLComputePipelineState> pipeline, unsigned count) {
        auto command = [queue commandBuffer];
        auto encoder = [command computeCommandEncoder];
        need(command && encoder, "Motion encoder");
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:arguments offset:0 atIndex:1];
        for (auto buffer : buffers)
            [encoder useResource:buffer usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        need(metal::complete(command, "Actual motion donor kernel").empty(),
             "Motion GPU completion");
    };
    const auto counters = [&] {
        mw::Counters c{};
        std::memcpy(&c, data(mw::counters_id), sizeof(c));
        return c;
    };
    auto *current = reinterpret_cast<mw::Geometry *>(data(mw::current_geometry_id));
    auto *previous = reinterpret_cast<mw::Geometry *>(data(mw::previous_geometry_id));
    auto *map = reinterpret_cast<mw::Donor *>(data(mw::donors_id));
    mw::Config config{};
    const auto fresh = [&] {
        for (auto buffer : buffers)
            std::memset(buffer.contents, 0xa5, buffer.length);
        config = {};
        config.version = mw::contract;
        config.width = width;
        config.height = height;
        config.history_valid = 1;
        config.current_camera = camera();
        config.previous_camera = camera(-1);
        config.previous_origin_to_common = {-1, 0, 0};
        config.policy = {.99f, .5f, 32.f * std::numeric_limits<float>::epsilon(), 0};
        config.serial_low = 0x76543210u;
        config.serial_high = 0xfedcba98u;
        for (unsigned d = 0; d < pixels; ++d)
            current[d] = previous[d] = sky();
        current[destination] = finite({.05f, -1.f / 6, 8});
    };
    const auto prepare = [&] {
        std::memcpy(data(mw::config_id), &config, sizeof(config));
        dispatch(reset, 1);
        auto c = counters();
        c.geometry_completed = pixels;
        c.geometry_hits = 0;
        c.geometry_sky = 0;
        for (unsigned d = 0; d < pixels; ++d) {
            c.geometry_hits += current[d].status == mw::finite;
            c.geometry_sky += current[d].status == mw::sky;
        }
        std::memcpy(data(mw::counters_id), &c, sizeof(c));
    };
    const auto guards = [&](const std::vector<unsigned char> &old,
                            const std::vector<unsigned char> &now) {
        need(std::memcmp(data(mw::previous_geometry_id), old.data(), old.size()) == 0,
             "Previous geometry is immutable");
        need(std::memcmp(data(mw::current_geometry_id), now.data(), now.size()) == 0,
             "Donor pass preserves current geometry");
        need(std::memcmp(data(mw::config_id), &config, sizeof(config)) == 0,
             "Donor config immutable");
        for (unsigned id = 0; id < buffers.size(); ++id) {
            const auto *bytes = static_cast<const unsigned char *>(buffers[id].contents);
            for (unsigned k = 0; k < guard; ++k)
                need(bytes[k] == 0xa5 && bytes[guard + lengths[id] + k] == 0xa5,
                     "Motion bounds guard");
        }
    };
    const auto execute = [&] {
        const std::vector<unsigned char> old(data(mw::previous_geometry_id),
                                             data(mw::previous_geometry_id) +
                                                 lengths[mw::previous_geometry_id]);
        const std::vector<unsigned char> now(data(mw::current_geometry_id),
                                             data(mw::current_geometry_id) +
                                                 lengths[mw::current_geometry_id]);
        prepare();
        dispatch(donors, pixels + 3);
        guards(old, now);
    };
    const auto validate = [&](unsigned source, motion::Classification classification) {
        const auto c = counters();
        need(c.failure == 0 && c.donor_completed == pixels &&
                 c.cold + c.ordinary + c.dual + c.fallback == pixels,
             "Every owner receives one classified donor");
        need(map[destination].source_pixel == source &&
                 map[destination].classification == unsigned(classification),
             "Actual donor agrees with independent fixed oracle");
        for (unsigned d = 0; d < pixels; ++d)
            need(map[d].source_pixel < pixels && map[d].serial_low == config.serial_low &&
                     map[d].serial_high == config.serial_high,
                 "Every donor bounded and tagged with full serial");
    };
    fresh();
    config.history_valid = 0;
    std::memset(previous, 0xff, lengths[mw::previous_geometry_id]);
    execute();
    validate(destination, motion::Classification::cold);
    need(counters().cold == pixels, "Cold never reads poisoned previous geometry");
    fresh();
    previous[ordinary_source] = finite({1.125f, -1.f / 6, 8});
    execute();
    need(index(project(.05, -1. / 6, 8, -1)) == ordinary_source, "Independent ordinary projection");
    validate(ordinary_source, motion::Classification::ordinary);
    fresh();
    previous[ordinary_source] = finite({.28125f, -1.f / 24, 2}, 4);
    previous[dual_source] = finite({4.125f, -1.f / 6, 8});
    execute();
    validate(dual_source, motion::Classification::dual);
    const auto x = project(.05, -1. / 6, 8), y0 = project(.05, -1. / 6, 8, -1);
    const auto y = project(-.71875, -1. / 24, 2, -1), z = project(-.71875, -1. / 24, 2);
    need(index({y[0] + x[0] - z[0], y[1] + x[1] - z[1]}) == dual_source &&
             index({y0[0] + x[0] - z[0], y0[1] + x[1] - z[1]}) != dual_source &&
             index(y0) != dual_source,
         "Wrong old anchor and O=X both fail the independent disocclusion oracle");
    // Rotate the complete camera/geometry pair in a common rebased frame.
    // The independent pinhole oracle predicts the same donor indices; no
    // absolute world position is required by the GPU donor construction.
    for (const float angle : {.37f, -.61f}) {
        fresh();
        previous[ordinary_source] = finite({.28125f, -1.f / 24, 2}, 4);
        previous[dual_source] = finite({4.125f, -1.f / 6, 8});
        const float cosine = std::cos(angle), sine = std::sin(angle);
        const auto rotate = [&](render_wire::Point p) {
            return render_wire::Point{cosine * p.x + sine * p.z, p.y, -sine * p.x + cosine * p.z};
        };
        for (unsigned d = 0; d < pixels; ++d)
            for (auto *g : {&current[d], &previous[d]}) {
                g->camera_relative_position = rotate(g->camera_relative_position);
                if (g->status == mw::finite)
                    g->geometric_normal = rotate(g->geometric_normal);
            }
        for (auto *c : {&config.current_camera, &config.previous_camera}) {
            c->world_to_view[0] = cosine;
            c->world_to_view[8] = -sine;
            c->world_to_view[2] = sine;
            c->world_to_view[10] = cosine;
        }
        config.previous_origin_to_common = {-cosine, 0, sine};
        // Change MATRIX origin while preserving the projection: an origin
        // translation enters before the inverse rotation and cancels in view.
        const motion::Vec3 matrix_delta{32, -64, 128};
        for (auto *c : {&config.current_camera, &config.previous_camera}) {
            c->common_to_matrix_origin = matrix_delta;
            c->world_to_view[12] -= cosine * matrix_delta.x - sine * matrix_delta.z;
            c->world_to_view[13] -= matrix_delta.y;
            c->world_to_view[14] -= sine * matrix_delta.x + cosine * matrix_delta.z;
        }
        execute();
        validate(dual_source, motion::Classification::dual);
    }
    fresh();
    execute();
    validate(destination, motion::Classification::same_pixel_fallback);
    fresh();
    config.previous_camera = camera(-1, .5f);
    current[destination + 1] = finite({.3f, -1.f / 6, 8});
    const auto shared = index(project(.05, -1. / 6, 8, -1, .5));
    need(shared == index(project(.3, -1. / 6, 8, -1, .5)), "Independent many-to-one floor map");
    previous[shared] = finite({1.25f, -1.f / 6, 8});
    execute();
    validate(shared, motion::Classification::ordinary);
    need(map[destination + 1].source_pixel == shared &&
             map[destination + 1].classification == unsigned(motion::Classification::ordinary),
         "Many destinations retain the same frozen donor");
    for (unsigned fault = 0; fault < 5; ++fault) {
        fresh();
        if (fault == 0)
            config.version++;
        else if (fault == 1)
            current[destination].status = 99;
        else if (fault == 2)
            current[destination].camera_relative_position.x =
                std::numeric_limits<float>::quiet_NaN();
        else if (fault == 3) {
            previous[ordinary_source] = finite({1.125f, -1.f / 6, 8});
            previous[ordinary_source].group = 0;
        }
        prepare();
        if (fault == 4) {
            auto c = counters();
            c.geometry_completed--;
            std::memcpy(data(mw::counters_id), &c, sizeof(c));
        }
        dispatch(donors, pixels);
        need(counters().failure != 0, "Malformed config/geometry/counts fail visibly");
    }
    std::cout
        << "Actual GPU motion donor cold/ordinary/dual/fallback/many-to-one controls passed\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            need(argc == 2, "Expected metallib");
            return run(argv[1]);
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
}
