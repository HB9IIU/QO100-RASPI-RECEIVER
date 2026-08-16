#!/bin/bash
# Adds a screenshot to the browsable album (screenshots/album/): a
# timestamped copy, a thumbnail, then regenerates index.html. Prunes to the
# most recent $ALBUM_KEEP images so the album doesn't grow forever.
#
# Called automatically by scripts/screenshot.sh and the in-app SNAP button
# (qo100datv.cpp's screenshot_btn_cb) - can also be run by hand to reindex
# after manually dropping PNGs into screenshots/album/.
#
# Usage: scripts/album_update.sh [source.png]
#   No argument: just reprune/reindex whatever's already in screenshots/album/.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ALBUM_DIR="$SCRIPT_DIR/../screenshots/album"
THUMB_DIR="$ALBUM_DIR/thumbs"
ALBUM_KEEP="${ALBUM_KEEP:-300}"

mkdir -p "$ALBUM_DIR" "$THUMB_DIR"

SRC="$1"
if [ -n "$SRC" ]; then
    if [ ! -f "$SRC" ]; then
        echo "album_update.sh: source '$SRC' not found" >&2
        exit 1
    fi
    STAMP="$(date +%Y-%m-%d_%H-%M-%S)"
    DEST="$ALBUM_DIR/$STAMP.png"
    cp "$SRC" "$DEST"
    convert "$DEST" -resize 320x "$THUMB_DIR/$STAMP.png"
fi

# Prune oldest beyond ALBUM_KEEP (newest-first listing, drop the tail).
mapfile -t FILES < <(ls -1t "$ALBUM_DIR"/*.png 2>/dev/null)
if [ "${#FILES[@]}" -gt "$ALBUM_KEEP" ]; then
    for f in "${FILES[@]:$ALBUM_KEEP}"; do
        rm -f "$f" "$THUMB_DIR/$(basename "$f")"
    done
fi

# Regenerate index.html, newest first.
INDEX="$ALBUM_DIR/index.html"
{
    echo '<!DOCTYPE html><html><head><meta charset="utf-8">'
    echo '<title>QO-100 DATV screenshots</title>'
    echo '<meta name="viewport" content="width=device-width, initial-scale=1">'
    echo '<style>
body{background:#111;color:#eee;font-family:sans-serif;margin:1rem}
h1{font-size:1.2rem;font-weight:normal}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(160px,1fr));gap:10px}
.grid a{display:block}
.grid img{width:100%;border-radius:4px;display:block}
.grid figcaption{font-size:0.75rem;text-align:center;color:#aaa;margin-top:2px}
</style></head><body>'
    echo "<h1>QO-100 DATV screenshots ($(ls -1 "$ALBUM_DIR"/*.png 2>/dev/null | wc -l))</h1>"
    echo '<div class="grid">'
    for f in $(ls -1t "$ALBUM_DIR"/*.png 2>/dev/null); do
        name="$(basename "$f")"
        echo "<figure><a href=\"$name\"><img src=\"thumbs/$name\" loading=\"lazy\"></a><figcaption>${name%.png}</figcaption></figure>"
    done
    echo '</div></body></html>'
} > "$INDEX"
