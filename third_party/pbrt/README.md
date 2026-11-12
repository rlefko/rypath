# PBRT v4 parser source

`src/pbrt_parser.cpp` adapts the tokenizer, escape decoding, directive grammar,
and Include tokenizer-stack behavior from
[pinned PBRT v4 parser.cpp](https://github.com/mmp/pbrt-v4/blob/b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f/src/pbrt/parser.cpp)
at revision `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`.

PBRT is Copyright (c) 1998–2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
The adapted file retains the Apache License, Version 2.0. See [LICENSE.txt](LICENSE.txt).

rypath modifications include owned source locations and tokens, typed values with
original numeric lexemes, locale-independent decimal conversion, bounded reads
and parsing, and preflight diagnostics. The pinned implementation spells
three-component normals `normal`. rypath also accepts `normal3`, the spelling
listed in the [format reference](https://www.pbrt.org/fileformat-v4), and retains
the authored spelling. Both the pinned implementation's `Option "name" value`
and the documented `Option "type name" value` are accepted. The parser is
standalone C++20 with no PBRT linkage.
