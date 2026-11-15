// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
namespace table_fixture {
inline void u(std::vector<unsigned char> &b, std::uint64_t v, unsigned n = 4) {
    for (unsigned i = 0; i < n; ++i)
        b.push_back(static_cast<unsigned char>(v >> (8 * i)));
}
inline void f(std::vector<unsigned char> &b, float x) { u(b, std::bit_cast<std::uint32_t>(x)); }
inline void write(const std::filesystem::path &p, const std::vector<unsigned char> &b) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
}
// Unit achromatic constant Fourier expansion, both reflection and transmission.
inline std::vector<unsigned char> fourier(unsigned channels = 1, bool harmonic = false,
                                          float amplitude = 1, bool varying_zenith = false) {
    std::vector<unsigned char> b;
    for (char c : std::string("SCATFUN"))
        b.push_back(c);
    b.push_back(1);
    for (auto v : {1u, 3u, 9u * channels * (harmonic ? 2u : 1u), harmonic ? 2u : 1u, channels, 1u,
                   0u, 0u, 0u})
        u(b, v);
    f(b, 1.5f);
    for (int i = 0; i < 4; ++i)
        u(b, 0);
    f(b, -1);
    f(b, 0);
    f(b, 1);
    for (int i = 0; i < 3; ++i) {
        f(b, 0);
        f(b, amplitude * (varying_zenith ? 2.75f : 1.f));
        f(b, amplitude * (varying_zenith ? 5.5f : 2.f));
    }
    for (unsigned i = 0; i < 9; ++i) {
        u(b, i * channels * (harmonic ? 2u : 1u));
        u(b, harmonic ? 2 : 1);
    }
    for (unsigned i = 0; i < 9; ++i) {
        float scale = amplitude * (varying_zenith && i % 3 == 1 ? 4.f : 1.f);
        f(b, scale);
        if (harmonic)
            f(b, .2f * scale);
        if (channels == 3) {
            f(b, .8f * scale);
            if (harmonic)
                f(b, .32f * scale);
            f(b, .6f * scale);
            if (harmonic)
                f(b, -.18f * scale);
        }
    }
    return b;
}
inline std::vector<unsigned char> measured(unsigned np = 1, bool plateau = false,
                                           unsigned normal_resolution = 0) {
    struct F {
        std::string name;
        unsigned type;
        std::vector<unsigned> dims;
        std::vector<unsigned char> data;
    };
    std::vector<F> fields;
    auto add = [&](std::string name, std::vector<unsigned> dims, std::vector<float> values) {
        F v{std::move(name), 10, std::move(dims), {}};
        for (float x : values)
            f(v.data, x);
        fields.push_back(std::move(v));
    };
    std::vector<float> phi(np);
    for (unsigned i = 0; i < np; ++i)
        phi[i] = np == 1 ? 0 : -3.141592653589793f + 6.283185307179586f * float(i) / float(np - 1);
    add("phi_i", {np}, phi);
    add("theta_i", {2}, {0, 1.5707963267948966f});
    add("wavelengths", {2}, {400, 700});
    add("ndf", {2, 2}, {1, 1, 1, 1});
    add("sigma", {2, 2}, {1, 1, 1, 1});
    const unsigned resolution = plateau ? 4 : 2;
    std::vector<float> density(np * 2 * resolution * resolution, 1);
    if (plateau)
        for (unsigned i = 0; i < density.size(); ++i) {
            unsigned x = i % resolution, y = (i / resolution) % resolution;
            density[i] = (x == 0 || x == 3) && (y == 0 || y == 3) ? 1.f : 0.f;
        }
    if (normal_resolution)
        add("vndf", {np, 2, normal_resolution, normal_resolution},
            std::vector<float>(np * 2 * normal_resolution * normal_resolution, 1));
    else
        add("vndf", {np, 2, resolution, resolution}, density);
    add("luminance", {np, 2, resolution, resolution}, density);
    std::vector<float> spectra(np * 4 * resolution * resolution);
    for (unsigned p = 0; p < np; ++p)
        for (unsigned t = 0; t < 2; ++t)
            for (unsigned l = 0; l < 2; ++l)
                for (unsigned v = 0; v < resolution * resolution; ++v)
                    spectra[((p * 2 + t) * 2 + l) * resolution * resolution + v] = l ? 2.f : .5f;
    add("spectra", {np, 2, 2, resolution, resolution}, spectra);
    fields.push_back({"description", 1, {1}, {0}});
    fields.push_back({"jacobian", 1, {1}, {0}});
    std::size_t off = 18;
    for (auto &v : fields)
        off += 2 + v.name.size() + 2 + 1 + 8 + 8 * v.dims.size();
    std::vector<unsigned char> b;
    for (char c : std::string("tensor_file", 12))
        b.push_back(c);
    u(b, 1, 2);
    u(b, fields.size());
    for (auto &v : fields) {
        u(b, v.name.size(), 2);
        for (char c : v.name)
            b.push_back(c);
        u(b, v.dims.size(), 2);
        u(b, v.type, 1);
        u(b, off, 8);
        for (unsigned d : v.dims)
            u(b, d, 8);
        off += v.data.size();
    }
    for (auto &v : fields)
        b.insert(b.end(), v.data.begin(), v.data.end());
    return b;
}
} // namespace table_fixture
