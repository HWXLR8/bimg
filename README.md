# bimg

Minimal Wayland image viewer

## Building

```
make
```

Requirements:

- wayland-client
- egl
- glesv2
- wayland-egl
- libjpeg
- libturbojpeg
- libpng
- libwebp
- wayland-protocols

## Usage

```
bimg [-f] [-o OUT] [-l|--jpeg-lossless] <file-or-dir>...
```

With no arguments, scans the current directory.

- `-f` — overwrite existing crop output
- `-o OUT` — explicit output path
- `-l` — JPEG crops use the lossless JPEG coder (origin snaps to MCU)

## Keys

View:

| Key | Action |
|-----|--------|
| `←` / `s` | previous |
| `→` / `t` | next |
| `+` / `-` / wheel | zoom |
| drag | pan |
| `1` | 1:1 |
| `0` | fit |
| `f` | fullscreen |
| `c` | crop |
| `q` / `Esc` | quit |

Crop:

| Key | Action |
|-----|--------|
| left-drag | create/move/resize |
| right-drag | pan |
| `1` | 1:1 |
| `4` | 4:3 |
| `5` | free |
| `6` | 16:9 |
| `o` | original |
| `r` | reset |
| `?` | help |
| Enter | save |
| `c` / `Esc` | back to view |

Crops save as `<name>.crop.<ext>`. Animated GIFs save as WebP.
