#!/bin/bash
# Update the repo from the newest brickcode_studio.zip in ~/Downloads, push, tag, reinstall.
set -e
REPO="$HOME/brickcode-repo"
ZIP="${1:-$(ls -t "$HOME"/Downloads/brickcode_studio*.zip 2>/dev/null | head -1)}"
[ -f "$ZIP" ] || { echo "No zip found in ~/Downloads (or pass the path)"; exit 1; }
[ -d "$REPO/.git" ] || { echo "Repo not found: $REPO"; exit 1; }

TMP="$HOME/Downloads/bcs_new"
rm -rf "$TMP"; mkdir -p "$TMP"
unzip -oq "$ZIP" -d "$TMP"

cd "$REPO"
git pull --rebase --autostash || true
rsync -a --exclude .git "$TMP"/ "$REPO"/

VER=$(sed -n 's/.*APP_VERSION *"\(.*\)".*/\1/p' brickcode-studio/brickcode-studio.c | head -1)
echo "Version: $VER"

git add -A
git commit -m "Update to $VER" || echo "Nothing new to commit"
git push
if git rev-parse "v$VER" >/dev/null 2>&1; then
  echo "Tag v$VER already exists, skipping"
else
  git tag "v$VER" && git push --tags
fi

bash macos/install-mac.sh
rm -rf "$TMP"
echo "Done. Version $VER installed. Create the GitHub release for tag v$VER."
