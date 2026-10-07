# Robust writes and quiet console setup
Compiler-control setup failed before radio loading with a truncated echoed
command (logs/compiler-optimization/runs/setup/configuration.raw).
The serial sender previously ignored the byte count returned by os.write.

New console_io.write_paced checks all write counts, retries BlockingIOError,
bounds write time, and sends16-byte chunks with10ms pauses. Reads remain full
speed. board_command, board_console and pull_pklg_chunk use it; raw115200/CR
and the1024-byte BusyBox command guard remain unchanged.
Three unit tests pass: short writes plus EAGAIN preserve all bytes, zero writes
fail, and write timeout is bounded.

Pacing alone did not solve the observed console stall: after hardware reset,
one600-byte payload command passed, then the second stopped mid-echo and
timed out. Ctrl-C/probe before that reset also received no response.
This is not evidence that all stalls were caused by host short writes.

A new hardware reset, login, then "stty -echo; exec /bin/sh +i" suppressed
per-character command echo. The shell still printed a prompt, so this is
described as a quiet/no-echo setup, not proven noninteractive mode.
Eight separately opened commands each returned an exact600-byte payload,
zero missing/corrupt bytes, with monotonically advancing board uptime.
The first checker rejected command1 because it required PAYLOAD at column0;
the retained raw response had a prompt prefix and was valid. Matching the
PAYLOAD content verified both existing responses and six further commands.

Use this quiet setup for the next optimization matrix. No throughput result
is claimed here, and the underlying console-stall cause remains unproved.
