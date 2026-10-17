#!/bin/bash
# BrickCode Studio - macOS (Apple Silicon) installer.
# Builds nqc, nbc (compile-only) and the app from source. No sudo.
# Everything goes to ~/.local/brickcode and ~/Applications.
set -e

PREFIX="$HOME/.local/brickcode"
SRC="$(cd "$(dirname "$0")/.." && pwd)/brickcode-studio"
FW="$(cd "$(dirname "$0")/.." && pwd)/firmware"
WORK="$HOME/.cache/brickcode-build"
mkdir -p "$PREFIX/bin" "$PREFIX/share/brickcode/nbc" "$PREFIX/share/firmware" "$WORK"

command -v brew >/dev/null || { echo "Install Homebrew first: https://brew.sh"; exit 1; }
[ -f "$SRC/brickcode-studio.c" ] || { echo "Cannot find $SRC/brickcode-studio.c"; exit 1; }

echo "==> 1/4 Homebrew packages"
brew install gtk4 libusb pkg-config fpc bison flex

# macOS yacc/flex are Xcode stubs: use Homebrew's bison/flex through a shim
mkdir -p "$WORK/shims"
printf '#!/bin/sh\nexec "%s/bin/bison" -y "$@"\n' "$(brew --prefix bison)" > "$WORK/shims/yacc"
chmod +x "$WORK/shims/yacc"
export PATH="$WORK/shims:$(brew --prefix flex)/bin:$(brew --prefix bison)/bin:$PATH"

echo "==> 2/4 nqc (RCX / Scout)"
[ -d "$WORK/nqc" ] || git clone --depth 1 https://github.com/jverne/nqc.git "$WORK/nqc"
( cd "$WORK/nqc"
  make clean >/dev/null 2>&1 || true
  make bin; make bin/mkdata; make nqh; make nub        # generate files in order
  make compiler/parse.cpp; make compiler/lexer.cpp
  make -j"$(sysctl -n hw.ncpu)"
  cp bin/nqc "$PREFIX/bin/nqc"
  cp firmware/*.lgo "$PREFIX/share/firmware/" )
cp "$FW"/*.lgo "$PREFIX/share/firmware/"      # bundled copies (offline)

echo "==> 3/4 nbc (NXT compiler, compile-only: the app uploads over USB itself)"
[ -d "$WORK/nbc-compiler" ] || git clone --depth 1 https://github.com/pierre-24/nbc-compiler.git "$WORK/nbc-compiler"
( cd "$WORK/nbc-compiler/src/NXT"
  make clean >/dev/null 2>&1 || true
  make all PFLAGS="-S2cdghi -dRELEASE -vewnhi -l -Fu../commons -Fu. -Fu../bricktools"
  cp nbc "$PREFIX/bin/nbc"
  cp NBCCommon.h NXCDefs.h NXTDefs.h "$PREFIX/share/brickcode/nbc/" )

echo "==> 4/4 BrickCode Studio"
cc -O2 $(pkg-config --cflags gtk4 libusb-1.0) -o "$PREFIX/bin/brickcode-studio" \
   "$SRC/brickcode-studio.c" $(pkg-config --libs gtk4 libusb-1.0)

# A small .app so it can be started from Finder / Spotlight
APP="$HOME/Applications/BrickCode Studio.app"
mkdir -p "$APP/Contents/MacOS"
cat > "$APP/Contents/MacOS/BrickCodeStudio" <<LAUNCH
#!/bin/bash
export PATH="/opt/homebrew/bin:/usr/local/bin:\$PATH"
exec "$PREFIX/bin/brickcode-studio" "\$@"
LAUNCH
chmod +x "$APP/Contents/MacOS/BrickCodeStudio"
# App icon (.icns built from the PNG)
mkdir -p "$APP/Contents/Resources" /tmp/cbs.iconset
for s in 16 32 64 128 256 512; do
  sips -z $s $s "$SRC/brickcode-studio.png" --out /tmp/cbs.iconset/icon_${s}x${s}.png >/dev/null
done
iconutil -c icns /tmp/cbs.iconset -o "$APP/Contents/Resources/AppIcon.icns" || true
rm -rf /tmp/cbs.iconset
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>BrickCode Studio</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundleExecutable</key><string>BrickCodeStudio</string>
  <key>CFBundleIdentifier</key><string>org.brickcode.Studio</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleVersion</key><string>2.0.5</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST

echo
echo "Done. Start it from: $APP"
echo "or in a terminal:    $PREFIX/bin/brickcode-studio"
echo "RCX firmware files:  $PREFIX/share/firmware  (e.g. firm0328.lgo)"
