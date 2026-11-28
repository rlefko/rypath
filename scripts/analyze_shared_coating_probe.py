#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Paired scalar/shared hidden-walk mean checks and local spectral variance."""
import argparse
from array import array
import csv
import hashlib
import json
import math
from pathlib import Path
import struct
import sys


def reduce(root, source, population_prefix="shared-probe"):
    assert sys.byteorder == 'little'
    trace = struct.unpack('<16384f', (root/'evaluation.bin').read_bytes())
    wavelengths, q = trace[260:264], trace[264:268]
    emission = trace[256*5+132:256*5+136]
    cie = {int(r[0]): tuple(float(x) for x in r[1:4])
           for r in csv.reader((source/'third_party/cie/CIE_xyz_1931_2deg.csv').open())}
    factors = [[v*e/(4*d) for v in cie[round(w)]] for w,e,d in zip(wavelengths,emission,q)]
    result = {'schema':1,'scope':'Local coated BSDF under fixed warm illumination; no visibility, reuse, reconstruction or native scene variance claim',
              'protocol':{'seeds':list(range(89001,89009)),'samples_per_seed':32768,
                          'mean_gate':'Every paired lane mean absolute delta <= 5 seed-cluster standard errors + 0.5% pooled mean + 1e-7; no reruns/outlier removal',
                          'variance':'Unbiased sample variance of spectral/XYZ/chroma output; old collapse hero integrated analytically over all four posterior probabilities'},
              'cases':[]}
    for angle in range(2):
        for black in range(2):
            path=root/f'{population_prefix}-angle{angle}-black{black}.bin'
            data=array('f');data.frombytes(path.read_bytes());assert len(data)==8*8*32768
            clusters=[]
            for seed in range(8):
                a=[0.0]*4;b=[0.0]*4
                for index in range(seed*32768,(seed+1)*32768):
                    row=data[index*8:index*8+8]
                    assert all(math.isfinite(x) and x>=0 for x in row)
                    for k in range(4):a[k]+=row[k]/32768;b[k]+=row[k+4]/32768
                clusters.append((a,b))
            gates=[]
            for k in range(4):
                diff=[a[k]-b[k] for a,b in clusters];mean=sum(diff)/8
                se=math.sqrt(sum((d-mean)**2 for d in diff)/(8*7))
                shared=sum(a[k] for a,b in clusters)/8;scalar=sum(b[k] for a,b in clusters)/8
                threshold=5*se+.005*abs((shared+scalar)/2)+1e-7
                gates.append({'lane':k,'shared_mean':shared,'scalar_mean':scalar,'delta':mean,'cluster_SE':se,'threshold':threshold,'passed':abs(mean)<=threshold})
            metrics=[]
            for posterior in [(0.25,)*4,(16/27,8/27,2/27,1/27)]:
                count=8*32768
                total_new=[0.0]*3;total_old=[0.0]*3;second_new=[0.0]*3;second_old=[0.0]*3
                for index in range(count):
                    row=data[index*8:index*8+8]
                    channels_new=[sum(row[k]*factors[k][c] for k in range(4)) for c in range(3)]
                    old_lanes=[[row[k+4]*factors[k][c] for c in range(3)] for k in range(4)]
                    # Opponent signals are linear, so the exact hero-mixture
                    # second moment is computed before summing color lanes.
                    transforms=[lambda v:sum(x*x for x in v),lambda v:(v[0]-v[1])**2+(v[2]-v[1])**2]
                    old_mean=[sum(v[c] for v in old_lanes) for c in range(3)]
                    for c in range(3):
                        total_new[c]+=channels_new[c]/count;total_old[c]+=old_mean[c]/count
                    for j,fn in enumerate(transforms):
                        second_new[j]+=fn(channels_new)/count
                        second_old[j]+=sum(fn(v)/p for v,p in zip(old_lanes,posterior))/count
                variance=[]
                for j,fn in enumerate(transforms):
                    new=(second_new[j]-fn(total_new))*count/(count-1)
                    old=(second_old[j]-fn(total_old))*count/(count-1)
                    variance.append({'signal':['XYZ','opponent_chroma'][j],'shared':new,'hero_collapse':old,'ratio':new/old})
                metrics.append({'hero_posterior':posterior,'shared_XYZ_mean':total_new,'scalar_XYZ_mean':total_old,'variance':variance})
            rr_complements=sum(1 for i in range(8*32768) for k in range(4)
                               if data[8*i+k]>0 and data[8*i+k+4]==0)
            result['cases'].append({'common_walk_survives_scalar_killed':rr_complements,'angle':angle,'black_hero':bool(black),'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'lane_mean_gates':gates,'metrics':metrics})
    result['passed']=all(g['passed'] for c in result['cases'] for g in c['lane_mean_gates'])
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('evidence',type=Path)
    parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--population-prefix',default='shared-probe')
    args=parser.parse_args();result=reduce(args.evidence,args.source,args.population_prefix)
    with args.output.open('x') as out:json.dump(result,out,indent=2);out.write('\n')
    print('Shared coating mean gate:',result['passed'])
    if not result['passed']:raise SystemExit(1)
