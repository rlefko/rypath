// SPDX-License-Identifier: GPL-3.0-only
#include "gltf.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace rypath::gltf;
namespace {
namespace fs = std::filesystem;
void require(bool yes, std::string_view why) {
    if (!yes)
        throw std::runtime_error(std::string(why));
}
void write(const fs::path &p, std::string_view s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
    f.close();
    require(f.good(), "fixture write");
}
void write(const fs::path &p, const std::vector<std::uint8_t> &b) {
    write(p, {reinterpret_cast<const char *>(b.data()), b.size()});
}
void put(std::vector<std::uint8_t> &b, float x) {
    const auto u = std::bit_cast<std::uint32_t>(x);
    for (unsigned i = 0; i < 4; ++i)
        b.push_back(static_cast<std::uint8_t>(u >> (8 * i)));
}
std::string b64(const std::vector<std::uint8_t> &b) {
    constexpr char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    for (std::size_t i = 0; i < b.size(); i += 3) {
        const auto left = b.size() - i;
        const unsigned x = (unsigned(b[i]) << 16) | (left > 1 ? unsigned(b[i + 1]) << 8 : 0) |
                           (left > 2 ? b[i + 2] : 0);
        s += tab[x >> 18];
        s += tab[(x >> 12) & 63];
        s += left > 1 ? tab[(x >> 6) & 63] : '=';
        s += left > 2 ? tab[x & 63] : '=';
    }
    return s;
}
std::vector<std::uint8_t> payload() {
    std::vector<std::uint8_t> b;
    for (int x : {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1})
        put(b, static_cast<float>(x));
    return b;
}
std::string
model(const std::vector<std::uint8_t> &b, std::string views, std::string accessors,
      std::string primitive = "{\"attributes\":{\"POSITION\":0}}",
      std::string rest = "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0",
      bool embedded = true) {
    return "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":" +
           std::to_string(b.size()) +
           (embedded ? ",\"uri\":\"data:application/octet-stream;base64," + b64(b) + "\"" : "") +
           "}],\"bufferViews\":" + views + ",\"accessors\":" + accessors +
           ",\"meshes\":[{\"primitives\":[" + primitive + "]}]," + rest + "}";
}
const std::string views =
    R"([{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":24}])";
const std::string accessors =
    R"([{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"}])";
