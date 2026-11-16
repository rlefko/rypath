// SPDX-License-Identifier: GPL-3.0-only
// Oracle adapter: links the unmodified pinned PBRT reference library.
#include <cstdint>
#include <cstring>
#include <iostream>
#include <pbrt/options.h>
#include <pbrt/pbrt.h>
#include <pbrt/samplers.h>
std::uint32_t bits(float value) {
    std::uint32_t result;
    std::memcpy(&result, &value, 4);
    return result;
}
int main() {
    pbrt::PBRTOptions options;
    options.nThreads = 1;
    pbrt::InitPBRT(options);
    int kind, w, h, x, y, sample, dimension, mode, seed, spp;
    while (std::cin >> kind >> w >> h >> x >> y >> sample >> dimension >> mode >> seed >> spp) {
        auto strategy = static_cast<pbrt::RandomizeStrategy>(mode);
        if (kind == 2) {
            pbrt::HaltonSampler sampler(spp, {w, h}, strategy, seed);
            sampler.StartPixelSample({x, y}, sample, dimension);
            auto camera = sampler.GetPixel2D();
            sampler.StartPixelSample({x, y}, sample, dimension);
            auto scalar = sampler.Get1D();
            sampler.StartPixelSample({x, y}, sample, dimension);
            auto pair = sampler.Get2D();
            std::cout << 0 << ' ' << bits(camera.x) << ' ' << bits(camera.y) << ' ' << bits(scalar)
                      << ' ' << bits(pair.x) << ' ' << bits(pair.y) << '\n';
        } else {
            pbrt::ZSobolSampler sampler(spp, {w, h}, strategy, seed);
            sampler.StartPixelSample({x, y}, sample, dimension);
            auto index = sampler.GetSampleIndex();
            auto scalar = sampler.Get1D();
            sampler.StartPixelSample({x, y}, sample, dimension);
            auto pair = sampler.Get2D();
            sampler.StartPixelSample({x, y}, sample, 1);
            auto camera = sampler.GetPixel2D();
            std::cout << index << ' ' << bits(camera.x) << ' ' << bits(camera.y) << ' '
                      << bits(scalar) << ' ' << bits(pair.x) << ' ' << bits(pair.y) << '\n';
        }
    }
    pbrt::CleanupPBRT();
}
