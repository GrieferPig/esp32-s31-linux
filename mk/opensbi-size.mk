# Loaded after the unmodified native OpenSBI Makefile via GNU Make -f.
# Retain all native ABI/ISA/freestanding flags and make -Os effective last.
override CFLAGS += -Os
