// SPDX-License-Identifier: GPL-3.0-only

#include "image_lighting_probe_wire.h"
#include "image_texture.hpp"
#include "metal_execution.hpp"
#include "rgb_spectrum.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numbers>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace rypath;
namespace probe = image_lighting_probe;
namespace wire = render_wire;

constexpr double pi = std::numbers::pi;
constexpr unsigned width = 3;
constexpr std::array<float, 16> skewed_cdf{0, 0, 0.25f,   1, 0, 1.f / 3, 2.f / 3, 1,
                                           0, 0, 1.f / 3, 1, 0, 0,       1,       1};
constexpr std::array<float, 16> uniform_cdf{0, 1.f / 3, 2.f / 3, 1, 0, 1.f / 3, 2.f / 3, 1,
                                            0, 1.f / 3, 2.f / 3, 1, 0, 1.f / 3, 2.f / 3, 1};

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

template <typename T> T image_or_throw(ImageResult<T> result) {
    if (!result.value)
        throw std::runtime_error(result.diagnostic);
    return std::move(*result.value);
}

void near(float actual, double expected, std::string_view item, double tolerance = 2e-6) {
    if (!(std::abs(actual - expected) <= tolerance * std::max(1.0, std::abs(expected))))
        throw std::runtime_error(std::string(item) + " got " + std::to_string(actual) +
                                 ", expected " + std::to_string(expected));
}

std::array<double, 2> reference_square(wire::Point world) {
    // The probe's environment-from-world rotation maps (x,y,z) -> (z,y,-x).
    const double x = world.z;
    const double y = world.y;
    const double z = -world.x;
    const double length = std::sqrt(x * x + y * y + z * z);
    const double phi = std::atan2(std::abs(y), std::abs(x)) * (2 / pi);
    const double r = std::sqrt(std::max(0.0, 1 - std::abs(z) / length));
    const double u = z < 0 ? 1 - phi * r : (1 - phi) * r;
    const double v = z < 0 ? 1 - (1 - phi) * r : phi * r;
    return {0.5 * (1 + std::copysign(u, x)), 0.5 * (1 + std::copysign(v, y))};
}

std::size_t reference_cell(wire::Point direction) {
    const auto [s, t] = reference_square(direction);
    const auto column = std::min(width - 1, static_cast<unsigned>(s * width));
    const auto row = std::min(width - 1, static_cast<unsigned>(t * width));
    return width * row + column;
}

std::size_t reference_radiance_pixel(wire::Point direction) {
    const auto [s, t] = reference_square(direction);
    // The test directions avoid internal cell boundaries, but include the exact south pole.
    int x = static_cast<int>(s * width), y = static_cast<int>(t * width);
    constexpr int size = static_cast<int>(width);
    if (x == size) {
        x = size - 1;
        y = size - 1 - y;
    }
    if (y == size) {
        x = size - 1 - x;
        y = size - 1;
    } else if (y < 0) {
        x = size - 1 - x;
        y = -y;
    }
    return static_cast<std::size_t>(size * y + x);
}

std::array<double, width * width> mixture_masses(const std::array<float, 16> &cdf) {
    std::array<double, width * width> result{};
    for (unsigned y = 0; y < width; ++y) {
        const auto base = (y + 1) * (width + 1);
        const double marginal = static_cast<double>(cdf[y + 1]) - cdf[y];
        for (unsigned x = 0; x < width; ++x) {
            const double conditional = static_cast<double>(cdf[base + x + 1]) - cdf[base + x];
            result[y * width + x] = 1.0 / (16 * width * width) + 15.0 / 16 * marginal * conditional;
        }
    }
    return result;
}

float expected_illuminant(std::array<float, 3> rgb, float wavelength) {
    const auto scale = 2 * *std::ranges::max_element(rgb);
    if (scale == 0)
        return 0;
    for (auto &channel : rgb)
        channel /= scale;
    const auto reflectance = reconstruct_srgb(rgb);
    if (!reflectance)
        throw std::runtime_error("Could not reconstruct the CPU illuminant control");
    const auto index = static_cast<std::size_t>(std::lround(wavelength) - 360);
    return scale * evaluate_srgb(*reflectance, wavelength) * pbrt_d65()[index];
}

