#!/usr/bin/env python3
"""Run the radio suspend transaction with controlled frontend failures."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RadioSuspend(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        linux = Path(os.environ.get("S31_TEST_LINUX_SOURCE", ROOT / "linux-esp32-s31"))
        source = (linux / "drivers/platform/esp32s31-radio-module.c").read_text()
        suspend = re.search(r"^static int esp32s31_radio_suspend\([^;{]*\{.*?^}",
                            source, re.M | re.S).group()
        cls.temp = tempfile.TemporaryDirectory(prefix="s31-radio-pm-")
        cls.work = Path(cls.temp.name)
        harness = r'''
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define S31_RADIO_FEATURE_WIFI 1U
#define S31_RADIO_FEATURE_BLUETOOTH 2U
struct device { int unused; };
static bool s31_radio_started = true, s31_radio_suspended;
static unsigned int s31_radio_configured_features = 3;
static int wifi_error, bt_error, shutdown_error, resume_error, wifi_resume_error;
static int errors, votes;
static char calls[32];
static void called(char c) { size_t n = strlen(calls); calls[n] = c; calls[n + 1] = 0; }
static int s31_radio_wifi_frontend_suspend(void) { called('w'); return wifi_error; }
static int s31_radio_btdm_frontend_suspend(void) { called('b'); return bt_error; }
int s31_radio_wifi_frontend_resume(void) { called('W'); return wifi_resume_error; }
static int s31_radio_runtime_shutdown(void) { called('s'); return shutdown_error; }
static int esp32s31_radio_resume(struct device *dev) {
    (void)dev; called('R');
    if (!resume_error) { s31_radio_started = true; s31_radio_suspended = false; }
    return resume_error;
}
static void s31_linux_pmu_radio_vote(bool enabled) { if (!enabled) votes++; called('p'); }
#define dev_err(...) (++errors)
''' + suspend + r'''
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #expr); return 1; } } while (0)
int main(int argc, char **argv)
{
    struct device dev = {0};
    int ret;
    CHECK(argc == 2);
    switch (atoi(argv[1])) {
    case 0:
        s31_radio_started = false;
        CHECK(esp32s31_radio_suspend(&dev) == 0);
        CHECK(!*calls && !votes && !s31_radio_suspended);
        break;
    case 1:
        wifi_error = -EBUSY;
        ret = esp32s31_radio_suspend(&dev);
        CHECK(ret == -EBUSY && !strcmp(calls, "w"));
        CHECK(s31_radio_started && !s31_radio_suspended && !votes);
        break;
    case 2:
        bt_error = -EAGAIN;
        CHECK(esp32s31_radio_suspend(&dev) == -EAGAIN);
        CHECK(!strcmp(calls, "wbW"));
        CHECK(s31_radio_started && !s31_radio_suspended && !votes && !errors);
        break;
    case 3:
        bt_error = -EBUSY; wifi_resume_error = -EIO;
        CHECK(esp32s31_radio_suspend(&dev) == -EBUSY);
        CHECK(!strcmp(calls, "wbW") && errors == 1 && !votes);
        break;
    case 4:
        CHECK(esp32s31_radio_suspend(&dev) == 0);
        CHECK(!strcmp(calls, "wbsp") && votes == 1);
        CHECK(!s31_radio_started && s31_radio_suspended);
        break;
    case 5:
        shutdown_error = -ETIMEDOUT;
        CHECK(esp32s31_radio_suspend(&dev) == -EIO);
        CHECK(!strcmp(calls, "wbsR") && !votes && !errors);
        CHECK(s31_radio_started && !s31_radio_suspended);
        break;
    case 6:
        shutdown_error = -ETIMEDOUT; resume_error = -EIO;
        CHECK(esp32s31_radio_suspend(&dev) == -EIO);
        CHECK(!strcmp(calls, "wbsR") && !votes && errors == 1);
        CHECK(!s31_radio_started && s31_radio_suspended);
        break;
    case 7:
        s31_radio_configured_features = S31_RADIO_FEATURE_BLUETOOTH;
        CHECK(esp32s31_radio_suspend(&dev) == 0);
        CHECK(!strcmp(calls, "bsp") && votes == 1);
        break;
    case 8:
        s31_radio_configured_features = S31_RADIO_FEATURE_WIFI;
        CHECK(esp32s31_radio_suspend(&dev) == 0);
        CHECK(!strcmp(calls, "wsp") && votes == 1);
        break;
    default: return 2;
    }
    return 0;
}
'''
        (cls.work / "suspend.c").write_text(harness)
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if os.environ.get("S31_TEST_SANITIZERS") else []
        subprocess.run(["cc", "-O1", "-g", "-Wall", "-Wextra", "-Werror", *flags,
                        str(cls.work / "suspend.c"), "-o", str(cls.work / "suspend")], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_suspend_paths(self):
        for case in range(9):
            with self.subTest(case=case):
                result = subprocess.run([str(self.work / "suspend"), str(case)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
