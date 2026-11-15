// SPDX-License-Identifier: GPL-3.0-only
#include "bsdf_table.hpp"
#include "bsdf_table_wire.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
namespace rypath {
namespace {
struct Reader {
    std::vector<unsigned char> b;
    std::size_t p = 0;
    std::uint64_t integer(unsigned n) {
        if (p > b.size() || n > b.size() - p)
            throw std::runtime_error("truncated table");
        std::uint64_t v = 0;
        for (unsigned i = 0; i < n; ++i)
            v |= std::uint64_t(b[p++]) << (8 * i);
        return v;
    }
    float scalar() {
        float f = std::bit_cast<float>(std::uint32_t(integer(4)));
        if (!std::isfinite(f))
            throw std::runtime_error("nonfinite table value");
        return f;
    }
    void magic(const char *s, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i)
            if (integer(1) != static_cast<unsigned char>(s[i]))
                throw std::runtime_error("unsupported table signature or version");
    }
};
std::uint32_t append(BsdfTable &t, const std::vector<float> &v) {
    auto off = static_cast<std::uint32_t>(t.words.size());
    for (float f : v)
        t.words.push_back(std::bit_cast<std::uint32_t>(f));
    return off;
}
void grid(const std::vector<float> &v, bool positive = false) {
    if (v.empty())
        throw std::runtime_error("empty parameter grid");
    for (std::size_t i = 0; i < v.size(); ++i)
        if ((positive && v[i] <= 0) || (i && v[i] <= v[i - 1]))
            throw std::runtime_error("parameter grid must strictly increase");
}
std::vector<float> floats(Reader &r, std::size_t n) {
    if (n > (r.b.size() - r.p) / 4)
        throw std::runtime_error("truncated float array");
    std::vector<float> v(n);
    for (auto &f : v)
        f = r.scalar();
    return v;
}
void fourier(Reader &r, BsdfTable &t) {
    r.magic("SCATFUN\1", 8);
    auto flags = r.integer(4), n = r.integer(4), nc = r.integer(4), m = r.integer(4),
         c = r.integer(4), bases = r.integer(4);
    for (int i = 0; i < 3; ++i)
        r.integer(4);
    float eta = r.scalar();
    for (int i = 0; i < 4; ++i)
        r.integer(4);
    if (flags != 1 || bases != 1 || (c != 1 && c != 3))
        throw std::runtime_error("unsupported SCATFUN flags, channels or bases");
    if (n < 2 || n > 4096 || m < 1 || m > 4096 || eta <= 0 || n * n > (r.b.size() - r.p) / 12)
        throw std::runtime_error("invalid SCATFUN dimensions or eta");
    auto mu = floats(r, n);
    unsigned repeated_zero = 0;
    for (std::size_t i = 1; i < mu.size(); ++i)
        if (mu[i] <= mu[i - 1]) {
            if (mu[i] != 0 || mu[i - 1] != 0 || ++repeated_zero > 1)
                throw std::runtime_error("invalid SCATFUN mu grid");
        }
    if (mu.front() != -1 || mu.back() != 1)
        throw std::runtime_error("SCATFUN mu grid must cover [-1,1]");
    auto cdf = floats(r, n * n);
    for (std::size_t o = 0; o < n; ++o) {
        if (cdf[o * n] != 0 || cdf[o * n + n - 1] < 0)
            throw std::runtime_error("invalid SCATFUN CDF endpoints");
        for (std::size_t i = 1; i < n; ++i)
            if (cdf[o * n + i] < cdf[o * n + i - 1])
                throw std::runtime_error("nonmonotone SCATFUN CDF");
    }
    std::vector<std::uint32_t> ol(n * n * 2);
    for (std::size_t i = 0; i < n * n; ++i) {
        auto off = r.integer(4), len = r.integer(4);
        if (len > m || off > nc || len * c > nc - off)
            throw std::runtime_error("SCATFUN coefficient range out of bounds");
        ol[2 * i] = static_cast<std::uint32_t>(off);
        ol[2 * i + 1] = static_cast<std::uint32_t>(len);
    }
    auto coeff = floats(r, nc);
    for (std::size_t i = 0; i < n * n; ++i)
        if (ol[2 * i + 1] && coeff[ol[2 * i]] < 0)
            throw std::runtime_error("negative SCATFUN zeroth coefficient");
    // Optional SCATFUN metadata after the coefficient payload is inert, as in PBRT.
    t.words[0] = RYPATH_TABLE_FOURIER;
    t.words[1] = static_cast<std::uint32_t>(n);
    t.words[2] = static_cast<std::uint32_t>(m);
    t.words[3] = static_cast<std::uint32_t>(c);
    t.words[4] = std::bit_cast<std::uint32_t>(eta);
    t.words[5] = append(t, mu);
    t.words[6] = append(t, cdf);
    t.words[7] = static_cast<std::uint32_t>(t.words.size());
    t.words.insert(t.words.end(), ol.begin(), ol.end());
    t.words[8] = append(t, coeff);
}
struct Field {
    unsigned type;
    std::size_t off, count;
    std::vector<std::size_t> shape;
};
void measured(Reader &r, BsdfTable &t, std::size_t budget) {
    r.magic("tensor_file", 12);
    r.magic("\1\0", 2);
    auto n = r.integer(4);
    if (n > 1024)
        throw std::runtime_error("too many tensor fields");
    std::map<std::string, Field> fields;
    for (std::size_t i = 0; i < n; ++i) {
        auto len = r.integer(2);
        if (len > 4096)
            throw std::runtime_error("tensor field name too long");
        std::string name;
        for (std::size_t j = 0; j < len; ++j)
            name += static_cast<char>(r.integer(1));
        auto rank = r.integer(2), type = r.integer(1), off = r.integer(8);
        if (rank > 8 || type < 1 || type > 12)
            throw std::runtime_error("invalid tensor field type or rank");
        const unsigned sizes[] = {0, 1, 1, 2, 2, 4, 4, 8, 8, 2, 4, 8};
        if (type > 11)
            throw std::runtime_error("invalid tensor dtype");
        Field f{static_cast<unsigned>(type), static_cast<std::size_t>(off), sizes[type], {}};
        for (std::size_t j = 0; j < rank; ++j) {
            auto d = r.integer(8);
            if (!d || d > r.b.size() || f.count > r.b.size() / d)
                throw std::runtime_error("tensor dimension overflow");
            f.count *= d;
            f.shape.push_back(d);
        }
        if (off > r.b.size() || f.count > r.b.size() - off || !fields.emplace(name, f).second)
            throw std::runtime_error("invalid tensor field range or duplicate");
    }
    std::vector<std::pair<std::size_t, std::size_t>> payloads;
    for (auto &[name, f] : fields) {
        (void)name;
        if (f.off < r.p)
            throw std::runtime_error("tensor payload overlaps directory");
        payloads.emplace_back(f.off, f.off + f.count);
    }
    std::sort(payloads.begin(), payloads.end());
    for (std::size_t i = 1; i < payloads.size(); ++i)
        if (payloads[i].first < payloads[i - 1].second)
            throw std::runtime_error("overlapping tensor fields");
    auto field = [&](const char *name, unsigned rank, unsigned type) -> const Field & {
        auto it = fields.find(name);
        if (it == fields.end() || it->second.type != type || it->second.shape.size() != rank)
            throw std::runtime_error(std::string("invalid tensor field ") + name);
        return it->second;
    };
    auto data = [&](const Field &f) {
        r.p = f.off;
        return floats(r, f.count / 4);
    };
    auto phi = data(field("phi_i", 1, 10)), theta = data(field("theta_i", 1, 10)),
         wave = data(field("wavelengths", 1, 10));
    grid(phi);
    grid(theta);
    grid(wave, true);
    field("description", 1, 1);
    auto &jac = field("jacobian", 1, 1);
    if (jac.count != 1)
        throw std::runtime_error("invalid jacobian field");
    if (phi.size() > 2 && std::abs(phi.back() - phi.front() - 6.283185307179586) > 1e-4)
        throw std::runtime_error("reduced azimuth measured tensors unsupported");
    const char *names[] = {"ndf", "sigma", "vndf", "luminance", "spectra"};
    const auto grid_words = phi.size() + theta.size() + wave.size();
    std::size_t packed_words = 32 + grid_words, max_decode_bytes = 0;
    for (unsigned k = 0; k < 5; ++k) {
        const auto &f = field(names[k], k < 2 ? 2 : k == 4 ? 5 : 4, 10);
        packed_words += f.count / 4;
        max_decode_bytes = std::max(max_decode_bytes, f.count);
        if (k == 2 || k == 3)
            packed_words += f.count / 4 + f.count / 4 / f.shape.back();
    }
    // Reserve the complete canonical payload once. Caches are generated in place,
    // so the only bulk scratch is one decoded field plus the parameter grids.
    // The source is bounded before this plan; all field byte ranges are disjoint.
    if (packed_words > 512ULL * 1024 * 1024 / 4 ||
        t.source_bytes + 4 * packed_words + max_decode_bytes + 4 * grid_words + 128 > budget)
        throw std::runtime_error(
            "measured canonical payload and decode scratch exceed working byte limit");
    t.words.reserve(packed_words);
    t.words[0] = RYPATH_TABLE_MEASURED;
    t.words[1] = static_cast<std::uint32_t>(phi.size());
    t.words[2] = static_cast<std::uint32_t>(theta.size());
    t.words[3] = static_cast<std::uint32_t>(wave.size());
    t.words[4] = phi.size() <= 2;
    t.words[5] = append(t, phi);
    t.words[6] = append(t, theta);
    t.words[7] = append(t, wave);
    std::size_t lumsize = 0;
    for (unsigned k = 0; k < 5; ++k) {
        auto &f = field(names[k], k < 2 ? 2 : k == 4 ? 5 : 4, 10);
        auto &s = f.shape;
        std::size_t h = s[s.size() - 2], w = s.back(), slices = f.count / 4 / w / h;
        if (w < 2 || h < 2 || (k >= 2 && (s[0] != phi.size() || s[1] != theta.size())) ||
            (k == 4 && s[2] != wave.size()) || (k >= 3 && w != h))
            throw std::runtime_error("inconsistent measured tensor dimensions");
        if (k == 3)
            lumsize = w;
        if (k == 4 && w != lumsize)
            throw std::runtime_error("spectra/luminance dimensions differ");
        auto v = data(f);
        for (float x : v)
            if (x < 0)
                throw std::runtime_error("negative measured tensor density");
        if (k == 2 || k == 3) {
            for (std::size_t z = 0; z < slices; ++z) {
                double sum = 0;
                for (std::size_t y = 0; y < h - 1; ++y)
                    for (std::size_t x = 0; x < w - 1; ++x) {
                        auto a = z * w * h + y * w + x;
                        sum += (double(v[a]) + v[a + 1] + v[a + w] + v[a + w + 1]) * 0.25;
                    }
                if (!(sum > 0) || !std::isfinite(sum))
                    throw std::runtime_error("zero measured distribution integral");
                for (std::size_t i = z * w * h; i < (z + 1) * w * h; ++i) {
                    v[i] = static_cast<float>(v[i] / sum);
                    if (!std::isfinite(v[i]))
                        throw std::runtime_error("measured normalization overflow");
                }
            }
        } else {
            float scale = 1.f / static_cast<float>((w - 1) * (h - 1));
            for (float &x : v)
                x *= scale;
        }
        unsigned d = 8 + 4 * k;
        t.words[d] = static_cast<std::uint32_t>(w);
        t.words[d + 1] = static_cast<std::uint32_t>(h);
        t.words[d + 2] = static_cast<std::uint32_t>(slices);
        t.words[d + 3] = append(t, v);
        if (k == 2 || k == 3) {
            const auto count = w * h * slices, marginal_count = h * slices;
            const auto conditional = t.words.size(), marginal = conditional + count;
            t.words.resize(marginal + marginal_count);
            for (std::size_t z = 0; z < slices; ++z) {
                for (std::size_t y = 0; y < h; ++y) {
                    double sum = 0;
                    const auto base = z * w * h + y * w;
                    for (std::size_t x = 1; x < w; ++x) {
                        sum += .5 * (double(v[base + x - 1]) + v[base + x]);
                        t.words[conditional + base + x] =
                            std::bit_cast<std::uint32_t>(static_cast<float>(sum));
                    }
                }
                double sum = 0;
                for (std::size_t y = 1; y < h; ++y) {
                    sum +=
                        .5 *
                        (double(
                             std::bit_cast<float>(t.words[conditional + z * w * h + y * w - 1])) +
                         std::bit_cast<float>(t.words[conditional + z * w * h + (y + 1) * w - 1]));
                    t.words[marginal + z * h + y] =
                        std::bit_cast<std::uint32_t>(static_cast<float>(sum));
                }
            }
            const auto cache = 28 + 2 * (k - 2);
            t.words[cache] = static_cast<std::uint32_t>(conditional);
            t.words[cache + 1] = static_cast<std::uint32_t>(marginal);
        }
    }
}
} // namespace
std::string validate_bsdf_table(const BsdfTable &t) {
    try {
        const auto &v = t.words;
        if (v.size() < 32 || v.size() > 512ULL * 1024 * 1024 / 4)
            throw std::runtime_error("invalid packed table word count");
        auto scalar = [&](std::size_t i) {
            if (i >= v.size())
                throw std::runtime_error("packed scalar out of bounds");
            float x = std::bit_cast<float>(v[i]);
            if (!std::isfinite(x))
                throw std::runtime_error("nonfinite packed scalar");
            return x;
        };
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        auto range = [&](std::size_t off, std::size_t n) {
            if (off < 32 || off > v.size() || n > v.size() - off)
                throw std::runtime_error("packed array out of bounds");
            ranges.emplace_back(off, off + n);
        };
        auto parameter = [&](std::size_t off, std::size_t n) {
            range(off, n);
            for (std::size_t i = 0; i < n; ++i)
                if (!std::isfinite(scalar(off + i)) ||
                    (i && scalar(off + i) <= scalar(off + i - 1)))
                    throw std::runtime_error("invalid packed parameter grid");
        };
        if (t.kind == BsdfTableKind::fourier) {
            if (v[0] != 1 || v[1] < 2 || v[1] > 4096 || v[2] < 1 || v[2] > 4096 ||
                (v[3] != 1 && v[3] != 3) || scalar(4) <= 0)
                throw std::runtime_error("invalid packed Fourier header");
            for (unsigned i = 9; i < 32; ++i)
                if (v[i])
                    throw std::runtime_error("nonzero reserved Fourier header word");
            std::size_t n = v[1], cells = n * n;
            range(v[5], n);
            unsigned repeated_zero = 0;
            for (std::size_t i = 0; i < n; ++i) {
                float x = scalar(v[5] + i);
                if (i && x <= scalar(v[5] + i - 1) &&
                    (x != 0 || scalar(v[5] + i - 1) != 0 || ++repeated_zero > 1))
                    throw std::runtime_error("invalid packed Fourier mu grid");
            }
            if (scalar(v[5]) != -1 || scalar(v[5] + n - 1) != 1)
                throw std::runtime_error("invalid packed Fourier support");
            range(v[6], cells);
            range(v[7], 2 * cells);
            if (v[8] < 32 || v[8] > v.size())
                throw std::runtime_error("packed coefficient offset out of bounds");
            std::size_t extent = 0;
            for (std::size_t i = 0; i < cells; ++i) {
                std::size_t off = v[v[7] + 2 * i], m = v[v[7] + 2 * i + 1];
                if (m > v[2] || off > v.size() - v[8] || m * v[3] > v.size() - v[8] - off)
                    throw std::runtime_error("packed coefficient range out of bounds");
                extent = std::max(extent, off + m * v[3]);
                if (m && scalar(v[8] + off) < 0)
                    throw std::runtime_error("negative packed Fourier a0");
            }
            range(v[8], extent);
            for (std::size_t i = 0; i < extent; ++i)
                scalar(v[8] + i);
            for (std::size_t o = 0; o < n; ++o) {
                std::size_t p = v[6] + o * n;
                if (scalar(p) != 0 || scalar(p + n - 1) < 0)
                    throw std::runtime_error("invalid packed Fourier CDF endpoints");
                auto a0 = [&](std::size_t i) {
                    std::size_t cell = o * n + i;
                    return v[v[7] + 2 * cell + 1] ? double(scalar(v[8] + v[v[7] + 2 * cell])) : 0.;
                };
                double integral = 0;
                for (std::size_t i = 0; i < n - 1; ++i) {
                    if (scalar(p + i + 1) < scalar(p + i))
                        throw std::runtime_error("invalid packed Fourier CDF order");
                    double x0 = scalar(v[5] + i), x1 = scalar(v[5] + i + 1), width = x1 - x0,
                           f0 = a0(i), f1 = a0(i + 1);
                    double d0 =
                        i > 0 ? width * (f1 - a0(i - 1)) / (x1 - scalar(v[5] + i - 1)) : f1 - f0;
                    double d1 = i + 2 < n ? width * (a0(i + 2) - f0) / (scalar(v[5] + i + 2) - x0)
                                          : f1 - f0;
                    integral += width * ((f0 + f1) * .5 + (d0 - d1) / 12);
                    if (std::abs(integral - scalar(p + i + 1)) >
                        1e-5 *
                            std::max(std::numeric_limits<double>::min(), double(scalar(p + n - 1))))
                        throw std::runtime_error("Fourier CDF disagrees with coefficient integral");
                }
            }
        } else if (t.kind == BsdfTableKind::measured) {
            if (v[0] != 2 || !v[1] || !v[2] || !v[3] || v[4] != (v[1] <= 2))
                throw std::runtime_error("invalid packed measured header");
            if (std::size_t(v[1]) > v.size() / v[2] || std::size_t(v[1]) * v[2] > v.size() / v[3])
                throw std::runtime_error("packed measured parameter product overflow");
            parameter(v[5], v[1]);
            parameter(v[6], v[2]);
            parameter(v[7], v[3]);
            if (scalar(v[7]) <= 0)
                throw std::runtime_error("invalid measured wavelengths");
            if (v[1] > 2 &&
                std::abs(scalar(v[5] + v[1] - 1) - scalar(v[5]) - 6.283185307179586) > 1e-4)
                throw std::runtime_error("reduced packed azimuth domain");
            for (unsigned k = 0; k < 5; ++k) {
                unsigned d = 8 + 4 * k;
                std::size_t w = v[d], h = v[d + 1], slices = v[d + 2];
                if (w < 2 || h < 2 || w > v.size() || h > v.size() / w ||
                    slices > v.size() / (w * h))
                    throw std::runtime_error("invalid packed measured dimensions");
                std::size_t expected = k < 2 ? 1 : std::size_t(v[1]) * v[2] * (k == 4 ? v[3] : 1);
                if (slices != expected || (k >= 3 && w != h) || (k == 4 && w != v[20]))
                    throw std::runtime_error("inconsistent packed measured dimensions");
                range(v[d + 3], w * h * slices);
                for (std::size_t i = 0; i < w * h * slices; ++i)
                    if (scalar(v[d + 3] + i) < 0 || (k == 1 && scalar(v[d + 3] + i) <= 0))
                        throw std::runtime_error("invalid packed measured density");
                if (k == 2 || k == 3) {
                    const unsigned cache = 28 + 2 * (k - 2);
                    const auto c = v[cache], m = v[cache + 1];
                    range(c, w * h * slices);
                    range(m, h * slices);
                    for (std::size_t z = 0; z < slices; ++z) {
                        for (std::size_t y = 0; y < h; ++y) {
                            const auto a = v[d + 3] + z * w * h + y * w, b = c + z * w * h + y * w;
                            if (scalar(b) != 0)
                                throw std::runtime_error("invalid measured conditional CDF origin");
                            double sum = 0;
                            for (std::size_t x = 1; x < w; ++x) {
                                sum += .5 * (double(scalar(a + x - 1)) + scalar(a + x));
                                if (scalar(b + x) != static_cast<float>(sum) ||
                                    scalar(b + x) < scalar(b + x - 1))
                                    throw std::runtime_error(
                                        "measured conditional CDF disagrees with density");
                            }
                        }
                        if (scalar(m + z * h) != 0)
                            throw std::runtime_error("invalid measured marginal CDF origin");
                        double sum = 0;
                        for (std::size_t y = 1; y < h; ++y) {
                            sum += .5 * (double(scalar(c + z * w * h + y * w - 1)) +
                                         scalar(c + z * w * h + (y + 1) * w - 1));
                            if (scalar(m + z * h + y) != static_cast<float>(sum) ||
                                scalar(m + z * h + y) < scalar(m + z * h + y - 1))
                                throw std::runtime_error(
                                    "measured marginal CDF disagrees with row integral");
                        }
                    }
                }
                if (k == 2 || k == 3)
                    for (std::size_t z = 0; z < slices; ++z) {
                        double sum = 0;
                        for (std::size_t y = 0; y < h - 1; ++y)
                            for (std::size_t x = 0; x < w - 1; ++x) {
                                auto a = v[d + 3] + z * w * h + y * w + x;
                                sum += (double(scalar(a)) + scalar(a + 1) + scalar(a + w) +
                                        scalar(a + w + 1)) *
                                       .25;
                            }
                        if (std::abs(sum - 1) > 2e-5)
                            throw std::runtime_error("unnormalized packed measured density");
                    }
            }
        } else
            throw std::runtime_error("invalid table kind");
        std::sort(ranges.begin(), ranges.end());
        std::size_t end = 32;
        for (auto [a, b] : ranges) {
            if (a != end)
                throw std::runtime_error("packed arrays overlap or have gaps");
            end = b;
        }
        if (end != v.size())
            throw std::runtime_error("unused packed table words");
        if (t.source_bytes > 128ULL * 1024 * 1024 ||
            t.working_bytes != t.source_bytes + 4 * v.size() ||
            t.working_bytes > 512ULL * 1024 * 1024)
            throw std::runtime_error("invalid packed resource accounting");
        return {};
    } catch (const std::exception &e) {
        return e.what();
    }
}
BsdfTableResult read_bsdf_table(const std::filesystem::path &path, BsdfTableKind kind,
                                BsdfTableLimits limits) {
    const auto budget = limits.remaining_budget;
    const auto per_file = limits.per_file_budget;
    try {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
            throw std::runtime_error("unable to open BSDF table");
        auto end = in.tellg();
        if (end < 0 || static_cast<std::uint64_t>(end) > per_file)
            throw std::runtime_error("BSDF table exceeds per-file byte limit");
        auto size = static_cast<std::size_t>(end);
        // Source plus canonical representation coexist; reserve a conservative bound before
        // allocation.
        if (size > budget / 3 || budget < 128)
            throw std::runtime_error("BSDF table exceeds aggregate working byte limit");
        Reader r;
        r.b.resize(size);
        in.seekg(0);
        if (!in.read(reinterpret_cast<char *>(r.b.data()), static_cast<std::streamsize>(size)))
            throw std::runtime_error("unable to read BSDF table");
        auto t = std::make_shared<BsdfTable>();
        t->kind = kind;
        t->source_bytes = size;
        t->words.resize(32);
        if (kind == BsdfTableKind::fourier)
            fourier(r, *t);
        else
            measured(r, *t, budget);
        t->working_bytes = size + t->words.size() * 4;
        if (t->working_bytes > budget)
            throw std::runtime_error("BSDF table exceeds aggregate working byte limit");
        auto diagnostic = validate_bsdf_table(*t);
        if (!diagnostic.empty())
            throw std::runtime_error(diagnostic);
        return {std::move(t), {}};
    } catch (const std::exception &e) {
        return {{}, e.what()};
    }
}
} // namespace rypath
