"""Compile the battery snapshot formatter and check every form of the 'K' line."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zmk_studio_log/battery.h>
#define snprintk snprintf
/* ACTUAL_FORMAT */
typedef struct zmk_studio_log_battery_half half;
static int failures;
static void expect(const half *left, const half *right, const char *want) {
    char line[64];
    zmk_studio_log_battery_format(line, sizeof(line), left, right);
    if (strcmp(line, want) != 0) {
        fprintf(stderr, "got \"%s\", want \"%s\"\n", line, want);
        failures++;
    }
}
int main(void) {
    half on_battery = {.level = 78};
    half charging = {.level = 78, .powered = true};
    half full = {.level = 100, .powered = true, .full = true};
    half usb_unknown = {.level = -1, .powered = true};
    half not_reported = {.level = -1};
    half right = {.level = 15};
    half right_charging = {.level = 15, .powered = true};

    expect(&on_battery, &right, "left=78 right=15");
    expect(&charging, &right, "left=78 usb right=15");
    expect(&full, &right, "left=100 usb full right=15");
    expect(&usb_unknown, &not_reported, "left=usb right=--");
    expect(&on_battery, &right_charging, "left=78 right=15 usb");
    expect(&charging, &full, "left=78 usb right=100 usb full");
    expect(&on_battery, NULL, "left=78"); /* not a split */

    char small[8];
    if (zmk_studio_log_battery_format(small, sizeof(small), NULL, NULL) != -EINVAL) {
        failures++;
    }
    zmk_studio_log_battery_format(small, sizeof(small), &full, &right); /* truncates, no overflow */
    if (strlen(small) != sizeof(small) - 1) {
        failures++;
    }
    return failures != 0;
}
"""


class BatterySnapshotTests(unittest.TestCase):
    def test_line_forms(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/battery_snapshot.c").read_text()
        start = source.index("static int format_half(")
        end = source.index("\n#if HAS_PERIPHERAL\nstatic struct", start)
        harness = HARNESS.replace("/* ACTUAL_FORMAT */", source[start:end])
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "test_battery_snapshot")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                 "-I", str(root / "include"), "-x", "c", "-", "-o", executable],
                input=harness, text=True, check=True, timeout=30,
            )
            subprocess.run([executable], check=True, timeout=3)


if __name__ == "__main__":
    unittest.main()
