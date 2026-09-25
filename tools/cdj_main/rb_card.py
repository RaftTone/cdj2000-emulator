"""Build an SD card image from rekordbox playlists, without rekordbox's export.

    python -m tools.cdj_main.rb_card "CDJTEST" ["Other playlist" ...] --out runs/cards/cdjtest

rekordbox only exports to a device it recognises as a USB stick or SD card,
and a mounted disk image is neither.  So this does the export's work itself,
reading the desktop library (`master.db`, through pyrekordbox) and writing
what a stick carries:

* `PIONEER/rekordbox/export.pdb` -- written with rekordbox-pdb's editor, which
  edits a real export the way rekordbox does rather than inventing a file.  The
  base is the one-song export in that project's test data; its track, artists,
  album, key, artwork and playlist are deleted (presence bit cleared, exactly
  how rekordbox deletes) and the playlists' tracks are appended.
* `PIONEER/USBANLZ/P001/0000000N/ANLZ0000.{DAT,EXT,2EX}` -- copied from the
  desktop analysis, with the `PPTH` tag rewritten from rekordbox's local
  placeholder (`?/name.mp3`) to the file's path on the card, as the export does.
* `Contents/<artist>/<album>/<file>` -- the audio itself.

Cue points are not carried yet: the desktop ANLZ files hold empty cue lists
(rekordbox keeps cues in master.db and writes them into the files on export).

The tree lands in OUT/stick and the image in OUT/card.img, sized to the next
power of two QEMU's sd-card accepts.  Boot it with
`python -m tools.cdj_main.view_vm --sd OUT/card.img`.
"""
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import sys
import unicodedata
from pathlib import Path

import rekordbox_pdb
from rekordbox_pdb.edit import PdbEditor
from rekordbox_pdb.pdb import TableType

REPO = Path(__file__).resolve().parents[2]
RB_SHARE = Path.home() / "Library/Pioneer/rekordbox/share"
BASE_PDB = Path(rekordbox_pdb.__file__).resolve().parents[2] / "tests/data/one-song-export.pdb"
# Tables whose rows describe the base export's own song; colours, columns and
# the unknown tables are the player's furniture and stay as rekordbox wrote them.
CLEARED = (TableType.TRACKS, TableType.ARTISTS, TableType.ALBUMS, TableType.GENRES,
           TableType.LABELS, TableType.KEYS, TableType.ARTWORK,
           TableType.PLAYLIST_TREE, TableType.PLAYLIST_ENTRIES)
ANLZ_EXTENSIONS = (".DAT", ".EXT", ".2EX")
UNSAFE = '/\\:*?"<>|'


def nfc(text: str | None) -> str:
    return unicodedata.normalize("NFC", text or "")


def safe(name: str, fallback: str) -> str:
    name = "".join("_" if c in UNSAFE else c for c in nfc(name)).strip(" .")
    return name or fallback


