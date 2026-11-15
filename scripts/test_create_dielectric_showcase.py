#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check authored glass mesh topology and anisotropic frames without rendering."""
from collections import defaultdict
import math
import re
import unittest

from create_dielectric_showcase import bevel_block, prism


def subtract(a, b):
    return tuple(x - y for x, y in zip(a, b))


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return (a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2],
            a[0]*b[1] - a[1]*b[0])


def normalized(vector):
    length = math.sqrt(dot(vector, vector))
    return tuple(x / length for x in vector)


class ShowcaseMeshTests(unittest.TestCase):
    def parse_mesh(self, source):
        def array(parameter, width, convert):
            matches = re.findall(r'"' + re.escape(parameter) + r'"\s*\[([^]]*)\]', source)
            self.assertEqual(len(matches), 1, f'expected one {parameter} array')
            values = [convert(value) for value in matches[0].split()]
            self.assertEqual(len(values) % width, 0)
            self.assertTrue(values)
            return [tuple(values[i:i+width]) for i in range(0, len(values), width)]

        points = array('point3 P', 3, float)
        uvs = array('point2 uv', 2, float)
        triangles = array('integer indices', 3, int)
        self.assertEqual(len(points), len(uvs))
        for triangle in triangles:
            self.assertTrue(all(0 <= index < len(points) for index in triangle))
        self.assertTrue(all(math.isfinite(x) for value in points + uvs for x in value))
        return points, uvs, triangles

    def test_face_fans_have_nondegenerate_consistent_horizontal_uv_frames(self):
        for make_mesh, face_count in ((bevel_block, 10), (prism, 5)):
            with self.subTest(mesh=make_mesh.__name__):
                points, uvs, triangles = self.parse_mesh(make_mesh())
                planes = []
                for triangle in triangles:
                    a, b, c = (points[i] for i in triangle)
                    uv_a, uv_b, uv_c = (uvs[i] for i in triangle)
                    ab, ac = subtract(b, a), subtract(c, a)
                    normal = normalized(cross(ab, ac))
                    du_b, dv_b = subtract(uv_b, uv_a)
                    du_c, dv_c = subtract(uv_c, uv_a)
                    determinant = du_b*dv_c - dv_b*du_c
                    self.assertGreater(abs(determinant), 1e-8, 'degenerate UV Jacobian')
                    # Recover the world-space derivative from emitted geometry/UVs,
                    # independently of the generator's choice of parameterization.
                    dpdu = normalized(tuple((dv_c*x - dv_b*y) / determinant
                                            for x, y in zip(ab, ac)))
                    self.assertAlmostEqual(dot(normal, dpdu), 0, delta=1e-6)
                    self.assertAlmostEqual(dpdu[1], 0, delta=1e-6)
                    if abs(normal[1]) > 1 - 1e-6:
                        self.assertGreater(dpdu[0], 1 - 1e-6)
                    else:
                        self.assertGreater(dot(cross(dpdu, (0, 1, 0)), normal), 0)

                    offset = dot(normal, a)
                    for plane_normal, plane_offset, tangents in planes:
                        if (dot(normal, plane_normal) > 1 - 1e-6
                                and abs(offset - plane_offset) < 1e-6):
                            tangents.append(dpdu)
                            break
                    else:
                        planes.append((normal, offset, [dpdu]))

                self.assertEqual(len(planes), face_count)
                self.assertTrue(any(len(tangents) > 1 for _, _, tangents in planes))
                for _, _, tangents in planes:
                    for tangent in tangents[1:]:
                        self.assertGreater(dot(tangents[0], tangent), 1 - 1e-6,
                                           'anisotropic frame changes across a face fan')

    def test_closed_meshes_keep_opposite_edge_pairs_and_outward_winding(self):
        for make_mesh, vertex_count, triangle_count in ((bevel_block, 16, 28), (prism, 6, 8)):
            with self.subTest(mesh=make_mesh.__name__):
                points, _, triangles = self.parse_mesh(make_mesh())
                # Weld by emitted position: face-local UVs may require separate indices
                # at a corner, but cannot introduce a geometric boundary or crack.
                unique_points = set(points)
                self.assertEqual(len(unique_points), vertex_count)
                self.assertEqual(len(triangles), triangle_count)
                center = tuple(sum(p[k] for p in unique_points) / len(unique_points)
                               for k in range(3))
                edges = defaultdict(list)
                for triangle in triangles:
                    a, b, c = (points[i] for i in triangle)
                    normal = cross(subtract(b, a), subtract(c, a))
                    self.assertGreater(dot(normal, subtract(a, center)), 1e-8,
                                       'triangle must point out of the convex solid')
                    for start, end in ((a, b), (b, c), (c, a)):
                        self.assertNotEqual(start, end)
                        edges[tuple(sorted((start, end)))].append((start, end))
                for paired in edges.values():
                    self.assertEqual(len(paired), 2, 'geometric edge must have two incident triangles')
                    self.assertEqual(paired[0], paired[1][::-1], 'edge winding must oppose')
                self.assertEqual(len(unique_points) - len(edges) + len(triangles), 2)


if __name__ == '__main__':
    unittest.main()
