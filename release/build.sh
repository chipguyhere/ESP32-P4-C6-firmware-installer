#!/bin/sh
# Build the InstallFirmware example for the web installer:
#   release/build.sh
# writes web-install/{bootloader,partitions,boot_app0,installer}.bin and
# launchpad.toml.  Four parts rather than one merged image, so that flashing
# never touches NVS (0x9000-0xDFFF).  Needs arduino-cli with the esp32 core and
# this library's dependencies installed.
set -e
cd "$(dirname "$0")/.."
FQBN=esp32:esp32:esp32p4:PSRAM=enabled,FlashSize=16M,CDCOnBoot=default,PartitionScheme=custom
OUT=$(mktemp -d)
arduino-cli compile --fqbn "$FQBN" --build-path "$OUT" examples/InstallFirmware
CORE=$(arduino-cli config get directories.data)/packages/esp32/hardware/esp32
CORE=$CORE/$(ls "$CORE" | sort -V | tail -1)
mkdir -p web-install
cp "$OUT/InstallFirmware.ino.bootloader.bin" web-install/bootloader.bin
cp "$OUT/InstallFirmware.ino.partitions.bin" web-install/partitions.bin
cp "$CORE/tools/partitions/boot_app0.bin"     web-install/boot_app0.bin
cp "$OUT/InstallFirmware.ino.bin"            web-install/installer.bin
VERSION=$(sed -n 's/^version=//p' library.properties)
sed "s/@VERSION@/version $VERSION/" release/launchpad.toml.in > launchpad.toml
rm -rf "$OUT"
ls -l web-install launchpad.toml
