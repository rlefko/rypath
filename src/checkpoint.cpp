// SPDX-License-Identifier: GPL-3.0-only
#include "checkpoint.hpp"
#include <algorithm>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
namespace rypath {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::array<std::uint8_t, 8> magic{'R', 'Y', 'P', 'C', 'H', 'K', '\r', '\n'};
constexpr std::uint64_t max_payload = std::uint64_t{4} * 1024 * 1024 * 1024;
constexpr std::uint64_t max_metadata = std::uint64_t{1024} * 1024;
constexpr std::size_t header_size = 44, pixel_size = 216;
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
#ifdef RYPATH_CHECKPOINT_TESTING
std::string injected_failure;
#endif
bool fail(std::string_view operation) {
#ifdef RYPATH_CHECKPOINT_TESTING
    if (injected_failure == operation) {
        errno = EIO;
        return true;
    }
#else
    (void)operation;
#endif
    return false;
}
std::string diagnostic(std::string_view operation) {
    return std::string(operation) + ": " +
           std::error_code(errno, std::generic_category()).message();
}
// Fixed-width encoder intentionally takes a value and byte width.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void put(Bytes &out, std::uint64_t value, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
void real(Bytes &out, double value) { put(out, std::bit_cast<std::uint64_t>(value), 8); }
struct Cursor {
    std::span<const std::uint8_t> bytes;
    std::uint64_t get(std::size_t n) {
        if (n > bytes.size())
            throw std::runtime_error("truncated checkpoint metadata");
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < n; ++i)
            value |= std::uint64_t(bytes[i]) << (8 * i);
        bytes = bytes.subspan(n);
        return value;
    }
    double real() { return std::bit_cast<double>(get(8)); }
    std::string string() {
        const auto n = get(4);
        if (n > 4096 || n > bytes.size())
            throw std::runtime_error("invalid checkpoint string length");
        std::string value(reinterpret_cast<const char *>(bytes.data()),
                          static_cast<std::size_t>(n));
        bytes = bytes.subspan(static_cast<std::size_t>(n));
        return value;
    }
};
void string(Bytes &out, const std::string &value) {
    put(out, value.size(), 4);
    out.insert(out.end(), value.begin(), value.end());
}
Bytes metadata(const RenderCheckpoint &c) {
    Bytes b;
    put(b, c.settings.width, 4);
    put(b, c.settings.height, 4);
    put(b, c.settings.samples, 8);
    put(b, c.settings.batch_size, 8);
    put(b, c.settings.seed, 8);
    put(b, static_cast<std::uint32_t>(c.settings.sampler.kind), 4);
    put(b, static_cast<std::uint32_t>(c.settings.sampler.randomization), 4);
    put(b, std::bit_cast<std::uint32_t>(c.settings.material_seed), 4);
    put(b, static_cast<std::uint32_t>(c.film.model), 4);
    real(b, c.film.exposure);
    put(b, c.film.packet, 1);
    put(b, c.film.compatibility_output, 1);
    real(b, c.film.max_component_value);
    put(b, static_cast<std::uint32_t>(c.film.filter.kind), 4);
    for (float value : {c.film.filter.x_radius, c.film.filter.y_radius, c.film.filter.sigma})
        put(b, std::bit_cast<std::uint32_t>(value), 4);
    b.insert(b.end(), c.identity.begin(), c.identity.end());
    put(b, c.segments.size(), 4);
    for (const auto &s : c.segments) {
        put(b, s.begin, 8);
        put(b, s.end, 8);
        string(b, s.build);
        string(b, s.device);
        string(b, s.architecture);
        string(b, s.os);
    }
    return b;
}
void append_pixel(Bytes &b, const FilmPixel &p) {
    for (auto x : p.sum)
        real(b, x);
    for (auto x : p.correction)
        real(b, x);
    put(b, p.count, 8);
    for (const auto *values :
         {&p.splat_sum, &p.splat_correction, &p.compatibility_sum, &p.compatibility_correction,
          &p.compatibility_splat_sum, &p.compatibility_splat_correction})
        for (double value : *values)
            real(b, value);
    real(b, p.weight_sum);
    real(b, p.weight_correction);
}
Bytes header(std::uint64_t meta, std::uint64_t payload) {
    Bytes b(magic.begin(), magic.end());
    for (auto version :
         {checkpoint_contract::format, checkpoint_contract::identity, checkpoint_contract::renderer,
          checkpoint_contract::sampling, checkpoint_contract::accumulation})
        put(b, version, 4);
    put(b, meta, 8);
    put(b, payload, 8);
    return b;
}
struct File {
    int fd = -1;
    std::string temporary;
    ~File() {
        if (fd >= 0)
            close(fd);
        if (!temporary.empty())
            unlink(temporary.c_str());
    }
};
bool read_at(int fd, std::uint64_t offset, std::span<std::uint8_t> bytes) {
    while (!bytes.empty()) {
#ifdef RYPATH_CHECKPOINT_TESTING
        if (injected_failure == "read-eintr") {
            injected_failure.clear();
            errno = EINTR;
            continue;
        }
#endif
        auto n = pread(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        offset += static_cast<std::uint64_t>(n);
        bytes = bytes.subspan(static_cast<std::size_t>(n));
    }
    return true;
}
bool write_all(int fd, std::span<const std::uint8_t> bytes) {
    while (!bytes.empty()) {
        if (fail("write"))
            return false;
        auto amount = bytes.size();
#ifdef RYPATH_CHECKPOINT_TESTING
        if (injected_failure == "write-eintr") {
            injected_failure.clear();
            errno = EINTR;
            continue;
        }
        if (injected_failure == "short-write")
            amount = std::min(amount, std::size_t{3});
#endif
        auto n = write(fd, bytes.data(), amount);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (n == 0)
                errno = EIO;
            return false;
        }
        bytes = bytes.subspan(static_cast<std::size_t>(n));
    }
    return true;
}
bool sync_file(int fd, bool directory) {
    if (fail(directory ? "directory-sync" : "file-sync"))
        return false;
    int result;
    do {
#ifdef __APPLE__
        result = directory ? fsync(fd) : fcntl(fd, F_FULLFSYNC);
#else
        result = fsync(fd);
#endif
    } while (result < 0 && errno == EINTR);
    return result == 0;
}
// Descriptor and byte length have distinct roles at every call site.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool checksum(int fd, std::uint64_t length, const Sha256Digest &expected) {
    Sha256 hash;
    std::array<std::uint8_t, 65536> buffer{};
    for (std::uint64_t at = 0; at < length;) {
        const auto n =
            static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), length - at));
        auto part = std::span(buffer).first(n);
        if (!read_at(fd, at, part))
            return false;
        hash.update(part);
        at += n;
    }
    return hash.finish() == expected;
}
} // namespace
#ifdef RYPATH_CHECKPOINT_TESTING
namespace checkpoint_test {
void set_failure(std::string operation) { injected_failure = std::move(operation); }
} // namespace checkpoint_test
#endif
std::string validate_checkpoint(const RenderCheckpoint &c) {
    if (auto error = validate_render_settings(c.settings); !error.empty())
        return error;
    if (static_cast<std::uint32_t>(c.settings.sampler.randomization) > 3)
        return "invalid checkpoint sampler randomization";
    const auto pixels = std::uint64_t(c.settings.width) * c.settings.height;
    if (pixels > max_payload / pixel_size || c.film.width != c.settings.width ||
        c.film.height != c.settings.height || pixels != c.film.pixels.size())
        return "invalid checkpoint film dimensions or payload exceeds 4 GiB";
    if ((c.film.model != RenderModel::demo && c.film.model != RenderModel::pbrt) ||
        !std::isfinite(c.film.exposure) || c.film.exposure < 0 ||
        (c.film.model == RenderModel::demo && c.film.exposure != 1))
        return "invalid checkpoint film model or exposure";
    if (c.segments.size() > 1024)
        return "checkpoint exceeds 1024 provenance segments";
    if (auto error = validate_film_accumulation(c.film); !error.empty())
        return error;
    if (!(c.film.max_component_value > 0) || static_cast<std::uint32_t>(c.film.filter.kind) > 2 ||
        !(c.film.filter.x_radius > 0 && c.film.filter.y_radius > 0) ||
        !std::isfinite(c.film.filter.x_radius) || !std::isfinite(c.film.filter.y_radius) ||
        !std::isfinite(c.film.filter.sigma) ||
        (c.film.filter.kind == PixelFilterKind::gaussian && !(c.film.filter.sigma > 0)))
        return "invalid checkpoint film filter or clamp";
    std::uint64_t meta = 118;
    const auto count = c.film.pixels.front().count;
    for (const auto &p : c.film.pixels) {
        if (p.count != count || p.count > c.settings.samples)
            return "invalid checkpoint sample counts";
        for (std::size_t i = 0; i < 3; ++i)
            if (!std::isfinite(p.sum[i]) || !std::isfinite(p.correction[i]) ||
                !std::isfinite(p.sum[i] + p.correction[i]) ||
                (count == 0 && (p.sum[i] != 0 || p.correction[i] != 0)))
                return "invalid checkpoint accumulation";
    }
    std::uint64_t end = 0;
    for (const auto &s : c.segments) {
        if (s.begin != end || s.end <= s.begin || s.end > count)
            return "invalid checkpoint provenance ranges";
        end = s.end;
        meta += 32;
        for (const auto *text : {&s.build, &s.device, &s.architecture, &s.os}) {
            if (text->size() > 4096)
                return "checkpoint provenance string exceeds 4096 bytes";
            meta += text->size();
        }
    }
    if (end != count)
        return "checkpoint provenance does not cover completed samples";
    if (meta > max_metadata)
        return "checkpoint metadata exceeds 1 MiB";
    return {};
}
std::string write_checkpoint(const RenderCheckpoint &c, const std::filesystem::path &path) {
    try {
        if (path.empty() || path.native().find('\0') != std::string::npos)
            return "invalid checkpoint path";
        if (auto error = validate_checkpoint(c); !error.empty())
            return error;
        const auto meta = metadata(c);
        const auto head = header(meta.size(), c.film.pixels.size() * pixel_size);
        const auto directory =
            path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
        File pending;
        auto temporary = (directory / ".rypath-checkpoint.XXXXXX").native();
        if (fail("create"))
            return diagnostic("checkpoint temporary creation failed");
        pending.fd = mkstemp(temporary.data());
        if (pending.fd < 0)
            return diagnostic("checkpoint temporary creation failed");
        pending.temporary = std::move(temporary);
        Sha256 hash;
        auto emit = [&](std::span<const std::uint8_t> b) {
            hash.update(b);
            return write_all(pending.fd, b);
        };
        if (!emit(head) || !emit(meta))
            return diagnostic("checkpoint write failed");
        if (fail("pixel-write"))
            return diagnostic("checkpoint pixel write failed");
        Bytes records;
        records.reserve(pixel_size * 1024);
        for (const auto &p : c.film.pixels) {
            append_pixel(records, p);
            if (records.size() == pixel_size * 1024) {
                if (!emit(records))
                    return diagnostic("checkpoint write failed");
                records.clear();
            }
        }
        if (!records.empty() && !emit(records))
            return diagnostic("checkpoint write failed");
        if (!write_all(pending.fd, hash.finish()))
            return diagnostic("checkpoint checksum write failed");
        if (!sync_file(pending.fd, false))
            return diagnostic("checkpoint file sync failed");
        const int fd = pending.fd;
        pending.fd = -1;
        // close() must not be retried: on several POSIX platforms EINTR already closes the fd.
        const int closed = close(fd);
        if (closed < 0 || fail("close"))
            return diagnostic("checkpoint close failed");
        int renamed;
        do {
            renamed = fail("rename") ? -1 : rename(pending.temporary.c_str(), path.c_str());
        } while (renamed < 0 && errno == EINTR);
        if (renamed < 0)
            return diagnostic("checkpoint publication failed");
        pending.temporary.clear();
        File parent;
        do {
            parent.fd =
                fail("directory-open") ? -1 : open(directory.c_str(), O_RDONLY | O_DIRECTORY);
        } while (parent.fd < 0 && errno == EINTR);
        if (parent.fd < 0 || !sync_file(parent.fd, true))
            return diagnostic("checkpoint publication succeeded but durability is uncertain");
        return {};
    } catch (const std::exception &error) {
        return std::string("checkpoint write failed: ") + error.what();
    }
}
CheckpointReadResult read_checkpoint(const std::filesystem::path &path) {
    try {
        if (path.empty() || path.native().find('\0') != std::string::npos)
            throw std::runtime_error("invalid checkpoint path");
        File input;
        do {
            input.fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        } while (input.fd < 0 && errno == EINTR);
        if (input.fd < 0)
            throw std::runtime_error(diagnostic("checkpoint open failed"));
        struct stat info {};
        if (fstat(input.fd, &info) < 0 || !S_ISREG(info.st_mode))
            throw std::runtime_error("checkpoint must be a regular file");
        std::array<std::uint8_t, header_size> head{};
        if (!read_at(input.fd, 0, head) || !std::equal(magic.begin(), magic.end(), head.begin()))
            throw std::runtime_error("invalid checkpoint header");
        Cursor h{std::span(head).subspan(8)};
        if (h.get(4) != checkpoint_contract::format)
            throw std::runtime_error("unsupported checkpoint format version (expected format 3)");
        for (auto version : {checkpoint_contract::identity, checkpoint_contract::renderer,
                             checkpoint_contract::sampling, checkpoint_contract::accumulation})
            if (h.get(4) != version)
                throw std::runtime_error("unsupported checkpoint contract version");
        const auto meta_size = h.get(8), payload = h.get(8);
        if (meta_size > max_metadata || payload > max_payload || payload % pixel_size != 0 ||
            info.st_size < 0 ||
            static_cast<std::uint64_t>(info.st_size) != header_size + meta_size + payload + 32)
            throw std::runtime_error("invalid checkpoint length or size limit");
        const auto length = header_size + meta_size + payload;
        Sha256Digest expected{};
        if (!read_at(input.fd, length, expected) || !checksum(input.fd, length, expected))
            throw std::runtime_error("checkpoint checksum mismatch or read failure");
        Bytes meta(static_cast<std::size_t>(meta_size));
        if (!read_at(input.fd, header_size, meta))
            throw std::runtime_error("checkpoint metadata read failed");
        Cursor m{meta};
        RenderCheckpoint c;
        c.settings.width = static_cast<std::uint32_t>(m.get(4));
        c.settings.height = static_cast<std::uint32_t>(m.get(4));
        c.settings.samples = m.get(8);
        c.settings.batch_size = m.get(8);
        c.settings.seed = m.get(8);
        c.settings.sampler.kind = static_cast<SamplerKind>(m.get(4));
        c.settings.sampler.randomization = static_cast<SobolRandomization>(m.get(4));
        c.settings.material_seed =
            std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(m.get(4)));
        c.film.model = static_cast<RenderModel>(m.get(4));
        c.film.exposure = m.real();
        const auto packet = m.get(1), compatibility = m.get(1);
        if (packet > 1 || compatibility > 1)
            throw std::runtime_error("invalid checkpoint film flags");
        c.film.packet = packet != 0;
        c.film.compatibility_output = compatibility != 0;
        c.film.max_component_value = m.real();
        c.film.filter.kind = static_cast<PixelFilterKind>(m.get(4));
        c.film.filter.x_radius = std::bit_cast<float>(static_cast<std::uint32_t>(m.get(4)));
        c.film.filter.y_radius = std::bit_cast<float>(static_cast<std::uint32_t>(m.get(4)));
        c.film.filter.sigma = std::bit_cast<float>(static_cast<std::uint32_t>(m.get(4)));
        for (auto &b : c.identity)
            b = static_cast<std::uint8_t>(m.get(1));
        const auto segments = m.get(4);
        if (segments > 1024)
            throw std::runtime_error("checkpoint exceeds 1024 provenance segments");
        for (std::uint64_t i = 0; i < segments; ++i) {
            CheckpointSegment s;
            s.begin = m.get(8);
            s.end = m.get(8);
            s.build = m.string();
            s.device = m.string();
            s.architecture = m.string();
            s.os = m.string();
            c.segments.push_back(std::move(s));
        }
        if (!m.bytes.empty())
            throw std::runtime_error("trailing checkpoint metadata");
        if (auto error = validate_render_settings(c.settings); !error.empty())
            throw std::runtime_error(error);
        const auto pixels = std::uint64_t(c.settings.width) * c.settings.height;
        if (pixels != payload / pixel_size)
            throw std::runtime_error("checkpoint film payload size mismatch");
        c.film.width = c.settings.width;
        c.film.height = c.settings.height;
        c.film.pixels.resize(static_cast<std::size_t>(pixels));
        Sha256 parsed;
        parsed.update(head);
        parsed.update(meta);
        std::array<std::uint8_t, pixel_size * 1024> records{};
        std::uint64_t at = header_size + meta_size;
        for (std::size_t first = 0; first < c.film.pixels.size();) {
            const auto n = std::min(std::size_t{1024}, c.film.pixels.size() - first);
            auto chunk = std::span(records).first(n * pixel_size);
            if (!read_at(input.fd, at, chunk))
                throw std::runtime_error("truncated checkpoint pixel payload");
            parsed.update(chunk);
            Cursor r{chunk};
            for (std::size_t i = 0; i < n; ++i) {
                auto &p = c.film.pixels[first + i];
                for (auto &x : p.sum)
                    x = r.real();
                for (auto &x : p.correction)
                    x = r.real();
                p.count = r.get(8);
                for (auto *values : {&p.splat_sum, &p.splat_correction, &p.compatibility_sum,
                                     &p.compatibility_correction, &p.compatibility_splat_sum,
                                     &p.compatibility_splat_correction})
                    for (auto &value : *values)
                        value = r.real();
                p.weight_sum = r.real();
                p.weight_correction = r.real();
            }
            at += chunk.size();
            first += n;
        }
        if (parsed.finish() != expected)
            throw std::runtime_error("checkpoint changed during reading");
        if (auto error = validate_checkpoint(c); !error.empty())
            throw std::runtime_error(error);
        return {std::move(c), {}};
    } catch (const std::exception &error) {
        return {{}, error.what()};
    }
}
} // namespace rypath
