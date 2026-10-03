#!/usr/bin/env python3
"""Check the patched BTstack reset handler's recovery ordering on the host.

Requires the local btstack-source fetched by the normal rootfs build.
Controller/transport behavior is covered separately by the connected BLE HIL.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class HardwareErrorRecovery(unittest.TestCase):
    def test_reset_and_custom_handler(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / 'src').mkdir()
            source = Path(os.environ.get('S31_BTSTACK_SOURCE', ROOT / 'cache/sources/btstack')) / 'src/hci.c'
            if not source.exists():
                self.skipTest('pinned BTstack source unavailable; run make fetch or set S31_BTSTACK_SOURCE')
            (work / 'src/hci.c').write_bytes(source.read_bytes())
            patch = ROOT / 'buildroot-external/package/btstack-s31/0020-hci-reinitialize-after-controller-hardware-error.patch'
            subprocess.run(['patch', '--batch', '-p1', '-i', str(patch)],
                           cwd=work, check=True, capture_output=True)
            text = (work / 'src/hci.c').read_text()
            handler = re.search(r'^static void hci_handle_hardware_error_event\([^;{]*\{.*?^}',
                                text, re.M | re.S).group()
            harness = r'''
#include <assert.h>
#include <stdint.h>
#define ENABLE_BLE
#define ENABLE_LE_PERIPHERAL
#define HCI_POWER_ON 1
#define log_error(...) ((void)0)
static struct { void (*hardware_error_callback)(uint8_t); } stack, *hci_stack = &stack;
static int step, connections, advertising, initialized, callback_error;
static void hci_power_control_off(void) { assert(step++ == 0); }
static void hci_discard_connections(void) { assert(step++ == 1); connections = 0; }
static void hci_update_advertisements_enabled_for_current_roles(void) {
    assert(step++ == 2); assert(!connections); advertising = 1;
}
static int hci_power_control(int mode) {
    assert(step++ == 3); assert(mode == HCI_POWER_ON);
    assert(!connections && advertising); initialized = 1; return 0;
}
static void custom(uint8_t error) { callback_error = error; }
''' + handler + r'''
int main(void) {
    uint8_t event[] = {0x10, 1, 1};
    hci_handle_hardware_error_event(event, 2);
    assert(step == 0);
    connections = 1;
    hci_handle_hardware_error_event(event, sizeof(event));
    assert(step == 4 && initialized && advertising && !connections);
    step = 0; initialized = 0; stack.hardware_error_callback = custom;
    hci_handle_hardware_error_event(event, sizeof(event));
    assert(step == 0 && !initialized && callback_error == 1);
    return 0;
}
'''
            (work / 'test.c').write_text(harness)
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(work / 'test.c'),
                            '-o', str(work / 'test')], check=True)
            subprocess.run([str(work / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
