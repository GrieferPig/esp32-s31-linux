#!/usr/bin/env python3
"""Check the shared SRAM contract and derive flash capacities from the slot map."""
import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SLOTS = ("SPL", "UBOOT_ITB", "DTB", "RADIO", "PERSIST", "KERNEL", "ROOTFS")

def flash_layout(root=ROOT):
    text = (root / "configs/esp32s31-layout.cfg").read_text()
    values = {k: int(v, 0) for k, v in re.findall(r"^(SLOT_\w+|SIZE_\w+|FLASH_SIZE)=(0x[0-9a-fA-F]+|[0-9]+)$", text, re.M)}
    if {key for key in values if key.startswith("SLOT_")} != {"SLOT_" + key for key in SLOTS}:
        raise ValueError("flash layout must contain exactly the seven compact slots")
    points = [values["SLOT_" + key] for key in SLOTS] + [values["FLASH_SIZE"]]
    if any(a >= b or a < 0 or a % 0x2000 for a, b in zip(points, points[1:])):
        raise ValueError("flash slots must be increasing and erase-block aligned")
    sizes = dict(zip(SLOTS, (b - a for a, b in zip(points, points[1:]))))
    sizes["SPL"] = values["SIZE_SPL"]
    expected = {"SPL": 48 * 1024, "UBOOT_ITB": 320 * 1024,
                "DTB": 64 * 1024, "RADIO": 1536 * 1024,
                "KERNEL": 6144 * 1024, "ROOTFS": 6144 * 1024,
                "PERSIST": 2120 * 1024}
    if sizes != expected or values["SLOT_SPL"] != 0x2000:
        raise ValueError("compact flash partition capacities differ from contract")
    if values["SLOT_UBOOT_ITB"] - (values["SLOT_SPL"] + sizes["SPL"]) != 0:
        raise ValueError("SPL and FIT must be contiguous without alignment padding")
    if values["FLASH_SIZE"] != 0x1000000 or values["SLOT_KERNEL"] != 0x400000:
        raise ValueError("Linux XIP must be 4 MiB aligned within 16 MiB flash")
    return values, sizes

def defines(text):
    result = {}
    for key, value in re.findall(r"^#define\s+(S31_\w+)\s+(\S+)", text, re.M):
        if re.fullmatch(r"0x[0-9a-fA-F]+[UuLl]*", value):
            result[key] = int(value.rstrip("UuLl"), 0)
        elif value in result:
            result[key] = result[value]
    return result

