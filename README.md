# Wig

Wig is a GTK web browser that uses WPE WebKit as its rendering engine.

> [!IMPORTANT]
> Wig is under active development. Some features may be incomplete or unstable.

## Description

Wig is a minimal but fully functional web browser.

It is built around [`WPEPlatformGTK`](https://github.com/Igalia/wpe-platform-gtk), which handles fetching, parsing, and rendering web pages, and provides a GTK widget that can be integrated with a minimal amount of code.

The user interface is implemented using GTK and Libadwaita and includes common web browser features such as tab-based navigation.

## Screenshots

![Main window](docs/screenshots/main.png)

## Dependencies

- wpe-platform-gtk-1.0
- wpe-webkit-2.0 (>= 2.51.3)
- libadwaita-1 (>= 1.6)

## Building and Installation

This project uses the [Meson](https://mesonbuild.com) build system and follows standard workflow:

```sh
meson setup builddir
meson compile -C builddir
meson install -C builddir
```

## Usage

```sh
wig https://wpewebkit.org
```

## License

This project is licensed under the terms of the MIT license.
