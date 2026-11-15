// SPDX-License-Identifier: GPL-3.0-only
// Three separately linked oracle/probe binaries; no installed SDK product dependency.
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(HDR_IO_RYPATH)
#include "image_output.hpp"
int run(int argc, char **argv) {
    using namespace rypath;
    if (argc == 3 && std::string(argv[1]) == "film") {
        for (const auto model : {RenderModel::demo, RenderModel::pbrt}) {
            auto film = make_film(2, 2, model, model == RenderModel::pbrt ? 2 : 1);
            film.pixels = {{{2, 0, 0}, {.5, 0, 0}, 2},
                           {{0, 2, 0}, {}, 2},
                           {{0, 0, 2}, {}, 2},
                           {{0, 0, 0}, {}, 2}};
            const auto original = film;
            for (const auto precision : {ExrPrecision::half, ExrPrecision::float32}) {
                auto name = std::string(model == RenderModel::demo ? "demo-" : "pbrt-") +
                            (precision == ExrPrecision::half ? "half.exr" : "float.exr");
                const std::pair<std::string, std::string> metadata{"rypath.oracle",
                                                                   "projection-v1"};
                auto output_film = film;
                if (precision == ExrPrecision::float32) {
                    output_film.pixels[0].sum[0] *= 100000;
                    output_film.pixels[0].correction[0] *= 100000;
                }
                auto error = write_exr(output_film, std::filesystem::path(argv[2]) / name,
                                       precision, std::span{&metadata, 1});
                if (!error.empty())
                    throw std::runtime_error(error);
            }
            if (film != original)
                throw std::runtime_error("film mutated");
        }
        return 0;
    }
    if (argc != 4)
        return 1;
    std::ifstream file(argv[3], std::ios::binary);
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{file}, {}};
    auto decoded = std::filesystem::path(argv[3]).extension() == ".hdr" ? decode_hdr(bytes)
                                                                        : decode_exr(bytes);
    if (!decoded.value)
        throw std::runtime_error(decoded.diagnostic);
    if (std::string(argv[1]) == "source") {
        std::printf("%u %u\n", decoded.value->width, decoded.value->height);
        for (float v : decoded.value->floating_rgb)
            std::printf("%.9g\n", v);
    } else {
        const auto wrap = std::string(argv[2]) == "repeat" ? ImageWrap::repeat : ImageWrap::clamp;
        auto pyramid = build_image_pyramid(*decoded.value, ImageEncoding::linear, wrap);
        if (!pyramid.value)
            throw std::runtime_error(pyramid.diagnostic);
        for (const auto &level : pyramid.value->levels) {
            auto view = *pyramid.value;
            view.levels = {level};
            view.native_base = true;
            std::printf("%u %u\n", level.width, level.height);
            for (std::uint32_t y = 0; y < level.height; ++y)
                for (std::uint32_t x = 0; x < level.width; ++x) {
                    const auto pixel = image_base_texel_linear_rgb(view, x, y);
                    for (float v : *pixel)
                        std::printf("%.9g\n", v);
                }
        }
    }
    return 0;
}
#elif defined(HDR_IO_PBRT)
#include <pbrt/options.h>
#include <pbrt/util/mipmap.h>
int run(int argc, char **argv) {
    if (argc != 4)
        return 1;
    pbrt::PBRTOptions options;
    options.nThreads = 2;
    pbrt::InitPBRT(options);
    pbrt::Allocator allocator;
    const auto emit = [](const pbrt::Image &image) {
        const auto size = image.Resolution();
        std::printf("%d %d\n", size.x, size.y);
        for (int y = 0; y < size.y; ++y)
            for (int x = 0; x < size.x; ++x)
                for (int c = 0; c < 3; ++c)
                    std::printf("%.9g\n", image.GetChannel({x, y}, c));
    };
    if (std::string(argv[1]) == "source") {
        const auto image = pbrt::Image::Read(argv[3], allocator).image;
        emit(image.SelectChannels(image.GetChannelDesc({"R", "G", "B"}), allocator));
    } else {
        auto *mip = pbrt::MIPMap::CreateFromFile(
            argv[3], {},
            std::string(argv[2]) == "repeat" ? pbrt::WrapMode::Repeat : pbrt::WrapMode::Clamp,
            pbrt::ColorEncoding::Linear, allocator);
        if (!mip)
            throw std::runtime_error("PBRT mip failed");
        for (int i = 0; i < mip->Levels(); ++i)
            emit(mip->GetLevel(i));
        allocator.delete_object(mip);
    }
    pbrt::CleanupPBRT();
    return 0;
}
#else
#include <Imath/half.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfStandardAttributes.h>
#include <OpenEXR/ImfStringAttribute.h>
int run(int argc, char **argv) {
    using namespace OPENEXR_IMF_NAMESPACE;
    if (argc != 3)
        return 1;
    if (std::string(argv[1]) == "generate") {
        for (const bool half : {true, false}) {
            const int width = half ? 2 : 3, height = half ? 2 : 3;
            std::vector<float> rgb;
            for (int i = 0; i < width * height; ++i) {
                if (half) {
                    rgb.push_back(i == 0 ? -.25f : .25f * i);
                    rgb.push_back(2.f + i);
                    rgb.push_back(.125f * i);
                } else {
                    rgb.push_back(100000.f + 10000.f * i);
                    rgb.push_back(80000.f + 20000.f * i);
                    rgb.push_back(120000.f - 5000.f * i);
                }
            }
            Header header(width, height);
            for (const char *c : {"R", "G", "B"})
                header.channels().insert(c, Channel(half ? HALF : FLOAT));
            addChromaticities(header, Chromaticities());
            const auto path =
                std::filesystem::path(argv[2]) / (half ? "signed-half.exr" : "bright-float.exr");
            OutputFile output(path.c_str(), header);
            FrameBuffer buffer;
            std::vector<IMATH_NAMESPACE::half> halves(rgb.begin(), rgb.end());
            for (int c = 0; c < 3; ++c)
                buffer.insert(
                    std::string(1, "RGB"[c]),
                    Slice(half ? HALF : FLOAT,
                          half ? reinterpret_cast<char *>(halves.data() + c)
                               : reinterpret_cast<char *>(rgb.data() + c),
                          3 * (half ? sizeof(IMATH_NAMESPACE::half) : sizeof(float)),
                          width * 3 * (half ? sizeof(IMATH_NAMESPACE::half) : sizeof(float))));
            output.setFrameBuffer(buffer);
            output.writePixels(height);
        }
        return 0;
    }
    InputFile input(argv[2]);
    const auto &header = input.header();
    if (header.compression() != ZIP_COMPRESSION)
        throw std::runtime_error("output compression is not ZIP");
    const auto *attribute = header.findTypedAttribute<StringAttribute>("rypath.oracle");
    if (!attribute || attribute->value() != "projection-v1")
        throw std::runtime_error("output metadata missing");
    for (const char *name : {"R", "G", "B"})
        if (!header.channels().findChannel(name) ||
            header.channels().findChannel(name)->type != header.channels().findChannel("R")->type)
            throw std::runtime_error("output channel types differ");
    const auto window = input.header().dataWindow();
    if (window.min.x || window.min.y)
        throw std::runtime_error("oracle expects zero origin");
    const int width = window.max.x + 1, height = window.max.y + 1;
    std::vector<float> rgb(width * height * 3);
    FrameBuffer buffer;
    for (int c = 0; c < 3; ++c)
        buffer.insert(std::string(1, "RGB"[c]),
                      Slice(FLOAT, reinterpret_cast<char *>(rgb.data() + c), 3 * sizeof(float),
                            width * 3 * sizeof(float)));
    input.setFrameBuffer(buffer);
    input.readPixels(0, height - 1);
    std::printf("%d %d %s\n", width, height,
                input.header().channels().findChannel("R")->type == HALF ? "HALF" : "FLOAT");
    const auto chroma = chromaticities(input.header());
    std::printf("chroma %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", chroma.red.x, chroma.red.y,
                chroma.green.x, chroma.green.y, chroma.blue.x, chroma.blue.y, chroma.white.x,
                chroma.white.y);
    for (float v : rgb)
        std::printf("%.9g\n", v);
    return 0;
}
#endif
int main(int argc, char **argv) try { return run(argc, argv); } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
}
