# ices0

ices0 is a command-line source client. It reads a playlist of audio files and sends one continuous MP3 stream to an Icecast server.

This tree is ices 0.4.11, kept so it builds and runs on Ubuntu 26.04 against Icecast 2.5, LAME 3.101, and current libshout. The sample configuration is [`conf/ices.conf.dist.in`](conf/ices.conf.dist.in). Playlist scripts are described in [`README.playlist`](README.playlist).

## What it plays

ices0 reads MP3, Ogg Vorbis, FLAC, and MP4/M4A. Everything it sends to the server is MP3. Vorbis, FLAC, and MP4 are decoded and encoded with LAME, so reencoding has to be on for those files. Turn it on with `-R` or `<Reencode>1</Reencode>` under `Execution`. The stream bitrate is `-b` or `Stream/Bitrate`.

ReplayGain and crossfading also run only while reencoding is on. ReplayGain is read from MP3 (`TXXX:replaygain_track_gain`, and `RVA2` when that TXXX frame is absent), from FLAC and Ogg Vorbis comments, and from the same replay-gain tags in an MP4 file. Crossfade length, a full-volume crossmix, and a minimum track length for the crossfader are `Playlist/Crossfade`, `Playlist/CrossMix`, and `Playlist/MinCrossfade`.

Song titles are sent to Icecast as UTF-8. On an MP3 mount, set `<charset>UTF8</charset>` so Icecast stores them that way. The update is a single `GET /admin/metadata?mode=updinfo` request. libshout's own metadata call asks Icecast 2.5 for `/admin/!POKE`, gets a 400, and retries without pausing, so ices0 does not use it.

A builtin playlist is a text file of paths, one per line. Lines that start with `#` are skipped, which covers M3U and M3U8. Save those files without a byte-order mark. A shell, Perl, or Python module can choose the next file instead. The shell module prints the path on the first line and an optional title on the second. Perl and Python are optional at build time.

Cue-file writing is off unless you pass `-Q` or set `Execution/CueFile` to 1. The cue file is rewritten as the track plays, so leave it off on a small disk. The source login can be a user other than `source`, with `-U` or `Server/Username`.

## Build

Install the libraries and a compiler:

```bash
sudo apt-get install build-essential automake autoconf libtool pkg-config
sudo apt-get install libxml2-dev libogg-dev libvorbis-dev libshout3-dev
sudo apt-get install libmp3lame-dev libflac-dev
sudo apt-get install libavformat-dev libavcodec-dev libswresample-dev libavutil-dev
```

MP4 decoding uses FFmpeg. Ubuntu 26.04 does not ship libmp4v2. `configure` stops if `libavformat`, `libavcodec`, `libswresample`, or `libavutil` is missing. `--with-faad=no` is the switch that builds without MP4.

```bash
aclocal
autoreconf -fi
automake --add-missing
./configure
make
sudo make install
```

`configure` prints a feature list before it finishes. XML, LAME, Vorbis, FLAC, and MP4 should all be `yes`. Python and Perl are `yes` only when their development libraries are installed. The binary is `ices`.

Source files in this repository use LF line endings, including `configure.ac`. A checkout that converts them to CRLF will make `configure` look for `conf/Makefile` with a carriage return in the name and then fail.
