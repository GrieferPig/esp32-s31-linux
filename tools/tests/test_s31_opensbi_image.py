"""Reproduce the leading-gap regression with real RISC-V ELF/raw artifacts."""
import importlib.util
import os
import shutil
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('check', Path(__file__).resolve().parents[1] / 'checks/opensbi.py')
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)
ROOT = Path(__file__).resolve().parents[2]
def fixture_prefix():
    candidates = [os.environ.get('CROSS_COMPILE', ''),
                  str(ROOT / 'cache/toolchains/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-'),
                  str(ROOT / 'toolchain/riscv32-esp-linux-musl/bin/riscv32-esp-linux-musl-'),
                  'riscv64-linux-gnu-']
    for prefix in candidates:
        if prefix and all(shutil.which(prefix + tool) for tool in ('gcc', 'ld', 'objcopy')):
            return str(Path(shutil.which(prefix + 'gcc')).parent / Path(prefix).name)
    return None


PREFIX = fixture_prefix()


def cell(n):
    return struct.pack('>I', n)


def fdt(rows):
    names, strings, tree = {}, bytearray(), {'props': {}, 'children': {}}
    for path, props in rows.items():
        node = tree
        for part in path.strip('/').split('/') if path.strip('/') else []:
            node = node['children'].setdefault(part, {'props': {}, 'children': {}})
        node['props'].update(props)
        for name in props:
            if name not in names:
                names[name] = len(strings)
                strings += name.encode() + b'\0'
    def pad(b): return b + bytes((-len(b)) % 4)
    def emit(name, node):
        block = cell(1) + pad(name.encode() + b'\0')
        for key, value in node['props'].items():
            block += cell(3) + cell(len(value)) + cell(names[key]) + pad(value)
        for key, value in node['children'].items():
            block += emit(key, value)
        return block + cell(2)
    block = emit('', tree) + cell(9)
    total = 56 + len(block) + len(strings)
    header = struct.pack('>10I', 0xd00dfeed, total, 56, 56 + len(block), 40, 17, 16, 0, len(strings), len(block))
    return header + bytes(16) + block + strings


def fit(raw, *, load=0x4000e400, placement=0x400, heap=0x8000):
    dtb = fdt({'/chosen/opensbi-config': {'compatible': b'opensbi,config\0', 'heap-size': cell(heap)}})
    dtpos = (placement + len(raw) + 3) & ~3
    tree = fdt({
        '/images/opensbi': {'load': cell(load), 'data-position': cell(placement), 'data-size': cell(len(raw)), 'compression': b'none\0'},
        '/images/fdt-1': {'data-position': cell(dtpos), 'data-size': cell(len(dtb))},
        '/configurations': {'default': b'conf-1\0'},
        '/configurations/conf-1': {'firmware': b'opensbi\0', 'fdt': b'fdt-1\0'},
    })
    assert len(tree) <= placement
    return tree + bytes(placement - len(tree)) + raw + bytes(dtpos - placement - len(raw)) + dtb


class ImageContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if PREFIX is None:
            raise unittest.SkipTest('RISC-V toolchain required for ELF/raw regression fixtures')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.work = Path(cls.tmp.name)
        source = cls.work / 'test.S'
        source.write_text('.section .entry,"ax"\n.globl _start\n_start: nop\nret\n.section .data\n.word 0x11223344\n.section .bss\n.space 16\n')
        subprocess.run([PREFIX + 'gcc', '-march=rv32imac', '-mabi=ilp32', '-c', str(source), '-o', str(cls.work / 'test.o')], check=True)
        cls.images = {}
        for label, gap in [('good', ''), ('gap', '. = ALIGN(0x1000);')]:
            script = cls.work / (label + '.ld')
            script.write_text('ENTRY(_start)\nSECTIONS { . = 0x4000e400; _fw_start = .; ' + gap + '\n.text : { *(.entry) }\n. = ALIGN(0x1000); __data_flash_start = .;\n.data 0x2f00f000 : AT(__data_flash_start) { _data_start = .; *(.data) }\n.bss (NOLOAD) : { *(.bss) }\n. = ALIGN(0x1000); _fw_end = .; }\n')
            elf, raw = cls.work / (label + '.elf'), cls.work / (label + '.bin')
            subprocess.run([PREFIX + 'ld', '-m', 'elf32lriscv', '-T', str(script), str(cls.work / 'test.o'), '-o', str(elf)], check=True)
            subprocess.run([PREFIX + 'objcopy', '-O', 'binary', str(elf), str(raw)], check=True)
            cls.images[label] = (elf.read_bytes(), raw.read_bytes())

    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()

    def test_good_entire_chain(self):
        elf, raw = self.images['good']
        image = fit(raw)
        result = CHECK.check(elf, raw, image, bytes(0xe000) + image)
        self.assertEqual(result['entry'], '0x4000e400')
        self.assertEqual(result['complete_rw_end'], '0x2f01a000')

    def test_original_leading_gap_rejected(self):
        with self.assertRaisesRegex(ValueError, 'entry/raw/link mismatch'):
            CHECK.check(*self.images['gap'])

    def test_corrupted_raw_section_rejected(self):
        elf, raw = self.images['good']
        with self.assertRaisesRegex(ValueError, 'raw bytes differ'):
            CHECK.check(elf, bytes([raw[0] ^ 1]) + raw[1:])

    def test_fit_different_binary_rejected(self):
        elf, raw = self.images['good']
        with self.assertRaisesRegex(ValueError, 'different OpenSBI'):
            CHECK.check(elf, raw, fit(bytes([raw[0] ^ 1]) + raw[1:]))

    def test_fit_address_and_placement_rejected(self):
        elf, raw = self.images['good']
        for opts in ({'load': 0x40001000}, {'placement': 0x800}):
            with self.subTest(opts=opts), self.assertRaisesRegex(ValueError, 'placement disagree'):
                CHECK.check(elf, raw, fit(raw, **opts))

    def test_runtime_heap_overlap_rejected(self):
        with self.assertRaisesRegex(ValueError, 'stacks/heap overlap'):
            CHECK.check(*self.images['good'], fit(self.images['good'][1], heap=0x30000))

    def test_merged_flash_mismatch_rejected(self):
        elf, raw = self.images['good']
        image = fit(raw)
        merged = bytearray(bytes(0xe000) + image)
        merged[0xe400] ^= 1
        with self.assertRaisesRegex(ValueError, 'different FIT'):
            CHECK.check(elf, raw, image, merged)

    def test_truncated_metadata_rejected(self):
        with self.assertRaises(ValueError): CHECK.elf_layout(b'\x7fELF\x01\x01')
        with self.assertRaises(ValueError): CHECK.fdt_props(bytes(39))


if __name__ == '__main__': unittest.main()
