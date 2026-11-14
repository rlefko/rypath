// SPDX-License-Identifier: GPL-3.0-only

#include "pbrt_render.hpp"
#include "reflectance_curve.hpp"

#include <bit>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <set>
#include <string_view>

namespace {

using namespace rypath;

constexpr auto first_visible = std::bit_cast<std::uint32_t>(360.f);
constexpr auto last_visible = std::bit_cast<std::uint32_t>(830.f);

void write_word(float value) {
    std::cout << ' ' << std::hex << std::setfill('0') << std::setw(8)
              << std::bit_cast<std::uint32_t>(value) << std::dec;
}

void write_word(double value) {
    std::cout << ' ' << std::hex << std::setfill('0') << std::setw(16)
              << std::bit_cast<std::uint64_t>(value) << std::dec;
}

bool read_word(float &value) {
    std::uint32_t bits = 0;
    if (!(std::cin >> std::hex >> bits >> std::dec)) {
        return false;
    }
    value = std::bit_cast<float>(bits);
    return true;
}

bool dump_curve(const std::string &name) {
    std::size_t knot_count = 0;
    std::size_t sample_count = 0;
    if (!(std::cin >> knot_count >> sample_count) || knot_count < 2 || knot_count > 1024 ||
        sample_count > 65536) {
        return false;
    }
    std::vector<SpectralKnot> source(knot_count);
    for (auto &[wavelength, value] : source) {
        if (!read_word(wavelength) || !read_word(value)) {
            return false;
        }
    }
    std::set<std::uint32_t> queries;
    for (std::size_t i = 0; i < sample_count; ++i) {
        float wavelength = 0;
        if (!read_word(wavelength) || !(wavelength >= 360 && wavelength <= 830)) {
            return false;
        }
        queries.insert(std::bit_cast<std::uint32_t>(wavelength));
    }
    const auto result = compile_reflectance_curve(source, 5 * knot_count);
    if (result.error != ReflectanceCurveError::none) {
        std::cerr << name << ": curve compilation failed\n";
        return false;
    }
    for (const auto &knot : result.knots) {
        const auto center = std::bit_cast<std::uint32_t>(knot.wavelength);
        if (center >= first_visible && center <= last_visible) {
            for (auto bits = center - 1; bits <= center + 1; ++bits) {
                if (bits >= first_visible && bits <= last_visible) {
                    queries.insert(bits);
                }
            }
        }
    }
    std::cout << name << ' ' << result.knots.size() << ' ' << queries.size();
    for (const auto &[wavelength, value] : result.knots) {
        write_word(wavelength);
        write_word(value);
    }
    for (const auto word : queries) {
        const float wavelength = std::bit_cast<float>(word);
        write_word(wavelength);
        write_word(static_cast<float>(sampled_spectrum(result.knots, wavelength)));
    }
    std::cout << '\n';
    return static_cast<bool>(std::cout);
}

bool dump_curves() {
    std::string name;
    while (std::cin >> name) {
        if (!dump_curve(name)) {
            std::cerr << "Invalid curve probe request or result for " << name << '\n';
            return false;
        }
    }
    return std::cin.eof() && !std::cin.bad();
}

bool dump_scene(const std::filesystem::path &entry, bool double_precision) {
    std::vector<double> wavelengths;
    if (double_precision) {
        std::size_t count = 0;
        if (!(std::cin >> count) || count == 0 || count > 65536) {
            std::cerr << "Invalid scene query count\n";
            return false;
        }
        wavelengths.resize(count);
        for (double &wavelength : wavelengths) {
            if (!(std::cin >> wavelength) || !(wavelength >= 360 && wavelength <= 830)) {
                std::cerr << "Expected a scene query wavelength in [360,830] nm\n";
                return false;
            }
        }
    }
    const auto checked = pbrt::check(entry);
    if (checked.exit_code() != 0) {
        std::cerr << pbrt::report(checked);
        return false;
    }
    const auto translated = pbrt::translate(checked, {entry.string()});
    if (translated.exit_code() != 0 || !translated.scene) {
        for (const auto &diagnostic : translated.diagnostics) {
            std::cerr << diagnostic.message << '\n';
        }
        return false;
    }
    for (const auto index : translated.scene->instance_materials) {
        std::string label;
        if (!(std::cin >> label)) {
            std::cerr << "Expected one label per translated instance\n";
            return false;
        }
        const auto &material = translated.scene->materials.at(index);
        std::cout << label;
        if (double_precision) {
            for (const auto wavelength : wavelengths) {
                write_word(spectral_reflectance(material, wavelength));
            }
        } else {
            for (int quarter_nm = 360 * 4; quarter_nm <= 830 * 4; ++quarter_nm) {
                const auto wavelength = static_cast<float>(quarter_nm) * 0.25f;
                write_word(static_cast<float>(spectral_reflectance(material, wavelength)));
            }
        }
        std::cout << '\n';
    }
    std::string extra;
    return !(std::cin >> extra) && std::cin.eof() && !std::cin.bad() && std::cout;
}

} // namespace

int main(int argc, char **argv) try {
    if (argc == 1) {
        return dump_curves() ? 0 : 1;
    }
    if (argc == 3 && std::string_view{argv[1]} == "--scene") {
        return dump_scene(argv[2], false) ? 0 : 1;
    }
    if (argc == 3 && std::string_view{argv[1]} == "--scene-double") {
        return dump_scene(argv[2], true) ? 0 : 1;
    }
    std::cerr << "Usage: reflectance_curve_probe [--scene FILE | --scene-double FILE]\n";
    return 1;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
