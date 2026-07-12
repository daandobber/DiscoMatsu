# Disc-O-Matsu

USB CD/DVD audio player for the Tanmatsu badge. Plug a USB CD/DVD drive into
the USB-A port, insert an audio CD, and it plays the tracks over the
speaker or headphones. If WiFi is available it also grabs the artist,
album, track names, and cover art from MusicBrainz.

## Features

- Play audio CDs from a USB CD/DVD drive.
- Rip the inserted CD manually with `F3` to WAV files on the SD card under
  `/sd/Music`.
- Scrobble playback to Last.fm automatically when Last.fm is configured.

WAV rips use this layout:

```text
/sd/Music/Artist - Album (Year)/
/sd/Music/Artist - Album (Year)/Artist - Album - 01 - Title (Year).wav
```

For multi-disc releases the disc marker is inserted after the album name:

```text
/sd/Music/Artist - Album - CD1 (Year)/
/sd/Music/Artist - Album - CD1 (Year)/Artist - Album - CD1 - 01 - Title (Year).wav
```

Last.fm needs an API key and shared secret from
https://www.last.fm/api/account/create. Configure these in menuconfig under
`Disc-O-Matsu`. Optionally set the bootstrap username/password there once;
Disc-O-Matsu will request a Last.fm session and then remember only the session
key in NVS.

## Install

Fresh clone, nothing set up yet:

```powershell
.\setup.ps1 -SetupSdk -SetupBadgeLink
.\install-badgelink.ps1
```
