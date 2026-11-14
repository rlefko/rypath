# Spectrum file comparison assets

These synthetic spectra are original rypath fixtures under GPL-3.0-only.
Each file contains decimal wavelength/value pairs and a comment. Fractional
wavelengths and decimal values exercise binary32 parsing before interpolation.

| Asset | Bytes | SHA-256 | Inclusive wavelength support |
| --- | ---: | --- | --- |
| `sky.spd` | 97 | `c6360061b815b316b150e2fb81024018c0c8a247e35812947143d185d618ac34` | 420–680 nm |
| `warm.spd` | 101 | `a0718831400d8d54df6f09ae06cddd96532cf7c96b103485d799f2ddd315236b` | 475–790 nm |
| `cool.spd` | 104 | `5de463bf77a8bd539de6baf4bce04546a0d2ed1c41ab679d5b5a96d38b6694ae` | 395–640 nm, with an internal zero interval at 544–559 nm |
| `endpoint.spd` | 72 | `185483a44256f1da8ffe9074c0d724a90e61d7a8ac11a1298f1810d6a46a4e65` | 360–364 nm |
| `paint.spd` | 107 | `5e781838297b7540ceeebef40b453f5c098881670e0834a977a004b1e9d5bc45` | 405.5–735.25 nm, with negative and above-one values |
| `clamp.spd` | 59 | `8d5eca7f673e3a87a08633ee7eec66886c793c2ad3a18a8fe7337e4c087c9722` | 499.25–500.75 nm, ramping from −1 to 2 |
| `narrow.spd` | 74 | `52456b9b9c3ab21c67b4ea0e4360ddafe898cc4ac632480ebd31a548b91b4f60` | 503.25–503.75 nm |
| `wide.spd` | 126 | `ae209f1f01f6151a1351fe6a8d5a69ffb596e5b9a623af99eac0e0374e04b298` | −2¹²⁰ to 2¹²⁰ nm, ramping from −2¹¹¹ to 2¹¹¹ |

The reference image harness verifies and stages the complete closure recorded
in [dependencies.json](../dependencies.json) under each relocated entry.
The emission spectrum probes compare the 471 integer-wavelength samples of
the first four assets. The narrow `endpoint.spd` spectrum also isolates the
half-width 360 nm bin in the native spectral quadrature test. The remaining
assets cover diffuse reflectance after interpolation and clamping. The
native Metal controls use `clamp.spd`, `narrow.spd`, and `wide.spd`.
