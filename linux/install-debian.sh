#!/bin/bash
# BrickCode Studio - Debian / Ubuntu / Linux Mint installer.
# Builds nqc, nbc (compile-only) and the app from source into ~/.local/brickcode.
# Only the apt packages and the optional udev rule need sudo.
set -e

PREFIX="$HOME/.local/brickcode"
SRC="$(cd "$(dirname "$0")/.." && pwd)/brickcode-studio"
FW="$(cd "$(dirname "$0")/.." && pwd)/firmware"
WORK="$HOME/.cache/brickcode-build"
mkdir -p "$PREFIX/bin" "$PREFIX/share/brickcode/nbc" "$PREFIX/share/firmware" "$WORK"
[ -f "$SRC/brickcode-studio.c" ] || { echo "Cannot find $SRC/brickcode-studio.c"; exit 1; }

echo "==> 1/5 Packages (sudo password needed)"
sudo apt install -y build-essential git pkg-config libgtk-4-dev libusb-1.0-0-dev \
    fpc bison flex

echo "==> 2/5 nqc (RCX / Scout)"
[ -d "$WORK/nqc" ] || git clone --depth 1 https://github.com/jverne/nqc.git "$WORK/nqc"
( cd "$WORK/nqc"
  # nqc waits ~0.5 s for a reply the RCX never sends to remote/message commands.
  # Re-enable the "no reply expected" case (commented out upstream) so the Remote is fast.
  perl -0pi -e 's|/\*\n(\s*case kRCX_Message:)|$1|; s|(return 0;\s*// unconfirmed\n)\*/\n|$1|' rcxlib/RCX_Link.cpp
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
