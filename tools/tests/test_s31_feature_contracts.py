#!/usr/bin/env python3
"""Host tests of actual driver command generation and EAP transport failures.

The C harness compiles functions extracted from the drivers, replacing only
the MMIO/IRQ batch executor. It checks wire transactions, not physical timing.
"""
import importlib.util
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("eap", ROOT / "tools/device/wifi_eap.py")
eap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(eap)


def function(source, name):
    match = re.search(r"^static [^\n]*\b" + name + r"\([^;{]*\{.*?^}\n", source, re.M | re.S)
    if not match:
        raise AssertionError("driver function not found: " + name)
    return match.group()


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint8_t u8;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U >> (31-(h))) & (~0U << (l)))
#define FIELD_PREP(mask,v) (((u32)(v) << __builtin_ctz(mask)) & (mask))
#define min_t(t,a,b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define I2C_M_RD 1
#define I2C_M_TEN 16
struct esp32s31_i2c { int unused; };
struct i2c_msg { uint16_t addr, flags, len; u8 *buf; };
static unsigned actual[140000], expected[140000], an, en, ri;
static unsigned batches, fail_at, scenarios;
static int failure;
static u8 storage[2][65535];
static int s31_i2c_batch(struct esp32s31_i2c *i2c, const u32 *commands,
        unsigned count, const u8 *tx, unsigned txlen, u8 *rx, unsigned rxlen)
{
    unsigned t = 0, r = 0;
    assert(count <= 8 && txlen <= 32 && rxlen <= 32);
    if (++batches == fail_at) return failure;
    for (unsigned i = 0; i < count; i++) {
        unsigned op = (commands[i] >> 11) & 7, len = commands[i] & 255;
        if (op == 6) actual[an++] = 0x1000;
        else if (op == 2) actual[an++] = 0x1001;
        else if (op == 1) {
            assert(commands[i] & BIT(8));
            while (len--) { assert(t < txlen); actual[an++] = tx[t++]; }
        } else if (op == 3) {
            /* Full FIFO plus final NACK stalls the actual command engine. */
            if (commands[i] & BIT(10)) assert(rxlen < 32);
            while (len--) {
                assert(r < rxlen);
                rx[r] = (ri++ * 17 + 3) & 255;
                actual[an++] = rx[r++] | ((commands[i] & BIT(10)) ? 0x300 : 0x200);
            }
        } else assert(op == 4);
    }
    assert(t == txlen && r == rxlen);
    return 0;
}
'''

MAIN = r'''
static void check(struct i2c_msg *msgs, unsigned num)
{
    unsigned read_index = 0;
    an = en = ri = batches = fail_at = 0;
    for (unsigned m = 0; m < num; m++) {
        struct i2c_msg *msg = &msgs[m];
        bool read = msg->flags & I2C_M_RD, ten = msg->flags & I2C_M_TEN;
        for (unsigned j = 0; j < msg->len; j++) msg->buf[j] = (j ^ (j >> 8)) & 255;
        expected[en++] = 0x1000;
        if (ten) {
            expected[en++] = 0xf0 | ((msg->addr >> 7) & 6);
            expected[en++] = msg->addr & 255;
            if (read) {
                expected[en++] = 0x1000;
                expected[en++] = 0xf1 | ((msg->addr >> 7) & 6);
            }
        } else expected[en++] = (msg->addr << 1) | read;
        for (unsigned j = 0; j < msg->len; j++)
            expected[en++] = read ? (((read_index++ * 17 + 3) & 255) |
                (j + 1 == msg->len ? 0x300 : 0x200)) : msg->buf[j];
    }
    expected[en++] = 0x1001;
    assert(s31_i2c_long_xfer(NULL, msgs, num) == (int)num);
    assert(an == en && !memcmp(actual, expected, an * sizeof(*actual)));
    read_index = 0;
    for (unsigned m = 0; m < num; m++)
        if (msgs[m].flags & I2C_M_RD)
            for (unsigned j = 0; j < msgs[m].len; j++)
                assert(msgs[m].buf[j] == ((read_index++ * 17 + 3) & 255));
    scenarios++;
}
int main(void)
{
    const unsigned lengths[] = {1, 30, 31, 32, 33, 63, 64, 65, 255, 256, 257, 4096, 65535};
    for (unsigned n = 0; n < sizeof(lengths)/sizeof(*lengths); n++)
        for (unsigned ten = 0; ten < 2; ten++)
            for (unsigned read = 0; read < 2; read++) {
                struct i2c_msg msg = {ten ? 0x2aa : 0x51,
                    (ten ? I2C_M_TEN : 0) | (read ? I2C_M_RD : 0), lengths[n], storage[0]};
                check(&msg, 1);
            }
    struct i2c_msg combined[] = {{0x51, 0, 257, storage[0]}, {0x2aa, I2C_M_TEN|I2C_M_RD, 4096, storage[1]}};
    check(combined, 2);
    combined[0].len = 0;
    check(combined, 2);
    combined[1].flags = I2C_M_TEN;
    combined[1].len = 0;
    check(combined, 2);
    combined[0].len = 257;
    for (unsigned stop = 1; stop < 8; stop++) {
        const int errors[] = {-ENXIO, -EAGAIN, -ETIMEDOUT};
        for (unsigned e = 0; e < 3; e++) {
            batches = an = ri = 0; fail_at = stop; failure = errors[e];
            assert(s31_i2c_long_xfer(NULL, combined, 2) == failure);
            assert(batches == stop);
            scenarios++;
        }
    }
    const u8 native[] = {0x78,0x56,0x34,0x12,0xef,0xcd,0xab,0x90};
    const u8 wire16[] = {0x56,0x78,0x12,0x34,0xcd,0xef,0x90,0xab};
    const u8 wire32[] = {0x12,0x34,0x56,0x78,0x90,0xab,0xcd,0xef};
    u8 out[8], back[8];
    for (unsigned bits = 8; bits <= 32; bits *= 2)
        for (unsigned lsb = 0; lsb < 2; lsb++) {
            esp32s31_spi_target_copy(out, native, 8, bits, lsb);
            assert(!memcmp(out, lsb || bits == 8 ? native : bits == 16 ? wire16 : wire32, 8));
            esp32s31_spi_target_copy(back, out, 8, bits, lsb);
            assert(!memcmp(back, native, 8));
            scenarios++;
        }
    printf("%u driver wire/error/byte-order scenarios passed\n", scenarios);
    return 0;
}
'''


class DriverContracts(unittest.TestCase):
    def test_lp_sleep_abi_matches_opensbi(self):
        protocol = (ROOT / "linux-esp32-s31/include/linux/soc/espressif/esp32s31-lp-protocol.h").read_text()
        opensbi = (ROOT / "opensbi-esp32-s31/platform/generic/espressif/esp32s31/services.c").read_text()
        linux_abi = re.search(r"#define S31_LP_ABI_VERSION\s+(\d+)U", protocol)
        opensbi_abi = re.search(r"#define S31_LP_SLEEP_ABI_VERSION\s+(\d+)U", opensbi)
        self.assertIsNotNone(linux_abi)
        self.assertIsNotNone(opensbi_abi)
        self.assertEqual(linux_abi.group(1), opensbi_abi.group(1))

    def test_lp_mem_timer_starts_after_hp_asleep(self):
        firmware = (ROOT / "firmware/lp/main/lp_core/main.c").read_text()
        self.assertIsNotNone(re.search(
            r"state == S31_LP_SLEEP_HP_ASLEEP.*?S31_LP_SLEEP_F_MEM.*?"
            r"sleep_timer_arm_from_now\(\)",
            firmware, re.S,
        ))
        arm_case = firmware[firmware.index("case S31_LP_CMD_SLEEP_ARM:"):
                            firmware.index("case S31_LP_CMD_SLEEP_ABORT:")]
        self.assertIn("S31_LP_SLEEP_F_DEEP_REBOOT", arm_case)
        self.assertIn("sleep_timer_target = 0", arm_case)

    def test_spi_controller_timeout_scales_with_transfer(self):
        driver = (ROOT / "linux-esp32-s31/drivers/spi/spi-esp32s31.c").read_text()
        self.assertIn("spi_controller_xfer_timeout(ctlr, xfer)", driver)
        wait = function(driver, "esp32s31_spi_wait_transaction")
        self.assertIn("msecs_to_jiffies(timeout_ms)", wait)

    def test_i2s_configuration(self):
        driver = (ROOT / "linux-esp32-s31/sound/soc/espressif/esp32s31-i2s.c").read_text()
        headers = "\n".join((ROOT / path).read_text() for path in (
            "linux-esp32-s31/include/sound/soc-dai.h",
            "linux-esp32-s31/include/uapi/sound/asoc.h"))
        defines = "\n".join(line for line in headers.splitlines() if re.match(
            r"#define SND_SOC_(DAIFMT_|DAI_FORMAT_|CLOCK_OUT)", line) and not line.endswith("\\"))
        source = "#include <assert.h>\n#include <stdbool.h>\n#include <stdint.h>\n#include <errno.h>\n"
        source += defines + r'''
