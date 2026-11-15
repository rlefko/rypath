# Owned Surface Material Reference Scenes

These GPL-3.0-only inline triangle scenes isolate six PBRT v4 material families.
Both renderers receive identical source, 8×8 linear RGB CIE1931 film, box filter,
4096 independent samples per pixel, and path maxdepth 5. The runner uses separate
seed sets and the established five-combined-standard-error regional gate.
Coatings retain matching finite maxdepth/nsamples; agreement is measured rather
than inferred from parser acceptance. The measured scene is an owned wrapper
around an unmodified externally cached tensor table supplied to the runner; its
hash and provenance are recorded separately. No table asset is stored here.
Legacy Fourier is excluded because pinned PBRT v4 has no Fourier material.
