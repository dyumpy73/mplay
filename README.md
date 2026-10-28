# mplay

Minimal FLAC player for the terminal (ALSA).

## Build

Install dependencies (Debian/Ubuntu):

```bash
  sudo apt install libflac-dev libasound2-dev
```

Compile:

```bash
  gcc -O2 -Wall -Wextra mplay.c -o mplay $(pkg-config --cflags --libs flac alsa) -lm

```

## Usage

```bash
  ./mplay song.flac
  ./mplay ~/Music/album/
  ./mplay a.flac b.flac ~/Music/other/
```

## Controls

| Key        | Action     |
|------------|------------|
| Space      | Pause      |
| ← / a      | Back 5s    |
| → / d      | Forward 5s |
| n / p      | Next / Prev|
| q          | Quit       |
