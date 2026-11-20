"""Make the public demo's library: a slice of sim/data that can go on the web.

The simulator's own copy (sim/data, from tools/sim_data.py) is the whole Music
Assistant library. The public demo gets about 220 albums, picked so every
letter has its share (fast browse lands on a letter, so the spread matters more
than the count), a few playlists, and a Recent made only of what's in the demo.
Nothing here talks to Music Assistant or Home Assistant.

    python3 tools/sim_demo.py <sim/data> <out/data> [<out/index.html>]

With the page given, its heading becomes the public one, with a link back to the project.
"""
import json
import shutil
import sys
from collections import defaultdict
from pathlib import Path

TARGET_ALBUMS = 220
MIN_PER_LETTER = 3
PLAYLISTS = ["Boiler Room", "Hackers OST", "Manhattan's", "Prince", "Sharam — Warung Beach Club"]
RECENT = 16

src, out = Path(sys.argv[1]), Path(sys.argv[2])
lib = json.loads((src / "library.json").read_text())


def letter(album):
    c = (album.get("sort_name") or album["name"])[:1].upper()
    return c if c.isalpha() else "#"


# Albums: each letter's share of the library, at least a few, spaced evenly
# through that letter so its artists vary.
by_letter = defaultdict(list)
for a in sorted(lib["albums"], key=lambda a: a.get("sort_name") or a["name"]):
    if a.get("cover"):
        by_letter[letter(a)].append(a)
total = sum(len(v) for v in by_letter.values())
keep = set()
for albums in by_letter.values():
    n = min(len(albums), max(MIN_PER_LETTER, round(TARGET_ALBUMS * len(albums) / total)))
    for i in range(n):
        keep.add(albums[int(i * len(albums) / n)]["id"])
# what was recently played from an album stays in, so Recent has its albums
keep |= {r["id"] for r in lib.get("recent", []) if r["type"] == "album"}
albums = [a for a in lib["albums"] if a["id"] in keep]

playlists = [p for p in lib.get("playlists", []) if p["name"] in PLAYLISTS]
pl_ids = {p["id"] for p in playlists}

# Recent: the real history where it's in the demo, then topped up from the demo
# playlists and albums, so it has a mix to step through.
recent = [r for r in lib.get("recent", [])
          if (r["type"] == "album" and r["id"] in keep) or (r["type"] == "playlist" and r["id"] in pl_ids)]
fill = [{"type": "playlist", "id": p["id"], "track": t["name"]} for p in playlists for t in p["tracks"][:2]]
fill += [{"type": "album", "id": a["id"], "track": a["tracks"][0]["name"]} for a in albums[::40] if a["tracks"]]
for r in fill:
    if len(recent) >= RECENT:
        break
    if r not in recent:
        recent.insert(len(recent) // 2, r)



def copy_clean(src_file, dst_dir):
    """Copy a JPEG without its metadata (Exif, XMP, Photoshop blocks), leaving the image untouched."""
    raw = src_file.read_bytes()
    keep, i = bytearray(raw[:2]), 2
    while i + 4 <= len(raw) and raw[i] == 0xFF:
        marker, length = raw[i + 1], int.from_bytes(raw[i + 2:i + 4], "big")
        if marker == 0xDA:  # start of the image data: the rest is copied as it is
            break
        if marker not in (0xE1, 0xED, 0xFE):  # APP1 Exif/XMP, APP13 Photoshop, comments
            keep += raw[i:i + 2 + length]
        i += 2 + length
    (dst_dir / src_file.name).write_bytes(bytes(keep) + raw[i:])


if out.exists():
    shutil.rmtree(out)
(out / "covers").mkdir(parents=True)
(out / "tracks").mkdir()
for a in albums:
    copy_clean(src / "covers" / f"{a['id']}.jpg", out / "covers")
pictures = 0
for p in playlists:
    for t in p["tracks"]:
        f = src / "tracks" / f"{t['id']}.jpg"
        if t.get("picture") and f.exists():
            copy_clean(f, out / "tracks")
            pictures += 1

demo = {
    "radio": False,
    "about": "A demo library: {} albums and {} playlists from Darren's own collection.".format(len(albums), len(playlists)),
    "albums": albums,
    "playlists": playlists,
    "recent": recent[:RECENT],
}
(out / "library.json").write_text(json.dumps(demo, ensure_ascii=False))
letters = defaultdict(int)
for a in albums:
    letters[letter(a)] += 1
print(f"{len(albums)} albums, {len(playlists)} playlists, {pictures} track pictures, {len(demo['recent'])} recent")
print(" ".join(f"{k}:{letters[k]}" for k in sorted(letters)))

if len(sys.argv) > 3:
    page = Path(sys.argv[3])
    html = page.read_text()
    for a, b in [
        ("<title>Knob Simulator</title>", "<title>Music Assistant Smart Dial: try it</title>"),
        ("<h1>Bathroom Music Knob</h1>", "<h1>Music Assistant Smart Dial</h1>"),
        ("<p>Simulator: the knob's own screen code, running in the browser</p>",
         "<p>The dial's own screen code, running in your browser. "
         '<a href="https://github.com/sygad/music-assistant-smart-dial">About the project</a></p>'),
    ]:
        assert a in html, a
        html = html.replace(a, b)
    page.write_text(html)
