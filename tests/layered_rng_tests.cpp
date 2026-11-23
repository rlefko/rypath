// SPDX-License-Identifier: GPL-3.0-only
#include "layered_rng_oracles.h"
#include "layered_rng_shared.h"
#include <bit>
#include <iostream>
#include <stdexcept>
void test_layered_rng() {
    using namespace rypath_layered_rng;
    for (const auto &q : layered_rng_oracles) {
        const Wide h[] = {seeded_direction(q.seed, .25f, -.5f, .75f),
                          direction(-.125f, .625f, -0.f),
                          seeded_direction(q.seed, -.125f, .625f, -0.f),
                          direction(.25f, -.5f, .75f), sample(.375f, .125f, .875f)};
        for (unsigned i = 0; i < 5; ++i)
            if (h[i] != q.hashes[i])
                throw std::runtime_error("PBRT typed layered hash mismatch");
        for (unsigned domain = 0; domain < 3; ++domain) {
            PCG rng(domain == 1 ? h[2] : h[0], domain == 0 ? h[1] : domain == 1 ? h[3] : h[4]);
            for (unsigned n = 0; n < 4; ++n)
                if (std::bit_cast<Word>(rng.next()) != q.draws[4 * domain + n])
                    throw std::runtime_error("PBRT layered PCG float mismatch");
        }
    }
    PCG stepped(17, 29), jumped(17, 29);
    for (unsigned i = 0; i < 65537; ++i)
        stepped.next_word();
    jumped.advance(65537);
    if (stepped.next_word() != jumped.next_word())
        throw std::runtime_error("PCG skip mismatch");
    if (direction(0.f, 1.f, 0.f) == direction(-0.f, 1.f, 0.f))
        throw std::runtime_error("Typed hashes must preserve signed zero");
    std::cout << "4 signed seeds, 20 typed hash and 48 PCG float PBRT oracles passed\n";
}
int main() {
    try {
        test_layered_rng();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
