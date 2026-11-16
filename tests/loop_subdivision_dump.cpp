// SPDX-License-Identifier: GPL-3.0-only
#include "loop_subdivision.hpp"

#include <iomanip>
#include <iostream>
#include <limits>

int main() try {
    using namespace rypath;
    int levels = 0;
    std::size_t vertices = 0, indices = 0;
    if (!(std::cin >> levels >> vertices >> indices) || vertices > 1'000'000 ||
        indices > 2'000'000 || indices % 3 != 0) {
        std::cerr << "Invalid bounded Loop probe header\n";
        return 1;
    }
    GeometryMesh control;
    control.positions.resize(vertices);
    control.triangles.resize(indices / 3);
    for (auto &p : control.positions)
        if (!(std::cin >> p.x >> p.y >> p.z)) {
            std::cerr << "Invalid Loop probe position\n";
            return 1;
        }
    for (auto &t : control.triangles)
        for (auto &index : t)
            if (!(std::cin >> index)) {
                std::cerr << "Invalid Loop probe index\n";
                return 1;
            }
    std::cin >> std::ws;
    if (!std::cin.eof()) {
        std::cerr << "Unexpected trailing Loop probe data\n";
        return 1;
    }
    const auto result = subdivide_loop(control, levels);
    if (!result.mesh) {
        std::cerr << result.diagnostic << '\n';
        return 1;
    }
    const auto &mesh = result.mesh.value();
    std::cout << std::setprecision(std::numeric_limits<float>::max_digits10);
    std::cout << "{\"indices\":[";
    bool first = true;
    for (const auto &t : mesh.triangles)
        for (auto i : t) {
            if (!first)
                std::cout << ',';
            first = false;
            std::cout << i;
        }
    const auto points = [](const auto &values) {
        bool first_point = true;
        for (const auto &p : values) {
            if (!first_point)
                std::cout << ',';
            first_point = false;
            std::cout << '[' << p.x << ',' << p.y << ',' << p.z << ']';
        }
    };
    std::cout << "],\"P\":[";
    points(mesh.positions);
    std::cout << "],\"N\":[";
    points(mesh.normals);
    std::cout << "]}\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
