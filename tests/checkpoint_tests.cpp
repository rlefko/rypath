// SPDX-License-Identifier: GPL-3.0-only
#include "checkpoint.hpp"
#include <bit>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <unistd.h>
#ifdef RYPATH_CHECKPOINT_TESTING
namespace rypath::checkpoint_test {
void set_failure(std::string operation);
}
#endif
namespace {
using namespace rypath;
using Bytes = std::vector<std::uint8_t>;
void require(bool ok, std::string_view text) {
    if (!ok)
        throw std::runtime_error(std::string(text));
}
std::span<const std::uint8_t> bytes(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t *>(s.data()), s.size()};
}
void hashes() {
    for (auto [input, expected] : std::array<std::pair<std::string_view, std::string_view>, 3>{
             {{"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
              {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
              {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"}}}) {
        require(digest_hex(sha256(bytes(input))) == expected, "FIPS SHA256 known answer");
        Sha256 streaming;
        for (auto b : bytes(input))
            streaming.update(std::span(&b, 1));
        require(digest_hex(streaming.finish()) == expected, "byte streaming known answer");
        require(streaming.finish() == streaming.finish(), "repeat finish");
        bool rejected = false;
        try {
            streaming.update({});
        } catch (const std::logic_error &) {
            rejected = true;
        }
        require(rejected, "finished hash cannot be updated");
    }
    const std::string million(1'000'000, 'a');
    require(digest_hex(sha256(bytes(million))) ==
                "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
            "million a known answer");
}
Bytes read(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    require(bool(in), "test read");
    return {std::istreambuf_iterator<char>(in), {}};
}
void write(const std::filesystem::path &p, const Bytes &b) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
    require(bool(out), "test write");
}
// Test fixture mutation explicitly names offset, value and encoded width.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void put(Bytes &b, std::size_t offset, std::uint64_t value, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        b.at(offset + i) = static_cast<std::uint8_t>(value >> (8 * i));
}
void rehash(Bytes &b) {
    const auto hash = sha256(std::span(b).first(b.size() - 32));
    std::copy(hash.begin(), hash.end(), b.end() - 32);
}
RenderCheckpoint fixture() {
    RenderCheckpoint c;
    c.settings.width = 2;
    c.settings.height = 1;
    c.settings.samples = (std::uint64_t{1} << 53) + 37;
    c.settings.batch_size = 19;
    c.settings.seed = UINT64_MAX;
    c.settings.material_seed = INT32_MIN;
    c.film.width = 2;
    c.film.height = 1;
    c.film.model = RenderModel::pbrt;
    c.film.exposure = 1.25;
    c.film.pixels.resize(2);
    for (auto &p : c.film.pixels) {
        p.sum = {-0.0, 1e100, -32};
        p.correction = {0.0, 1e-100, -0.125};
        p.count = c.settings.samples - 2;
    }
    c.identity = sha256(bytes("identity"));
    c.segments = {{0, 5, "build-one", "device-one", "arm64", "system-one"},
                  {5, c.film.pixels[0].count, "build-two", "device-two", "arm64", "system-two"}};
    return c;
}
void exact(const RenderCheckpoint &a, const RenderCheckpoint &b) {
    require(
        a.identity == b.identity && a.settings.width == b.settings.width &&
            a.settings.height == b.settings.height && a.settings.samples == b.settings.samples &&
            a.settings.batch_size == b.settings.batch_size && a.settings.seed == b.settings.seed &&
            a.settings.material_seed == b.settings.material_seed &&
            a.settings.sampler.kind == b.settings.sampler.kind &&
            a.settings.sampler.randomization == b.settings.sampler.randomization,
        "exact settings and identity");
    require(a.film.model == b.film.model && a.film.exposure == b.film.exposure &&
                a.film.pixels.size() == b.film.pixels.size() && a.film.packet == b.film.packet &&
                a.film.compatibility_output == b.film.compatibility_output &&
                a.film.max_component_value == b.film.max_component_value &&
                a.film.filter == b.film.filter,
            "film metadata");
    for (std::size_t i = 0; i < a.film.pixels.size(); ++i) {
        const auto &x = a.film.pixels[i], &y = b.film.pixels[i];
        require(x.count == y.count, "exact high count");
        const std::array<const std::array<double, 3> *, 8> xp{&x.sum,
                                                              &x.correction,
                                                              &x.splat_sum,
                                                              &x.splat_correction,
                                                              &x.compatibility_sum,
                                                              &x.compatibility_correction,
                                                              &x.compatibility_splat_sum,
                                                              &x.compatibility_splat_correction};
        const std::array<const std::array<double, 3> *, 8> yp{&y.sum,
                                                              &y.correction,
                                                              &y.splat_sum,
                                                              &y.splat_correction,
                                                              &y.compatibility_sum,
                                                              &y.compatibility_correction,
                                                              &y.compatibility_splat_sum,
                                                              &y.compatibility_splat_correction};
        for (std::size_t plane = 0; plane < xp.size(); ++plane)
            for (std::size_t j = 0; j < 3; ++j)
                require(std::bit_cast<std::uint64_t>((*xp[plane])[j]) ==
                            std::bit_cast<std::uint64_t>((*yp[plane])[j]),
                        "bit exact accumulation plane");
        require(std::bit_cast<std::uint64_t>(x.weight_sum) ==
                        std::bit_cast<std::uint64_t>(y.weight_sum) &&
                    std::bit_cast<std::uint64_t>(x.weight_correction) ==
                        std::bit_cast<std::uint64_t>(y.weight_correction),
                "bit exact reconstruction weights");
    }
    require(a.segments.size() == b.segments.size(), "segments count");
    for (std::size_t i = 0; i < a.segments.size(); ++i) {
        const auto &x = a.segments[i], &y = b.segments[i];
        require(x.begin == y.begin && x.end == y.end && x.build == y.build &&
                    x.device == y.device && x.architecture == y.architecture && x.os == y.os,
                "exact provenance");
    }
}
const RenderCheckpoint &checked(const CheckpointReadResult &result) {
    if (!result.value)
        throw std::runtime_error(result.diagnostic);
    return *result.value;
}
void codec(const std::filesystem::path &dir) {
    const auto path = dir / "checkpoint.bin";
    auto c = fixture();
    require(validate_checkpoint(c).empty(), "valid fixture");
    require(write_checkpoint(c, path).empty(), "checkpoint publication");
    const auto original = read(path);
    // Independent Python struct.pack('<...') + hashlib implementation of the documented layout.
    require(digest_hex(sha256(std::span(original).first(original.size() - 32))) ==
                "df872cb1bf2accebb7a1de23ef0397473bb777a9a93f6340e0f5656c9e027da2",
            "independent binary layout checksum");
    auto result = read_checkpoint(path);
    require(result.value.has_value(), result.diagnostic);
    exact(c, checked(result));
    require(write_checkpoint(checked(result), dir / "copy.bin").empty(), "checkpoint rewrite");
    require(original == read(dir / "copy.bin"), "byte exact serialization");
    for (auto kind : {SamplerKind::halton, SamplerKind::zsobol}) {
        auto qmc = c;
        qmc.settings.sampler.kind = kind;
        qmc.settings.sampler.randomization = SobolRandomization::permutedigits;
        qmc.settings.samples = 16;
        for (auto &pixel : qmc.film.pixels)
            pixel.count = 8;
        qmc.segments.back().end = 8;
        require(write_checkpoint(qmc, dir / "qmc.bin").empty(), "QMC checkpoint publication");
        exact(qmc, checked(read_checkpoint(dir / "qmc.bin")));
    }
    auto packet = c;
    packet.film.packet = true;
    packet.film.compatibility_output = true;
    packet.film.max_component_value = 50;
    packet.film.filter = {PixelFilterKind::gaussian, 1.5f, 2.f, .5f};
    for (auto &pixel : packet.film.pixels) {
        pixel.splat_sum = {1, 2, 3};
        pixel.splat_correction = {1e-20, -1e-20, -0.};
        pixel.compatibility_sum = {4, 5, 6};
        pixel.compatibility_correction = {-1e-30, 1e-30, 0};
        pixel.compatibility_splat_sum = {7, 8, 9};
        pixel.compatibility_splat_correction = {1e-15, 0, -1e-15};
        pixel.weight_sum = 7;
        pixel.weight_correction = 1e-15;
    }
    require(write_checkpoint(packet, dir / "packet.bin").empty(), "packet checkpoint publication");
    exact(packet, checked(read_checkpoint(dir / "packet.bin")));
    packet.film.pixels.front().compatibility_splat_correction[0] =
        std::numeric_limits<double>::infinity();
    require(!write_checkpoint(packet, path).empty(),
            "invalid packet plane rejected transactionally");
    require(read(path) == original, "invalid packet plane preserves prior checkpoint");
    // Prior identity/renderer contracts remain unsupported even with valid authentication.
    for (const auto [offset, version] : std::array<std::pair<std::size_t, std::uint32_t>, 34>{
             {{16, 18}, {16, 14}, {16, 15}, {16, 16}, {16, 17}, {20, 6},  {8, 2},
              {12, 11}, {16, 13}, {20, 5},  {20, 4},  {8, 1},   {12, 10}, {16, 12},
              {20, 3},  {24, 1},  {12, 3},  {12, 4},  {12, 5},  {12, 6},  {12, 7},
              {12, 8},  {12, 9},  {16, 3},  {16, 4},  {16, 5},  {16, 6},  {16, 7},
              {16, 8},  {16, 9},  {16, 10}, {16, 11}, {20, 1},  {20, 2}}}) {
        auto legacy = original;
        put(legacy, offset, version, 4);
        rehash(legacy);
        write(path, legacy);
        require(!read_checkpoint(path).value, "legacy rendering contract rejection");
    }
    // Header, metadata, pixel, checksum, truncation, and trailing data all fail.
    for (auto offset : {std::size_t{0}, std::size_t{8}, std::size_t{44}, original.size() - 33,
                        original.size() - 1}) {
        auto bad = original;
        bad[offset] ^= 1;
        write(path, bad);
        require(!read_checkpoint(path).value, "corruption rejection");
    }
    for (auto length : {std::size_t{0}, std::size_t{43}, original.size() - 1}) {
        auto bad = original;
        bad.resize(length);
        write(path, bad);
        require(!read_checkpoint(path).value, "truncation rejection");
    }
    auto bad = original;
    bad.push_back(0);
    write(path, bad);
    require(!read_checkpoint(path).value, "trailing bytes rejection");
    for (auto [offset, value, n] :
         std::array<std::tuple<std::size_t, std::uint64_t, std::size_t>, 7>{
             {{28, 1024 * 1024 + 1, 8},
              {36, std::uint64_t{4} * 1024 * 1024 * 1024 + 56, 8},
              {44, UINT32_MAX, 4},
              {44 + 32, 9, 4},
              {44 + 36, 9, 4},
              {44 + 44, 9, 4},
              {44 + 114, 1025, 4}}}) {
        bad = original;
        put(bad, offset, value, n);
        rehash(bad);
        write(path, bad);
        require(!read_checkpoint(path).value, "malicious bounded metadata rejection");
    }
    // Authenticated malformed string length, count and nonfinite component.
    for (auto [offset, value, n] :
         std::array<std::tuple<std::size_t, std::uint64_t, std::size_t>, 3>{
             {{44 + 118 + 16, 4097, 4},
              {original.size() - 32 - 432 + 48, UINT64_MAX, 8},
              {original.size() - 32 - 432, 0x7ff0000000000000ULL, 8}}}) {
        bad = original;
        put(bad, offset, value, n);
        rehash(bad);
        write(path, bad);
        require(!read_checkpoint(path).value, "invalid authenticated state rejection");
    }
    auto exposure = c;
    exposure.film.exposure = 0;
    require(write_checkpoint(exposure, path).empty(), "zero PBRT exposure is valid");
    result = read_checkpoint(path);
    exact(exposure, checked(result));
    exposure.film.model = RenderModel::demo;
    require(!validate_checkpoint(exposure).empty(), "demo exposure must be one");
    exposure.film.exposure = 1;
    require(validate_checkpoint(exposure).empty(), "demo unit exposure");
    auto invalid = c;
    invalid.film.pixels[0].count--;
    require(!validate_checkpoint(invalid).empty(), "nonuniform counts");
    invalid = c;
    invalid.film.pixels[0].sum[0] = std::numeric_limits<double>::max();
    invalid.film.pixels[0].correction[0] = std::numeric_limits<double>::max();
    require(!validate_checkpoint(invalid).empty(), "overflowing finite total");
    invalid = c;
    invalid.segments[1].begin++;
    require(!validate_checkpoint(invalid).empty(), "provenance gap");
    invalid = c;
    invalid.segments[0].build.resize(4097);
    require(!validate_checkpoint(invalid).empty(), "oversized provenance string");
    invalid = c;
    invalid.segments.resize(1025);
    require(!validate_checkpoint(invalid).empty(), "too many segments");
    invalid = c;
    invalid.segments.clear();
    for (auto &p : invalid.film.pixels)
        p = {};
    invalid.film.pixels[0].sum[0] = -0.0;
    require(write_checkpoint(invalid, path).empty(), "zero count checkpoint");
    result = read_checkpoint(path);
    require(result.value.has_value(), result.diagnostic);
    exact(invalid, checked(result));
    invalid.film.pixels[0].correction[0] = 1e-100;
    require(!validate_checkpoint(invalid).empty(), "zero count requires zero state");
    auto high = c;
    high.settings.width = 1;
    high.film.width = 1;
    high.film.pixels.resize(1);
    high.settings.samples = UINT64_MAX;
    high.film.pixels[0].count = UINT64_MAX;
    high.segments.resize(1);
    high.segments[0].end = UINT64_MAX;
    require(write_checkpoint(high, path).empty(), "maximum uint64 count write");
    result = read_checkpoint(path);
    require(result.value.has_value(), result.diagnostic);
    exact(high, checked(result));
#ifdef RYPATH_CHECKPOINT_TESTING
    write(path, original);
    for (auto phase : {"create", "write", "pixel-write", "file-sync", "close", "rename"}) {
        checkpoint_test::set_failure(phase);
        require(!write_checkpoint(high, path).empty(), "injected publication failure");
        checkpoint_test::set_failure({});
        require(read(path) == original, "failed publication preserves prior file");
        for (const auto &entry : std::filesystem::directory_iterator(dir))
            require(!entry.path().filename().native().starts_with(".rypath-checkpoint."),
                    "temporary cleanup");
    }
    for (auto phase : {"short-write", "write-eintr"}) {
        checkpoint_test::set_failure(phase);
        require(write_checkpoint(high, path).empty(), "short write and EINTR completion");
        checkpoint_test::set_failure({});
        result = read_checkpoint(path);
        require(result.value.has_value(), result.diagnostic);
        exact(high, checked(result));
    }
    checkpoint_test::set_failure("read-eintr");
    result = read_checkpoint(path);
    require(result.value.has_value(), result.diagnostic);
    exact(high, checked(result));
    checkpoint_test::set_failure({});
    for (auto phase : {"directory-open", "directory-sync"}) {
        checkpoint_test::set_failure(phase);
        require(write_checkpoint(high, path)
                        .find("publication succeeded but durability is uncertain") !=
                    std::string::npos,
                "post-publication durability diagnostic");
        checkpoint_test::set_failure({});
        result = read_checkpoint(path);
        require(result.value.has_value(), result.diagnostic);
        exact(high, checked(result));
    }
#endif
    auto large = c;
    large.settings.width = 2051;
    large.film.width = 2051;
    large.settings.samples = 100;
    large.film.pixels.assign(2051, c.film.pixels[0]);
    for (auto &p : large.film.pixels)
        p.count = 7;
    large.segments = {{0, 7, "build", "device", "architecture", "os"}};
    require(write_checkpoint(large, path).empty(), "multiple I/O batches write");
    result = read_checkpoint(path);
    require(result.value.has_value(), result.diagnostic);
    exact(large, checked(result));
    require(!read_checkpoint(dir).value, "non-regular input rejection");
    require(!write_checkpoint(c, dir / "missing" / "checkpoint").empty(), "missing parent");
    require(!write_checkpoint(c, dir).empty(), "rename to directory fails");
}
} // namespace
int main() {
    std::string temporary;
    try {
        temporary =
            (std::filesystem::temp_directory_path() / "rypath-checkpoint-tests.XXXXXX").native();
        require(mkdtemp(temporary.data()) != nullptr, "temporary directory");
        hashes();
        codec(temporary);
        std::filesystem::remove_all(temporary);
        std::cout << "checkpoint codec and SHA256 tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        try {
            std::filesystem::remove_all(temporary);
        } catch (...) {
            std::cerr << "temporary directory cleanup failed\n";
        }
        std::cerr << error.what() << '\n';
        return 1;
    }
}
