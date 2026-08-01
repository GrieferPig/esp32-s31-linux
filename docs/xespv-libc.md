# XEspV musl memory/string acceleration

The S31 crosstool-NG build carries a RISC-V-specific musl patch for the
routines whose assembly prototypes measured at least 1.1x faster on hardware:
`memcpy`, `memchr`, `memrchr`, `memcmp`, `strnlen`, and `strcmp`.  `strnlen`
inherits the accelerated path through musl's existing call to `memchr`.

Bounded operations enter XEspV only when at least 64 bytes remain.  Shorter
inputs use scalar code.  `memcpy` additionally requires matching source and
destination alignment, copies the alignment prefix scalarly, and sends only
whole 64-byte blocks to its four-load/four-store XEspV kernel.  Since
`strcmp` has no length argument, it first checks 64 equal, non-NUL bytes with
scalar loads, then aligns both pointers before entering the vector loop.
Vector loads operate only on aligned, complete 16-byte blocks, so unbounded
string loads cannot cross a page boundary.

The XEspV `memset` prototype remains in the benchmark but is not integrated
into musl: on 256 and 512 KiB target buffers it measured 1.00x against its
scalar counterpart.  XEspV `memcpy` measured 1.10x to 1.11x on those same
large working sets and is retained.

Buildroot also packages the isolated assembly prototypes into
`s31-string-bench`; their symbols use the `s31_xespv_` prefix and therefore do
not interpose on libc.

The assembly library covers `memcpy`, `memset`, `memmove`, `memchr`,
`memrchr`, `memcmp`, `memccpy`, `strlen`, `strnlen`, `strchrnul`, `strchr`,
`strcmp`, `stpcpy`, `strcpy`, `stpncpy`, `strncpy`, and `strlcpy`.  Composite
copy functions reuse the accelerated primitive routines.  Vector loads and
stores are used only for aligned complete 16-byte blocks; scalar code handles
alignment, tails, terminators, mismatches, and overlapping move edges.

Run `/usr/sbin/s31-string-bench` on the target.  It compares all 17 prefixed
implementations with musl over aligned and misaligned lengths, tails, and
overlaps, then benchmarks both over the same 256 KiB buffers.  For the five
promoted functions, the `libc` column measures the integrated, gated musl
implementation and the `XEspV` column measures the original isolated
prototype.
