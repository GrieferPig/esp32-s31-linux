#!/usr/bin/env python3
"""Validate S31 bindings/DTBs and fail on diagnostics even when make returns zero."""
import argparse
import json
import os
import shutil
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cross-compile", default=os.environ.get("CROSS_COMPILE", "riscv64-linux-gnu-"))
    p.add_argument("--output", type=Path)
    a = p.parse_args()
    bindings = ROOT / "linux-esp32-s31/Documentation/devicetree/bindings"
    schemas = sorted(bindings.rglob("*esp32s31*.yaml")) + [bindings / name for name in ("serial/esp,esp32-uart.yaml", "mmc/synopsys-dw-mshc.yaml", "spi/spi-controller.yaml", "spi/rohm,dh2228fv.yaml", "usb/dwc2.yaml", "riscv/cpus.yaml", "riscv/extensions.yaml", "firmware/opensbi,config.yaml")]
    subprocess.run(["dt-doc-validate", *map(str, schemas)], check=True)
    with tempfile.TemporaryDirectory(prefix="s31-dt-") as tmp:
        out = a.output or Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        base = ["make", "-C", str(ROOT / "linux-esp32-s31"), "O=" + str(out.resolve()), "ARCH=riscv", "CROSS_COMPILE=" + a.cross_compile]
        subprocess.run(base + ["esp32s31_defconfig"], check=True)
        result = subprocess.run(base + ["dtbs", "dt_binding_schemas"], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(result.stdout, end="")
        (out / "s31-dt-check.log").write_text(result.stdout)
        # dt-validate does not reliably propagate a nonzero status through Kbuild.
        diagnostic = re.search(r"(?:from schema \$id:|Warning \(|Error:|error:|warning:|missing type definition|multiple incompatible types|is not valid under|ignoring, error in schema)", result.stdout)
        if result.returncode or diagnostic:
            p.exit(1, "S31 DT/schema validation failed; see diagnostics above\n")

        dts = out / "arch/riscv/boot/dts/espressif"
        merged = out / "s31-merged-overlays"
        merged.mkdir(exist_ok=True)
        outputs = []
        for overlay in sorted(dts.glob("*.dtbo")):
            target = merged / (overlay.stem + ".dtb")
            subprocess.run(["fdtoverlay", "-i", str(dts / "esp32s31_generic.dtb"), "-o", str(target), str(overlay)], check=True)
            outputs.append(str(target))
        if not outputs:
            p.exit(1, "No overlays were built\n")
        schema = out / "Documentation/devicetree/bindings/processed-schema.json"
        data = json.loads(schema.read_text())
        # dtschema's generic /chosen schema predates this OpenSBI child.
        # Extend only that named child using the meta-validated in-tree binding;
        # all other chosen properties and all hardware schemas remain checked.
        data["http://devicetree.org/schemas/chosen.yaml"]["properties"]["opensbi-config"] = {
            "$ref": "http://devicetree.org/schemas/firmware/opensbi,config.yaml"
        }
        schema = out / "s31-processed-schema.json"
        schema.write_text(json.dumps(data))
        checked = subprocess.run(["dt-validate", "-s", str(schema),
                                  *map(str, sorted(dts.glob("*.dtb"))), *outputs],
                                 text=True, capture_output=True)
        (out / "s31-merged-check.log").write_text(checked.stdout + checked.stderr)
        if checked.returncode or checked.stdout or checked.stderr:
            print(checked.stdout + checked.stderr, end="")
            p.exit(1, "Merged overlay schema validation failed\n")
        # A negative control must fail under the same unfiltered schema set.
        invalid = out / "invalid-sdmmc.dtb"
        shutil.copyfile(merged / "esp32s31-overlay-sdmmc0.dtb", invalid)
        node = subprocess.check_output(["fdtget", "-t", "s", str(invalid), "/__symbols__", "sdmmc"], text=True).strip()
        subprocess.run(["fdtput", "-t", "i", str(invalid), node, "bus-width", "3"], check=True)
        rejected = subprocess.run(["dt-validate", "-s", str(schema), str(invalid)], text=True, capture_output=True)
        (out / "s31-negative-control.log").write_text(rejected.stdout + rejected.stderr)
        if "bus-width" not in rejected.stdout + rejected.stderr:
            p.exit(1, "Invalid SDMMC bus width escaped validation\n")
        print("Negative control: invalid SDMMC bus width rejected")
        print(f"Validated base DTBs and {len(outputs)} individually merged overlays")

if __name__ == "__main__":
    main()
