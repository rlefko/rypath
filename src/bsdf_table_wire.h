// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// All offsets are uint32 word offsets relative to this table's start.
// Kind 1: Fourier: nMu, mMax, channels, eta, mu, cdf, offset/length, coefficients.
// Kind 2: measured: phi count, theta count, wavelengths count, isotropic,
// phi, theta, wavelengths, then descriptors ndf/sigma/vndf/luminance/spectra.
// A measured descriptor occupies four words: width, height, slices, data offset.
#define RYPATH_TABLE_KIND 0
#define RYPATH_TABLE_HEADER_WORDS 32
#define RYPATH_TABLE_FOURIER 1
#define RYPATH_TABLE_MEASURED 2
#define RYPATH_TABLE_NDF 8
#define RYPATH_TABLE_SIGMA 12
#define RYPATH_TABLE_VNDF 16
#define RYPATH_TABLE_LUMINANCE 20
#define RYPATH_TABLE_SPECTRA 24
