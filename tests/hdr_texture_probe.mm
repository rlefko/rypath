// SPDX-License-Identifier: GPL-3.0-only
#include "hdr_probe_wire.h"
#include "image_texture.hpp"
#include "metal_execution.hpp"
#include "render_wire.h"
#include "rgb_spectrum.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace rypath;
namespace wire = render_wire;
namespace probe = hdr_probe;
void require(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}

void run_unbounded(id<MTLDevice> device, id<MTLLibrary> library) {
    // Independently recorded RGBUnboundedSpectrum values from unmodified PBRT
    // b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f, Release/binary32, contraction off.
    // These angular RGB values are reconstructed only after Fourier evaluation.
    constexpr std::array wavelengths{360.f, 400.f, 550.f, 700.f, 830.f};
    constexpr std::array colors{std::array{0.f, 0.f, 0.f}, std::array{.25f, .25f, .25f},
                                std::array{3.f, 3.f, 3.f}, std::array{2.f, .5f, 1.f},
                                std::array{8.f, 2.f, 4.f}, std::array{100.f, 25.f, 50.f},
                                std::array{0.f, 2.f, .25f}};
    constexpr std::array reference{
        std::array{0.f, 0.f, 0.f, 0.f, 0.f},
        std::array{.25f, .25f, .25f, .25f, .25f},
        std::array{3.f, 3.f, 3.f, 3.f, 3.f},
        std::array{3.574015141f, 2.470099211f, .5986864567f, 3.785815239f, 3.9843297f},
        std::array{14.29606056f, 9.880396843f, 2.394745827f, 15.14326096f, 15.9373188f},
        std::array{178.7007599f, 123.5049591f, 29.93432236f, 189.2907562f, 199.2164917f},
        std::array{.01533305645f, .04286825657f, 2.058033228f, .01983582973f, .001925587654f}};
    std::vector<std::array<float, 4>> queries;
    for (const auto &rgb : colors)
        for (float wavelength : wavelengths)
            queries.push_back({rgb[0], rgb[1], rgb[2], wavelength});
    const auto table = srgb_reconstruction_table();
    const auto input = [device newBufferWithBytes:queries.data()
                                           length:queries.size() * sizeof(queries[0])
                                          options:MTLResourceStorageModeShared];
    const auto coefficients = [device newBufferWithBytes:table.data()
                                                  length:table.size_bytes()
                                                 options:MTLResourceStorageModeShared];
    const auto output = [device newBufferWithLength:(queries.size() + 1) * sizeof(float)
                                            options:MTLResourceStorageModeShared];
    require(input && coefficients && output, "Unbounded spectrum probe allocation failed");
    std::memset(output.contents, 0x5a, output.length);
    std::string diagnostic;
    const auto pipeline =
        metal::load_pipeline(device, library, @"rypath_unbounded_spectrum_probe", diagnostic);
    require(pipeline != nil, diagnostic);
    const auto queue = [device newCommandQueue];
    const auto command = [queue commandBuffer];
    const auto encoder = [command computeCommandEncoder];
    require(queue && command && encoder, "Unbounded spectrum probe command creation failed");
    const unsigned count = static_cast<unsigned>(queries.size());
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:coefficients offset:0 atIndex:1];
    [encoder setBuffer:output offset:0 atIndex:2];
    [encoder setBytes:&count length:sizeof(count) atIndex:3];
    const auto group = pipeline.threadExecutionWidth;
    [encoder dispatchThreadgroups:MTLSizeMake((count + group - 1) / group, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(group, 1, 1)];
    [encoder endEncoding];
    diagnostic = metal::complete(command, "Unbounded spectrum probe");
    require(diagnostic.empty(), diagnostic);
    const auto *values = static_cast<const float *>(output.contents);
    constexpr double tolerance = 2e-5;
    for (std::size_t c = 0; c < colors.size(); ++c)
        for (std::size_t w = 0; w < wavelengths.size(); ++w) {
            auto cpu = evaluate_unbounded_srgb(colors[c], wavelengths[w]);
            require(cpu.has_value(), "CPU unbounded spectrum evaluation failed");
            const double actual = values[c * wavelengths.size() + w], expected = reference[c][w];
            if (c < 3)
                require(actual == expected,
                        "Zero/neutral unbounded spectrum changed its exact value");
            require(std::isfinite(actual) &&
                        std::abs(actual - *cpu) <= tolerance * std::max(1., std::abs(double(*cpu))),
                    "GPU/CPU unbounded spectrum mismatch");
            require(std::abs(actual - expected) <= tolerance * std::max(1., std::abs(expected)),
                    "GPU/pinned PBRT unbounded spectrum mismatch");
        }
    for (std::size_t w = 0; w < wavelengths.size(); ++w) {
        require(values[2 * wavelengths.size() + w] == 3.f,
                "Unbounded neutral spectrum was albedo clamped");
        const double base = values[3 * wavelengths.size() + w];
        require(std::abs(values[4 * wavelengths.size() + w] - 4 * base) <=
                    tolerance * std::max(1., 4 * base),
                "Unbounded spectrum lost amplitude homogeneity at scale four");
        require(std::abs(values[5 * wavelengths.size() + w] - 50 * base) <=
                    tolerance * std::max(1., 50 * base),
                "Unbounded spectrum lost amplitude homogeneity at scale fifty");
    }
    require(values[3 * wavelengths.size()] > 1.f, "Chromatic Fourier value was albedo clamped");
    std::array<unsigned char, sizeof(float)> guard;
    guard.fill(0x5a);
    require(std::memcmp(&values[queries.size()], guard.data(), guard.size()) == 0,
            "Unbounded spectrum probe overran output");
    std::cout << "Validated " << queries.size()
              << " unbounded RGB GPU spectra against CPU and pinned PBRT values\n";
}

