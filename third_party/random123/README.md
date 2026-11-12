# Random123 Philox

The CPU and Metal samplers implement Philox4x32-10, described by John K.
Salmon, Mark A. Moraes, Ron O. Dror, and David E. Shaw in [Parallel Random
Numbers: As Easy as 1, 2, 3](https://doi.org/10.1145/2063384.2063405), SC11.
The constants, round function, and test vectors were checked against
[Random123 v1.14.0](https://github.com/DEShawResearch/random123/tree/726a093cd9a73f3ec3c8d7a70ff10ed8efec8d13).
The three Philox4x32-10 known-answer vectors come from the release's
[`tests/kat_vectors`](https://github.com/DEShawResearch/random123/blob/726a093cd9a73f3ec3c8d7a70ff10ed8efec8d13/tests/kat_vectors).

Copyright 2010–2012, D. E. Shaw Research. See [license text](LICENSE.txt).
