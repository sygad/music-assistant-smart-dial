"""Copy the Music Assistant library into the browser simulator (read-only).

Fetches what the knob fetches: every library album (title, artist, id, cover),
each album's tracks, whether there are radio stations, and the covers at 512 px
(scaled to the knob's 360 px with sips); the playlists, their tracks, and each
track's own picture (512 px, kept as it comes: YouTube thumbnails are wide and
some have black bars, which are cropped when shown, as the knob will); and
what was recently played, as albums and playlists with the track last played
in each (a library track counts as its album, a YouTube track as the playlist
it's on). Writes
sim/data/library.json, sim/data/covers/ and sim/data/tracks/, all git-ignored:
the art isn't ours to publish.

Usage: python3 tools/sim_data.py [--no-covers | --recent]
  --recent: only refresh what was recently played (quick), keeping the rest.
Nothing is played or changed. The token is read from config/secrets.local and
never printed.
"""
import concurrent.futures
import json
import subprocess
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import secrets as sec  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "sim" / "data"
COVERS = OUT / "covers"
PICTURES = OUT / "tracks"
cfg = sec.load()
MA = cfg["MA_SERVER"]  # config/secrets.local, as the firmware reads it


def ma(command, **args):
    req = urllib.request.Request(
        MA.rstrip("/") + "/api", data=json.dumps({"command": command, "args": args}).encode(),
        headers={"Authorization": "Bearer " + cfg["MA_TOKEN"], "Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req, timeout=60).read())


def first_artist(item):
    a = (item.get("artists") or [{}])[0]
    return a.get("name"), a.get("sort_name")


def album_of(item):
    proxy = next((i.get("proxy_id") for i in (item.get("metadata") or {}).get("images") or [] if i.get("proxy_id")), None)
    artist, artist_sort = first_artist(item)
    return {"id": item["item_id"], "name": item["name"], "sort_name": item.get("sort_name"),
            "artist": artist, "artist_sort": artist_sort, "proxy": proxy}


def tracks_of(album_id):
    out = []
    for t in ma("music/albums/album_tracks", item_id=album_id, provider_instance_id_or_domain="library"):
        if t.get("name"):
            out.append({"name": t["name"], "seconds": int(t.get("duration") or 0), "artist": first_artist(t)[0]})
    return out


def cover(album):
    dst = COVERS / (album["id"] + ".jpg")
    if dst.exists() or not album["proxy"]:
        return bool(album["proxy"])
    tmp = dst.with_suffix(".tmp.jpg")
    try:
        data = urllib.request.urlopen(f"{MA.rstrip('/')}/imageproxy/{album['proxy']}?size=512", timeout=60).read()
        tmp.write_bytes(data)
        subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", "85", "-z", "360", "360",
                        str(tmp), "--out", str(dst)], check=True, capture_output=True, timeout=60)
        return True
    except Exception as e:  # noqa: BLE001
        print(f"  cover failed for {album['name']}: {e}")
        return False
    finally:
        tmp.unlink(missing_ok=True)


def picture_of(item):
    return next((i.get("proxy_id") for i in (item.get("metadata") or {}).get("images") or [] if i.get("proxy_id")), None)


def playlist_tracks(pl):
    out = []
    for t in ma("music/playlists/playlist_tracks", item_id=pl["item_id"], provider_instance_id_or_domain=pl["provider"]):
        if t.get("name"):
            out.append({"id": t["item_id"], "name": t["name"], "seconds": int(t.get("duration") or 0),
                        "artist": first_artist(t)[0], "proxy": picture_of(t)})
    return out


def picture(track):
    dst = PICTURES / (track["id"] + ".jpg")
    if dst.exists() or not track["proxy"]:
        return
    try:
        dst.write_bytes(urllib.request.urlopen(f"{MA}/imageproxy/{track['proxy']}?size=512", timeout=60).read())
    except Exception as e:  # noqa: BLE001
        print(f"  picture failed for {track['name']}: {e}")


def recent(playlists):
    """Recently played tracks, newest first, partly played ones too (Music
    Assistant counts only finished ones by default, and a DJ set takes an
    hour). Each with where it lives: a library track on its album, a YouTube
    track on the playlist it's on; a track in neither is left out."""
    out, seen = [], set()
    for item in ma("music/recently_played_items", limit=50, media_types=["track"], fully_played_only=False):
        iid, prov, name = item.get("item_id"), item.get("provider"), item.get("name")
        entry = None
        if prov == "library":
            album = (ma("music/tracks/get", item_id=iid, provider_instance_id_or_domain="library").get("album") or {})
            if album.get("item_id"):
                entry = {"type": "album", "id": album["item_id"], "track": name}
        else:
            pl = next((p for p in playlists if any(t["id"] == iid for t in p["tracks"])), None)
            if pl:
                entry = {"type": "playlist", "id": pl["id"], "track": name}
        if entry and (entry["type"], entry["id"], name) not in seen:
            seen.add((entry["type"], entry["id"], name))
            out.append(entry)
        if len(out) >= 20:
            break
    return out


def main():
    if "--recent" in sys.argv:
        lib = json.loads((OUT / "library.json").read_text())
        lib["recent"] = recent(lib.get("playlists") or [])
        (OUT / "library.json").write_text(json.dumps(lib, ensure_ascii=False))
        print(f"{len(lib['recent'])} recently played tracks")
        return
    COVERS.mkdir(parents=True, exist_ok=True)
    PICTURES.mkdir(parents=True, exist_ok=True)
    albums, offset = [], 0
    while True:
        page = ma("music/albums/library_items", limit=200, offset=offset, order_by="sort_name")
        if not page:
            break
        albums += [album_of(i) for i in page if i.get("name") and i.get("item_id")]
        offset += len(page)
    print(f"{len(albums)} albums")
    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        for a, t in zip(albums, pool.map(lambda a: tracks_of(a["id"]), albums)):
            a["tracks"] = t
    print(f"{sum(len(a['tracks']) for a in albums)} tracks")
    radio = len(ma("music/radios/library_items", limit=1)) > 0
    playlists = []
    for pl in ma("music/playlists/library_items", limit=500):
        playlists.append({"id": pl["item_id"], "name": pl["name"], "tracks": playlist_tracks(pl)})
    print(f"{len(playlists)} playlists, {sum(len(p['tracks']) for p in playlists)} tracks")
    if "--no-covers" not in sys.argv:
        with concurrent.futures.ThreadPoolExecutor(6) as pool:
            list(pool.map(picture, [t for p in playlists for t in p["tracks"]]))
    played = recent(playlists)
    print(f"{len(played)} recently played")
    for p in playlists:
        for t in p["tracks"]:
            t["picture"] = (PICTURES / (t["id"] + ".jpg")).exists()
            del t["proxy"]
    if "--no-covers" not in sys.argv:
        with concurrent.futures.ThreadPoolExecutor(6) as pool:
            got = sum(pool.map(cover, albums))
        print(f"{got} covers")
    for a in albums:
        a["cover"] = (COVERS / (a["id"] + ".jpg")).exists()
        del a["proxy"]
    (OUT / "library.json").write_text(json.dumps({"radio": radio, "albums": albums, "playlists": playlists, "recent": played}, ensure_ascii=False))
    print(f"wrote {OUT / 'library.json'} (radio stations: {'yes' if radio else 'none'})")


if __name__ == "__main__":
    main()
