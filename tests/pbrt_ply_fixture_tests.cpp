// SPDX-License-Identifier: GPL-3.0-only
#include "pbrt_render.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 2, "Expected fixture directory");
        for (const auto name : {"ply-external", "ply-mixed", "ply-curved-emitter"}) {
            const auto path = std::filesystem::path{argv[1]} / (std::string{name} + ".pbrt");
            const auto checked = rypath::pbrt::check(path);
            require(checked.exit_code() == 0 && checked.scene &&
                        checked.document.dependencies_complete,
                    rypath::pbrt::report(checked));
            require(checked.document.dependencies.size() == 1 &&
                        checked.document.dependencies.front().visited,
                    "PLY fixture must have exactly one checked input dependency");
            auto translated = rypath::pbrt::translate(checked, {path.string()});
            for (const auto &issue : translated.diagnostics) {
                std::cerr << *issue.location.file << ':' << issue.location.line << ": "
                          << issue.message << '\n';
            }
            require(translated.exit_code() == 0 && translated.scene,
                    "PLY fixture must translate into renderer data");
            const auto &scene = *translated.scene;
            std::size_t triangles = 0, patches = 0;
            for (const auto &mesh : scene.geometry.meshes) {
                triangles += mesh.triangles.size();
                patches += mesh.patches.size();
            }
            require(patches == 1, "PLY quad must remain one bilinear patch");
            const std::string_view label{name};
            if (label == "ply-external") {
                require(triangles == 0 && scene.geometry.meshes.size() == 1 &&
                            scene.geometry.meshes.front().positions.size() == 4 &&
                            scene.geometry.meshes.front().normals.size() == 4,
                        "External quad geometry or normals changed");
            } else if (label == "ply-mixed") {
                require(triangles == 1 && scene.geometry.meshes.size() == 1 &&
                            scene.geometry.meshes.front().normals.size() == 7 &&
                            scene.geometry.meshes.front().uvs.size() == 7,
                        "Mixed PLY geometry or attributes changed");
                auto repeated = checked;
                auto &geometry = repeated.scene->geometry;
                geometry.meshes.front().normals.clear();
                geometry.meshes.front().uvs.clear();
                const auto instance = geometry.instances.front();
                geometry.instances.assign(300, instance);
                rypath::pbrt::Limits limits;
                limits.total_values = 1024;
                const auto bounded = rypath::pbrt::translate(repeated, {path.string()}, limits);
                require(bounded.exit_code() == 1 &&
                            std::ranges::any_of(bounded.diagnostics,
                                                [](const auto &issue) {
                                                    return issue.code == "limit-exceeded" &&
                                                           issue.message.find("patch validation") !=
                                                               std::string::npos;
                                                }),
                        "Reused meshes must not amplify per-placement patch validation work");
            } else {
                require(triangles == 2 && scene.geometry.meshes.size() == 2 &&
                            scene.area_emissions.size() == 1,
                        "Curved light fixture lost the receiver or emission binding");
            }
            require(translated.settings.width == 8 && translated.settings.height == 8 &&
                        translated.settings.samples == 4096 && translated.settings.seed == 0,
                    "Comparison settings changed");
        }
    } catch (const std::exception &error) {
        std::cerr << "PLY fixture failure: " << error.what() << '\n';
        return 1;
    }
}