std::vector<probe::Query> queries(std::uint32_t seed) {
    constexpr std::array directions{wire::Point{1, 0, 0},
                                    wire::Point{-1, 0, 0},
                                    wire::Point{0, 0, 1},
                                    wire::Point{0, 0, -1},
                                    wire::Point{0.3f, -0.55f, 0.81f},
                                    wire::Point{-0.36f, 0.8f, -0.48f}};
    constexpr std::array uv{wire::Point2{0, 0}, wire::Point2{1, 1}, wire::Point2{0.49f, 0.24f},
                            wire::Point2{-0.1f, 0.27f}};
    constexpr std::array wavelengths{360.0f, 460.45f, 550.5f, 610.75f, 830.0f};
    std::mt19937 random(seed);
    const auto uniform = [&] { return static_cast<float>(random() >> 8) * 0x1p-24f; };
    std::vector<probe::Query> result;
    constexpr unsigned count = 16384;
    result.reserve(count);
    for (unsigned i = 0; i < count; ++i)
        result.push_back({directions[i % directions.size()],
                          uv[i % uv.size()],
                          {uniform(), uniform()},
                          wavelengths[i % wavelengths.size()]});
    return result;
}

template <typename Range> id<MTLBuffer> buffer(id<MTLDevice> device, const Range &values) {
    const auto data = std::span(values);
    return [device newBufferWithBytes:data.data()
                               length:data.size_bytes()
                              options:MTLResourceStorageModeShared];
}

void verify(std::span<const probe::Query> cases, std::span<const probe::Result> results,
            const std::array<float, 16> &cdf, const ImagePyramid &base, const ImagePyramid &gamma) {
    const auto masses = mixture_masses(cdf);
    std::array<std::size_t, width * width> hits{};
    constexpr double pdf_factor = width * width / (4 * pi);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto &query = cases[i];
        const auto &result = results[i];
        const auto sampled = result.sampled;
        require(result.valid == 1, "Image environment sample failed");
        near(sampled.x * sampled.x + sampled.y * sampled.y + sampled.z * sampled.z, 1.0,
             "Environment sample direction norm");
        const auto sampled_cell = reference_cell(sampled);
        ++hits[sampled_cell];
        near(result.sample_pdf, pdf_factor * masses[sampled_cell], "Sample environment PDF");
        near(result.query_pdf, pdf_factor * masses[reference_cell(query.direction)],
             "Evaluated environment PDF");
        const auto area = image_base_linear_rgb(base, query.uv.x, 1.0f - query.uv.y);
        require(area.has_value(), "CPU area image control failed");
        near(result.area, 1.75 * expected_illuminant(area.value(), query.wavelength),
             "Area RGB illuminant lookup");
        const auto pixel = reference_radiance_pixel(query.direction);
        const auto x = static_cast<unsigned>(pixel % width);
        const auto y = static_cast<unsigned>(pixel / width);
        const auto environment = image_base_texel_linear_rgb(base, x, y);
        require(environment.has_value(), "CPU environment image control failed");
        near(result.environment, 0.6 * expected_illuminant(environment.value(), query.wavelength),
             "Environment RGB illuminant lookup");
    }
    for (std::size_t i = 0; i < hits.size(); ++i) {
        const double expected = masses[i] * static_cast<double>(cases.size());
        const double standard_error = std::sqrt(expected * (1 - masses[i]));
        require(std::abs(static_cast<double>(hits[i]) - expected) < 7 * standard_error,
                "Environment direction frequency differs from the quantized mixture PDF");
    }

    const auto scalar = [&](ImageFilter filter, float dx, float dy) {
        return image_linear_scalar(gamma, 0.25f, 0.25f, dx, 0, 0, dy, filter).value_or(NAN);
    };
    near(results[0].scalar_bilinear, scalar(ImageFilter::bilinear, 0, 0),
         "Gamma RGBA scalar bilerp");
    near(results[0].scalar_point, scalar(ImageFilter::point, 0, 0), "Gamma RGBA scalar point");
    near(results[0].scalar_ewa, scalar(ImageFilter::ewa, 0.07f, 0.04f), "Gamma RGBA scalar EWA");
    near(results[0].scalar_coarsest, scalar(ImageFilter::bilinear, 0.5f, 0),
         "Gamma RGBA scalar wide footprint");
}

