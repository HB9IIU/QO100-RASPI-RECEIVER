#!/bin/bash
# Adds a screenshot to the browsable album (screenshots/album/): a
# timestamped copy, a thumbnail, then regenerates index.html. Prunes to the
# most recent $ALBUM_KEEP images so the album doesn't grow forever.
#
# Called automatically by scripts/screenshot.sh (and so, in turn, by
# album_server.py's POST /snap route) - can also be run by hand to reindex
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
.top{display:flex;align-items:center;gap:16px;flex-wrap:wrap}
h1{font-size:1.2rem;font-weight:normal;margin:0}
button{background:#2b8ea3;color:#fff;border:none;border-radius:6px;padding:10px 18px;font-size:1rem;cursor:pointer}
button:active{background:#236f80}
#bulk-delete button{background:#a33232}
#bulk-delete button:active{background:#802626}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(160px,1fr));gap:10px;margin-top:16px}
figure{position:relative;margin:0}
figure a{display:block}
figure img{width:100%;border-radius:4px;display:block}
figure figcaption{font-size:0.75rem;text-align:center;color:#aaa;margin-top:2px}
.sel{position:absolute;top:6px;left:6px;width:20px;height:20px;z-index:1}
.del{position:absolute;top:4px;right:4px;margin:0}
.del button{background:rgba(20,20,20,0.75);color:#fff;border:none;border-radius:4px;
  width:26px;height:26px;padding:0;font-size:1rem;line-height:1;cursor:pointer}
.del button:active{background:#a33232}
</style></head><body>'
    echo "<h1>QO-100 DATV screenshots ($(ls -1 "$ALBUM_DIR"/*.png 2>/dev/null | wc -l))</h1>"
    echo '<div class="top">'
    echo '<form method="POST" action="/snap"><button type="submit">Take Screenshot</button></form>'
    echo '<form id="bulk-delete" method="POST" action="/delete" onsubmit="return confirm(&quot;Delete selected screenshots?&quot;)"><button type="submit">Delete Selected</button></form>'
    echo '</div>'
    echo '<div class="grid">'
    for f in $(ls -1t "$ALBUM_DIR"/*.png 2>/dev/null); do
        name="$(basename "$f")"
        echo "<figure>"
        echo "<input type=\"checkbox\" class=\"sel\" name=\"name\" value=\"$name\" form=\"bulk-delete\" title=\"select for bulk delete\">"
        echo "<form class=\"del\" method=\"POST\" action=\"/delete\" onsubmit=\"return confirm('Delete this screenshot?')\"><input type=\"hidden\" name=\"name\" value=\"$name\"><button type=\"submit\" title=\"delete\">&times;</button></form>"
        echo "<a href=\"$name\"><img src=\"thumbs/$name\" loading=\"lazy\"></a><figcaption>${name%.png}</figcaption>"
        echo "</figure>"
    done
    echo '</div></body></html>'
} > "$INDEX"