def check(root=ROOT):
    slots, sizes = flash_layout(root)
    shared = defines((root / "shared/s31_memory_layout.h").read_text())
    radio = defines((root / "linux-esp32-s31/drivers/platform/esp32s31-radio-smode.c").read_text())
    for key in ("HEAP_BASE", "HEAP_END", "HEAP_LOW_BASE", "HEAP_LOW_END", "HEAP2_BASE", "HEAP2_END"):
        name = "S31_RADIO_" + key
        if radio[name] != shared[name]:
            raise ValueError(name + " differs between shared contract and radio driver")
    if not shared["S31_RADIO_EXC_BASE"] <= radio["S31_RADIO_EXC_STACK_TOP"] < shared["S31_RADIO_EXC_END"]:
        raise ValueError("radio exception stack is outside its reservation")
    regions = [("S31_OPENSBI_RW_BASE", "S31_OPENSBI_RW_END"),
               ("S31_RADIO_HEAP_LOW_BASE", "S31_RADIO_HEAP_LOW_END"),
               ("S31_RADIO_HEAP_BASE", "S31_RADIO_HEAP_END"),
               ("S31_RADIO_EXC_BASE", "S31_RADIO_EXC_END")]
    spans = [(shared[a], shared[b]) for a, b in regions]
    for name in ("AXI_DESC", "AHB_DESC", "USB_LOCAL", "UART_DMA"):
        spans.append((shared["S31_" + name + "_BASE"], shared["S31_" + name + "_BASE"] + shared["S31_" + name + "_SIZE"]))
    spans.append((shared["S31_RADIO_HEAP2_BASE"], shared["S31_RADIO_HEAP2_END"]))
    if any(a >= b for a, b in spans) or any(a[1] > b[0] for a, b in zip(spans, spans[1:])):
        raise ValueError("SRAM reservations overlap or are empty")
    if spans[-2][1] != shared["S31_HP_SHARED_END"] or spans[-1][0] != shared["S31_HP_SHARED_END"]:
        raise ValueError("UART DMA/high radio heap boundary differs")
    dtsdir = root / "linux-esp32-s31/arch/riscv/boot/dts/espressif"
    dts = (dtsdir / "esp32s31.dtsi").read_text()
    for name in ("PSRAM", "AXI_DESC", "AHB_DESC", "USB_LOCAL"):
        pattern = r"<0x0*%x\s+0x0*%x>" % (shared["S31_" + name + "_BASE"], shared["S31_" + name + "_SIZE"])
        if not re.search(pattern, dts, re.I):
            raise ValueError(name + " missing or different in DTS reg")
    for overlay in dtsdir.glob("esp32s31-overlay-uart*-dma.dtso"):
        if not re.search(r"<0x0*%x\s+0x0*%x>" % (shared["S31_UART_DMA_BASE"], shared["S31_UART_DMA_SIZE"]), overlay.read_text(), re.I):
            raise ValueError(str(overlay.name) + " UART DMA reservation differs")
    mappings = {"spl": "SPL", "u-boot-fit": "UBOOT_ITB", "dtb": "DTB", "radio-bundle": "RADIO", "linux": "KERNEL", "persist": "PERSIST", "rootfs": "ROOTFS"}
    for label, key in mappings.items():
        match = re.search(r'label\s*=\s*"' + label + r'";\s*reg\s*=\s*<(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)>', dts)
        actual = tuple(int(x, 0) for x in match.groups()) if match else None
        expected = (slots["SLOT_" + key], sizes[key])
        if actual != expected:
            raise ValueError(f"DTS partition {label}: {actual} != slot map {expected}")
    if "hil-scratch" in dts or not re.search(r"reg = <0x40000000 0x01000000>", dts):
        raise ValueError("DTS must expose the full identity-mapped flash without HIL scratch")
    linker = (root / "linux-esp32-s31/arch/riscv/kernel/vmlinux-xip.lds.S").read_text()
    start = int(re.search(r"\.s31_radio.data\s+(0x[0-9a-fA-F]+)", linker)[1], 0)
    if start != shared["S31_RADIO_HEAP_BASE"]:
        raise ValueError("kernel radio link address differs from shared contract")
    makefile = "\n".join(path.read_text() for path in [root / "Makefile", *sorted((root / "mk").glob("*.mk"))])
    start = int(re.search(r"FW_RW_START\s*\?=\s*(0x[0-9a-fA-F]+)", makefile)[1], 0)
    if start != shared["S31_OPENSBI_RW_BASE"]:
        raise ValueError("OpenSBI writable base differs from shared contract")
    xip = defines((root / "linux-esp32-s31/drivers/platform/esp32s31-radio-xip.h").read_text())
    if xip["S31_XIP_PHYS"] != 0x40000000 + slots["SLOT_RADIO"]:
        raise ValueError("radio XIP physical mapping differs from flash slot")
    if xip["S31_XIP_SLOT_SIZE"] != sizes["RADIO"]:
        raise ValueError("radio XIP capacity differs from flash slot")
    if (xip["S31_XIP_BASE"] - xip["S31_XIP_MAP_BASE"] !=
            xip["S31_XIP_PHYS"] - xip["S31_XIP_MAP_PHYS"] or
            xip["S31_XIP_BASE"] + sizes["RADIO"] >
            xip["S31_XIP_MAP_BASE"] + xip["S31_XIP_MAP_SIZE"] or
            xip["S31_XIP_MAP_SIZE"] != 0x400000 or
            xip["S31_XIP_MAP_BASE"] % 0x400000 or
            xip["S31_XIP_MAP_PHYS"] % 0x400000):
        raise ValueError("radio XIP Sv32 leaf mapping is inconsistent")
    print("SRAM reservations and all mapped flash partitions match the shared contracts")

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--size", choices=SLOTS)
    p.add_argument("--root", type=Path, default=ROOT)
    p.add_argument("--image-slot", nargs=2, metavar=("SLOT", "IMAGE"))
    a = p.parse_args()
    try:
        if a.image_slot:
            key, name = a.image_slot
            capacity = flash_layout(a.root)[1][key]
            size = Path(name).stat().st_size
            if not 0 < size <= capacity:
                raise ValueError(f"{key} image size {size} exceeds capacity {capacity} or is empty")
        elif a.size:
            print(flash_layout(a.root)[1][a.size])
        else:
            check(a.root)
    except (ValueError, KeyError, OSError) as e:
        p.exit(1, "layout: " + str(e) + "\n")

if __name__ == "__main__":
    main()