void stress_environment_cells(id<MTLDevice> device, id<MTLLibrary> library,
                              id<MTLCommandQueue> queue, id<MTLBuffer> transfer,
                              id<MTLBuffer> spectra, id<MTLBuffer> illuminant) {
    std::string diagnostic;
    const auto pipeline =
        metal::load_pipeline(device, library, @"rypath_probe_environment_cell", diagnostic);
    require(pipeline != nil, diagnostic);
    const auto group = metal::group_width(device, pipeline);
    require(group != 0, "Environment cell probe thread group is invalid");

    // Concentrate every importance draw in one bin next to zero-mass bins.
    // Exercise both near-edge draws and ordinary interior draws.
    std::vector<wire::Point2> inputs;
    constexpr float threshold = 0.0625f;
    for (unsigned i = 0; i < 128; ++i) {
        for (unsigned j = 0; j < 64; ++j) {
            const float near = static_cast<float>(i) * 0x1p-24f;
            const float middle = (static_cast<float>(j) + 0.5f) / 64.0f;
            inputs.insert(inputs.end(), {{threshold + near, middle},
                                         {1.0f - near, middle},
                                         {threshold + (1.0f - threshold) * middle, near},
                                         {threshold + (1.0f - threshold) * middle, 1.0f - near}});
        }
    }
    std::mt19937 random(483950);
    const auto uniform = [&] { return static_cast<float>(random() >> 8) * 0x1p-24f; };
    for (unsigned i = 0; i < 65536; ++i)
        inputs.push_back({threshold + (1.0f - threshold) * uniform(), uniform()});
    const auto count = static_cast<wire::Word>(inputs.size());
    constexpr unsigned targeted_count = 128 * 64 * 4;
    const auto input = buffer(device, inputs);
    const auto output = [device newBufferWithLength:inputs.size() * sizeof(probe::StressResult)
                                            options:MTLResourceStorageModeShared];
    require(input != nil && output != nil, "Environment cell probe allocation failed");

    struct Case {
        unsigned source_width, grid_width, x, y;
        float angle, stretch;
    };
    const std::array cases{Case{4096, 128, 115, 99, 0, 0}, Case{4096, 128, 2, 124, 0.713f, 0},
                           Case{4093, 128, 70, 90, -1.247f, 0x1.ep-19f},
                           Case{127, 127, 69, 89, 0.937f, -0x1.ep-19f}};
    std::size_t total_corrections = 0;
    for (const auto &[source, grid, column, row, angle, stretch] : cases) {
        std::vector<float> cdf(std::size_t(grid + 1) * (grid + 1));
        for (unsigned y = row + 1; y <= grid; ++y)
            cdf[y] = 1;
        for (unsigned x = column + 1; x <= grid; ++x)
            cdf[std::size_t(row + 1) * (grid + 1) + x] = 1;
        const auto cdf_buffer = buffer(device, cdf);
        const float cosine = std::cos(angle), sine = std::sin(angle);
        const float a = (1 + stretch) * cosine, b = (1 + stretch) * sine;
        wire::TextureGraphParameters graph_parameters{
            .environment_image = 0,
            .environment_scale = 1,
            .environment_from_world = {a, 0, b, 0, 1, 0, -sine, 0, cosine},
            .world_from_environment = {a, 0, -sine, 0, 1, 0, b, 0, cosine},
            .environment_width = grid,
            .environment_cdf_count = static_cast<wire::Word>(cdf.size())};
        const std::array textures{wire::ImageTexture{0, 1, 1, wire::linear8_texel_format, 1, 1, 0,
                                                     0, 1, 0, wire::bilinear_texture_filter, 8,
                                                     256}};
        const std::array levels{wire::TextureLevel{source, source, 0}};
        const wire::TextureParameters image_parameters{.texture_count = 1,
                                                       .level_count = 1,
                                                       .texel_word_count = source * source,
                                                       .transfer_count = 768};
        const auto texture = buffer(device, textures);
        const auto level = buffer(device, levels);
        const auto bright_x = ((2 * column + 1) * source) / (2 * grid);
        const auto bright_y = ((2 * row + 1) * source) / (2 * grid);
        const wire::Point2 bright_uv{(static_cast<float>(bright_x) + 0.5f) / source,
                                     (static_cast<float>(bright_y) + 0.5f) / source};
        const auto texels =
            [device newBufferWithLength:std::size_t(source) * source * sizeof(wire::Word)
                                options:MTLResourceStorageModeShared];
        require(cdf_buffer != nil && texture != nil && level != nil && texels != nil,
                "Environment cell probe inputs could not be allocated");
        std::memset(texels.contents, 0, texels.length);
        static_cast<wire::Word *>(texels.contents)[std::size_t(bright_y) * source + bright_x] =
            0x00ffffff;
        const auto command = [queue commandBuffer];
        const auto encoder = [command computeCommandEncoder];
        require(command != nil && encoder != nil,
                "Environment cell probe encoder allocation failed");
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:input offset:0 atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        [encoder setBuffer:texture offset:0 atIndex:2];
        [encoder setBuffer:level offset:0 atIndex:3];
        [encoder setBuffer:texels offset:0 atIndex:4];
        [encoder setBuffer:transfer offset:0 atIndex:5];
        [encoder setBuffer:spectra offset:0 atIndex:6];
        [encoder setBuffer:illuminant offset:0 atIndex:7];
        [encoder setBytes:&image_parameters length:sizeof(image_parameters) atIndex:8];
        [encoder setBytes:&graph_parameters length:sizeof(graph_parameters) atIndex:9];
        [encoder setBuffer:cdf_buffer offset:0 atIndex:10];
        [encoder setBytes:&count length:sizeof(count) atIndex:11];
        [encoder setBytes:&bright_uv length:sizeof(bright_uv) atIndex:12];
        [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(group, 1, 1)];
        [encoder endEncoding];
        diagnostic = metal::complete(command, "Environment cell probe");
        require(diagnostic.empty(), diagnostic);

        const double expected = (0.0625 + 0.9375 * grid * grid) / (4 * pi);
        const auto results = static_cast<const probe::StressResult *>(output.contents);
        std::size_t targeted_corrections = 0, random_corrections = 0;
        float max_delta = 0;
        for (unsigned i = 0; i < count; ++i) {
            const auto &result = results[i];
            require(result.valid == 1 && result.sampled_pdf == result.queried_pdf,
                    "Environment edge draw has different sampled and queried PDFs");
            near(result.sampled_pdf, expected, "Environment concentrated cell PDF");
            near(result.bright_texel_radiance, pbrt_d65()[555 - 360],
                 "Native-resolution environment radiance");
            require(result.direction_delta >= 0 && result.direction_delta <= 1e-4f,
                    "Environment cell correction moved the direction too far");
            if (result.initial_cell_matches == 0) {
                (i < targeted_count ? targeted_corrections : random_corrections)++;
            } else {
                require(result.direction_delta == 0, "Environment changed an in-cell draw");
            }
            max_delta = std::max(max_delta, result.direction_delta);
        }
        total_corrections += targeted_corrections + random_corrections;
        std::cout << "Environment " << source << " source / " << grid << " grid corrected "
                  << targeted_corrections << '/' << targeted_count << " edge draws and "
                  << random_corrections << '/' << count - targeted_count
                  << " ordinary draws, max direction displacement " << max_delta << '\n';
    }
    require(total_corrections != 0, "Environment edge cases did not exercise the correction");
}

