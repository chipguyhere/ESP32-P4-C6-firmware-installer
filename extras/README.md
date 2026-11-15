# extras

## `gsl3680/` - the touch firmware's source form

- `gslx680_firmware_table.h` - Silead's GSL3680 firmware table, as it appears
  in their gslX680 touchscreen driver (GPL v2; see `../LICENSE-GPL-2.0.txt`).
  Pairs of register offset and 32-bit value; offset `0xF0` selects a 128-byte
  page of the controller's RAM, and the 32 entries after it fill that page.
- `make_blob.py` - turns the table into `examples/InstallFirmware/gsl3680_firmware_blob.h`:
  the page-by-page image (page number, then 32 words, little-endian; 18,348
  bytes), its SHA-256, and the image zlib-compressed (9,878 bytes) for NVS.
  Run it with `python3 make_blob.py` after changing the table.

The display libraries check the installed image against the same SHA-256
(`650876e3…94006`), so a different firmware needs a matching change there.

## `c6/` - the ESP32-C6 firmware's provenance

- `make_c6_header.py` - turns an ESP-Hosted co-processor build into
  `examples/InstallFirmware/c6_firmware.h`.
- `README.md` - exactly which build is shipped, and how to make the next one.
