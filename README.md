# BrickCode Studio

A native GTK4 + C editor that brings classic robot bricks back to life, in one window:

- **NQC** for the RCX, Scout, CyberMaster and Spybotics (compiled and sent by `nqc`)
- **NXC** for the NXT (compiled by `nbc`; the app uploads and starts the program over USB with libusb)

Write a program, press **F5**, and it compiles, downloads and runs on the brick.
Works on **Linux** (Omarchy theme followed live) and **macOS on Apple Silicon**, with no vendor driver.
The interface is available in **English and Spanish** (selector in the top bar).

![NQC mode](docs/screenshot-nqc-en.png)
![NXC mode, Spanish](docs/screenshot-nxc-es.png)

## Features

- Syntax highlighting, error line highlighted in the editor
- Compile / Run / Download from the top bar, console below
- Device chip: shows whether an NXT or an RCX tower is connected
- RCX tools: tower port detection, firmware, datalog, IR range, auto-shutoff
- NXT Explorer: files, battery and firmware over USB

## Install

### Linux (Arch / Omarchy)

```
cd nbc-git && makepkg -si                 # NXC compiler + udev rules for the NXT
cd ../brickcode-studio && makepkg -si
```
For NQC also install `nqc` (AUR). Other distros: `cd brickcode-studio && make && sudo make install`.

### macOS (Apple Silicon)

Needs Homebrew and the Xcode Command Line Tools.
```
bash macos/install-mac.sh
```
It builds nqc, nbc and the app into `~/.local/brickcode` and creates `~/Applications/BrickCode Studio.app`.
The RCX needs its firmware once: gear button → Brick utilities → pick
`~/.local/brickcode/share/firmware/firm0328.lgo` → Send firmware
(keep the RCX 10-20 cm from the tower, about 3-5 minutes).

## Examples

See the [`examples`](examples) folder: `forward_and_back.nqc` (RCX / Scout) and `forward_and_back.nxc` (NXT).

## Español

BrickCode Studio es un editor nativo para programar ladrillos clásicos: **NQC** (RCX, Scout) y **NXC** (NXT).
Escribe el programa, pulsa **F5** y se compila, se descarga y se ejecuta en el ladrillo.
La interfaz está en inglés y español (selector en la barra superior). La instalación es la misma de arriba.

## Notes

- nbc is built compile-only; sending the program to the NXT is done by the app itself.
- Settings are stored in `~/.config/brickcode-studio/settings.ini`.

## Legal

BrickCode Studio is an independent project. It is not affiliated with, or endorsed by, the LEGO Group.
LEGO and MINDSTORMS are trademarks of the LEGO Group. nqc (Mozilla Public License) and nbc (GPL) are separate programs by their authors.

MIT License - © 2026 Jim Bolaños · [github.com/JimBolanos](https://github.com/JimBolanos)