typedef uint32_t u32;
struct esp32s31_i2s {
    u32 configured, dai_format, slots, slot_width, tx_mask, rx_mask, sysclk;
    bool tx_slave, rx_slave, running;
};
struct snd_soc_dai { struct esp32s31_i2s *i2s; };
static struct esp32s31_i2s *esp32s31_i2s_from_dai(struct snd_soc_dai *dai) { return dai->i2s; }
static bool esp32s31_i2s_running(struct esp32s31_i2s *i2s) { return i2s->running; }
'''
        for name in ("set_fmt", "set_tdm_slot", "set_sysclk"):
            source += function(driver, "esp32s31_i2s_" + name)
        source += r'''
int main(void) {
    struct esp32s31_i2s i2s = {0};
    struct snd_soc_dai dai = {&i2s};
    const unsigned formats[] = {SND_SOC_DAIFMT_I2S, SND_SOC_DAIFMT_LEFT_J,
        SND_SOC_DAIFMT_DSP_A, SND_SOC_DAIFMT_DSP_B};
    for (unsigned i = 0; i < 4; i++) {
        assert(!esp32s31_i2s_set_fmt(&dai, formats[i] | SND_SOC_DAIFMT_BP_FP));
        assert(!i2s.tx_slave && !i2s.rx_slave);
        assert(!esp32s31_i2s_set_fmt(&dai, formats[i] | SND_SOC_DAIFMT_BC_FC | SND_SOC_DAIFMT_NB_IF));
        assert(i2s.tx_slave && i2s.rx_slave);
    }
    assert(esp32s31_i2s_set_fmt(&dai, SND_SOC_DAIFMT_RIGHT_J | SND_SOC_DAIFMT_BP_FP) == -EINVAL);
    assert(esp32s31_i2s_set_fmt(&dai, SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_BP_FC) == -EINVAL);
    assert(esp32s31_i2s_set_fmt(&dai, SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_BP_FP | SND_SOC_DAIFMT_IB_NF) == -EINVAL);
    assert(!esp32s31_i2s_set_tdm_slot(&dai, 0x81, 0x24, 8, 32));
    assert(esp32s31_i2s_set_tdm_slot(&dai, 0x100, 0, 8, 32) == -EINVAL);
    assert(esp32s31_i2s_set_tdm_slot(&dai, 0, 0, 17, 32) == -EINVAL);
    assert(esp32s31_i2s_set_tdm_slot(&dai, 0, 0, 2, 7) == -EINVAL);
    assert(!esp32s31_i2s_set_sysclk(&dai, 0, 12288000, SND_SOC_CLOCK_OUT));
    assert(esp32s31_i2s_set_sysclk(&dai, 1, 12288000, SND_SOC_CLOCK_OUT) == -EINVAL);
    assert(esp32s31_i2s_set_sysclk(&dai, 0, 12288000, 0) == -EINVAL);
    i2s.configured = 1;
    assert(esp32s31_i2s_set_sysclk(&dai, 0, 24576000, SND_SOC_CLOCK_OUT) == -EBUSY);
    assert(!esp32s31_i2s_set_sysclk(&dai, 0, 12288000, SND_SOC_CLOCK_OUT));
    assert(esp32s31_i2s_set_tdm_slot(&dai, 3, 3, 2, 16) == -EBUSY);
    assert(!esp32s31_i2s_set_tdm_slot(&dai, 0x81, 0x24, 8, 32));
    assert(esp32s31_i2s_set_fmt(&dai, SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_BP_FP) == -EBUSY);
    i2s.configured = 0;
    assert(!esp32s31_i2s_set_tdm_slot(&dai, 0, 0, 0, 0));
    assert(i2s.slots == 0 && i2s.slot_width == 0 && i2s.tx_mask == 0 && i2s.rx_mask == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(source)
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror", "-fsanitize=undefined",
                            str(path / "test.c"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)

    def test_driver_wire_contracts(self):
        i2c = (ROOT / "linux-esp32-s31/drivers/i2c/busses/i2c-esp32s31.c").read_text()
        spi = (ROOT / "linux-esp32-s31/drivers/spi/spi-esp32s31.c").read_text()
        constants = "\n".join(line for line in i2c.splitlines() if re.match(
            r"#define S31_I2C_(CMD_|COMMANDS|FIFO_LEN)", line))
        source = HARNESS + constants + "\n" + function(i2c, "s31_i2c_command")
        source += function(i2c, "s31_i2c_long_xfer")
        source += function(spi, "esp32s31_spi_target_copy") + MAIN
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(source)
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-parameter", "-fsanitize=undefined",
                            str(path / "test.c"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)

    def test_i2s_asoc_device_ownership(self):
        i2s = (ROOT / "linux-esp32-s31/sound/soc/espressif/esp32s31-i2s.c").read_text()
        source = r'''
#include <assert.h>
#include <stddef.h>
#include <stdbool.h>
struct device { void *data; bool external; };
struct snd_soc_card { unsigned sentinel; };
struct esp32s31_i2s { void *base; struct snd_soc_card card; };
struct snd_soc_dai { struct device *dev; };
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
static void *dev_get_drvdata(struct device *d) { return d->data; }
static bool device_property_read_bool(struct device *d, const char *p) { return d->external; }
'''
        source += function(i2s, "esp32s31_i2s_from_dai")
        source += r'''
int main(void) {
    struct esp32s31_i2s controller = {0};
    struct device dev = {.data = &controller.card, .external = false};
    struct snd_soc_dai dai = {.dev = &dev};
    assert(esp32s31_i2s_from_dai(&dai) == &controller);
    dev.external = true; dev.data = &controller;
    assert(esp32s31_i2s_from_dai(&dai) == &controller);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(source)
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror", "-fsanitize=undefined",
                            str(path / "test.c"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)


class EapContracts(unittest.TestCase):
    def test_profile_validation(self):
        import json
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "ca.pem").write_bytes(b"test CA placeholder")
            (path / "password.txt").write_bytes(b"test-only-password")
            profile = {"identity": "anonymous", "username": "test-user",
                       "domain": "radius.example.invalid", "ca_file": "ca.pem",
                       "password_file": "password.txt"}
            config = path / "profile.json"
            config.write_text(json.dumps(profile))
            self.assertEqual(eap.load_profile(config)[2], b"test-only-password")
            for name in ("domain", "identity", "ca_file", "password_file"):
                invalid = dict(profile)
                del invalid[name]
                config.write_text(json.dumps(invalid))
                with self.assertRaises(ValueError):
                    eap.load_profile(config)
            config.write_text(json.dumps(profile))
            (path / "ca.pem").write_bytes(b"x" * 4096)
            with self.assertRaises(ValueError):
                eap.load_profile(config)

    def test_binary_chunks_and_commit(self):
        fields = {0: b"identity", 3: bytes(range(256)) * 7, 4: b"radius.example.invalid"}
        sent = []
        eap.provision(fields, sent.append)
        self.assertEqual(sent[0], struct.pack("<5I", 8, 0, 0, 0, 0))
        self.assertEqual(sent[-1], struct.pack("<5I", 7, 0, 0, 0, 0))
        reconstructed = {}
        for item in sent[1:-1]:
            op, field, offset, total, length = struct.unpack("<5I", item[:20])
            self.assertEqual(op, 6)
            self.assertLessEqual(length, 512)
            self.assertEqual(len(item), length + 20)
            self.assertEqual(total, len(fields[field]))
            reconstructed.setdefault(field, bytearray())
            self.assertEqual(offset, len(reconstructed[field]))
            reconstructed[field].extend(item[20:])
        self.assertEqual(reconstructed, fields)

    def test_failure_clears_incomplete_profile(self):
        for failing_call in (2, 3, 4):
            sent = []
            def send(data):
                sent.append(data)
                if len(sent) == failing_call:
                    raise OSError("injected transport failure")
            with self.assertRaises(OSError):
                eap.provision({0: b"x" * 600}, send)
            self.assertEqual(sent[-1], eap.packet(eap.CLEAR))
            self.assertEqual(len(sent), failing_call + 1)


if __name__ == "__main__":
    unittest.main()
