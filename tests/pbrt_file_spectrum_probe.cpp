// SPDX-License-Identifier: GPL-3.0-only

#include "pbrt_render.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Expected one or more PBRT scene files\n";
        return 1;
    }
    using namespace rypath;
    std::map<std::string, std::shared_ptr<const pbrt::CheckedSpectrumSource>> sources;
    const auto collect = [&](const pbrt::Descriptor &descriptor) {
        if (const auto &source = descriptor.emission_file) {
            sources.try_emplace(source->resolved_path.filename().string(), source);
        }
    };
    for (int i = 1; i < argc; ++i) {
        const auto checked = pbrt::check(argv[i]);
        if (checked.exit_code() != 0 || !checked.scene) {
            std::cerr << pbrt::report(checked);
            return 1;
        }
        for (const auto &light : checked.scene->lights) {
            collect(light);
        }
        for (const auto &light : checked.scene->area_lights) {
            collect(*light);
        }
    }
    std::cout << std::setprecision(std::numeric_limits<double>::max_digits10);
    for (const auto &[name, source] : sources) {
        const auto &knots = source->knots;
        if (knots.empty()) {
            std::cerr << name << ": checked source has no knots\n";
            return 1;
        }
        const auto peak = std::ranges::max_element(knots, {}, &SpectralKnot::value)->value;
        std::cout << name << ' ' << peak;
        for (int wavelength = 360; wavelength <= 830; ++wavelength) {
            std::cout << ' ' << static_cast<float>(sampled_spectrum(knots, wavelength));
        }
        std::cout << '\n';
    }
    return std::cout ? 0 : 1;
}