void run(id<MTLDevice> device, const std::filesystem::path &library_path) {
    std::string diagnostic;
    const auto library = metal::load_library(device, library_path, diagnostic);
    require(library != nil, diagnostic);
    const auto pipeline =
        metal::load_pipeline(device, library, @"rypath_probe_image_lighting", diagnostic);
    require(pipeline != nil, diagnostic);
    const auto queue = [device newCommandQueue];
    require(queue != nil, "Metal light image command queue allocation failed");

    const DecodedImage source{width, width, {0,  0,   0,   43,  17,  53, 98,  65,  176,
                                             17, 15,  35,  124, 117, 92, 117, 117, 117,
                                             3,  143, 175, 255, 65,  91, 0,   0,   0}};
    const auto base =
        image_or_throw(build_image_base(source, ImageEncoding::linear, ImageWrap::clamp));
    const DecodedImage rgba{.width = 2,
                            .height = 2,
                            .rgb = {51, 102, 153, 22, 8, 0, 50, 100, 150, 3, 2, 10},
                            .source_channels = 4,
                            .alpha = {85, 127, 255, 0}};
    const auto gamma =
        image_or_throw(build_image_pyramid(rgba, ImageEncoding::gamma, ImageWrap::clamp, {}, 2));
    require(gamma.custom_transfer != nullptr, "Expected a gamma transfer table");
    std::array<wire::ImageTexture, 4> textures{
        wire::ImageTexture{0, 1, 1, wire::linear8_texel_format, 1, 1, 0, 0, 1, 0,
                           wire::bilinear_texture_filter, 8, 256},
        wire::ImageTexture{1, 2, 1, wire::linear8_texel_format, 4, 3, 0.31f, 0.73f, 1, 0,
                           wire::bilinear_texture_filter, 8, 512, 4},
        wire::ImageTexture{1, 2, 1, wire::linear8_texel_format, 4, 3, 0.31f, 0.73f, 1, 0,
                           wire::point_texture_filter, 8, 512, 4},
        wire::ImageTexture{1, 2, 1, wire::linear8_texel_format, 4, 3, 0.31f, 0.73f, 1, 0,
                           wire::ewa_texture_filter, 8, 512, 4}};
    std::array levels{wire::TextureLevel{width, width, 0}, wire::TextureLevel{2, 2, 9},
                      wire::TextureLevel{1, 1, 13}};
    std::vector<std::uint32_t> texels = base.texels;
    texels.insert(texels.end(), gamma.texels.begin(), gamma.texels.end());
    std::array<float, 768> transfer{};
    std::ranges::copy(image_transfer_table(ImageEncoding::linear), transfer.begin() + 256);
    std::ranges::copy(*gamma.custom_transfer, transfer.begin() + 512);
    const wire::TextureParameters image_parameters{
        .texture_count = static_cast<wire::Word>(textures.size()),
        .level_count = static_cast<wire::Word>(levels.size()),
        .texel_word_count = static_cast<wire::Word>(texels.size()),
        .transfer_count = static_cast<wire::Word>(transfer.size())};
    const wire::TextureGraphParameters graph_parameters{
        .environment_image = 0,
        .environment_scale = 0.6f,
        .environment_from_world = {0, 0, 1, 0, 1, 0, -1, 0, 0},
        .world_from_environment = {0, 0, -1, 0, 1, 0, 1, 0, 0},
        .environment_width = width,
        .environment_cdf_count = static_cast<wire::Word>(skewed_cdf.size())};
    const auto texture_buffer = buffer(device, textures);
    const auto level_buffer = buffer(device, levels);
    const auto texel_buffer = buffer(device, texels);
    const auto transfer_buffer = buffer(device, transfer);
    const auto spectra_buffer = buffer(device, srgb_reconstruction_table());
    const auto illuminant_buffer = buffer(device, pbrt_d65());
    require(texture_buffer != nil && level_buffer != nil && texel_buffer != nil &&
                transfer_buffer != nil && spectra_buffer != nil && illuminant_buffer != nil,
            "Metal light image input buffer allocation failed");

    std::uint32_t seed = 2482059;
    for (const auto &cdf : {skewed_cdf, uniform_cdf}) {
        const auto inputs = queries(seed++);
        const auto count = static_cast<wire::Word>(inputs.size());
        const auto input_buffer = buffer(device, inputs);
        const auto cdf_buffer = buffer(device, cdf);
        [[clang::objc_precise_lifetime]] id<MTLBuffer> output =
            [device newBufferWithLength:inputs.size() * sizeof(probe::Result)
                                options:MTLResourceStorageModeShared];
        const auto command = [queue commandBuffer];
        const auto encoder = [command computeCommandEncoder];
        const auto group = metal::group_width(device, pipeline);
        require(input_buffer != nil && cdf_buffer != nil && output != nil && command != nil &&
                    encoder != nil && group != 0,
                "Metal light image probe allocation failed");
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:input_buffer offset:0 atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        [encoder setBuffer:texture_buffer offset:0 atIndex:2];
        [encoder setBuffer:level_buffer offset:0 atIndex:3];
        [encoder setBuffer:texel_buffer offset:0 atIndex:4];
        [encoder setBuffer:transfer_buffer offset:0 atIndex:5];
        [encoder setBuffer:spectra_buffer offset:0 atIndex:6];
        [encoder setBuffer:illuminant_buffer offset:0 atIndex:7];
        [encoder setBytes:&image_parameters length:sizeof(image_parameters) atIndex:8];
        [encoder setBytes:&graph_parameters length:sizeof(graph_parameters) atIndex:9];
        [encoder setBuffer:cdf_buffer offset:0 atIndex:10];
        [encoder setBytes:&count length:sizeof(count) atIndex:11];
        [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(group, 1, 1)];
        [encoder endEncoding];
        diagnostic = metal::complete(command, "Image lighting probe");
        require(diagnostic.empty(), diagnostic);
        verify(inputs, {static_cast<probe::Result *>(output.contents), inputs.size()}, cdf, base,
               gamma);
    }
    stress_environment_cells(device, library, queue, transfer_buffer, spectra_buffer,
                             illuminant_buffer);
    std::cout << "Image lighting checked 32768 environment samples, 18 cell frequency gates and "
                 "32,768 transformed radiance and PDF queries\n";
}

} // namespace

int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            const auto device = MTLCreateSystemDefaultDevice();
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
            if (device == nil) {
                std::cout << "SKIP Image lighting GPU probe: no Metal device available\n";
                return 77;
            }
#endif
            require(device != nil, "Image lighting GPU probe requires a Metal device");
            run(device, argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{});
        } catch (const std::exception &error) {
            std::cerr << "Image lighting probe: " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
