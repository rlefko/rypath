// SPDX-License-Identifier: GPL-3.0-only
// Standalone CPU probe. Build and link against the pinned PBRT, outside rypath.
#include <pbrt/options.h>
#include <pbrt/util/mipmap.h>

#include <cstdio>
#include <string_view>
#include <type_traits>

int main(int argc, char **argv) {
    static_assert(std::is_same_v<pbrt::Float, float>);
    if (argc != 4) {
        std::fprintf(stderr, "Expected ENCODING WRAP INPUT.png\n");
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
    auto *mipmap = pbrt::MIPMap::CreateFromFile(
        argv[3], {}, wrap == "repeat" ? pbrt::WrapMode::Repeat : pbrt::WrapMode::Clamp,
        pbrt::ColorEncoding::Get(argv[1], allocator), allocator);
    for (int level = 0; level < mipmap->Levels(); ++level) {
        const auto &image = mipmap->GetLevel(level);
        const int channels = image.NChannels();
        if (image.Format() != pbrt::PixelFormat::Half || (channels != 1 && channels < 3)) {
            std::fprintf(stderr, "Expected binary16 RGB or grayscale mip storage\n");
            return 1;
        }
        const auto resolution = image.Resolution();
        std::printf("%d %d\n", resolution.x, resolution.y);
        for (int y = 0; y < resolution.y; ++y) {
            for (int x = 0; x < resolution.x; ++x) {
                const auto *pixel = static_cast<const pbrt::Half *>(image.RawPointer({x, y}));
                std::printf("%s%04x %04x %04x", x ? " " : "", unsigned(pixel[0].Bits()),
                            unsigned(pixel[channels == 1 ? 0 : 1].Bits()),
                            unsigned(pixel[channels == 1 ? 0 : 2].Bits()));
            }
            std::printf("\n");
        }
    }
    allocator.delete_object(mipmap);
    pbrt::CleanupPBRT();
    return std::ferror(stdout) ? 1 : 0;
}
