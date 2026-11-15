// SPDX-License-Identifier: GPL-3.0-only
// Standalone oracle for the unmodified pinned PBRT v4 library.
#include <iomanip>
#include <iostream>
#include <pbrt/bxdfs.h>
#include <pbrt/options.h>
int main(int argc, char **argv) {
    using namespace pbrt;
    if (argc != 2)
        return 1;
    PBRTOptions options;
    Options = &options;
    Allocator alloc;
    auto data = MeasuredBxDF::BRDFDataFromFile(argv[1], alloc);
    if (!data)
        return 1;
    SampledWavelengths lambda;
    for (int i = 0; i < NSpectrumSamples; ++i)
        lambda[i] = 550;

    std::cout << std::setprecision(10);
    for (unsigned id = 0; id < 31 * 31; ++id) {
        for (int i = 0; i < NSpectrumSamples; ++i)
            lambda[i] = 400.f + 75.f * float(id % 5);
        MeasuredBxDF bsdf(data, lambda);
        auto wo = Normalize(Vector3f(.2f, .3f, id % 2 == 0 ? 1.f : -1.f));
        auto s = bsdf.Sample_f(wo, .5f, {(float(id % 31) + .5f) / 31, (float(id / 31) + .5f) / 31},
                               TransportMode::Radiance, BxDFReflTransFlags::All);
        if (s)
            std::cout << 1 << ' ' << s->wi.x << ' ' << s->wi.y << ' ' << s->wi.z << ' ' << s->f[0]
                      << ' ' << s->pdf << '\n';
        else
            std::cout << "0 0 0 0 0 0\n";
    }
    Options = nullptr;
}
