"""
Creates a portable release .zip for Ext4Windows.

Usage:
    python make_portable.py [version]

Example:
    python make_portable.py 1.0.0

Output:
    Ext4Windows-1.0.0-portable.zip
"""

import os
import sys
import shutil
import zipfile

VERSION = sys.argv[1] if len(sys.argv) > 1 else "1.0.0"
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(PROJECT_ROOT, "build")
OUTPUT_NAME = f"Ext4Windows-{VERSION}-portable"
OUTPUT_ZIP = f"{OUTPUT_NAME}.zip"

# Files to include in the portable release
FILES = [
    (os.path.join(BUILD_DIR, "ext4windows.exe"), "ext4windows.exe"),
    (os.path.join(BUILD_DIR, "winfsp-x64.dll"), "winfsp-x64.dll"),
    (os.path.join(PROJECT_ROOT, "LICENSE"), "LICENSE"),
]

def main():
    # Verify all files exist
    for src, _ in FILES:
        if not os.path.exists(src):
            print(f"ERROR: Missing file: {src}")
            print("       Did you build the project first? (cmake --build build)")
            sys.exit(1)

    # Create zip
    print(f"Creating {OUTPUT_ZIP}...")
    with zipfile.ZipFile(OUTPUT_ZIP, "w", zipfile.ZIP_DEFLATED) as zf:
        for src, dst in FILES:
            arc_name = f"{OUTPUT_NAME}/{dst}"
            zf.write(src, arc_name)
            size = os.path.getsize(src) / 1024
            print(f"  + {dst} ({size:.0f} KB)")

        # Add a README.txt with quick start instructions
        readme = f"""\
Ext4Windows v{VERSION} - Portable Release
==========================================

Mount ext4 Linux partitions on Windows.

REQUIREMENTS:
  - Windows 10 or 11 (64-bit)
  - WinFsp: https://winfsp.dev/rel/

QUICK START:
  1. Install WinFsp from the link above
  2. Run ext4windows.exe (interactive menu)

COMMAND LINE:
  ext4windows mount <image.img> [Z:] [--rw]
  ext4windows scan [--rw]
  ext4windows status
  ext4windows unmount Z:
  ext4windows quit

MORE INFO:
  https://github.com/user/ext4windows
"""
        zf.writestr(f"{OUTPUT_NAME}/README.txt", readme)
        print(f"  + README.txt")

    final_size = os.path.getsize(OUTPUT_ZIP) / 1024
    print(f"\nDone! {OUTPUT_ZIP} ({final_size:.0f} KB)")

if __name__ == "__main__":
    main()
