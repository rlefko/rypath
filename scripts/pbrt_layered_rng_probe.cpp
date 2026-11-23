// SPDX-License-Identifier: GPL-3.0-only
// Standalone oracle compiled against unmodified pinned PBRT b0bdf461 headers.
#include <cstring>
#include <iostream>
#include <pbrt/util/hash.h>
#include <pbrt/util/rng.h>
#include <pbrt/util/vecmath.h>
int main() {
    for (int seed : {0, -1, -2147483647 - 1, 2147483647}) {
        pbrt::Vector3f wo(.25f, -.5f, .75f), wi(-.125f, .625f, -0.f);
        auto a = pbrt::Hash(seed, wo), b = pbrt::Hash(wi), c = pbrt::Hash(seed, wi),
             d = pbrt::Hash(wo), e = pbrt::Hash(.375f, pbrt::Point2f(.125f, .875f));
        std::cout << seed << " " << a << " " << b << " " << c << " " << d << " " << e;
        for (auto pair : {std::pair{a, b}, std::pair{c, d}, std::pair{a, e}}) {
            pbrt::RNG rng(pair.first, pair.second);
            for (int i = 0; i < 4; ++i) {
                float f = rng.Uniform<float>();
                unsigned u;
                memcpy(&u, &f, 4);
                std::cout << " " << u;
            }
        }
        std::cout << "\n";
    }
}
