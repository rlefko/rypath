// SPDX-License-Identifier: GPL-3.0-only
// Reuse the guarded dispatcher for complete closest-seed state controls.
void seed_cases(const fs::path &render_library, const fs::path &old_library,
                const fs::path &new_library, const fs::path &out, Counters &counters,
                std::ostream &rows) {
    for (unsigned variant = 0; variant < 5; ++variant) {
        GeometryMesh opaque;
        opaque.positions = {{-1, -1, .75f}, {1, -1, .75f}, {0, 1, .75f}};
        opaque.triangles = {{0, 1, 2}};
        if (variant == 4)
            for (auto &p : opaque.positions)
                p.z = .5f;
        GeometryMesh near = opaque;
        for (auto &p : near.positions)
            p.z = .25f;
        if (variant == 1) {
            near = {};
            near.analytics.push_back(
                {.kind = AnalyticKind::sphere, .radius = .125f, .z_min = -.125f, .z_max = .125f});
        }
        if (variant == 2) {
            near.positions = {{-1, -1, .25f}, {1, -1, .25f}, {-1, 1, .25f}, {1, 1, .25f}};
            near.triangles.clear();
            near.patches = {{0, 1, 2, 3}};
        }
        if (variant == 3)
            near = opaque;
        GeometryData data;
        if (variant == 4) {
            data.meshes = {opaque};
            data.instances = {{.mesh_index = 0}};
        } else {
            data.meshes = {near, opaque};
            data.instances = {{.mesh_index = 0, .non_opaque = variant == 0}, {.mesh_index = 1}};
            if (variant == 1)
                data.instances[0].transform.values[11] = .375f;
        }
        auto built = build_metal_geometry_with_hints(data, render_library, true);
        need(built.status == GeometryStatus::success && built.scene, built.diagnostic);
        auto &scene = *built.scene;
        const auto device = MetalGeometryAccess::device(scene);
        std::string error;
        const auto library = metal::load_library(device, new_library, error);
        need(library != nil, error);
        const unsigned count = MetalGeometryAccess::instance_count(scene);
        need(count <= 8, "seed metadata bound");
        for (unsigned mode : {0u, 1u, 2u, 3u}) {
            std::vector<closest_seed_probe::Case> cases;
            auto make = [&](unsigned test, unsigned rule) {
                closest_seed_probe::Case c{};
                auto &b = c.base;
                b.ray = {{variant == 1 ? .03125f : 0.f, variant == 1 ? .03125f : 0.f, 0},
                         {0, 0, variant == 4 ? 3.f : 1.f},
                         0,
                         1};
                b.mask = wire::all_visible;
                b.alpha_ordinal = variant == 0 ? 0u : UINT32_MAX;
                b.rule = rule;
                b.instance_count = count;
                b.material_count = count;
                c.test = test;
                c.seed_ordinal = 1;
                for (unsigned i = 0; i < count; ++i) {
                    const auto source = MetalGeometryAccess::source_instance(scene, i);
                    const auto &m = data.meshes[data.instances[source].mesh_index];
                    b.instances[i] = {static_cast<unsigned>(m.triangles.size()),
                                      i,
                                      data.instances[source].non_opaque ? 0u : UINT32_MAX,
                                      0,
                                      0,
                                      0};
                    b.materials[i] = {0};
                }
                return c;
            };
            cases.push_back(make(0, 0));
            if (variant == 0) {
                cases.push_back(make(0, 1));
                cases.push_back(make(0, 2));
            }
            if ((mode & 2u) && variant == 4) {
                for (unsigned test : {1u, 2u, 4u, 5u})
                    cases.push_back(make(test, 0));
            }
            if ((mode & 2u) && variant == 3)
                cases.push_back(make(3, 0));
            const auto label =
                "seed-variant" + std::to_string(variant) + "-mode" + std::to_string(mode);
            const auto bytes =
                dispatch(scene, pipeline(device, library, "closest_seed_cases", mode), cases.data(),
                         cases.size(), sizeof(cases[0]), sizeof(closest_seed_probe::Result),
                         wire::all_visible, true, false, out / label, counters);
            for (unsigned i = 0; i < cases.size(); ++i) {
                closest_seed_probe::Result r;
                std::memcpy(&r, bytes.data() + i * sizeof(r), sizeof(r));
                const auto &c = cases[i];
                const bool invalid =
                    (variant == 0 && c.base.rule == 2) || c.test == 4 || c.test == 5;
                if (invalid)
                    need(r.hit.kind == wire::invalid_hit,
                         "Visited invalid alpha / malformed seed / anyhit-seed visibly invalid");
                else {
                    const unsigned expected_kind = variant == 1   ? wire::analytic
                                                   : variant == 2 ? wire::bilinear_patch
                                                                  : wire::triangle;
                    const unsigned expected_instance = variant == 0 && c.base.rule == 1 ? 1u : 0u;
                    need(r.hit.kind == expected_kind && r.hit.instance == expected_instance &&
                             r.hit.geometry == 0 && r.hit.primitive == 0,
                         "Independent seeded closest kind/stable identity");
                    if (variant == 0)
                        need(r.hit.t == (c.base.rule == 0 ? .25f : .75f),
                             "Accepted/rejected closer alpha exact distance");
                    if (c.test == 1 || c.test == 2) {
                        need(r.seed_available == 1 &&
                                 std::memcmp(&r.hit, &r.seed, sizeof(r.hit)) == 0,
                             "No-offer internal control retains full seed public fields");
                        need(r.pair[3] == 1 && r.pair[7] == 1 && r.pair[0] != 0 && r.pair[4] != 0,
                             "Retained supported full Pair with nonzero low component (computed "
                             "one-sixth)");
                        need(std::bit_cast<unsigned>(r.hit.t) == 0x3e2aaaabu,
                             "One-sixth public distance is correctly rounded");
                        // RN24(1/6) = 11184811 * 2^-26; RN24 of its
                        // exact correction is -11184811 * 2^-51.
                        const std::array<unsigned, 8> expected_pair{
                            11184811u, std::bit_cast<unsigned>(-26), 0u, 1u,
                            11184811u, std::bit_cast<unsigned>(-51), 1u, 1u};
                        need(
                            std::equal(expected_pair.begin(), expected_pair.end(), r.pair),
                            "One-sixth complete pair retains the independently derived correction");
                        // This dyadic sum spans 49 bits and is exact even where
                        // long double has only the 53-bit binary64 significand.
                        const long double distance =
                            std::ldexp(11184811.0L, -26) - std::ldexp(11184811.0L, -51);
                        if (c.test == 2) {
                            need(r.has_cap == 1, "Computed strict exterior cap exercised");
                            const auto cap = std::bit_cast<float>(r.cap_bits);
                            need(std::isfinite(cap) && cap > c.base.ray.t_min &&
                                     cap<c.base.ray.t_max &&static_cast<long double>(cap)> distance,
                                 "Computed cap is strictly exterior and inside original bounds");
                        } else {
                            need(r.has_cap == 0 && r.cap_bits == 0,
                                 "Unavailable seed cap explicitly observed");
                        }
                        need(r.alpha_calls == 0 && r.opaque_calls == 0,
                             "No procedural callback when offers deliberately suppressed");
                    }
                }
                rows << "{\"fixture\":" << std::quoted(label) << ",\"test\":" << c.test
                     << ",\"rule\":" << c.base.rule << ",\"kind\":" << r.hit.kind
                     << ",\"instance\":" << r.hit.instance << ",\"t\":" << r.hit.t
                     << ",\"seed_available\":" << r.seed_available << ",\"has_cap\":" << r.has_cap
                     << ",\"internal_state_control\":" << (c.test ? "true" : "false") << "}\n";
            }
        }
    }
    // Same AS/ray/mask/metadata provenance through the actual wrapper for every
    // preexisting bounded interior/bounds/transform/coverage fixture.
    for (const auto &f : fixtures()) {
        auto built = build_metal_geometry_with_hints(f.data, render_library, true);
        need(built.status == GeometryStatus::success && built.scene, built.diagnostic);
        auto &scene = *built.scene;
        const auto device = MetalGeometryAccess::device(scene);
        std::string error;
        const auto old = metal::load_library(device, old_library, error);
        need(old != nil, error);
        const auto current = metal::load_library(device, new_library, error);
        need(current != nil, error);
        const unsigned count = MetalGeometryAccess::instance_count(scene);
        need(count <= 8, "actual wrapper metadata bound");
        std::vector<wire::Ray> rays;
        std::vector<closest_seed_probe::Case> cases;
        for (const auto &ray : f.rays) {
            rays.push_back(packed(ray));
            closest_seed_probe::Case c{};
            c.base.ray = packed(ray);
            c.base.mask = wire::all_visible;
            c.base.alpha_ordinal = UINT32_MAX;
            c.base.instance_count = count;
            c.base.material_count = count;
            for (unsigned i = 0; i < count; ++i) {
                const auto source = MetalGeometryAccess::source_instance(scene, i);
                const auto &m = f.data.meshes[f.data.instances[source].mesh_index];
                c.base.instances[i] = {static_cast<unsigned>(m.triangles.size()),
                                       i,
                                       f.data.instances[source].non_opaque ? 0u : UINT32_MAX,
                                       0,
                                       0,
                                       0};
                c.base.materials[i] = {0};
            }
            cases.push_back(c);
        }
        const auto reference =
            dispatch(scene, pipeline(device, old, "control_closest"), rays.data(), rays.size(),
                     sizeof(wire::Ray), sizeof(wire::Closest), wire::all_visible, false, false,
                     out / ("seed-reference-" + f.name), counters);
        for (unsigned mode : {0u, 1u, 2u, 3u}) {
            const auto bytes = dispatch(
                scene, pipeline(device, current, "closest_seed_cases", mode), cases.data(),
                cases.size(), sizeof(cases[0]), sizeof(closest_seed_probe::Result),
                wire::all_visible, true, false,
                out / ("seed-actual-" + f.name + "-mode" + std::to_string(mode)), counters);
            for (unsigned i = 0; i < cases.size(); ++i) {
                closest_seed_probe::Result result;
                std::memcpy(&result, bytes.data() + i * sizeof(result), sizeof(result));
                need(std::memcmp(&result.hit, reference.data() + i * sizeof(wire::Closest),
                                 sizeof(wire::Closest)) == 0,
                     "Actual wrapper closest exact unseeded control fields");
            }
            rows << "{\"fixture\":" << std::quoted("seed-actual-" + f.name) << ",\"mode\":" << mode
                 << ",\"rays\":" << cases.size() << ",\"all_closest_fields_exact\":true}\n";
        }
    }
}
