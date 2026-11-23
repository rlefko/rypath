// SPDX-License-Identifier: GPL-3.0-only
// Independent query/export wrapper for unmodified PBRT v4 b0bdf461 binary32.
#include "../tests/layered_schedule_oracles.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <pbrt/bxdfs.h>
#include <pbrt/options.h>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using Q = rypath::layered_probe::Query;
constexpr const char *protocol = "RYPATH_LAYERED_SCHEDULE_1";

void validate(const Q &q) {
    const std::array<float, 20> values = {
        q.eta,         q.ax,        q.ay,     q.base_eta, q.base_k, q.base_ax, q.base_ay,
        q.reflectance, q.thickness, q.albedo, q.g,        q.ox,     q.oy,      q.oz,
        q.ix,          q.iy,        q.iz,     q.uc,       q.u,      q.v};
    for (float value : values)
        if (!std::isfinite(value))
            throw std::runtime_error("Nonfinite query input");
    const auto unit = [](float x, float y, float z) {
        const double length2 = double(x) * x + double(y) * y + double(z) * z;
        return std::abs(length2 - 1) <= 1e-4;
    };
    if (!(q.eta > 0) || q.ax < 0 || q.ay < 0 || q.base_eta < 0 || q.base_k < 0 || q.base_ax < 0 ||
        q.base_ay < 0 || q.reflectance < 0 || q.reflectance > 1 || q.thickness < 0 ||
        q.albedo < 0 || q.albedo > 1 || std::abs(q.g) > 1 || !unit(q.ox, q.oy, q.oz) ||
        !unit(q.ix, q.iy, q.iz) || q.oz == 0 || q.iz == 0 || q.uc < 0 || q.uc >= 1 || q.u < 0 ||
        q.u >= 1 || q.v < 0 || q.v >= 1 || q.conductor_base > 1 || q.max_depth > 1024 ||
        q.samples == 0 || q.samples > 1024 ||
        (q.operation != 0 && q.operation != 3 && q.operation != 4))
        throw std::runtime_error("Query outside supported finite-layered input domain");
}

unsigned read_unsigned(std::istream &input) {
    std::int64_t value = 0;
    if (!(input >> value) || value < 0 || value > std::numeric_limits<unsigned>::max())
        throw std::runtime_error("Invalid unsigned query field");
    return static_cast<unsigned>(value);
}

Q read_query(std::istream &input) {
    Q q{};
    if (!(input >> q.eta >> q.ax >> q.ay >> q.base_eta >> q.base_k >> q.base_ax >> q.base_ay >>
          q.reflectance >> q.thickness >> q.albedo >> q.g >> q.ox >> q.oy >> q.oz >> q.ix >> q.iy >>
          q.iz >> q.uc >> q.u >> q.v))
        throw std::runtime_error("Missing or malformed floating query field");
    q.conductor_base = read_unsigned(input);
    q.max_depth = read_unsigned(input);
    q.samples = read_unsigned(input);
    q.seed = read_unsigned(input);
    q.operation = read_unsigned(input);
    std::int64_t material_seed = 0;
    if (!(input >> material_seed) || material_seed < std::numeric_limits<std::int32_t>::min() ||
        material_seed > std::numeric_limits<std::int32_t>::max())
        throw std::runtime_error("Invalid signed32 material seed");
    q.material_seed = static_cast<int>(material_seed);
    validate(q);
    return q;
}

void write_query(const Q &q) {
    std::cout << q.eta << ' ' << q.ax << ' ' << q.ay << ' ' << q.base_eta << ' ' << q.base_k << ' '
              << q.base_ax << ' ' << q.base_ay << ' ' << q.reflectance << ' ' << q.thickness << ' '
              << q.albedo << ' ' << q.g << ' ' << q.ox << ' ' << q.oy << ' ' << q.oz << ' ' << q.ix
              << ' ' << q.iy << ' ' << q.iz << ' ' << q.uc << ' ' << q.u << ' ' << q.v << ' '
              << q.conductor_base << ' ' << q.max_depth << ' ' << q.samples << ' ' << q.seed << ' '
              << q.operation << ' ' << q.material_seed << '\n';
}

