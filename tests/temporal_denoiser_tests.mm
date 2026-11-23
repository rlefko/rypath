// SPDX-License-Identifier: GPL-3.0-only
// Numerical/API qualification, not image-quality or performance evidence.
#include "metal_frame_access.hpp"
#include "temporal_denoiser.hpp"
#import <Foundation/Foundation.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace rypath;
unsigned width = 129, height = 129, pixels = width * height;
using Color = std::array<float, 4>;
using Identity = std::array<std::uint32_t, 4>;
using RGB = std::array<double, 3>;
using Image = std::vector<RGB>;
void require(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
void near(double actual, double expected, const std::string &message, double relative = 2e-4,
          double absolute = 2e-5) {
    require(std::isfinite(actual) &&
                std::abs(actual - expected) <= absolute + relative * std::abs(expected),
            message + " actual=" + std::to_string(actual) +
                " expected=" + std::to_string(expected));
}
id<MTLTexture> texture(id<MTLDevice> device, MTLPixelFormat format) {
    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                         width:width
                                                                        height:height
                                                                     mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    auto result = [device newTextureWithDescriptor:descriptor];
    require(result != nil, "Test texture allocation");
    return result;
}
struct Fixture {
    TemporalDenoiserInputs inputs;
    std::vector<Color> color = std::vector<Color>(pixels, Color{0, 0, 0, 1});
    std::vector<Color> diffuse = std::vector<Color>(pixels, Color{.5f, .5f, .5f, 0});
    std::vector<Color> specular = std::vector<Color>(pixels, Color{0, 0, 0, 0});
    std::vector<Color> normal = std::vector<Color>(pixels, Color{0, 0, 1, 0});
    std::vector<std::array<float, 2>> motion = std::vector<std::array<float, 2>>(pixels);
    std::vector<float> depth = std::vector<float>(pixels, .25f),
                       previous = std::vector<float>(pixels, .25f);
    std::vector<Identity> identity = std::vector<Identity>(pixels, Identity{0, 0, 1, 0});
    std::vector<Color> position = std::vector<Color>(pixels, Color{0, 0, 4, 0});
    std::vector<Color> geometric_normal = std::vector<Color>(pixels, Color{0, 0, 1, 0});
    std::vector<std::uint32_t> group = std::vector<std::uint32_t>(pixels, UINT32_MAX);
    explicit Fixture(id<MTLDevice> device) {
        inputs.color = texture(device, MTLPixelFormatRGBA32Float);
        inputs.depth = texture(device, MTLPixelFormatR32Float);
        inputs.motion = texture(device, MTLPixelFormatRG16Float);
        inputs.diffuse_albedo = texture(device, MTLPixelFormatRGBA16Float);
        inputs.specular_albedo = texture(device, MTLPixelFormatRGBA16Float);
        inputs.normal = texture(device, MTLPixelFormatRGBA16Float);
        inputs.previous_depth = texture(device, MTLPixelFormatR32Float);
        inputs.identity = texture(device, MTLPixelFormatRGBA32Uint);
        inputs.spatial_position = texture(device, MTLPixelFormatRGBA32Float);
        inputs.spatial_normal = texture(device, MTLPixelFormatRGBA32Float);
        inputs.spatial_group = texture(device, MTLPixelFormatR32Uint);
        inputs.output = texture(device, MTLPixelFormatRGBA32Float);
    }
    static void upload(id<MTLTexture> target, const void *data, NSUInteger row) {
        [target replaceRegion:MTLRegionMake2D(0, 0, width, height)
                  mipmapLevel:0
                    withBytes:data
                  bytesPerRow:row];
    }
    static void half4(id<MTLTexture> target, const std::vector<Color> &source) {
        std::vector<std::array<_Float16, 4>> half(pixels);
        for (unsigned p = 0; p < pixels; ++p)
            for (unsigned c = 0; c < 4; ++c)
                half[p][c] = static_cast<_Float16>(source[p][c]);
        upload(target, half.data(), width * sizeof(half[0]));
    }
    void upload() {
        upload(inputs.spatial_position, position.data(), width * sizeof(Color));
        upload(inputs.spatial_normal, geometric_normal.data(), width * sizeof(Color));
        upload(inputs.spatial_group, group.data(), width * sizeof(std::uint32_t));
        upload(inputs.color, color.data(), width * sizeof(Color));
        upload(inputs.depth, depth.data(), width * sizeof(float));
        upload(inputs.previous_depth, previous.data(), width * sizeof(float));
        upload(inputs.identity, identity.data(), width * sizeof(Identity));
        std::vector<std::array<_Float16, 2>> half(pixels);
        for (unsigned p = 0; p < pixels; ++p)
            for (unsigned c = 0; c < 2; ++c)
                half[p][c] = static_cast<_Float16>(motion[p][c]);
        upload(inputs.motion, half.data(), width * sizeof(half[0]));
        half4(inputs.diffuse_albedo, diffuse);
        half4(inputs.specular_albedo, specular);
        half4(inputs.normal, normal);
    }
    Image capture() const {
        std::vector<Color> result(pixels);
        [inputs.output getBytes:result.data()
                    bytesPerRow:width * sizeof(Color)
                     fromRegion:MTLRegionMake2D(0, 0, width, height)
                    mipmapLevel:0];
        Image image(pixels);
        for (unsigned p = 0; p < pixels; ++p) {
            require(result[p][3] == 1, "Opaque output alpha");
            for (unsigned c = 0; c < 3; ++c)
                image[p][c] = result[p][c];
        }
        return image;
    }
};
std::unique_ptr<TemporalDenoiser>
adapter(MetalFrameLease &lease, unsigned iterations = 0, unsigned history = 64,
        bool prefilter = false, bool strict_segments = false,
        SpatialVariancePolicy policy = SpatialVariancePolicy::paired) {
    auto built = TemporalDenoiser::create(lease, {.width = width,
                                                  .height = height,
                                                  .history_limit = history,
                                                  .spatial_iterations = iterations,
                                                  .variance_prefilter = prefilter,
                                                  .spatial_variance_policy = policy,
                                                  .strict_spatial_segments = strict_segments});
    require(built.denoiser != nullptr, built.diagnostic);
    return std::move(built.denoiser);
}
std::string execute(TemporalDenoiser &denoiser, Fixture &fixture, id<MTLCommandQueue> queue) {
    fixture.upload();
    auto command = [queue commandBuffer];
    const auto error = denoiser.encode(command, fixture.inputs);
    require(error.empty(), error);
    [command commit];
    [command waitUntilCompleted];
    return denoiser.complete(command.status == MTLCommandBufferStatusCompleted);
}
void compare(const Image &actual, const Image &expected, const std::string &message,
             double tolerance = 2e-4) {
    for (unsigned p = 0; p < pixels; ++p)
        for (unsigned c = 0; c < 3; ++c)
            near(actual[p][c], expected[p][c],
                 message + " p=" + std::to_string(p) + " c=" + std::to_string(c), tolerance);
}
// Independent double stencil: accumulate weighted sums then normalize. The GPU
// uses incremental weighted means. Geometry fixtures use equal depth/normals,
// with deliberate primitive/material discontinuities and exact binary albedos.
double geometry(const Fixture &f, unsigned p, unsigned q) {
    if (f.identity[p] != f.identity[q])
        return 0;
    double geometry_weight = 1.;
    if (f.identity[p][2] != 1u && f.identity[p][0] != UINT32_MAX) {
        const double za = f.depth[p], zb = f.depth[q];
        const double tolerance = .02 * std::max(std::abs(za), std::abs(zb)) +
                                 std::max(double(std::numeric_limits<float>::min()),
                                          8. * std::numeric_limits<float>::epsilon() *
                                              std::max(std::abs(za), std::abs(zb)));
        double na = 0, nb = 0, product = 0;
        for (unsigned c = 0; c < 3; ++c) {
            na += double(f.normal[p][c]) * f.normal[p][c];
            nb += double(f.normal[q][c]) * f.normal[q][c];
            product += double(f.normal[p][c]) * f.normal[q][c];
        }
        geometry_weight = std::exp(-std::abs(za - zb) / tolerance) *
                          std::pow(std::max(0., product / std::sqrt(na * nb)), 32.);
    }
    double distance = 0;
    for (unsigned c = 0; c < 3; ++c)
        distance += std::abs(double(f.diffuse[p][c]) - f.diffuse[q][c]) +
                    std::abs(double(f.specular[p][c]) - f.specular[q][c]);
    return geometry_weight * std::exp(-4 * distance);
}
Image cold_variance(const Fixture &f, const Image &input) {
    Image result(pixels);
    for (unsigned p = 0; p < pixels; ++p) {
        double sum = 0;
        RGB first{}, second{};
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                const int qx = int(p % width) + x, qy = int(p / width) + y;
                if (qx < 0 || qy < 0 || qx >= int(width) || qy >= int(height))
                    continue;
                const unsigned q = unsigned(qy) * width + unsigned(qx);
                const double w = geometry(f, p, q);
                sum += w;
                for (unsigned c = 0; c < 3; ++c) {
                    const double delta = input[q][c] - input[p][c];
                    first[c] += w * delta;
                    second[c] += w * delta * delta;
                }
            }
        for (unsigned c = 0; c < 3; ++c)
            result[p][c] = std::max(0., second[c] / sum - (first[c] / sum) * (first[c] / sum));
    }
    return result;
}
struct Moments {
    Image mean, variance;
};
Image spatial(const Fixture &f, Moments moments, unsigned iterations, bool prefilter = false,
              SpatialVariancePolicy policy = SpatialVariancePolicy::paired) {
    auto &[mean, variance] = moments;
    constexpr std::array<double, 5> kernel{1, 4, 6, 4, 1};
    for (unsigned iteration = 0; iteration < iterations; ++iteration) {
        Image next(pixels), next_variance(pixels);
        const int step = 1 << iteration;
        for (unsigned p = 0; p < pixels; ++p) {
            RGB guidance = variance[p];
            if (prefilter) {
                guidance = {};
                double normalization = 0;
                for (int y = -1; y <= 1; ++y)
                    for (int x = -1; x <= 1; ++x) {
                        int qx = int(p % width) + x, qy = int(p / width) + y;
                        if (qx < 0 || qy < 0 || qx >= int(width) || qy >= int(height))
                            continue;
                        unsigned q = unsigned(qy) * width + unsigned(qx);
                        double weight = geometry(f, p, q) * (x == 0 ? 2 : 1) * (y == 0 ? 2 : 1);
                        normalization += weight;
                        for (unsigned c = 0; c < 3; ++c)
                            guidance[c] += weight * variance[q][c];
                    }
                for (auto &c : guidance)
                    c /= normalization;
            }
            double sum = 0;
            for (int y = -2; y <= 2; ++y)
                for (int x = -2; x <= 2; ++x) {
                    const int qx = int(p % width) + x * step, qy = int(p / width) + y * step;
                    if (qx < 0 || qy < 0 || qx >= int(width) || qy >= int(height))
                        continue;
                    const unsigned q = unsigned(qy) * width + unsigned(qx);
                    double distance = 0;
                    for (unsigned c = 0; c < 3; ++c) {
                        const double delta = mean[q][c] - mean[p][c];
                        const double edge_variance = policy == SpatialVariancePolicy::paired
                                                         ? guidance[c] + variance[q][c]
                                                     : policy == SpatialVariancePolicy::center
                                                         ? guidance[c]
                                                         : std::min(guidance[c], variance[q][c]);
                        distance += delta * delta / (4 * edge_variance + 1e-4);
                    }
                    const double weight = kernel[unsigned(x + 2)] * kernel[unsigned(y + 2)] / 256 *
                                          geometry(f, p, q) * std::exp(-distance);
                    sum += weight;
                    for (unsigned c = 0; c < 3; ++c) {
                        next[p][c] += weight * mean[q][c];
                        next_variance[p][c] += weight * weight * variance[q][c];
                    }
                }
            require(sum > 0, "CPU stencil positive center support");
            for (unsigned c = 0; c < 3; ++c) {
                next[p][c] /= sum;
                next_variance[p][c] /= sum * sum;
            }
        }
        mean = std::move(next);
        variance = std::move(next_variance);
    }
    return std::move(moments.mean);
}
void recurrence(MetalFrameLease &lease) {
    auto temporal = adapter(lease), filtered = adapter(lease, 1);
    Fixture f(lease.device()), g(lease.device());
    Image mean(pixels), variance(pixels);
    for (unsigned sample = 0; sample < 137; ++sample) {
        for (unsigned p = 0; p < pixels; ++p) {
            const float noise = float((sample * 17 + p * 3) % 23) - 11;
            f.color[p] = {noise + float(p) * .2f, -noise * .5f - float(p) * .1f,
                          noise * 2 + 100000.f, 1};
            const double alpha = 1. / std::min(sample + 1, 64u);
            for (unsigned c = 0; c < 3; ++c) {
                const double delta = double(f.color[p][c]) - mean[p][c];
                if (sample == 0) {
                    mean[p][c] = f.color[p][c];
                    variance[p][c] = 0;
                } else {
                    variance[p][c] = (1 - alpha) * (variance[p][c] + alpha * delta * delta);
                    mean[p][c] += alpha * delta;
                }
            }
        }
        f.inputs.reset_history = sample == 0;
        g.color = f.color;
        g.inputs.reset_history = sample == 0;
        require(execute(*temporal, f, lease.queue()).empty(), "Temporal recurrence execution");
        require(execute(*filtered, g, lease.queue()).empty(), "Filtered recurrence execution");
        compare(f.capture(), mean, "Centered recurrence beyond history cap");
        require(temporal->stats().history_accepted == (sample == 0 ? 0 : pixels),
                "Temporal history acceptance");
    }
    compare(g.capture(),
            spatial(g,
                    {mean,
                     [&] {
                         auto proxy = variance;
                         for (auto &v : proxy)
                             for (auto &c : v)
                                 c /= 64.;
                         return proxy;
                     }()},
                    1),
            "Temporal variance drives independent spatial stencil", 8e-4);
    // Finite signed HDR above half range must survive unchanged.
    for (auto &pixel : f.color)
        pixel = {-1e20f, 1e20f, -123456.f, 1};
    f.inputs.reset_history = true;
    require(execute(*temporal, f, lease.queue()).empty(), "HDR temporal execution");
    for (const auto &pixel : f.capture())
        for (unsigned c = 0; c < 3; ++c)
            near(pixel[c], f.color[0][c], "Signed HDR preservation", 0, 0);
    auto replacement = adapter(lease, 0, 1);
    f.inputs.reset_history = false;
    for (const float value :
         {1e10f, 1.f, std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()}) {
        for (auto &pixel : f.color)
            pixel = {value, -value, 1, 1};
        require(execute(*replacement, f, lease.queue()).empty(), "One-frame replacement");
        require(replacement->stats().history_accepted == 0 &&
                    replacement->stats().history_rejected == pixels,
                "One-frame limit does not consume old history");
        for (const auto &pixel : f.capture())
            for (unsigned c = 0; c < 3; ++c)
                near(pixel[c], f.color[0][c], "Exact replacement avoids cancellation/overflow", 0,
                     0);
    }
}
void spatial_cases(MetalFrameLease &lease) {
    auto denoiser = adapter(lease, 5);
    Fixture f(lease.device());
    Image values(pixels);
    for (unsigned p = 0; p < pixels; ++p) {
        f.identity[p][1] = p % width >= width / 2 ? 1u : 0u;
        f.diffuse[p][0] = p / width >= height / 2 ? .25f : .5f;
        f.color[p] = {float((p * 7) % 13) - 6, float(p % 5) - 2, float(p % 3) - 1, 1};
        for (unsigned c = 0; c < 3; ++c)
            values[p][c] = f.color[p][c];
    }
    const auto expected = spatial(f, {values, cold_variance(f, values)}, 5);
    require(execute(*denoiser, f, lease.queue()).empty(), "Spatial cold execution");
    const auto positive = f.capture();
    auto prefiltered = adapter(lease, 5, 64, true, false, SpatialVariancePolicy::center);
    require(execute(*prefiltered, f, lease.queue()).empty(),
            "Optional guidance prefilter execution");
    compare(f.capture(),
            spatial(f, {values, cold_variance(f, values)}, 5, true, SpatialVariancePolicy::center),
            "Independent guidance-only prefilter oracle", 6e-4);
    compare(positive, expected, "Independent five-pass spatial stencil", 6e-4);
    for (auto &pixel : f.color)
        for (unsigned c = 0; c < 3; ++c)
            pixel[c] = -pixel[c];
    require(execute(*denoiser, f, lease.queue()).empty(), "Sign symmetry execution");
    const auto negative = f.capture();
    for (unsigned p = 0; p < pixels; ++p)
        for (unsigned c = 0; c < 3; ++c)
            near(negative[p][c], -positive[p][c], "Signed spatial symmetry", 5e-5);
    for (auto &pixel : f.color)
        pixel = {-100000.f, 250000.f, -.25f, 1};
    require(execute(*denoiser, f, lease.queue()).empty(), "Spatial constant field execution");
    for (const auto &pixel : f.capture())
        for (unsigned c = 0; c < 3; ++c)
            near(pixel[c], f.color[0][c], "Spatial constant field", 2e-6);
    // Disjoint geometry has zero support, so opposite HDR neighbors must not
    // overflow a squared color difference that can never affect this pixel.
    for (unsigned p = 0; p < pixels; ++p) {
        f.identity[p][1] = p;
        f.color[p] = {p % 2 ? -1e20f : 1e20f, 0, 0, 1};
    }
    require(execute(*denoiser, f, lease.queue()).empty(), "Zero-support HDR neighbors");
    const auto disjoint = f.capture();
    for (unsigned p = 0; p < pixels; ++p)
        near(disjoint[p][0], f.color[p][0], "Zero-support HDR preservation", 0, 0);
}
// Full 129-pixel support exposes the step-16 halo that small fixtures miss.
void continuity_and_halo(MetalFrameLease &lease) {
    Fixture whole(lease.device()), split(lease.device());
    auto a = adapter(lease, 5), b = adapter(lease, 5);
    for (unsigned p = 0; p < pixels; ++p) {
        const float x = float(p % width) * .01f, y = float(p / width) * .01f;
        // Oblique sheet has substantial depth variation but a shared geometric plane.
        whole.position[p] = {x, y, 4.f + x, 0};
        whole.geometric_normal[p] = {-.70710677f, 0, .70710677f, 0};
        whole.group[p] = 0;
        whole.color[p] = {float(int(p % 11) - 5), float(int(p % 7) - 3), float(int(p % 3) - 1), 1};
    }
    split.position = whole.position;
    split.geometric_normal = whole.geometric_normal;
    split.group = whole.group;
    split.color = whole.color;
    for (unsigned p = 0; p < pixels; ++p)
        split.identity[p][1] = (p % width) / 8;
    require(execute(*a, whole, lease.queue()).empty(), "Oblique whole-plane execution");
    require(execute(*b, split, lease.queue()).empty(), "Indexed triangulated-plane execution");
    compare(split.capture(), whole.capture(), "Triangulation invariance across connected plane",
            1e-5);
    const auto baseline = whole.capture();
    for (auto &pixel : whole.color)
        for (unsigned c = 0; c < 3; ++c)
            pixel[c] *= 1024.f;
    require(execute(*a, whole, lease.queue()).empty(), "Exposure-scaled execution");
    auto exposed = whole.capture();
    for (unsigned p = 0; p < pixels; ++p)
        for (unsigned c = 0; c < 3; ++c)
            near(exposed[p][c], baseline[p][c] * 1024., "Exposure equivariance", 2e-4, .02);
    // Distinct connected components and close parallel layers cannot exchange color.
    Fixture layers(lease.device());
    auto no_leak = adapter(lease, 5, 64, false, true);
    for (unsigned p = 0; p < pixels; ++p) {
        const bool right = p % width >= width / 2;
        layers.identity[p][1] = right ? 1 : 0;
        layers.group[p] = right ? 1 : 0;
        layers.position[p] = {float(p % width) * .01f, 0, right ? 4.0001f : 4.f, 0};
        layers.color[p] = right ? Color{100000.f, -50000.f, 25000.f, 1} : Color{0, 0, 0, 1};
    }
    require(execute(*no_leak, layers, lease.queue()).empty(), "Disconnected close-layer execution");
    auto output = layers.capture();
    for (unsigned p = 0; p < pixels; ++p)
        for (unsigned c = 0; c < 3; ++c)
            near(output[p][c], layers.color[p][c], "No disconnected-layer leak", 0, 0);
    // A same-group plane with a visible gap must not be jumped by dilated taps.
    for (unsigned p = 0; p < pixels; ++p) {
        layers.group[p] = 0;
        layers.position[p][2] = 4.f;
        if (p % width == width / 2)
            layers.identity[p] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    }
    require(execute(*no_leak, layers, lease.queue()).empty(), "Visible-gap execution");
    output = layers.capture();
    for (unsigned p = 0; p < pixels; ++p)
        for (unsigned c = 0; c < 3; ++c)
            near(output[p][c], layers.color[p][c], "Dilated taps cannot jump gap", 0, 0);
    Fixture impulse(lease.device());
    auto halo = adapter(lease, 5, 64, false, false, SpatialVariancePolicy::center);
    for (unsigned frame = 0; frame < 70; ++frame) {
        impulse.inputs.reset_history = frame == 0;
        require(execute(*halo, impulse, lease.queue()).empty(), "Mature impulse history");
    }
    const unsigned center = (height / 2) * width + width / 2;
    impulse.color[center] = {128000.f, -64000.f, 32000.f, 1};
    require(execute(*halo, impulse, lease.queue()).empty(), "Mature signed HDR impulse");
    output = impulse.capture();
    for (unsigned p = 0; p < pixels; ++p)
        if (p != center)
            for (unsigned c = 0; c < 3; ++c)
                near(output[p][c], 0, "Center guidance rejects high-variance neighbor halo", 0, 0);
}
void curved_primitive(MetalFrameLease &lease) {
    for (auto kind : {2u, 4u}) {
        Fixture f(lease.device());
        auto filter = adapter(lease, 1);
        Image values(pixels);
        for (unsigned p = 0; p < pixels; ++p) {
            const float angle = (float(p % width) - float(width / 2)) * .01f;
            f.identity[p][2] = kind;
            // Half-representable appearance normal for independent CPU oracle.
            f.normal[p] = {float(_Float16(std::sin(angle))), 0, float(_Float16(std::cos(angle))),
                           0};
            f.geometric_normal[p] = f.normal[p];
            f.position[p] = {std::sin(angle), float(p / width) * .01f, 4.f + 1.f - std::cos(angle),
                             0};
            f.depth[p] = .25f / (1.f + .1f * (1.f - std::cos(angle)));
            f.color[p] = {float(int(p % 7) - 3), float(int(p % 5) - 2), float(int(p % 3) - 1), 1};
            for (unsigned c = 0; c < 3; ++c)
                values[p][c] = f.color[p][c];
        }
        require(execute(*filter, f, lease.queue()).empty(), "Curved same-primitive execution");
        auto result = f.capture();
        compare(result, spatial(f, {values, cold_variance(f, values)}, 1),
                "Original depth/shading-normal curved geometry oracle", 8e-4);
        bool changed = false;
        for (unsigned p = 0; p < pixels; ++p)
            changed = changed || std::abs(result[p][0] - values[p][0]) > .01;
        require(changed, "Curved primitive must meaningfully filter neighboring estimates");
    }
}
void variance_policy_signals(MetalFrameLease &lease) {
    const unsigned center = (height / 2) * width + width / 2;
    for (auto policy : {SpatialVariancePolicy::paired, SpatialVariancePolicy::center,
                        SpatialVariancePolicy::conservative}) {
        Fixture signal(lease.device());
        auto filter = adapter(lease, 5, 64, false, false, policy);
        // Known deterministic incident radiance: a narrow signed HDR reflection/
        // caustic-shaped signal. This is a filter fixture, not transport qualification.
        for (unsigned p = 0; p < pixels; ++p) {
            const bool stripe =
                p % width == width / 2 && std::abs(int(p / width) - int(height / 2)) <= 2;
            signal.color[p] = stripe ? Color{128000.f, 96000.f, -32000.f, 1} : Color{0, 0, 0, 1};
        }
        for (unsigned frame = 0; frame < 5; ++frame) {
            signal.inputs.reset_history = frame == 0;
            require(execute(*filter, signal, lease.queue()).empty(),
                    "Persistent noise-free HDR signal");
        }
        auto output = signal.capture();
        for (unsigned p = 0; p < pixels; ++p)
            for (unsigned c = 0; c < 3; ++c)
                near(output[p][c], signal.color[p][c],
                     "Mature noise-free narrow HDR signal preservation", 0, 0);
        Fixture transient(lease.device());
        auto temporal = adapter(lease, 5, 64, false, false, policy);
        for (unsigned frame = 0; frame < 65; ++frame) {
            transient.inputs.reset_history = frame == 0;
            require(execute(*temporal, transient, lease.queue()).empty(),
                    "Transient signal baseline history");
        }
        // A legitimate newly appearing signal has the same numeric shape as a
        // noise impulse; measure attenuation explicitly instead of declaring it noise.
        transient.color[center] = {128000.f, 96000.f, -32000.f, 1};
        require(execute(*temporal, transient, lease.queue()).empty(),
                "Transient legitimate HDR signal");
        output = transient.capture();
        const double temporal_peak = 128000. / 64.;
        double roi_energy = 0, halo_energy = 0;
        for (unsigned p = 0; p < pixels; ++p) {
            require(std::isfinite(output[p][0]), "Finite policy signal output");
            if (std::abs(int(p % width) - int(width / 2)) <= 32 &&
                std::abs(int(p / width) - int(height / 2)) <= 32)
                roi_energy += output[p][0];
            if (p != center)
                halo_energy += std::abs(output[p][0]);
        }
        std::cout << "variance_policy_signal policy=" << static_cast<unsigned>(policy)
                  << " ground_truth_peak=128000 temporal_peak=" << temporal_peak
                  << " reconstructed_peak=" << output[center][0]
                  << " center_retention=" << output[center][0] / temporal_peak
                  << " halo_energy=" << halo_energy << " roi_energy=" << roi_energy << '\n';
        if (policy == SpatialVariancePolicy::conservative) {
            near(output[center][0], temporal_peak, "Conservative transient center preservation",
                 2e-5);
            near(halo_energy, 0, "Conservative transient halo rejection", 0, 0);
        }
    }
    Fixture hdr(lease.device());
    auto prefilter = adapter(lease, 1, 64, true, false, SpatialVariancePolicy::center);
    require(execute(*prefilter, hdr, lease.queue()).empty(),
            "HDR variance prefilter zero initial state");
    hdr.inputs.reset_history = false;
    for (auto &pixel : hdr.color)
        pixel = {1.8e19f, -1.8e19f, 1.8e19f, 1};
    require(execute(*prefilter, hdr, lease.queue()).empty(),
            "Finite HDR Gaussian variance guidance");
    for (auto value : hdr.capture())
        for (unsigned c = 0; c < 3; ++c)
            near(value[c], double(hdr.color[0][c]) * .5, "Stable finite HDR prefilter mean", 2e-6);
    require(!TemporalDenoiser::create(
                 lease, {.width = width, .height = height, .variance_prefilter = true})
                 .denoiser,
            "Prefilter with paired policy must reject ambiguous ablation");
}
void reprojection(MetalFrameLease &lease) {
    auto denoiser = adapter(lease);
    Fixture f(lease.device());
    const auto fresh = [&] {
        f.inputs.reset_history = true;
        for (unsigned p = 0; p < pixels; ++p)
            f.color[p] = {float(p % width), float(p % width), float(p % width), 1};
        require(execute(*denoiser, f, lease.queue()).empty(), "Reprojection source");
        f.inputs.reset_history = false;
        for (auto &pixel : f.color)
            pixel = {100, 100, 100, 1};
    };
    fresh();
    for (auto &motion : f.motion)
        motion = {1, 0};
    require(execute(*denoiser, f, lease.queue()).empty(), "Nearest reprojection execution");
    const auto shifted = f.capture();
    for (unsigned p = 0; p < pixels; ++p)
        near(shifted[p][0], p % width == width - 1 ? 100 : (100 + double(p % width + 1)) / 2,
             "Nearest reprojection rejects boundary");
    require(denoiser->stats().history_accepted == (width - 1) * height &&
                denoiser->stats().history_rejected == height,
            "Reprojection counts");
    for (auto &motion : f.motion)
        motion = {0, 0};
    fresh();
    std::fill(f.previous.begin(), f.previous.end(), .5f);
    require(execute(*denoiser, f, lease.queue()).empty(), "Depth rejection execution");
    require(denoiser->stats().history_accepted == 0, "Expected previous depth rejects mismatch");
    std::fill(f.previous.begin(), f.previous.end(), .25f);
    std::fill(f.depth.begin(), f.depth.end(), 1e-8f);
    std::fill(f.previous.begin(), f.previous.end(), 1e-8f);
    fresh();
    std::fill(f.previous.begin(), f.previous.end(), 2e-8f);
    require(execute(*denoiser, f, lease.queue()).empty(), "Far depth rejection execution");
    require(denoiser->stats().history_accepted == 0, "Relative far-depth mismatch rejected");
    std::fill(f.depth.begin(), f.depth.end(), .25f);
    std::fill(f.previous.begin(), f.previous.end(), .25f);
    fresh();
    for (auto &normal : f.normal)
        normal = {0, 0, -1, 0};
    require(execute(*denoiser, f, lease.queue()).empty(), "Normal rejection execution");
    require(denoiser->stats().history_accepted == 0, "World normal rejection");
    for (auto &normal : f.normal)
        normal = {0, 0, 1, 0};
    fresh();
    for (auto &id : f.identity)
        id[3] = 1;
    require(execute(*denoiser, f, lease.queue()).empty(), "Identity rejection execution");
    require(denoiser->stats().history_accepted == 0, "Exact identity rejection");
    // Sky has no surface depth/normal correspondence, but all guide values must remain finite.
    for (auto &id : f.identity)
        id.fill(UINT32_MAX);
    for (auto &normal : f.normal)
        normal = {0, 0, 0, 0};
    std::fill(f.depth.begin(), f.depth.end(), 0.f);
    std::fill(f.previous.begin(), f.previous.end(), 0.f);
    fresh();
    require(execute(*denoiser, f, lease.queue()).empty(), "Sky ignores depth/normal");
    require(denoiser->stats().history_accepted == pixels, "Sky history acceptance");
}
void failure_and_validation(MetalFrameLease &lease) {
    require(TemporalDenoiser::owned_resource_bytes(0, height) == UINT64_MAX,
            "Invalid budget dimensions");
    require(!TemporalDenoiser::create(lease, {.width = width, .height = height, .history_limit = 0})
                 .denoiser,
            "Invalid history limit");
    require(!TemporalDenoiser::create(lease,
                                      {.width = width, .height = height, .spatial_iterations = 6})
                 .denoiser,
            "Invalid spatial iterations");
    require(!TemporalDenoiser::create(lease, {.width = 16384, .height = 16384}).denoiser,
            "Oversized owned budget rejected");
    auto denoiser = adapter(lease);
    Fixture f(lease.device());
    for (auto &pixel : f.color)
        pixel = {std::numeric_limits<float>::max(), 0, 0, 1};
    require(execute(*denoiser, f, lease.queue()).empty(), "Finite extreme initial mean");
    f.inputs.reset_history = false;
    for (auto &pixel : f.color)
        pixel[0] = -std::numeric_limits<float>::max();
    require(!execute(*denoiser, f, lease.queue()).empty() && denoiser->stats().failure != 0,
            "Explicit centered overflow must fail");
    for (auto &pixel : f.color)
        pixel = {-3, 4, -5, 1};
    require(execute(*denoiser, f, lease.queue()).empty() && denoiser->stats().history_accepted == 0,
            "Failure invalidates history");
    auto output = f.inputs.output;
    f.inputs.output = f.inputs.color;
    require(!denoiser->encode([lease.queue() commandBuffer], f.inputs).empty(),
            "Output alias rejected");
    f.inputs.output = output;
    require(execute(*denoiser, f, lease.queue()).empty() && denoiser->stats().history_accepted == 0,
            "Encode rejection invalidates history without pending state");
    auto previous = f.inputs.previous_depth;
    f.inputs.previous_depth = f.inputs.identity;
    require(!denoiser->encode([lease.queue() commandBuffer], f.inputs).empty(),
            "Wrong guide format rejected");
    f.inputs.previous_depth = previous;
    auto tiny_descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                                           width:1
                                                          height:1
                                                       mipmapped:NO];
    tiny_descriptor.usage = MTLTextureUsageShaderRead;
    f.inputs.previous_depth = [lease.device() newTextureWithDescriptor:tiny_descriptor];
    require(!denoiser->encode([lease.queue() commandBuffer], f.inputs).empty(),
            "Wrong guide dimensions rejected");
    f.inputs.previous_depth = previous;
    auto color = f.inputs.color;
    auto write_descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    write_descriptor.usage = MTLTextureUsageShaderWrite;
    f.inputs.color = [lease.device() newTextureWithDescriptor:write_descriptor];
    require(!denoiser->encode([lease.queue() commandBuffer], f.inputs).empty(),
            "Missing read usage rejected");
    f.inputs.color = color;
    f.upload();
    auto command = [lease.queue() commandBuffer];
    require(denoiser->encode(command, f.inputs).empty(), "Pending test encode");
    require(!denoiser->encode(command, f.inputs).empty(), "Overlapping encode rejected");
    [command commit];
    [command waitUntilCompleted];
    require(denoiser->complete(command.status == MTLCommandBufferStatusCompleted).empty(),
            "Overlap preserves running command");
    for (auto &pixel : f.color)
        pixel[0] = std::numeric_limits<float>::infinity();
    require(!execute(*denoiser, f, lease.queue()).empty(), "Nonfinite input must fail");
    for (auto &pixel : f.color)
        pixel = {1, 2, 3, 1};
    for (auto &id : f.identity)
        id.fill(UINT32_MAX);
    f.normal[0][0] = std::numeric_limits<float>::quiet_NaN();
    require(!execute(*denoiser, f, lease.queue()).empty(), "Nonfinite sky guide must fail");
    f.normal[0][0] = 0;
    f.previous[0] = std::numeric_limits<float>::quiet_NaN();
    require(!execute(*denoiser, f, lease.queue()).empty(), "Nonfinite expected depth must fail");
    f.previous[0] = .25f;
    f.upload();
    command = [lease.queue() commandBuffer];
    require(denoiser->encode(command, f.inputs).empty(), "Forced failure encode");
    [command commit];
    [command waitUntilCompleted];
    require(!denoiser->complete(false).empty(), "Command failure invalidates history");
    require(execute(*denoiser, f, lease.queue()).empty() && denoiser->stats().history_accepted == 0,
            "Command failure recovery");
}
void counter_sizes(const RenderScene &scene) {
    for (const unsigned size : {1u, 31u, 32u, 33u}) {
        width = size;
        height = size == 1 ? 1 : 3;
        pixels = width * height;
        auto built = build_frame_session(scene, {.width = width, .height = height});
        require(built.session != nullptr, built.diagnostic);
        auto submitted = built.session->submit({});
        require(submitted.frame != nullptr, submitted.diagnostic);
        require(submitted.frame->wait().status == FrameStatus::complete,
                "Counter fixture source frame completion");
        auto acquired = MetalFrameAccess::acquire(*submitted.frame);
        require(acquired.lease != nullptr, acquired.diagnostic);
        auto &lease = *acquired.lease;
        auto denoiser = adapter(lease);
        Fixture f(lease.device());
        for (auto &value : f.color)
            value = {-2, 4, 100000, 1};
        require(execute(*denoiser, f, lease.queue()).empty(), "Counter cold frame");
        require(denoiser->stats().completed == pixels && denoiser->stats().history_accepted == 0 &&
                    denoiser->stats().history_rejected == pixels,
                "Exact cold counters across partial and full SIMD groups");
        f.inputs.reset_history = false;
        for (unsigned p = 0; p < pixels; ++p) {
            f.color[p] = {-4, 8, 100004, 1};
            f.identity[p][1] = p % 2;
        }
        require(execute(*denoiser, f, lease.queue()).empty(), "Counter mixed-history frame");
        require(denoiser->stats().completed == pixels &&
                    denoiser->stats().history_accepted == (pixels + 1) / 2 &&
                    denoiser->stats().history_rejected == pixels / 2,
                "Exact mixed-history counters across partial and full SIMD groups");
        Image expected(pixels);
        for (unsigned p = 0; p < pixels; ++p)
            expected[p] = p % 2 ? RGB{-4, 8, 100004} : RGB{-3, 6, 100002};
        compare(f.capture(), expected, "Independent exact two-frame recurrence", 0);
        f.color[0][0] = std::numeric_limits<float>::quiet_NaN();
        require(!execute(*denoiser, f, lease.queue()).empty(), "Partial-group input failure");
        f.color[0][0] = -4;
        require(execute(*denoiser, f, lease.queue()).empty() &&
                    denoiser->stats().completed == pixels &&
                    denoiser->stats().history_rejected == pixels,
                "Failure invalidates history and recovers with exact counters");
    }
}
} // namespace
int main() {
    @autoreleasepool {
        try {
            const auto info = query_metal_device();
            if (info.status == MetalStatus::no_device) {
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE) && RYPATH_ALLOW_NO_METAL_DEVICE
                std::cerr << "Temporal denoiser unavailable: " << info.diagnostic << '\n';
                return 77;
#else
                throw std::runtime_error("Temporal denoiser requires native Metal: " +
                                         info.diagnostic);
#endif
            }
            require(info.status == MetalStatus::success, info.diagnostic);
            if (!info.device.compute_ray_tracing) {
#if defined(RYPATH_ALLOW_NO_METAL_RAY_TRACING) && RYPATH_ALLOW_NO_METAL_RAY_TRACING
                std::cerr << "Temporal denoiser source unavailable: no compute ray tracing\n";
                return 77;
#else
                throw std::runtime_error("Temporal denoiser source requires compute ray tracing");
#endif
            }
            RenderScene scene;
            scene.model = RenderModel::pbrt;
            scene.raster_camera = RasterCamera{{}, {-.3f, -.3f, 1}, {.6f, 0, 0}, {0, .6f, 0}};
            scene.max_depth = 1;
            scene.environment.fill(.1f);
            auto built = build_frame_session(scene, {.width = width, .height = height});
            require(built.session != nullptr, built.diagnostic);
            auto submitted = built.session->submit({});
            require(submitted.frame != nullptr, submitted.diagnostic);
            require(submitted.frame->wait().status == FrameStatus::complete,
                    "Source frame completion");
            auto acquired = MetalFrameAccess::acquire(*submitted.frame);
            require(acquired.lease != nullptr, acquired.diagnostic);
            recurrence(*acquired.lease);
            spatial_cases(*acquired.lease);
            continuity_and_halo(*acquired.lease);
            curved_primitive(*acquired.lease);
            variance_policy_signals(*acquired.lease);
            reprojection(*acquired.lease);
            failure_and_validation(*acquired.lease);
            counter_sizes(scene);
            std::cout << "Signed temporal denoiser numerical/API qualification passed; image "
                         "quality unqualified\n";
            return 0;
        } catch (const std::exception &error) {
            std::cerr << "Temporal denoiser qualification: " << error.what() << '\n';
            return 1;
        }
    }
}
