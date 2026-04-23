# Wig

Wig is a web browser built on top of [WPE Platform GTK](https://github.com/Igalia/wpe-platform-gtk).

## Dependencies

- wpe-platform-gtk-1.0
- wpe-webkit-2.0 (>= 2.51.3)
- libadwaita-1 (>= 1.6)

## Building

```sh
meson setup --prefix=/usr builddir
meson compile -C builddir
```

## Installation

```sh
sudo meson install -C builddir
```

## Usage

```sh
wig https://wpewebkit.org
```
