#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Independent exact-rational finite-state oracle, not renderer/GPU evidence."""
from fractions import Fraction as Q
from itertools import product
import struct
import unittest


def pool(scores, states, target, vectors):
    total = sum(scores)
    conditional = [sum(s * vectors[u][c] / target[u] for s, u in zip(scores, states)) for c in range(3)]
    selected = [(s / total, [total * x / target[u] for x in vectors[u]], u) for s, u in zip(scores, states)]
    return selected, conditional


def moments(pools, target, vectors):
    mean = [Q(0)] * 3
    second = rb_second = Q(0)
    law = [Q(0)] * 2
    for probability, scores, states in pools:
        choices, conditional = pool(scores, states, target, vectors)
        for c in range(3):mean[c] += probability * conditional[c]
        rb_second += probability * sum(x*x for x in conditional)
        for p, output, u in choices:
            second += probability * p * sum(x*x for x in output)
            law[u] += probability * p
    return mean, second, rb_second, law


class ExactReuseOracle(unittest.TestCase):
    def test_random_normalizer_multigeneration_reuse_and_MH(self):
        # Cold weighted states have Z=t(U), not a constant expected normalizer.
        # Keep the complete joint law of payload and randomZ across generations.
        old = [Q(3),Q(7)]
        distribution = {(u,old[u]):Q(1,2) for u in range(2)}
        vectors = [[Q(3),Q(-4),Q(1)],[Q(-1),Q(8),Q(7)]]
        truth = [Q(1),Q(2),Q(4)]
        def check(law,target):
            mean = [sum(p*Z*vectors[u][c]/target[u] for (u,Z),p in law.items()) for c in range(3)]
            self.assertEqual(sum(law.values()),1)
            self.assertEqual(mean,truth)
            return sum(p*sum((Z*x/target[u])**2 for x in vectors[u]) for (u,Z),p in law.items())
        check(distribution,old)
        for target in ([Q(11),Q(2)],[Q(1,13),Q(17)],[Q(5),Q(3)]):
            next_law = {}
            for (h,Z),p in distribution.items():
                for fresh in range(2):
                    scores = [target[fresh]**2/(target[fresh]+20*old[fresh]),
                              Z*20*target[h]/(target[h]+20*old[h])]
                    total = sum(scores)
                    for u,s in zip((fresh,h),scores):
                        key = (u,total)
                        next_law[key] = next_law.get(key,Q(0))+p*Q(1,2)*s/total
            before = check(next_law,target)
            moved = {}
            for (u,Z),p in next_law.items():
                v = 1-u
                accept = min(Q(1),target[v]/target[u])
                # Exact symmetric MH changes payload; ideal normalizerZ stays.
                for state,probability in (((v,Z),accept),((u,Z),1-accept)):
                    if probability:moved[state] = moved.get(state,Q(0))+p*probability
            after = check(moved,target)
            self.assertGreater(before,0)
            self.assertGreater(after,0)  # No unsupported universal MSE claim.
            self.assertGreater(len({Z for _,Z in moved}),1)
            distribution,old = moved,target

    def test_extreme_support_floor_HDR_and_rare_tail_cancellation(self):
        # Binary32 normal extremes, evaluated exactly as rational numbers.
        low = Q(1,2**126)
        high = Q(2**24-1)*2**104
        previous = [low,high]
        current = [high,low]  # Completely opposite importance/visibility.
        Z = sum(previous)/2
        vectors = [[Q(0),Q(-3),Q(0)],[Q(7),Q(0),Q(11)]]
        truth = [Q(7,2),Q(-3,2),Q(11,2)]
        contributions = []
        rare_mass = Q(0)
        for f,h in product(range(2),repeat=2):
            p = Q(1,2)*previous[h]/(2*Z)
            scores = [current[f]**2/(current[f]+20*previous[f]),
                      Z*20*current[h]/(current[h]+20*previous[h])]
            total = sum(scores)
            for u,s in zip((f,h),scores):
                branch = p*s/total
                self.assertGreater(branch,0)
                contributions.append((branch,[total*x/current[u] for x in vectors[u]]))
                if branch<Q(1,2**120):rare_mass+=branch
        mean = [sum(p*F[c] for p,F in contributions) for c in range(3)]
        self.assertEqual(mean,truth)
        self.assertLess(rare_mass,8*Q(1,2**120))
        self.assertGreater(sum(p*F[0] for p,F in contributions if p<Q(1,2**120)),1)
        # A small probability carries finite mass; never drop/clamp this tail.
        clipped_mean = [sum(p*F[c] for p,F in contributions if p>=Q(1,2**120)) for c in range(3)]
        self.assertNotEqual(clipped_mean,truth)
        # Several positive MIS scores are below binary32 normal range. This
        # exact support witness does NOT authorize silently returning zero: the
        # C++/GPU API must fail visibly before selection in that arithmetic domain.
        tiny_score = low*low/(low+20*high)
        self.assertGreater(tiny_score,0)
        self.assertLess(tiny_score,low)
    def test_temporal_partition_vector_moments_and_pool_dependent_negative_control(self):
        t, old = list(map(Q, (2, 9))), list(map(Q, (13, 1)))
        vectors = [[Q(3), Q(-4), Q(1)], [Q(-1), Q(8), Q(7)]]
        Z = sum(old) / 2
        pools, adaptive = [], []
        for f, h in product(range(2), repeat=2):
            p = Q(1, 2) * old[h] / (2*Z)
            def scores(a):return [t[f]**2 / (t[f]+a*old[f]), Z*a*t[h] / (t[h]+a*old[h])]
            pools.append((p, scores(20), (f,h)))
            adaptive.append((p, scores(1 if f == 0 else 20), (f,h)))
        mean, second, rb_second, law = moments(pools, t, vectors)
        self.assertEqual(mean, [Q(1), Q(2), Q(4)])
        self.assertLess(rb_second, second)
        self.assertEqual(sum(law), 1)
        self.assertNotEqual(moments(adaptive, t, vectors)[0], mean)
        # Wrong legacy /K at output divides every conditional mean by two.
        wrong = [sum(p*sum(s*vectors[u][c]/t[u] for s,u in zip(scores,U))/2 for p,scores,U in pools) for c in range(3)]
        self.assertEqual(wrong, [x/2 for x in mean])
        # Displaying the conditional mean does not change any selected-payload
        # branch or probability above. Thus history-law equality is exact.
        displayed_law = [sum(p*branch for p,scores,U in pools for branch,_,state in pool(scores,U,t,vectors)[0] if state==u) for u in range(2)]
        self.assertEqual(displayed_law, law)

    def test_spatial_opposite_visibility_independent_correlated_and_duplicate_sources(self):
        t = list(map(Q, (1, 11)))
        neighbors = [list(map(Q, p)) for p in ((20,1),(1,17),(Q(1,100),40),(13,Q(1,50)))]
        targets = [t]+neighbors
        vectors = [[Q(4),Q(-3),Q(0)],[Q(0),Q(7),Q(-2)]]
        def entry(probability, U):
            center = sum(t)/2
            scores = [sum(center*t[U[0]]/(4*(t[U[0]]+n[U[0]])) for n in neighbors)]
            scores += [(sum(n)/2)*t[u]/(4*(t[u]+n[u])) for n,u in zip(neighbors,U[1:])]
            return probability, scores, U
        independent = []
        for U in product(range(2), repeat=5):
            p = Q(1)
            for source,u in zip(targets,U):p *= source[u]/sum(source)
            independent.append(entry(p,U))
        cuts = sorted({Q(0), Q(1), *(s[0]/sum(s) for s in targets)})
        correlated = [entry(b-a,tuple(0 if (a+b)/2 < s[0]/sum(s) else 1 for s in targets)) for a,b in zip(cuts,cuts[1:])]
        for pools in (independent, correlated):
            mean, second, rb_second, law = moments(pools,t,vectors)
            self.assertEqual(mean,[Q(2),Q(2),Q(-1)])
            self.assertLessEqual(rb_second,second)
            self.assertEqual(sum(law),1)
        # If all neighbors are clamped copies of the center, pair scores areZ/8;
        # their four center terms plus four neighbor terms sum exactly toZ.
        self.assertEqual(4*Q(6,8)+4*Q(6,8),6)


if __name__ == '__main__':unittest.main()
