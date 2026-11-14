// SPDX-License-Identifier: GPL-3.0-only
// Standalone CPU probe. Build and link against the pinned PBRT, outside rypath.
#include <pbrt/options.h>
#include <pbrt/util/mipmap.h>

#include <cstdint>
#include <cstdio>
#include <string_view>
#include <type_traits>

namespace {

bool emit_image(const pbrt::Image &image, bool u256) {
    const int channels = image.NChannels();
    const auto format = u256 ? pbrt::PixelFormat::U256 : pbrt::PixelFormat::Half;
    if (image.Format() != format || (channels != 1 && channels < 3)) {
        std::fputs(u256 ? "Expected U256 RGB or grayscale storage\n"
                        : "Expected binary16 RGB or grayscale mip storage\n",
                   stderr);
        return false;
    }
    const auto resolution = image.Resolution();
    std::printf("%d %d\n", resolution.x, resolution.y);
    const int g = channels == 1 ? 0 : 1;
    const int b = channels == 1 ? 0 : 2;
    for (int y = 0; y < resolution.y; ++y) {
        for (int x = 0; x < resolution.x; ++x) {
            const auto *pixel = image.RawPointer({x, y});
            if (u256) {
                const auto *codes = static_cast<const std::uint8_t *>(pixel);
                std::printf("%s%04x %04x %04x", x ? " " : "", unsigned(codes[0]),
                            unsigned(codes[g]), unsigned(codes[b]));
            } else {
                const auto *codes = static_cast<const pbrt::Half *>(pixel);
                std::printf("%s%04x %04x %04x", x ? " " : "", unsigned(codes[0].Bits()),
                            unsigned(codes[g].Bits()), unsigned(codes[b].Bits()));
            }
        }
        std::printf("\n");
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    static_assert(std::is_same_v<pbrt::Float, float>);
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "Expected ENCODING WRAP INPUT.png [u256|source-u256]\n");
        return 1;
    }
    const std::string_view mode = argc == 5 ? argv[4] : "";
    if (argc == 5 && mode != "u256" && mode != "source-u256") {
        std::fprintf(stderr, "Expected u256 or source-u256 probe mode\n");
        return 1;
    }
    const std::string_view encoding(argv[1]);
    const std::string_view wrap(argv[2]);
    if ((encoding != "linear" && encoding != "sRGB") || (wrap != "repeat" && wrap != "clamp")) {
        std::fprintf(stderr, "Expected linear or sRGB encoding and repeat or clamp wrap\n");
        return 1;
    }

    pbrt::PBRTOptions options;
    options.nThreads = 2;
    pbrt::InitPBRT(options);
    pbrt::Allocator allocator;
    auto color_encoding = pbrt::ColorEncoding::Get(argv[1], allocator);
    bool success = true;
    if (mode == "source-u256") {
        const auto source = pbrt::Image::Read(argv[3], allocator, color_encoding);
        success = emit_image(source.image, true);
    } else {
        auto *mipmap = pbrt::MIPMap::CreateFromFile(
            argv[3], {}, wrap == "repeat" ? pbrt::WrapMode::Repeat : pbrt::WrapMode::Clamp,
            color_encoding, allocator);
        for (int level = 0; success && level < mipmap->Levels(); ++level) {
            success = emit_image(mipmap->GetLevel(level), mode == "u256");
        }
        allocator.delete_object(mipmap);
    }
    pbrt::CleanupPBRT();
    return success && !std::ferror(stdout) ? 0 : 1;
}
