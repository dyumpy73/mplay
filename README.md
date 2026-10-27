# mplay

Minimal FLAC player for the terminal (ALSA).

## Build

Install dependencies (Debian/Ubuntu):

    sudo apt install libflac-dev libasound2-dev

Compile:

    gcc -O3 -Wall mplay.c -o mplay -lFLAC -lasound

## Usage

    ./mplay song.flac
    ./mplay ~/Music/album/
    ./mplay a.flac b.flac ~/Music/other/

## Controls

| Key        | Action     |
|------------|------------|
| Space      | Pause      |
| ← / a      | Back 5s    |
| → / d      | Forward 5s |
| n / p      | Next / Prev|
| q          | Quit       |
