"""Execute the compiled SBI boundary with a fake firmware return and CSR bank."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]


class SbiSretTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tool_dir = ROOT / "toolchain/riscv32-esp-linux-musl/bin"
        pairs = [(str(tool_dir / "riscv32-esp-linux-musl-gcc"),
                  str(tool_dir / "riscv32-esp-linux-musl-objdump"))]
        for prefix in ("riscv32-linux-gnu-", "riscv64-linux-gnu-"):
            pairs.append((shutil.which(prefix + "gcc"), shutil.which(prefix + "objdump")))
        pair = next((pair for pair in pairs if all(p and Path(p).is_file() for p in pair)), None)
        if pair is None:
            raise unittest.SkipTest("RV32-capable GCC and objdump are required for compiled SBI checks")
        compiler, objdump = pair
        cls.tmp = tempfile.TemporaryDirectory()
        text = (ROOT / "linux-esp32-s31/arch/riscv/kernel/sbi_ecall.c").read_text()
        body = text[text.index("struct sbiret __sbi_ecall"):text.index("EXPORT_SYMBOL(__sbi_ecall)")]
        cls.programs = {}
        for enabled, wfi in ((False, False), (True, False), (True, True)):
            function = "esp32s31_sbi_wfi" if wfi else "__sbi_ecall"
            selected_body = body
            if wfi:
                text = (ROOT / "linux-esp32-s31/drivers/irqchip/irq-esp32s31-smp.c").read_text()
                selected_body = text[text.index("void noinstr esp32s31_sbi_wfi"):text.index("static unsigned int esp32s31_doorbell_offset")]
            source = Path(cls.tmp.name) / (str(enabled) + str(wfi) + ".c")
            source.write_text("typedef unsigned long uintptr_t;\nstruct sbiret {long error,value;};\n"
                              "#define trace_sbi_call(...) ((void)0)\n"
                              "#define trace_sbi_return(...) ((void)0)\n"
                              + ("#define CONFIG_SOC_ESP32S31 1\n" if enabled else "")
                              + "#define noinstr\n#define ESP32S31_SBI_CLIC_WFI 4\n"
                              "#define ESP32S31_SBI_EXT_CLIC 0x09000003UL\n"
                              '#define raw_local_irq_enable() asm volatile("csrsi sstatus, 2" ::: "memory")\n'
                              '#define raw_local_irq_disable() asm volatile("csrci sstatus, 2" ::: "memory")\n'
                              + (ROOT / "linux-esp32-s31/arch/riscv/include/asm/sbi-ecall.h").read_text()
                              + selected_body)
            elf = source.with_suffix(".elf")
            subprocess.run([compiler, "-O2", "-nostdlib",
                            "-nostartfiles", "-march=rv32imac_zicsr_zifencei", "-mabi=ilp32",
                            "-Wl,-Ttext=0x40400000,-e," + function, str(source), "-o", str(elf)], check=True)
            dis = subprocess.check_output([objdump,
                                           "-d", str(elf)], text=True)
            program = []
            for line in dis.splitlines():
                match = re.match(r"\s*([0-9a-f]+):\s+[0-9a-f]+\s+(\S+)\s*(.*)", line)
                if match:
                    program.append((int(match[1], 16), match[2], match[3].split(" # ")[0]))
            cls.programs[enabled, wfi] = program

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def execute(self, enabled, status, delta=0, wfi=False):
        program = [(pc + delta, op, args) for pc, op, args in self.programs[enabled, wfi]]
        positions = {row[0]: i for i, row in enumerate(program)}
        regs = {"a" + str(i): 0x12340000 + i for i in range(8)}
        regs.update({"t" + str(i): 0x45670000 + i for i in range(5)})
        regs.update({"sp": 0x3f800000, "ra": 0xc0012340})
        original_regs = dict(regs)
        original = {"sstatus": status, "sepc": 0xc0091234,
                    "scause": 0x80120009 | ((status & 0x100) << 20)}
        csrs = dict(original)
        level = 0
        returns = 0
        index = 0
        for _ in range(50):
            pc, op, operands = program[index]
            index += 1
            args = operands.split(",")
            if op == "ret":
                break
            if op == "csrr":
                regs[args[0]] = csrs[args[1]]
            elif op == "csrc":
                csrs[args[0]] &= ~regs[args[1]] & 0xffffffff
            elif op == "csrsi":
                csrs[args[0]] |= int(args[1], 0)
            elif op == "csrci":
                csrs[args[0]] &= ~int(args[1], 0) & 0xffffffff
            elif op == "csrw":
                if enabled and args[0] != "sstatus":
                    self.assertFalse(csrs["sstatus"] & 2, "IRQs enabled with temporary trap CSRs")
                csrs[args[0]] = regs[args[1]]
            elif op == "li":
                regs[args[0]] = int(args[1], 0) & 0xffffffff
            elif op == "lui":
                regs[args[0]] = (int(args[1], 0) << 12) & 0xffffffff
            elif op == "ori":
                regs[args[0]] = regs[args[1]] | int(args[2], 0)
            elif op == "and":
                regs[args[0]] = regs[args[1]] & regs[args[2]]
            elif op == "auipc":
                regs[args[0]] = pc + (int(args[1], 0) << 12)
            elif op in ("addi", "add"):
                regs[args[0]] = (regs[args[1]] + int(args[2], 0)) & 0xffffffff
            elif op == "ecall":
                if wfi:
                    self.assertEqual(tuple(regs["a" + str(i)] for i in range(8)),
                                     (0, 0, 0, 0, 0, 0, 4, 0x09000003))
                if enabled:
                    self.assertFalse(csrs["sstatus"] & 2)
                    level = 255
                regs["a0"], regs["a1"] = 0xfffffffd, 0x789abcde
            elif op == "sret":
                self.assertTrue(csrs["sstatus"] & 0x100, "recovery returned to user mode")
                self.assertFalse(csrs["sstatus"] & 0x22)
                self.assertTrue(csrs["scause"] & 0x80000000)
                self.assertFalse(csrs["scause"] & 0x00ff0000)
                level = 0
                csrs["sstatus"] = (csrs["sstatus"] & ~0x102) | 0x20
                index = positions[csrs["sepc"]]
                returns += 1
            else:
                self.fail("Unsupported fixture instruction: " + op + " " + operands)
        else:
            self.fail("SBI boundary did not terminate")
        if wfi:
            original["sstatus"] &= ~2
        self.assertEqual(csrs, original)
        self.assertEqual((regs["a0"], regs["a1"]), (0xfffffffd, 0x789abcde))
        for name in (("sp", "ra") if wfi else ("a2", "a3", "a4", "a5", "a6", "a7", "sp", "ra")):
            self.assertEqual(regs[name], original_regs[name])
        self.assertEqual(returns, int(enabled))
        self.assertEqual(level, 0)

    def test_s31_recovers_with_interrupts_and_trap_state_preserved(self):
        for status in (0, 2, 0x20, 0x22, 0x100, 0x102, 0x120, 0x122):
            self.execute(True, status | 0x40000)

    def test_recovery_label_works_before_and_after_virtual_mapping(self):
        self.execute(True, 0x122, 0)
        self.execute(True, 0x122, 0x7fc00000)

    def test_other_platforms_use_the_existing_ecall(self):
        self.execute(False, 0x122)

    def test_direct_wfi_uses_recovery_and_returns_with_irqs_disabled(self):
        for status in (0x20, 0x22, 0x120, 0x122):
            self.execute(True, status, wfi=True)
