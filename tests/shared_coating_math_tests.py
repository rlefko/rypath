#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exact finite-measure oracle for a common coating proposal and hero collapse."""
from fractions import Fraction as F
import unittest


class CommonCoatingMeasure(unittest.TestCase):
    def test_common_hidden_walk_and_compensated_stopping(self):
        # These are enumerated path atoms, not the renderer's sampler/helper.
        q = [F(1, 4), F(1, 2), F(1, 4)]
        target = [[F(0), F(1, 4), F(1, 2), F(1, 4)],
                  [F(1, 4), F(1, 2), F(3, 4), F(1, 4)],
                  [F(0), F(0), F(1, 8), F(0)]]
        common_survival = [F(1, 2), F(3, 4), F(1, 8)]
        for posterior in [[F(1, 4)] * 4, [F(16, 27), F(8, 27), F(2, 27), F(1, 27)]]:
            for active in [1, 3, 15]:
                normalized = [p * bool(active & (1 << k)) for k, p in enumerate(posterior)]
                total = sum(normalized)
                normalized = [p / total for p in normalized]
                expected = [sum(row[k] for row in target) if active & (1 << k) else F(0)
                            for k in range(4)]
                shared = [F(0)] * 4
                scalar = [F(0)] * 4
                for atom, (mass, survival) in enumerate(zip(q, common_survival)):
                    for k, pi in enumerate(normalized):
                        if not pi:
                            continue
                        shared[k] += mass * survival * target[atom][k] / (mass * survival)
                        # Scalar roulette can differ from shared maximum-weight
                        # roulette. The stopped-path measure includes both gates.
                        scalar_survival = survival * F(k + 1, 4)
                        scalar[k] += pi * mass * scalar_survival * target[atom][k] / (
                            pi * mass * scalar_survival)
                self.assertEqual(shared, expected)
                self.assertEqual(scalar, expected)

    def test_nonuniform_prefix_common_density_cancels(self):
        prefix = [F(16), F(8), F(2), F(1)]
        physical = [F(1), F(2), F(0), F(3)]
        joint = F(7, 4096)
        numerator = [F(1, 8), F(1, 4), F(0), F(1, 2)]
        old_beta = [4 * x / sum(prefix) for x in physical]
        next_prefix = [p * joint for p in prefix]
        direct = [4 * x * n / sum(next_prefix) for x, n in zip(physical, numerator)]
        ratio = [b * n / joint for b, n in zip(old_beta, numerator)]
        self.assertEqual(direct, ratio)
        for shift in range(4):
            rotated = prefix[shift:] + prefix[:shift]
            light, bsdf = F(3, 19), F(2, 7)
            sampled = sum(p * light for p in rotated)
            other = sum(p * bsdf for p in rotated)
            self.assertEqual(sampled / (sampled + other), light / (light + bsdf))

    def test_black_hero_does_not_remove_companion_support(self):
        # A uniform diffuse direction proposal is common even when lane zero's
        # material response vanishes. The common draw still carries lane one.
        response = [F(0), F(3, 4), F(0), F(1, 8)]
        proposal = F(1, 2)
        for k in range(4):
            self.assertEqual(proposal * (response[k] / proposal), response[k])
        self.assertEqual(response[0], 0)
        self.assertGreater(response[1], 0)


if __name__ == '__main__':
    unittest.main()
