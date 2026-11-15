// SPDX-License-Identifier: GPL-3.0-only
#include "bsdf_table.hpp"
#include "bsdf_table_fixture.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace rypath;
static void require(bool b, const char *s) {
    if (!b)
        throw std::runtime_error(s);
}
int main() {
    try {
        auto path = std::filesystem::temp_directory_path() / "rypath-bsdf-table-tests.binary";
        for (auto kind : {BsdfTableKind::fourier, BsdfTableKind::measured}) {
            auto bytes = kind == BsdfTableKind::fourier ? table_fixture::fourier()
                                                        : table_fixture::measured();
            table_fixture::write(path, bytes);
            auto good = read_bsdf_table(path, kind);
            require(bool(good.table), good.diagnostic.c_str());
            require(validate_bsdf_table(*good.table).empty(), "packed validator");
            auto bad = *good.table;
            bad.words[0] = 99;
            require(!validate_bsdf_table(bad).empty(), "packed kind mutation");
            bad = *good.table;
            bad.words[5] = 0xffffffff;
            require(!validate_bsdf_table(bad).empty(), "packed offset mutation");
            bad = *good.table;
            bad.words[1] = 0xffffffff;
            require(!validate_bsdf_table(bad).empty(), "packed count mutation");
            bad = *good.table;
            bad.words.push_back(0);
            require(!validate_bsdf_table(bad).empty(), "packed trailing mutation");
            bad = *good.table;
            bad.words.back() = 0x7fc00000;
            require(!validate_bsdf_table(bad).empty(), "packed nonfinite mutation");
            bad = *good.table;
            bad.working_bytes = 0;
            require(!validate_bsdf_table(bad).empty(), "packed accounting mutation");
            if (kind == BsdfTableKind::measured)
                for (unsigned slot = 28; slot < 32; ++slot) {
                    bad = *good.table;
                    bad.words[slot] = 0xffffffff;
                    require(!validate_bsdf_table(bad).empty(), "CDF cache offset mutation");
                    bad = *good.table;
                    bad.words[bad.words[slot] + 1] ^= 1;
                    require(!validate_bsdf_table(bad).empty(), "CDF cache value mutation");
                }
            require(good.table->source_bytes == bytes.size(), "source accounting");
            require(good.table->working_bytes == bytes.size() + 4 * good.table->words.size(),
                    "working accounting");
            require(!read_bsdf_table(path, kind, {.remaining_budget = bytes.size()}).table,
                    "aggregate byte limit");
            require(!read_bsdf_table(path, kind, {.per_file_budget = bytes.size() - 1}).table,
                    "per file limit");
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                auto b = bytes;
                b.resize(i);
                table_fixture::write(path, b);
                require(!read_bsdf_table(path, kind).table, "truncated input accepted");
            }
            table_fixture::write(path, bytes);
            auto held = read_bsdf_table(path, kind).table;
            std::filesystem::remove(path);
            require(!held->words.empty(), "snapshot must own data");
        }
        for (unsigned channels : {1u, 3u}) {
            table_fixture::write(path, table_fixture::fourier(channels));
            require(bool(read_bsdf_table(path, BsdfTableKind::fourier).table), "Fourier channels");
        }
        // Independently integrate the canonical angular Fourier luminance. This
        // checks retained harmonic ordering and its solid-angle PDF normalization.
        table_fixture::write(path, table_fixture::fourier(3, true));
        auto harmonic = read_bsdf_table(path, BsdfTableKind::fourier);
        require(bool(harmonic.table), "harmonic table");
        const auto &words = harmonic.table->words;
        auto number = [&](std::size_t i) { return double(std::bit_cast<float>(words[i])); };
        auto coeff = words[8] + words[words[7]];
        double integral = 0, first_moment = 0;
        constexpr unsigned quadrature = 8192;
        for (unsigned i = 0; i < quadrature; ++i) {
            double phi = (double(i) + .5) * 2 * std::acos(-1.) / quadrature;
            double y = number(coeff) + number(coeff + 1) * std::cos(phi);
            require(y > 0, "positive harmonic density");
            integral += y;
            first_moment += y * std::cos(phi);
        }
        require(std::abs(integral / quadrature - 1) < 1e-12, "Fourier normalization quadrature");
        require(std::abs(first_moment / quadrature - .1) < 1e-8, "Fourier first angular moment");
        auto inconsistent = *harmonic.table;
        inconsistent.words[inconsistent.words[6] + 1] = std::bit_cast<std::uint32_t>(.9f);
        require(!validate_bsdf_table(inconsistent).empty(), "CDF integral mutation");
        for (float amplitude : {1e-8f, 1.f, 1e8f}) {
            table_fixture::write(path, table_fixture::fourier(3, true, amplitude, true));
            const auto scaled = read_bsdf_table(path, BsdfTableKind::fourier);
            require(bool(scaled.table), scaled.diagnostic.c_str());
            auto missing_cdf = *scaled.table;
            for (unsigned i = 0; i < 9; ++i)
                missing_cdf.words[missing_cdf.words[6] + i] = 0;
            require(!validate_bsdf_table(missing_cdf).empty(), "scaled zero CDF rejected");
        }
        for (unsigned np : {1u, 2u, 3u, 5u}) {
            table_fixture::write(path, table_fixture::measured(np));
            auto r = read_bsdf_table(path, BsdfTableKind::measured);
            require(bool(r.table), r.diagnostic.c_str());
        }
        auto density_heavy = table_fixture::measured(1, false, 64);
        table_fixture::write(path, density_heavy);
        require(bool(read_bsdf_table(path, BsdfTableKind::measured).table),
                "Density-heavy table accepted within default budget");
        auto cache_budget = read_bsdf_table(path, BsdfTableKind::measured,
                                            {.remaining_budget = 3 * density_heavy.size()});
        require(!cache_budget.table &&
                    cache_budget.diagnostic.find("decode scratch") != std::string::npos,
                "Cache and decode peak checked before allocation");
        table_fixture::write(path, table_fixture::measured(1, true));
        auto plateau = read_bsdf_table(path, BsdfTableKind::measured);
        require(bool(plateau.table), "zero-density plateau fixture");
        auto marginal = plateau.table->words[29];
        require(std::bit_cast<float>(plateau.table->words[marginal + 1]) == .5f &&
                    std::bit_cast<float>(plateau.table->words[marginal + 2]) == .5f,
                "CDF plateau retained exactly");
        auto tensor_offset = [](const std::vector<unsigned char> &bytes, const std::string &name) {
            auto it = std::search(bytes.begin(), bytes.end(), name.begin(), name.end());
            if (it == bytes.end())
                throw std::runtime_error("fixture field missing");
            auto p = std::size_t(it - bytes.begin()) + name.size() + 3;
            std::size_t value = 0;
            for (unsigned i = 0; i < 8; ++i)
                value |= std::size_t(bytes[p + i]) << (8 * i);
            return value;
        };
        auto tensor = table_fixture::measured(3);
        auto phi = tensor_offset(tensor, "phi_i");
        auto store_float = [](std::size_t p, std::vector<unsigned char> &bytes, float value) {
            auto bits = std::bit_cast<std::uint32_t>(value);
            for (unsigned i = 0; i < 4; ++i)
                bytes[p + i] = static_cast<unsigned char>(bits >> (8 * i));
        };
        store_float(phi + 8, tensor, 1.5f);
        table_fixture::write(path, tensor);
        require(!read_bsdf_table(path, BsdfTableKind::measured).table, "reduced azimuth rejection");
        tensor = table_fixture::measured();
        store_float(tensor_offset(tensor, "theta_i") + 4, tensor, 0);
        table_fixture::write(path, tensor);
        require(!read_bsdf_table(path, BsdfTableKind::measured).table,
                "collapsed angular grid rejection");
        tensor = table_fixture::measured();
        store_float(tensor_offset(tensor, "vndf"), tensor, -1);
        table_fixture::write(path, tensor);
        require(!read_bsdf_table(path, BsdfTableKind::measured).table,
                "negative distribution rejection");
        auto b = table_fixture::fourier();
        b[8] = 2;
        table_fixture::write(path, b);
        require(!read_bsdf_table(path, BsdfTableKind::fourier).table, "flags");
        b = table_fixture::fourier();
        b[28] = 2;
        table_fixture::write(path, b);
        require(!read_bsdf_table(path, BsdfTableKind::fourier).table, "basis");
        b = table_fixture::fourier();
        b.back() = 0x7f;
        table_fixture::write(path, b);
        require(!read_bsdf_table(path, BsdfTableKind::fourier).table, "nonfinite coefficient");
        std::filesystem::remove(path);
        std::cout << "BSDF table checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
