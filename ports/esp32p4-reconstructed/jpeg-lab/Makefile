# SPDX-License-Identifier: GPL-2.0-only
obj-m += p4-jpeg-lab.o

targets += vector.h
quiet_cmd_jpeg_vectors = GEN     $@
      cmd_jpeg_vectors = python3 $(src)/gen.py > $@.tmp && mv $@.tmp $@

$(obj)/vector.h: $(src)/gen.py FORCE
	$(call if_changed,jpeg_vectors)

$(obj)/p4-jpeg-lab.o: $(obj)/vector.h
ccflags-y += -I$(obj)
