// SPDX-License-Identifier: GPL-3.0-only
// Independent weighted-moment oracle linked to unmodified PBRT b0bdf461.
#include <iomanip>
#include <iostream>
#include <pbrt/bxdfs.h>
#include <pbrt/options.h>
int main() {
    using namespace pbrt;
    PBRTOptions options;
    Options = &options;
    constexpr int count = 131072;
    std::cout << std::setprecision(12);
    Vector3f wo = Normalize(Vector3f(.2f, .1f, 1.f));
    for (int configuration = 0; configuration < 3; ++configuration)
        for (int conductor = 0; conductor < 2; ++conductor)
            for (int importance = 0; importance < 2; ++importance) {
                int depth = configuration == 1 ? 4 : configuration == 2 ? 24 : 12;
                int samples = configuration == 1 ? 1 : configuration == 2 ? 16 : 4;
                wo = configuration == 2 ? Normalize(Vector3f(.8f, .3f, .05f))
                                        : Normalize(Vector3f(.2f, .1f, 1.f));
                if (configuration == 1)
                    wo = -wo;
                auto mode = importance ? TransportMode::Importance : TransportMode::Radiance;
                auto run = [&](auto &bxdf) {
                    RNG outer(17, 29);
                    double sum[5] = {}, square[5] = {};
                    for (int n = 0; n < count; ++n) {
                        float uc = outer.Uniform<float>();
                        float x = outer.Uniform<float>(), y = outer.Uniform<float>();
                        float z = outer.Uniform<float>(), phi = 2 * Pi * outer.Uniform<float>();
                        Vector3f wi(std::sqrt(std::max(0.f, 1 - z * z)) * std::cos(phi),
                                    std::sqrt(std::max(0.f, 1 - z * z)) * std::sin(phi), z);
                        if (configuration == 1)
                            wi = -wi;
                        auto environment = [](Vector3f w) {
                            return 1 + .25f * w.x + .5f * w.z * w.z;
                        };
                        auto sample = bxdf.Sample_f(wo, uc, Point2f(x, y), mode);
                        double weight =
                            sample ? sample->f[0] * std::abs(sample->wi.z) / sample->pdf : 0;
                        double f = bxdf.f(wo, wi, mode)[0] * std::abs(wi.z) * (2 * Pi);
                        double value[5] = {weight, sample ? weight * environment(sample->wi) : 0, f,
                                           f * environment(wi), bxdf.PDF(wo, wi, mode) * (2 * Pi)};
                        for (int i = 0; i < 5; ++i) {
                            sum[i] += value[i];
                            square[i] += value[i] * value[i];
                        }
                    }
                    std::cout << configuration << ' ' << conductor << ' ' << importance;
                    for (int i = 0; i < 5; ++i)
                        std::cout << ' ' << sum[i] / count << ' ' << square[i] / count;
                    std::cout << '\n';
                };
                DielectricBxDF top(1.5f, TrowbridgeReitzDistribution(.2f, .3f));
                if (conductor) {
                    CoatedConductorBxDF b(top,
                                          ConductorBxDF(TrowbridgeReitzDistribution(.25f, .15f),
                                                        SampledSpectrum(.3f), SampledSpectrum(3.f)),
                                          .2f, SampledSpectrum(.6f), .1f, depth, samples);
                    run(b);
                } else {
                    CoatedDiffuseBxDF b(top, DiffuseBxDF(SampledSpectrum(.7f)), .2f,
                                        SampledSpectrum(.6f), .1f, depth, samples);
                    run(b);
                }
            }
    Options = nullptr;
}
