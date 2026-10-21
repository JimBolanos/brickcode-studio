#!/bin/bash
# BrickCode Studio - Linux uninstaller (Arch/Omarchy, Fedora, Debian/Ubuntu).
# Removes what the installers put in your home folder, and asks before touching
# system files. Your repo and your own programs are not touched.

PREFIX="$HOME/.local/brickcode"
echo "This will remove:"
echo "  $PREFIX  (app, nqc, nbc, firmware)"
echo "  ~/.local/share/applications/brickcode-studio.desktop and its icon"
echo "  ~/.config/brickcode-studio  (saved settings)"
echo "  ~/.cache/brickcode-build  (build cache)"
read -r -p "Continue? [y/N] " a
[ "$a" = "y" ] || [ "$a" = "Y" ] || { echo "Cancelled."; exit 0; }

rm -rf "$PREFIX" "$HOME/.config/brickcode-studio" "$HOME/.cache/brickcode-build"
rm -f "$HOME/.local/share/applications/brickcode-studio.desktop" \
      "$HOME/.local/share/icons/hicolor/256x256/apps/brickcode-studio.png"
update-desktop-database "$HOME/.local/share/applications" >/dev/null 2>&1 || true

# System packages (older installs made with makepkg -si)
if command -v pacman >/dev/null 2>&1; then
  pk=$(pacman -Qq 2>/dev/null | grep -E '^(brickcode-studio|brickcode-studio-debug|nbc-git|nbc-git-debug|nqc)$')
  if [ -n "$pk" ]; then
    echo "System packages found: $(echo $pk)"
    read -r -p "Remove them with pacman? [y/N] " a
    [ "$a" = "y" ] || [ "$a" = "Y" ] && sudo pacman -Rns $pk
  fi
fi

# Old manual installs and the NXT udev rule
for f in /usr/local/bin/nbc /usr/local/bin/nqc /usr/local/bin/brickcode-studio /etc/udev/rules.d/70-nxt.rules; do
  if [ -e "$f" ]; then
    read -r -p "Remove $f (sudo)? [y/N] " a
    [ "$a" = "y" ] || [ "$a" = "Y" ] && sudo rm -f "$f"
  fi
done
sudo udevadm control --reload-rules 2>/dev/null || true

echo "Done. Check with: which nqc nbc brickcode-studio"
