# Spectrum file comparison assets

These four synthetic spectra are original rypath fixtures under GPL-3.0-only.
Each file contains decimal wavelength/value pairs and a comment. Fractional
wavelengths and decimal values exercise binary32 parsing before interpolation.

| Asset | Bytes | SHA-256 | Inclusive wavelength support |
| --- | ---: | --- | --- |
| `sky.spd` | 97 | `c6360061b815b316b150e2fb81024018c0c8a247e35812947143d185d618ac34` | 420–680 nm |
| `warm.spd` | 101 | `a0718831400d8d54df6f09ae06cddd96532cf7c96b103485d799f2ddd315236b` | 475–790 nm |
| `cool.spd` | 104 | `5de463bf77a8bd539de6baf4bce04546a0d2ed1c41ab679d5b5a96d38b6694ae` | 395–640 nm, with an internal zero interval at 544–559 nm |
| `endpoint.spd` | 72 | `185483a44256f1da8ffe9074c0d724a90e61d7a8ac11a1298f1810d6a46a4e65` | 360–364 nm |

The reference image harness verifies and stages the complete closure recorded
in [dependencies.json](../dependencies.json) under each relocated entry.
The raw spectrum probes compare the 471 integer-wavelength samples of each
asset. The narrow `endpoint.spd` spectrum also isolates the half-width 360 nm
bin in the native spectral quadrature test.