void run(id<MTLDevice> device) {
    std::vector<wire::ImageTexture> textures;
    std::vector<wire::TextureLevel> levels;
    std::vector<wire::Word> texels;
    std::vector<probe::Query> queries;
    std::vector<probe::Result> expected;
    std::vector<bool> exact;
    for (const auto format : {ImageSampleFormat::half, ImageSampleFormat::float32}) {
        for (const auto channels : {1u, 3u, 4u}) {
            DecodedImage source{.width = 4,
                                .height = 2,
                                .rgb = {},
                                .sample_format = format,
                                .source_channels = channels,
                                .file_format = ImageFileFormat::exr};
            for (unsigned pixel = 0; pixel < 8; ++pixel) {
                const float r = float(pixel) * 0.25f - 0.75f;
                source.floating_rgb.insert(
                    source.floating_rgb.end(),
                    {r,
                     channels == 1 ? r
                                   : (format == ImageSampleFormat::half ? 4096.0f : 100000.125f) +
                                         float(pixel) * 4.0f,
                     channels == 1
                         ? r
                         : (format == ImageSampleFormat::half ? 0x1p-24f : 0x1.000002p0f)});
                if (channels == 4)
                    source.floating_alpha.push_back(float(pixel) / 8.0f);
            }
            for (const auto wrap : {ImageWrap::repeat, ImageWrap::clamp, ImageWrap::black}) {
                for (const bool native : {false, true}) {
                    auto built = native ? build_image_base(source, ImageEncoding::linear, wrap)
                                        : build_image_pyramid(source, ImageEncoding::linear, wrap);
                    require(built.value.has_value(), built.diagnostic);
                    const auto &image = *built.value;
                    const auto level_begin = static_cast<unsigned>(levels.size());
                    const auto word_begin = static_cast<unsigned>(texels.size());
                    for (const auto &level : image.levels)
                        levels.push_back({level.width, level.height, word_begin + level.offset});
                    texels.insert(texels.end(), image.texels.begin(), image.texels.end());
                    for (const auto filter : {ImageFilter::point, ImageFilter::bilinear,
                                              ImageFilter::trilinear, ImageFilter::ewa}) {
                        const unsigned filter_id =
                            filter == ImageFilter::point       ? wire::point_texture_filter
                            : filter == ImageFilter::bilinear  ? wire::bilinear_texture_filter
                            : filter == ImageFilter::trilinear ? wire::trilinear_texture_filter
                                                               : wire::ewa_texture_filter;
                        const auto index = static_cast<unsigned>(textures.size());
                        textures.push_back(
                            {level_begin, static_cast<unsigned>(image.levels.size()),
                             static_cast<unsigned>(wrap),
                             format == ImageSampleFormat::half ? wire::linear_half_texel_format
                                                               : wire::linear_float_texel_format,
                             1, 1, 0, 0, 1, 0, filter_id, 8, 0, image.source_channels});
                        for (const auto uv : {std::array{0.125f, 0.25f}, std::array{0.5f, 0.5f},
                                              std::array{-0.35f, 1.4f}, std::array{0.91f, 0.72f}}) {
                            for (const auto width : {0.0f, 0.1f, 0.37f, 2.0f}) {
                                queries.push_back({index, unsigned(native), uv[0], uv[1], width,
                                                   0.13f * width, -0.07f * width, width * 0.4f});
                                const auto rgb =
                                    native ? image_base_linear_rgb(image, uv[0], uv[1])
                                           : image_linear_rgb(image, uv[0], 1.0f - uv[1], width,
                                                              0.13f * width, -0.07f * width,
                                                              width * 0.4f, filter);
                                const auto scalar =
                                    native ? std::optional{0.0f}
                                           : image_linear_scalar(image, uv[0], 1.0f - uv[1], width,
                                                                 0.13f * width, -0.07f * width,
                                                                 width * 0.4f, filter);
                                require(rgb && scalar, "CPU HDR reference lookup failed");
                                expected.push_back({(*rgb)[0], (*rgb)[1], (*rgb)[2], *scalar});
                                exact.push_back(!native && filter == ImageFilter::point &&
                                                width == 0);
                            }
                        }
                    }
                }
            }
        }
    }
    std::string diagnostic;
    const auto library = metal::load_library(device, {}, diagnostic);
    require(library != nil, diagnostic);
    run_unbounded(device, library);
    const auto pipeline =
        metal::load_pipeline(device, library, @"rypath_hdr_lookup_probe", diagnostic);
    require(pipeline != nil, diagnostic);
    const auto buffer = [&](const auto &data) {
        return [device newBufferWithBytes:data.data()
                                   length:data.size() * sizeof(data[0])
                                  options:MTLResourceStorageModeShared];
    };
    const auto texture_buffer = buffer(textures), level_buffer = buffer(levels),
               texel_buffer = buffer(texels), query_buffer = buffer(queries);
    const std::array<float, 1> transfer{};
    const auto transfer_buffer = buffer(transfer);
    const auto output = [device newBufferWithLength:(queries.size() + 1) * sizeof(probe::Result)
                                            options:MTLResourceStorageModeShared];
    require(texture_buffer && level_buffer && texel_buffer && query_buffer && transfer_buffer &&
                output,
            "HDR probe allocation failed");
    std::memset(output.contents, 0x5a, output.length);
    wire::TextureParameters parameters{};
    parameters.texture_count = static_cast<unsigned>(textures.size());
    parameters.level_count = static_cast<unsigned>(levels.size());
    parameters.texel_word_count = static_cast<unsigned>(texels.size());
    const unsigned count = static_cast<unsigned>(queries.size());
    const auto queue = [device newCommandQueue];
    const auto command = [queue commandBuffer];
    const auto encoder = [command computeCommandEncoder];
    require(queue && command && encoder, "HDR probe command creation failed");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:texture_buffer offset:0 atIndex:0];
    [encoder setBuffer:level_buffer offset:0 atIndex:1];
    [encoder setBuffer:texel_buffer offset:0 atIndex:2];
    [encoder setBuffer:transfer_buffer offset:0 atIndex:3];
    [encoder setBytes:&parameters length:sizeof(parameters) atIndex:4];
    [encoder setBuffer:query_buffer offset:0 atIndex:5];
    [encoder setBuffer:output offset:0 atIndex:6];
    [encoder setBytes:&count length:sizeof(count) atIndex:7];
    const auto group = pipeline.threadExecutionWidth;
    [encoder dispatchThreadgroups:MTLSizeMake((queries.size() + group - 1) / group, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(group, 1, 1)];
    [encoder endEncoding];
    diagnostic = metal::complete(command, "HDR texture probe");
    require(diagnostic.empty(), diagnostic);
    const auto *results = static_cast<const probe::Result *>(output.contents);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const std::array actual{results[i].r, results[i].g, results[i].b, results[i].scalar};
        const auto &e = expected[i];
        const std::array reference{e.r, e.g, e.b, e.scalar};
        for (std::size_t c = 0; c < actual.size(); ++c) {
            const double tolerance =
                exact[i] ? 0 : 8e-6 * std::max(1.0, std::abs(double(reference[c])));
            require(std::isfinite(actual[c]) &&
                        std::abs(double(actual[c]) - reference[c]) <= tolerance,
                    "HDR lookup mismatch at query " + std::to_string(i) + " channel " +
                        std::to_string(c));
        }
    }
    std::array<unsigned char, sizeof(probe::Result)> guard;
    guard.fill(0x5a);
    require(std::memcmp(&results[expected.size()], guard.data(), guard.size()) == 0,
            "HDR probe overran its output");
    std::cout << "Validated " << expected.size()
              << " HALF/FLOAT native and filtered HDR GPU queries\n";
}
} // namespace

int main() {
    @autoreleasepool {
        try {
            const id<MTLDevice> device = MTLCreateSystemDefaultDevice();
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
            if (device == nil) {
                std::cout << "SKIP HDR texture probe: no Metal device available\n";
                return 77;
            }
#endif
            require(device != nil, "HDR texture probe requires Metal");
            run(device);
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
