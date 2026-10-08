# Experimental publication snapshot — 2026-10-07

This branch publishes the existing Bluetooth investigation history and the
previously uncommitted source snapshot. It preserves Linux scheduling,
compatibility kthreads and the ordered radio workqueue.

## Source and validation

Kernel commit: 030d4b9d6d75826636f9f004577fcf0ab24169d6.
Parent base: a291b4578d9d2081e94d037f97d7614d5dd962a6. Kernel base: 7a76873da67f2b47e7fcca1ea1f05868858cb878.
The 11 changed kernel files passed a clean patch -p1 apply and whitespace
checks. All 39 shipped BTstack patches applied to a scratch source copy.
Six flash safety unit tests passed. This publication did not flash the board,
resume the hardware mission, or run new radio benchmarks.

These checks do not establish final runtime correctness. The retained
image-v2/extension-hil.log contains failed CPU0 HWLoop and CPU1 PIE result
assertions. The earlier extension instruction/context test passed, but it
preceded the revised default-on initial-OFF policy. The later throughput matrix
exists despite this failure and does not supersede extension correctness tests.

Direct-HCI writes in this kernel require the 0xff batch envelope. Use the
matching BTstack patch. Legacy unbatched writers, including the transport's
S31_BTSTACK_TX_BATCH=0 fallback path, are not compatible with this snapshot.
The CPU/timing evidence is experimental and must not be read as a proved
production CPU reduction or complete ESP-IDF parity.

## Evidence

linux-cpu-retain-sched.tar.gz and cpu-txbatch.tar.gz contain point-in-time
copies of raw runs, exact commands, profiles, source snapshots, build/flash
bindings, test scripts and results. Failed and incomplete runs remain evidence.
A run may have been in progress when its files were captured.

manifest.json records every archived file's SHA256 and each archive's SHA256.
Archives were reopened and every file hash checked before committing.
Scratch working copies, Python bytecode and the host candidate-test executable
are excluded; source and recorded test output remain included.
Selected small reports and reproduction scripts are also available under
readable/. Other pre-existing ignored logs remain local.

Extract each archive in an empty directory with tar -xzf ARCHIVE.tar.gz.
The historical BT_DEBUG_MISSION.md is retained as supplied; its older UART
baud and workflow statements are not current operational instructions.
Current board console requires raw termios at 115200 and CR-terminated commands.
Never write the persist partition.