template <typename B> std::array<float, 8> evaluate(const B &bxdf, const Q &q) {
    using namespace pbrt;
    const Vector3f wo(q.ox, q.oy, q.oz), wi(q.ix, q.iy, q.iz);
    const auto mode = q.operation == 3 ? TransportMode::Importance : TransportMode::Radiance;
    std::array<float, 8> result{};
    result[0] = bxdf.f(wo, wi, mode)[0];
    result[1] = bxdf.PDF(wo, wi, mode, BxDFReflTransFlags::All);
    const auto sample = bxdf.Sample_f(wo, q.uc, {q.u, q.v}, mode, BxDFReflTransFlags::All);
    if (sample && sample->pdf > 0) {
        result[2] = sample->f[0] * std::abs(sample->wi.z) / sample->pdf;
        result[3] = sample->pdf;
        result[4] = sample->wi.x;
        result[5] = sample->wi.y;
        result[6] = sample->wi.z;
        result[7] = sample->pdfIsProportional ? 1.f : 0.f;
    }
    for (float value : result)
        if (!std::isfinite(value))
            throw std::runtime_error("Nonfinite upstream oracle result");
    return result;
}

std::array<float, 8> upstream(const Q &q) {
    using namespace pbrt;
    if (q.operation == 4)
        return evaluate(DiffuseBxDF(SampledSpectrum(q.reflectance)), q);
    const DielectricBxDF top(q.eta, TrowbridgeReitzDistribution(q.ax, q.ay));
    if (q.conductor_base) {
        const ConductorBxDF bottom(TrowbridgeReitzDistribution(q.base_ax, q.base_ay),
                                   SampledSpectrum(q.base_eta), SampledSpectrum(q.base_k));
        return evaluate(CoatedConductorBxDF(top, bottom, q.thickness, SampledSpectrum(q.albedo),
                                            q.g, static_cast<int>(q.max_depth),
                                            static_cast<int>(q.samples)),
                        q);
    }
    return evaluate(CoatedDiffuseBxDF(top, DiffuseBxDF(SampledSpectrum(q.reflectance)), q.thickness,
                                      SampledSpectrum(q.albedo), q.g, static_cast<int>(q.max_depth),
                                      static_cast<int>(q.samples)),
                    q);
}
} // namespace

int main(int argc, char **argv) {
    static_assert(std::is_same_v<pbrt::Float, float> && sizeof(int) == 4);
    try {
        std::vector<Q> queries;
        const bool fixtures = argc == 2 && std::string(argv[1]) == "--fixtures";
        const bool export_queries = argc == 2 && std::string(argv[1]) == "--queries";
        if (fixtures || export_queries) {
            for (const auto &fixture : layered_schedule_oracles::fixtures) {
                validate(fixture.query);
                queries.push_back(fixture.query);
            }
        } else if (argc == 1) {
            std::string version;
            if (!(std::cin >> version) || version != protocol)
                throw std::runtime_error("Expected RYPATH_LAYERED_SCHEDULE_1 input");
            const unsigned count = read_unsigned(std::cin);
            if (count == 0 || count > 65536)
                throw std::runtime_error("Expected 1..65536 queries");
            queries.reserve(count);
            for (unsigned n = 0; n < count; ++n)
                queries.push_back(read_query(std::cin));
            std::cin >> std::ws;
            if (std::cin.peek() != std::char_traits<char>::eof())
                throw std::runtime_error("Trailing query input");
        } else {
            throw std::runtime_error("Usage: pbrt_layered_schedule_probe [--fixtures|--queries]");
        }
        std::cout << std::setprecision(std::numeric_limits<float>::max_digits10);
        if (export_queries) {
            std::cout << protocol << '\n' << queries.size() << '\n';
            for (const auto &q : queries)
                write_query(q);
            return 0;
        }
        pbrt::PBRTOptions options;
        pbrt::Options = &options;
        // Only fixture.query is read; expected values never enter the oracle.
        std::vector<std::array<float, 8>> results;
        results.reserve(queries.size());
        for (const auto &q : queries) {
            options.seed = q.material_seed;
            results.push_back(upstream(q));
        }
        pbrt::Options = nullptr;
        for (const auto &result : results) {
            for (std::size_t n = 0; n < result.size(); ++n)
                std::cout << (n == 0 ? "" : " ") << result[n];
            std::cout << '\n';
        }
    } catch (const std::exception &error) {
        pbrt::Options = nullptr;
        std::cerr << "Layered schedule oracle: " << error.what() << '\n';
        return 2;
    }
}
