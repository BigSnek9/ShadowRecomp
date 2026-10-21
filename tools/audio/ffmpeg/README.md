# Portable ffmpeg (song decoder fallback for the Stage Workshop)

Used by tools/audio/adx.py `song_to_wav` only when Windows' own Media Foundation cannot read a song (e.g. OGG on a PC
without Microsoft's Web Media Extensions). Called by its full path from this folder: no PATH, no installer, nothing for
the user to add.

| | |
|---|---|
| Name | FFmpeg (ffmpeg.exe only; ffprobe / ffplay / docs left out) |
| Version | n8.1.3-18-g46d899d7a0-20261018 |
| Build | ffmpeg-n8.1-latest-win64-lgpl-8.1.zip from https://github.com/BtbN/FFmpeg-Builds (release "latest", 2026-10-18), win64, static |
| Size | ffmpeg.exe 128 MB (zip 170.5 MB, sha256 abff292c5cee9f44e3c4d2830ba424708f7135601c998830faa3b22d79542454) |
| ffmpeg.exe sha256 | d99608a9f2662003104c921ce23e7aa857b3a9bcac368b936e4fb01dcf2110b7 |
| Licence | GNU LGPL version 3 or later (built with --enable-version3, without --enable-gpl / --enable-nonfree): LICENSE.txt in this folder |

Redistribution: ffmpeg.exe is shipped unmodified with its licence text. The corresponding source is FFmpeg n8.1 from
https://ffmpeg.org/download.html (git tag n8.1 + the build scripts at https://github.com/BtbN/FFmpeg-Builds); keep this
README and LICENSE.txt next to the exe when the project is shared.

Added 2026-10-19.