std::string basic() { return model(payload(), views, accessors); }
void replace(std::string &s, std::string_view from, std::string_view to) {
    auto p = s.find(from);
    require(p != std::string::npos, "replace source");
    s.replace(p, from.size(), to);
}
const Asset &success(const Result &r) {
    if (r.exit_code() || !r.asset)
        throw std::runtime_error(report(r));
    return *r.asset;
}
void failure(const Result &r, int code = 1) {
    require(!r.asset && r.exit_code() == code, report(r));
    require(!r.diagnostics.empty() && r.diagnostics.front().location.line > 0 &&
                !r.diagnostics.front().location.file->empty(),
            "located glTF diagnostic");
}
void malformed(const fs::path &p) {
    for (auto s : {std::string("{}"), std::string("{\"asset\":{},\"asset\":{}}"), basic() + "true",
                   std::string("{\"asset\":{\"version\":\"2.0\"},\"x\":NaN}"),
                   std::string("{\"asset\":{\"version\":\"2.0\"},\"x\":01}"),
                   std::string("{\"asset\":{\"version\":\"2.0\"},\"x\":\"\\ud800\"}")}) {
        write(p, s);
        failure(load(p));
    }
    for (auto pair :
         {std::pair{"\"count\":3", "\"count\":-3"}, std::pair{"\"count\":3", "\"count\":3.0"},
          std::pair{"\"count\":3", "\"count\":9223372036854775808"},
          std::pair{"\"buffer\":0", "\"buffer\":99"},
          std::pair{"\"POSITION\":0", "\"POSITION\":99"},
          std::pair{"\"byteLength\":36", "\"byteLength\":1"},
          std::pair{"\"type\":\"VEC3\"", "\"type\":\"VEC2\""}}) {
        auto s = basic();
        replace(s, pair.first, pair.second);
        write(p, s);
        failure(load(p));
    }
    auto s = basic();
    replace(s, "\"scene\":0",
            "\"scene\":0,\"extensionsRequired\":[\"KHR_draco_mesh_compression\"],"
            "\"extensionsUsed\":[\"KHR_draco_mesh_compression\"]");
    write(p, s);
    failure(load(p), 2);
    s = basic();
    replace(s, "\"attributes\":{\"POSITION\":0}", "\"attributes\":{\"POSITION\":0},\"mode\":5");
    write(p, s);
    failure(load(p), 2);
    s = basic();
    replace(s, "\"POSITION\":0", "\"POSITION\":0,\"COLOR_0\":1");
    write(p, s);
    failure(load(p), 2);
}
void scene_cases(const fs::path &p) {
    auto s = basic();
    replace(s, "\"scenes\":[{\"nodes\":[0]}],\"scene\":0",
            "\"scenes\":[{\"nodes\":[0]},{\"name\":\"other\",\"nodes\":[0]}]");
    write(p, s);
    failure(load(p));
    require(success(load(p, Options{1})).selected_scene == 1, "explicit scene");
    failure(load(p, Options{2}));
    s = basic();
    replace(
        s, "\"nodes\":[{\"mesh\":0}]",
        "\"nodes\":[{\"children\":[1],\"translation\":[2,0,0]},{\"mesh\":0,\"scale\":[-1,2,1]}]");
    write(p, s);
    auto r = load(p);
    auto &a = success(r);
    require(a.placements.size() == 1 && a.placements[0].transform[0] == -1 &&
                a.placements[0].transform[3] == 2 && a.placements[0].transform[5] == 2,
            "parent reflection composition");
    s = basic();
    replace(s, "\"nodes\":[{\"mesh\":0}]", "\"nodes\":[{\"mesh\":0,\"children\":[0]}]");
    write(p, s);
    failure(load(p));
    s = basic();
    replace(s, "\"nodes\":[{\"mesh\":0}]", "\"nodes\":[{\"mesh\":0,\"scale\":[0,1,1]}]");
    write(p, s);
    failure(load(p));
    s = basic();
    replace(s, "\"nodes\":[{\"mesh\":0}]",
            "\"nodes\":[{\"mesh\":0,\"translation\":[100000000,0,0]}]");
    write(p, s);
    failure(load(p));
    s = basic();
    replace(s, "\"scene\":0",
            "\"scene\":0,\"animations\":[{\"samplers\":[{\"input\":0,\"output\":0}],\"channels\":[{"
            "\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}}]}]");
    write(p, s);
    failure(load(p), 2);
}
void sparse(const fs::path &p) {
    std::vector<std::uint8_t> b;
    for (int x : {0, 0, 0, 91, 1, 0, 0, 92, 0, 0, 0, 93})
        put(b, static_cast<float>(x));
    b.push_back(2);
    b.resize(52);
    for (int x : {0, 1, 0})
        put(b, static_cast<float>(x));
    const auto v =
        R"([{"buffer":0,"byteOffset":0,"byteLength":48,"byteStride":16},{"buffer":0,"byteOffset":48,"byteLength":1},{"buffer":0,"byteOffset":52,"byteLength":12}])";
    auto a =
        R"([{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0],"sparse":{"count":1,"indices":{"bufferView":1,"componentType":5121},"values":{"bufferView":2}}}])";
    write(p, model(b, v, a));
    const auto r = load(p);
    require(success(r).primitives[0].geometry.positions[2].y == 1,
            "packed sparse overlay over stride16 base");
    b[48] = 3;
    write(p, model(b, v, a));
    failure(load(p));
    b[48] = 2;
    auto s = model(b, v, a);
    replace(s, "\"byteLength\":12", "\"byteLength\":11");
    write(p, s);
    failure(load(p));
}
void normal_map(const fs::path &p) {
    const auto png = fs::path(__FILE__).parent_path() /
                     "fixtures/pbrt-render/core-textures/assets/normal-flat.png";
    fs::copy_file(png, p.parent_path() / "normal.png", fs::copy_options::overwrite_existing);
    auto s = model(
        payload(), views, accessors,
        R"({"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"material":0})",
        R"("nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"materials":[{"normalTexture":{"index":0,"scale":-2},"doubleSided":true,"alphaMode":"MASK","alphaCutoff":0.3}],"textures":[{"source":0,"sampler":0}],"images":[{"uri":"normal.png"}],"samplers":[{"wrapS":33648,"wrapT":33071,"minFilter":9986,"magFilter":9728}])");
    write(p, s);
    const auto r = load(p);
    const auto &a = success(r);
    const auto &pr = a.primitives[0];
    require(pr.generated_tangents && pr.geometry.tangents.size() == 3 &&
                pr.geometry.tangent_signs == std::vector<float>({1, 1, 1}),
            "Mikk signed tangent generation");
    require(pr.geometry.uvs[2][1] == 1, "native glTF UV origin");
    require(a.materials[0].normal_scale == -2 && a.materials[0].double_sided &&
                a.materials[0].alpha_mode == "MASK" && a.textures[0].wrap_s == 33648 &&
                a.textures[0].wrap_t == 33071,
            "material and independent sampler preservation");
    auto missing = s;
    replace(missing, "normal.png", "missing.png");
    write(p, missing);
    failure(load(p));
    auto mismatch = s;
    replace(mismatch, "\"normalTexture\":{\"index\":0",
            "\"normalTexture\":{\"texCoord\":1,\"index\":0");
    write(p, mismatch);
    failure(load(p));
}
void shared_sparse_work(const fs::path &p) {
    auto b = payload();
    for (unsigned i = 0; i < 16; ++i)
        b.push_back(static_cast<std::uint8_t>(i));
    b.resize(b.size() + 64);
    auto v = views;
    v.pop_back();
    v +=
        R"(,{"buffer":0,"byteOffset":96,"byteLength":16},{"buffer":0,"byteOffset":112,"byteLength":64}])";
    auto a = accessors;
    a.pop_back();
    for (unsigned i = 0; i < 100; ++i)
        a +=
            R"(,{"componentType":5125,"count":16,"type":"SCALAR","sparse":{"count":16,"indices":{"bufferView":3,"componentType":5121},"values":{"bufferView":4}}})";
    a += ']';
    write(p, model(b, v, a));
    success(load(p));
    Limits l;
    l.work_items = 2500;
    const auto r = load(p, {}, l);
    failure(r);
    require(r.diagnostics.back().message.find("work") != std::string::npos,
            "shared unused sparse validation consumes cumulative work");
}
void reversed_hierarchy(const fs::path &p) {
    for (bool reverse : {false, true}) {
        std::string nodes = "\"nodes\":[";
        constexpr std::size_t n = 300;
        for (std::size_t i = 0; i < n; ++i) {
            if (i)
                nodes += ',';
            const bool leaf = reverse ? i == 0 : i == n - 1;
            nodes += leaf ? "{\"mesh\":0}"
                          : "{\"children\":[" + std::to_string(reverse ? i - 1 : i + 1) + "]}";
        }
        nodes += "],\"scenes\":[{\"nodes\":[" + std::to_string(reverse ? n - 1 : 0) + "]}]";
        write(p, model(payload(), views, accessors, "{\"attributes\":{\"POSITION\":0}}", nodes));
        failure(load(p));
    }
}
void escaped_json(const fs::path &p) {
    auto s = basic();
    replace(s, "\"nodes\":[{\"mesh\":0}]",
            R"("nodes":[{"mesh":0,"\u0063hildren":[1]},{"mesh":0,"name":"\ud83d\ude00"}])");
    write(p, s);
    const auto r = load(p);
    const auto &a = success(r);
    require(a.placements.size() == 2 && a.placements[1].name == "😀",
            "decoded escaped keys and Unicode surrogate-pair names");
    require(prepare(a).exit_code() == 0, "Unicode metadata survives bundle preparation");
    for (auto [from, to] :
         {std::pair{std::string("\"min\":[0,0,0]"), std::string("\"min\":[0]")},
          std::pair{
              std::string("\"scene\":0"),
              std::string(
                  "\"scene\":0,\"materials\":[{\"pbrMetallicRoughness\":{\"metallicFactor\":2}}]")},
          std::pair{std::string("\"scene\":0"),
                    std::string("\"scene\":0,\"samplers\":[{\"wrapS\":17}]")},
          std::pair{std::string("\"scene\":0"),
                    std::string("\"scene\":0,\"cameras\":[{\"type\":\"perspective\","
                                "\"perspective\":{\"znear\":-1,\"yfov\":1}}]")}}) {
        auto invalid = basic();
        replace(invalid, from, to);
        write(p, invalid);
        failure(load(p));
    }
    auto controls = basic();
    replace(controls, "\"scene\":0", R"("scene":0,"extras":{"test":"\n\b\t"})");
    write(p, controls);
    success(load(p));
    auto restricted = basic();
    replace(restricted, "\"componentType\":5126",
            R"("extensions":{"REVIEW_unknown":{}},"componentType":5126)");
    write(p, restricted);
    failure(load(p), 2);
}
void containers_and_limits(const fs::path &p) {
    auto b = payload();
    auto j = model(b, views, accessors, "{\"attributes\":{\"POSITION\":0}}",
                   "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}]", false);
    while (j.size() % 4)
        j += ' ';
    std::vector<std::uint8_t> glb;
    const auto u32 = [&](std::uint32_t n) {
        for (unsigned i = 0; i < 4; ++i)
            glb.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
    };
    u32(0x46546c67);
    u32(2);
    u32(static_cast<std::uint32_t>(28 + j.size() + b.size()));
    u32(static_cast<std::uint32_t>(j.size()));
    u32(0x4e4f534a);
    glb.insert(glb.end(), j.begin(), j.end());
    u32(static_cast<std::uint32_t>(b.size()));
    u32(0x004e4942);
    glb.insert(glb.end(), b.begin(), b.end());
    write(p, glb);
    require(success(load(p)).binary, "GLB decode");
    glb.pop_back();
    write(p, glb);
    failure(load(p));
    write(p, basic());
    for (unsigned which = 0; which < 6; ++which) {
        Limits l;
        if (which == 0)
            l.input.file_bytes = 10;
        if (which == 1)
            l.working_bytes = 200;
        if (which == 2)
            l.input.total_values = 3;
        if (which == 3)
            l.input.scopes = 1;
        if (which == 4)
            l.input.expansions = 0;
        if (which == 5)
            l.work_items = 1;
        failure(load(p, {}, l));
    }
}
} // namespace
int main() try {
    const auto d = fs::temp_directory_path() /
                   ("rypath-gltf-tests-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(d);
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(p, e);
        }
    } cleanup{d};
    const auto p = d / "test.gltf";
    write(p, basic());
    auto r = load(p);
    require(success(r).primitives[0].generated_normals, "missing normals are flat");
    const auto &loaded = success(r);
    require(retained_bytes(loaded) > loaded.bytes.size(), "retained accounting");
    malformed(p);
    scene_cases(p);
    sparse(p);
    normal_map(p);
    containers_and_limits(p);
    escaped_json(p);
    reversed_hierarchy(p);
    shared_sparse_work(p);
    const auto fixtures = fs::path(__FILE__).parent_path() / "fixtures/gltf/external";
    require(success(load(fixtures / "Triangle/glTF/Triangle.gltf"))
                    .primitives[0]
                    .geometry.triangles.size() == 1,
            "external Triangle");
    require(success(load(fixtures / "Box/glTF-Binary/Box.glb"))
                    .primitives[0]
                    .geometry.triangles.size() == 12,
            "external Box");
    std::cout << "glTF importer checks passed\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