def delete_all_rows(editor: PdbEditor, table_type: int) -> int:
    """Clear the presence bit of every live row of a table; return the count."""
    buf, size = editor._buf, editor.page_size
    table = next(t for t in editor.db.tables if t.type == table_type)
    deleted = 0
    page = table.first_page
    while True:
        off = page * size
        if not buf[off + 0x1B] & 0x40:
            slots = buf[off + 0x18] + 0x100 * (buf[off + 0x19] & 1)
            for group in range((slots + 15) // 16):
                at = off + size - group * 0x24 - 4
                present = struct.unpack_from("<H", buf, at)[0]
                deleted += bin(present).count("1")
                struct.pack_into("<H", buf, at, 0)
            if slots:
                struct.pack_into("<H", buf, off + 0x19, 1 if slots > 255 else 0)
                buf[off + 0x1B] |= 0x10
        if page == table.last_page:
            break
        page = struct.unpack_from("<I", buf, off + 0x0C)[0]
    editor._db = None
    return deleted


def rewrite_ppth(data: bytes, card_path: str) -> bytes:
    """An ANLZ file with its PPTH tag pointing at CARD_PATH."""
    if data[:4] != b"PMAI":
        raise ValueError("not an ANLZ file")
    header_len = struct.unpack_from(">I", data, 4)[0]
    out = bytearray(data[:header_len])
    at = header_len
    while at < len(data):
        tag_len = struct.unpack_from(">I", data, at + 8)[0]
        if data[at:at + 4] == b"PPTH":
            path = (card_path + "\0").encode("utf-16-be")
            out += b"PPTH" + struct.pack(">III", 16, 16 + len(path), len(path)) + path
        else:
            out += data[at:at + tag_len]
        at += tag_len
    struct.pack_into(">I", out, 8, len(out))
    return bytes(out)


def playlist_contents(db, names: list[str]) -> list[tuple[str, list]]:
    playlists = []
    for name in names:
        found = [p for p in db.get_playlist() if p.Name == name and not p.is_folder]
        if len(found) != 1:
            raise SystemExit(f"rb_card: {len(found)} playlists named {name!r} in the library")
        songs = db.get_playlist_songs(PlaylistID=found[0].ID).order_by("TrackNo")
        playlists.append((name, [db.get_content(ID=s.ContentID) for s in songs]))
    return playlists


def image_size(tree: Path) -> int:
    used = sum(f.stat().st_size for f in tree.rglob("*") if f.is_file())
    size = 64 << 20
    while size < used * 1.1 + (16 << 20):
        size *= 2
    return size


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("playlists", nargs="+", help="playlist names as rekordbox shows them")
    parser.add_argument("--out", type=Path, required=True, help="directory for stick/ and card.img")
    parser.add_argument("--base", type=Path, default=BASE_PDB,
                        help="a real rekordbox export.pdb to edit (default: rekordbox-pdb's one-song export)")
    args = parser.parse_args(argv)

    from pyrekordbox import Rekordbox6Database  # slow import, and only needed here

    stick = args.out / "stick"
    if stick.exists():
        shutil.rmtree(stick)
    (stick / "PIONEER/rekordbox").mkdir(parents=True)

    editor = PdbEditor.from_file(args.base)
    for table in CLEARED:
        delete_all_rows(editor, table)

    db = Rekordbox6Database()
    track_ids: dict[str, int] = {}
    playlists = playlist_contents(db, args.playlists)
    for name, contents in playlists:
        for content in contents:
            if content.ID in track_ids:
                continue
            source = Path(content.FolderPath)
            artist, album = nfc(content.ArtistName), nfc(content.AlbumName)
            filename = safe(source.name, "track" + source.suffix)
            card_path = "/Contents/%s/%s/%s" % (safe(artist, "UnknownArtist"),
                                                safe(album, "UnknownAlbum"), filename)
            number = len(track_ids) + 1
            anlz_dir = "/PIONEER/USBANLZ/P%03X/%08X" % (1 + number // 256, number)

            destination = stick / card_path.lstrip("/")
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            anlz = RB_SHARE / (content.AnalysisDataPath or "").lstrip("/")
            (stick / anlz_dir.lstrip("/")).mkdir(parents=True)
            for extension in ANLZ_EXTENSIONS:
                original = anlz.with_suffix(extension)
                if original.exists():
                    (stick / anlz_dir.lstrip("/") / original.name).write_bytes(
                        rewrite_ppth(original.read_bytes(), card_path))

            track_ids[content.ID] = editor.add_track(
                title=nfc(content.Title),
                file_path=card_path,
                filename=filename,
                artist=artist or None,
                album=album or None,
                genre=nfc(content.GenreName) or None,
                key=nfc(content.KeyName) or None,
                label=nfc(content.LabelName) or None,
                comment=nfc(content.Commnt),
                analyze_path=anlz_dir + "/ANLZ0000.DAT",
                tempo=int(content.BPM or 0),
                duration=int(content.Length or 0),
                bitrate=int(content.BitRate or 0),
                sample_rate=int(content.SampleRate or 44100),
                sample_depth=int(content.BitDepth or 16),
                file_size=destination.stat().st_size,
                track_number=int(content.TrackNo or 0),
                rating=int(content.Rating or 0),
                color_id=int(content.ColorID or 0) if str(content.ColorID or 0).isdigit() else 0,
            )
            print(f"  {number:2d}  {content.Title}  ->  {card_path}")
        playlist_id = editor.create_playlist(nfc(name))
        for content in contents:
            editor.add_to_playlist(playlist_id, track_ids[content.ID])
        print(f"playlist {name!r}: {len(contents)} tracks")

    pdb_path = stick / "PIONEER/rekordbox/export.pdb"
    editor.save(pdb_path)
    check = rekordbox_pdb.Database.from_file(pdb_path)
    print(f"{pdb_path}: {len(check.tracks)} tracks, "
          f"{sum(not n.is_folder for n in check.playlist_tree)} playlists read back")

    image = args.out / "card.img"
    size = image_size(stick)
    subprocess.run([sys.executable, "-m", "tools.cdj_main.make_sd_image", str(stick),
                    str(image), "--size", "%dM" % (size >> 20)], check=True, cwd=REPO)
    print(f"boot it:  python -m tools.cdj_main.view_vm --sd {image}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
