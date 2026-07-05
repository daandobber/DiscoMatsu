# Disc-O-Matsu

Disc-O-Matsu is a USB CD/DVD audio player for Tanmatsu and Konsool.

Plug a USB CD/DVD drive into the Tanmatsu's USB-A host port, insert an audio
CD, and Disc-O-Matsu reads the table of contents and plays tracks back over
the built-in speaker or headphones - keyboard-driven track list, play/pause,
next/previous track, eject, and volume control. If WiFi is available, it
also looks up the album/artist/track titles and cover art online
(MusicBrainz + Cover Art Archive) for discs it recognizes.

## Status

Working end-to-end on real hardware. There is no filesystem on an audio CD,
so this does not use ESP-IDF's `usb_host_msc` component (which only mounts
FAT-formatted drives) - track audio is read directly via SCSI/MMC commands
over a hand-rolled USB Mass Storage Bulk-Only Transport driver.

Features:

- Track list with play/pause, next/previous, and eject (F2).
- Online metadata lookup (artist, album, track titles) and cover art for
  recognized discs - falls back to a generic track list if WiFi isn't
  available or the disc isn't recognized; never blocks playback.
- Detects disc swaps without needing to unplug the USB cable.

Known limitations:

- Only one connected USB drive/LUN is supported at a time.
- Data tracks are listed but not played.
- SCSI/MMC command quirks vary between drives; this has been written to
  spec and tested against a handful of real drives, but not every model.
- Only the first matching MusicBrainz release is used when a disc's table
  of contents matches multiple cataloged pressings.

## Quick Start

Fresh clone, nothing installed yet:

```powershell
.\setup.ps1 -SetupSdk -SetupBadgeLink
.\install-badgelink.ps1
```

That fetches ESP-IDF v5.5.1 and the BadgeLink tooling next to the source,
then builds and installs the app on a Tanmatsu in BadgeLink mode. Skip
`-SetupSdk`/`-SetupBadgeLink` if you already have ESP-IDF/BadgeLink set up
elsewhere.

## Building

This project is based on the Tanmatsu ESP-IDF template.

Build for Tanmatsu:

```powershell
.\esp-idf\export.ps1
idf.py --no-ccache -B build/tanmatsu build -DDEVICE=tanmatsu -DSDKCONFIG_DEFAULTS='sdkconfigs/general;sdkconfigs/tanmatsu' -DSDKCONFIG=sdkconfig_tanmatsu -DIDF_TARGET=esp32p4 -DFAT=0
```

The resulting AppFS binary is:

```text
build/tanmatsu/application.bin
```

## Installing With Badgelink

With the device in Badgelink mode:

```powershell
.\install-badgelink.ps1 -NoBuild
```

By default this installs the app as `discmatsu` with the title `Disc-O-Matsu`.

## App Repository Package

After building the firmware, create the Tanmatsu app-repository folder:

```powershell
.\tools\package-app-repository.ps1
```

The generated folder is:

```text
dist/app-repository/nl.daandobber.discmatsu
```

That folder is meant to be copied into a fork of:

```text
https://github.com/Nicolai-Electronics/app-repository
```

## License

MIT. See [LICENSE](LICENSE).
