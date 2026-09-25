"""Build an SD card image from a real rekordbox stick, for a few playlists.

    python -m tools.cdj_main.stick_card /Volumes/STICK "2026.05 AConcert" --out runs/cards/aconcert

The stick's own database goes onto the card untouched -- `export.pdb` and
`exportExt.pdb` byte for byte, every track and playlist rekordbox wrote -- so
what the firmware reads is a genuine export, cues and all.  Only the files of
the named playlists come along: their audio, their ANLZ files and artwork, and
the player settings files in PIONEER/.  Tracks of other playlists are still
listed, but their files are not on the card, so they will not load.

With --only, the card's database keeps just the named playlists and their
tracks: every other playlist, playlist entry and track is marked deleted the
way rekordbox deletes (the presence bit cleared, the bytes left in place), so
the kept rows stay byte for byte what rekordbox wrote and the player's PLAYLIST
list starts on the one you want.

The stick is only read.  Audio is linked rather than copied into OUT/stick
(make_sd_image follows the links), so the disk holds the image and not a
second copy of the music.  The image is sized to the next power of two; above
2 GiB that is an SDHC card.
"""
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

import struct

from rekordbox_pdb import Database
from rekordbox_pdb.edit import PdbEditor
from rekordbox_pdb.pdb import TableType

from tools.cdj_main.rb_card import image_size

REPO = Path(__file__).resolve().parents[2]
SETTINGS = ("DEVSETTING.DAT", "MYSETTING.DAT", "MYSETTING2.DAT", "DJMMYSETTING.DAT")
ANLZ_EXTENSIONS = (".DAT", ".EXT", ".2EX")


def delete_rows(editor: PdbEditor, table_type: int, keep) -> int:
    """Clear the presence bit of every live row of a table that KEEP rejects."""
    db, buf, size = editor.db, editor._buf, editor.page_size
    deleted = 0
    for row, loc in zip(db.rows(table_type), db.row_locations(table_type)):
        if keep(row):
            continue
        page = loc - loc % size
        heap = loc % size - 0x28
        slots = buf[page + 0x18] + 0x100 * (buf[page + 0x19] & 1)
        for slot in range(slots):
            group, bit = divmod(slot, 16)
            base = page + size - group * 0x24
            if struct.unpack_from("<H", buf, base - 6 - 2 * bit)[0] != heap:
                continue
            present = struct.unpack_from("<H", buf, base - 4)[0]
            if present & (1 << bit):
                struct.pack_into("<H", buf, base - 4, present & ~(1 << bit))
                count = struct.unpack_from("<H", buf, page + 0x19)[0]
                struct.pack_into("<H", buf, page + 0x19, count - 0x20)
                buf[page + 0x1B] |= 0x10
                deleted += 1
            break
    editor._db = None
    return deleted


def trim(pdb: Path, playlists: list[str]) -> str:
    """Keep only PLAYLISTS (by name) and their tracks in PDB, in place."""
    editor = PdbEditor.from_file(pdb)
    db = editor.db
    keep_nodes = {n.id for n in db.playlist_tree if n.name in playlists and not n.is_folder}
    keep_tracks = {e.track_id for e in db.playlist_entries if e.playlist_id in keep_nodes}
    counts = (delete_rows(editor, TableType.PLAYLIST_TREE, lambda n: n.id in keep_nodes),
              delete_rows(editor, TableType.PLAYLIST_ENTRIES, lambda e: e.playlist_id in keep_nodes),
              delete_rows(editor, TableType.TRACKS, lambda t: t.id in keep_tracks))
    # Kept playlists move to the top level, so the list opens on them.
    for node, loc in zip(editor.db.playlist_tree, editor.db.row_locations(TableType.PLAYLIST_TREE)):
        struct.pack_into("<I", editor._buf, loc, 0)
    editor.save(pdb)
    check = Database.from_file(pdb)
    return (f"trimmed: removed {counts[0]} playlists/folders, {counts[1]} entries, "
            f"{counts[2]} tracks; left {len(check.playlist_tree)} playlist(s), "
            f"{len(check.tracks)} tracks")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("stick", type=Path, help="mounted rekordbox stick, e.g. /Volumes/SNAVS")
    parser.add_argument("playlists", nargs="+", help="playlist names as the stick has them")
    parser.add_argument("--out", type=Path, required=True, help="directory for stick/ and card.img")
    parser.add_argument("--only", action="store_true",
                        help="keep only these playlists (and their tracks) in the card's database")
    args = parser.parse_args(argv)

    source_db = args.stick / "PIONEER/rekordbox/export.pdb"
    db = Database.from_file(source_db)
    tracks = {t.id: t for t in db.tracks}
    artwork = {a.id: a.path for a in db.artwork}

    tree = args.out / "stick"
    if tree.exists():
        shutil.rmtree(tree)
    (tree / "PIONEER/rekordbox").mkdir(parents=True)
    for name in ("export.pdb", "exportExt.pdb"):
        if (args.stick / "PIONEER/rekordbox" / name).exists():
            shutil.copyfile(args.stick / "PIONEER/rekordbox" / name, tree / "PIONEER/rekordbox" / name)
    for name in SETTINGS:
        if (args.stick / "PIONEER" / name).exists():
            shutil.copyfile(args.stick / "PIONEER" / name, tree / "PIONEER" / name)

    def bring(card_path: str, link: bool = False) -> bool:
        source = args.stick / card_path.lstrip("/")
        if not card_path or not source.is_file():
            return False
        destination = tree / card_path.lstrip("/")
        destination.parent.mkdir(parents=True, exist_ok=True)
        if link:
            destination.symlink_to(source.resolve())
        else:
            shutil.copyfile(source, destination)
        return True

    seen: set[int] = set()
    for wanted in args.playlists:
        nodes = [n for n in db.playlist_tree if n.name == wanted and not n.is_folder]
        if len(nodes) != 1:
            raise SystemExit(f"stick_card: {len(nodes)} playlists named {wanted!r} on the stick")
        entries = sorted((e for e in db.playlist_entries if e.playlist_id == nodes[0].id),
                         key=lambda e: e.entry_index)
        missing = 0
        for entry in entries:
            if entry.track_id in seen:
                continue
            seen.add(entry.track_id)
            track = tracks[entry.track_id]
            if not bring(track.file_path, link=True):
                missing += 1
            for extension in ANLZ_EXTENSIONS:
                bring(track.analyze_path.rsplit(".", 1)[0] + extension)
            if track.artwork_id in artwork:
                path = artwork[track.artwork_id]
                bring(path)
                bring(path.replace(".jpg", "_m.jpg"))
        print(f"playlist {wanted!r}: {len(entries)} tracks, {missing} audio files not on the stick")

    if args.only:
        print(trim(tree / "PIONEER/rekordbox/export.pdb", args.playlists))

    image = args.out / "card.img"
    size = image_size(tree)
    subprocess.run([sys.executable, "-m", "tools.cdj_main.make_sd_image", str(tree),
                    str(image), "--size", "%dM" % (size >> 20)], check=True, cwd=REPO)
    print(f"boot it:  python -m tools.cdj_main.view_vm --sd {image}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
