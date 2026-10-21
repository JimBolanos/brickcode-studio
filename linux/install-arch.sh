#!/bin/bash
# BrickCode Studio - Arch / Omarchy installer.
# Builds nqc, nbc (compile-only) and the app from source into ~/.local/brickcode.
# Only the pacman packages and the optional udev rule need sudo.
set -e

PREFIX="$HOME/.local/brickcode"
SRC="$(cd "$(dirname "$0")/.." && pwd)/brickcode-studio"
FW="$(cd "$(dirname "$0")/.." && pwd)/firmware"
WORK="$HOME/.cache/brickcode-build"
mkdir -p "$PREFIX/bin" "$PREFIX/share/brickcode/nbc" "$PREFIX/share/firmware" "$WORK"
[ -f "$SRC/brickcode-studio.c" ] || { echo "Cannot find $SRC/brickcode-studio.c"; exit 1; }

echo "==> 1/5 Packages (sudo password needed)"
sudo pacman -S --needed --noconfirm base-devel git gtk4 libusb pkgconf fpc bison flex perl

echo "==> 2/5 nqc (RCX / Scout)"
[ -d "$WORK/nqc" ] || git clone --depth 1 https://github.com/jverne/nqc.git "$WORK/nqc"
( cd "$WORK/nqc"
  # nqc waits ~0.5 s for a reply the RCX never sends to remote/message commands.
  # Re-enable the "no reply expected" case (commented out upstream) so the Remote is fast.
  perl -0pi -e 's|/\*\n(\s*case kRCX_Message:)|$1|; s|(return 0;\s*// unconfirmed\n)\*/\n|$1|' rcxlib/RCX_Link.cpp
  # Enable the Linux USB tower driver (commented out upstream). The old LegoUSB header is
  # not available any more, so a tiny stand-in is used (the kernel dropped that ioctl).
  sed -i 's/^\t#USBOBJ = RCX_USBTowerPipe_linux/\tUSBOBJ = RCX_USBTowerPipe_linux/' Makefile
  # The tower file name is hard-coded to the old /dev/usb/lego0; current kernels use legousbtower0.
  sed -i 's|/dev/usb/lego0|/dev/usb/legousbtower0|' rcxlib/RCX_USBTowerPipe_linux.cpp
  # Its Read() sleeps half of the timeout before every read (0.5 s per message); the macOS pipe does not.
  sed -i 's|^\tusleep(timeout_ms \* 1000 / 2);|\t/* no sleep */|' rcxlib/RCX_USBTowerPipe_linux.cpp
  # The kernel driver ignores the old timeout ioctl and blocks 200 ms on every empty read.
  # Wait with poll() and the requested timeout instead (like the macOS pipe does).
  sed -i 's|^#include <sys/ioctl.h>|#include <sys/ioctl.h>\n#include <poll.h>|' rcxlib/RCX_USBTowerPipe_linux.cpp
  perl -0pi -e 's|\n\tSetTimeout\(timeout_ms\);\n|\n\t{ struct pollfd p; p.fd = fd; p.events = POLLIN; p.revents = 0;\n\t  if (poll(&p, 1, (int)timeout_ms) <= 0) return 0; }\n|' rcxlib/RCX_USBTowerPipe_linux.cpp
  mkdir -p "$WORK/shim/LegoUSB"
  cat > "$WORK/shim/LegoUSB/legousbtower.h" <<'EOF'
#ifndef LEGOUSBTOWER_SHIM_H
#define LEGOUSBTOWER_SHIM_H
#include <sys/ioctl.h>
#define LEGO_TOWER_SET_READ_TIMEOUT _IOW('u', 0xFE, unsigned int)
#endif
EOF
  export CPLUS_INCLUDE_PATH="$WORK/shim"
  make clean >/dev/null 2>&1 || true
  make bin; make bin/mkdata; make nqh; make nub        # generate files in order
  make compiler/parse.cpp; make compiler/lexer.cpp
  make -j"$(nproc)"
  cp bin/nqc "$PREFIX/bin/nqc"
  cp firmware/*.lgo "$PREFIX/share/firmware/" )
cp "$FW"/*.lgo "$PREFIX/share/firmware/"      # bundled copies (offline)

echo "==> 3/5 nbc (NXT compiler, compile-only: the app uploads over USB itself)"
[ -d "$WORK/nbc-compiler" ] || git clone --depth 1 https://github.com/pierre-24/nbc-compiler.git "$WORK/nbc-compiler"
( cd "$WORK/nbc-compiler/src/NXT"
  make clean >/dev/null 2>&1 || true
  make all PFLAGS="-S2cdghi -dRELEASE -vewnhi -l -Fu../commons -Fu. -Fu../bricktools"
  cp nbc "$PREFIX/bin/nbc"
  cp NBCCommon.h NXCDefs.h NXTDefs.h "$PREFIX/share/brickcode/nbc/" )

echo "==> 4/5 BrickCode Studio"
cc -O2 $(pkg-config --cflags gtk4 libusb-1.0) -o "$PREFIX/bin/brickcode-studio" \
   "$SRC/brickcode-studio.c" $(pkg-config --libs gtk4 libusb-1.0) -lm
mkdir -p "$HOME/.local/share/applications" "$HOME/.local/share/icons/hicolor/256x256/apps"
cp "$SRC/icon-256.png" "$HOME/.local/share/icons/hicolor/256x256/apps/brickcode-studio.png"
sed "s|^Exec=.*|Exec=$PREFIX/bin/brickcode-studio|" "$SRC/brickcode-studio.desktop" \
   > "$HOME/.local/share/applications/brickcode-studio.desktop"
update-desktop-database "$HOME/.local/share/applications" >/dev/null 2>&1 || true

echo "==> 5/5 USB access for the NXT (sudo, optional)"
read -r -p "Install the udev rule so the NXT works without root? [Y/n] " a
if [ "${a:-Y}" != "n" ] && [ "${a:-Y}" != "N" ]; then
  sudo install -Dm644 "$SRC/../nbc-git/70-nxt.rules" /etc/udev/rules.d/70-nxt.rules
  sudo udevadm control --reload-rules && sudo udevadm trigger
fi

echo
echo "Done. Start it from the app menu (BrickCode Studio) or run:"
echo "  $PREFIX/bin/brickcode-studio"
echo "Unplug and plug the NXT again so the USB rule applies."
