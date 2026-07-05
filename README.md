# Disc-O-Matsu

USB CD/DVD audio player for the Tanmatsu badge. Plug a USB CD/DVD drive into
the USB-A port, insert an audio CD, and it plays the tracks over the
speaker or headphones. If WiFi is available it also grabs the artist,
album, track names, and cover art from MusicBrainz.

## Install

Fresh clone, nothing set up yet:

```powershell
.\setup.ps1 -SetupSdk -SetupBadgeLink
.\install-badgelink.ps1
```

That pulls in ESP-IDF and BadgeLink tooling next to the source, builds the
firmware, and installs it on a Tanmatsu that's in BadgeLink mode. Already
have ESP-IDF/BadgeLink set up? Skip the flags you don't need, or just run
`.\install-badgelink.ps1` directly.

MIT licensed, see [LICENSE](LICENSE).
