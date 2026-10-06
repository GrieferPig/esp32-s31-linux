# STEP 0 — repo readiness (bt-latency-fix, 2026-10-06)

## Branch
- Created `bt-latency-fix` from `main` (was at 1c132b4). `git status -sb` shows
  `## bt-latency-fix`.

## Submodules
- Initial `git submodule status` showed:
  - `buildroot` at different checkout (prefix `+`), `docs` ok,
    `linux-esp32-s31` empty worktree (only `.git` file) with broken
    `.git/modules/linux-esp32-s31/HEAD` (`ref: refs/heads/.invalid`, no refs),
  - `opensbi-esp32-s31` and `u-boot-esp32-s31` uninitialized (prefix `-`).
- Ran `git submodule update --init --recursive`. Kernel fetch is large;
  first attempt timed out on `git -C linux-esp32-s31 fetch --all`, so fetched
  `origin v6.18-esp32-s31` in background, then re-ran update. Final status:
  - `buildroot cb857ba (2026.05.1)`, `docs 45e0882`,
    `linux-esp32-s31 52dc6ea (v6.18-29-g52dc6ea)`,
    `opensbi 344cfcb (v1.9-29-g344cfcb2)`, `u-boot 5806e42`.
  - `opensbi`/`u-boot` worktrees were empty with deleted-files dirt; fixed with
    `git submodule update --init --force -- opensbi-esp32-s31 u-boot-esp32-s31`.
  - Final `git submodule status` shows no `-`/`+` prefixes.

## Doctor (readiness gate)
- `make doctor` fails: missing
  `cache/toolchains/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-gcc`.
  Doctor also checks host tools + IDF `export.sh`. IDF at `$HOME/esp-idf`
  exists but is `08e0d30a` vs pinned `a602e67b` (see below). Toolchain must be
  fetched via `make fetch` before `make build`.
- `local.mk` (gitignored) created with `S31_ALLOW_UNPINNED=1`.
- IDF choice (recorded per mission): **used `S31_ALLOW_UNPINNED=1` with the
  existing `$HOME/esp-idf` checkout (`08e0d30a`, ESP-IDF 6.2)** instead of a
  scratch pinned clone. Reason: pinned `a602e67b` would need a full second
  IDF clone; the experiment explicitly allows unpinned with this flag.
  Pinned ref: `ESP_IDF_REF=a602e67b0bf9ee0806dc4e1df7afc9affedf5c33`
  (configs/build-versions.mk). Local: `08e0d30a74ad0bfd5a34933142b80f45619ee410`.

## Fetch / build
- `cache/sources/btstack` fetched via `tools/build/fetch_btstack.sh`
  (BTSTACK_REF=431d58d5, background job completed, `a2dp_sink_demo.c` present).
- Full `make fetch` (toolchain + buildroot downloads) and `make build`
  (uboot/linux/rootfs/radio-image) NOT yet completed at this commit; they run
  after this checkpoint (see NEXT). Board under test still runs its Oct-4
  factory image (Linux 6.18.0 #38, see bt-latency/logs/before-*.log).
