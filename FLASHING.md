# First update (manual / serial)

The running firmware has no working OTA, so the first flash must be over
USB serial. After this update every future firmware/filesystem change can
be done from the **Update** tab at `http://switchctl.local/`.

## What you need

- The device connected over USB to a machine with this repo checked out
  (PlatformIO installed) — or the pre-built `merged-full.bin`
- Membership in the `dialout` group (`ls -l /dev/ttyUSB*` to find the port)

## Option A — PlatformIO (recommended)

```bash
cd switchctl
pio run -t upload      # firmware
pio run -t uploadfs    # filesystem (web UI)
```

The build script resolves `OTA_PASSWORD` from the environment (loud
fallback `ota`). Set it to your real password before building:

```bash
export OTA_PASSWORD='...'
pio run -t upload && pio run -t uploadfs
```

## Option B — esptool with the pre-built merged image

One shot, everything (bootloader, partition table, firmware, filesystem):

```bash
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32 -p /dev/ttyUSB0 -b 460800 \
  write_flash 0x0 .pio/build/az-delivery-devkit-v4/merged-full.bin
```

Do **not** pass `--erase-all`/`-e`: WiFi credentials live in NVS and are
kept, so the device rejoins the network after reboot.

Regenerate the image after any change with:

```bash
B=.pio/build/az-delivery-devkit-v4
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32 merge_bin \
  -o $B/merged-full.bin --flash_mode dio --flash_size 4MB \
  0x1000 $B/bootloader.bin 0x8000 $B/partitions.bin \
  0x10000 $B/firmware.bin 0x290000 $B/spiffs.bin
```

## Verify

1. `http://switchctl.local/api/status` → `"version":"1.1"` plus
   `uptime`, `rssi`, `heap`
2. `http://switchctl.local/` → tabbed UI (Antennas / System / Update)
3. Antenna buttons switch relays (`/api/on?pin=N`)
4. `http://switchctl.local/update` → authenticated OTA page
   (`ota` / your `OTA_PASSWORD`)

From then on: build, then upload `firmware.bin` / `spiffs.bin` from the
Update tab.
