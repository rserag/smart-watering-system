#!/usr/bin/env python3
"""Check compiled ESP32 frames that caused the Telegram task stack overflow.

Run after `pio run -e esp32dev`. This checks actual Xtensa frame allocations;
the runtime watermark separately measures nested library/HTTPS stack use.
"""
import argparse
from pathlib import Path
import re
import subprocess


def main() -> None:
    firmware_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path, help="Built ELF or unlinked Telegram object file",
                        default=firmware_root / ".pio/build/esp32dev/firmware.elf")
    parser.add_argument("--objdump", type=Path,
                        default=Path.home() / ".platformio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-objdump")
    args = parser.parse_args()
    disassembly = subprocess.check_output([str(args.objdump), "-dC", str(args.elf)], text=True)
    limits = {"loadAudit()": 256, "persistAuditLocked()": 256, "workerLoop()": 1024}
    failed = False
    for method, maximum in limits.items():
        function = re.escape("watering::TelegramNotifier::" + method)
        match = re.search(r"^[0-9a-f]+ <" + function + r">:\n[^\n]*\bentry\s+a1,\s*(0x[0-9a-f]+|\d+)",
                          disassembly, re.MULTILINE)
        if match is None:
            print(f"FAIL: compiled frame not found for {method}")
            failed = True
            continue
        frame = int(match.group(1), 0)
        passed = frame <= maximum
        print(f"{'PASS' if passed else 'FAIL'}: {method} frame {frame} bytes (limit {maximum})")
        failed |= not passed
    if failed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
