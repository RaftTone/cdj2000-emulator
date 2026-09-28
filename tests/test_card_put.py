# SPDX-License-Identifier: GPL-2.0-or-later
"""card_put on images make_sd_image builds: replace, create, grow, and no lost clusters."""

import struct

import pytest

from tools.cdj_main import card_put
from tools.cdj_main.make_sd_image import Builder

ANLZ = "PIONEER/USBANLZ/P04A/0001519E/ANLZ0000.EXT"


@pytest.fixture
def image(tmp_path):
    source = tmp_path / "src"
    anlz = source / "PIONEER" / "USBANLZ" / "P04A" / "0001519E"
    anlz.mkdir(parents=True)
    (anlz / "ANLZ0000.EXT").write_bytes(b"PMAI" + bytes(range(256)) * 20)
    (anlz / "ANLZ0000.DAT").write_bytes(b"DAT" * 100)
    (source / "PIONEER" / "rekordbox").mkdir()
    (source / "PIONEER" / "rekordbox" / "export.pdb").write_bytes(b"pdb" * 5000)
    path = tmp_path / "card.img"
    builder = Builder(64 << 20, path)
    root = builder.alloc(1)[0]
    builder.add_dir(source, root, 0, is_root=True)
    builder.finish()
    builder.image.flush()
    return path


def check_volume(path):
    """Every used cluster is in exactly one chain reachable from the root, both
    FATs agree, and FSInfo's free count is the real one."""
    with card_put.Fat32(path) as card:
        _check(card)


def _check(card):
    blob = card.read(card.fat_offsets[0], (card.clusters + 2) * 4)
    assert all(card.read(offset, len(blob)) == blob for offset in card.fat_offsets)
    seen = set()

    def visit(cluster, is_dir):
        chain = card.chain(cluster)
        assert not seen & set(chain)
        seen.update(chain)
        if is_dir:
            for entry in card.listing(cluster)[2]:
                if entry["short"][0:1] != b"." and entry["cluster"]:
                    visit(entry["cluster"], entry["attr"] & 0x10)

    visit(card.root, True)
    used = {c for c in range(2, card.clusters + 2) if card.next(c)}
    assert used == seen
    info = card.read(card.fsinfo, 512)
    assert struct.unpack_from("<I", info, 488)[0] == card.clusters - len(used)


def test_replace_keeps_the_rest(image, tmp_path):
    local = tmp_path / "local.EXT"
    local.write_bytes(b"PMAI" + b"\x11" * 20000)
    assert card_put.main([str(image), ANLZ, str(local)]) == 0
    with card_put.Fat32(image) as card:
        assert card.get(ANLZ) == local.read_bytes()
        assert card.get(ANLZ.replace(".EXT", ".DAT")) == b"DAT" * 100
        assert card.get("PIONEER/rekordbox/export.pdb") == b"pdb" * 5000
    check_volume(image)


def test_shrink_and_empty(image):
    with card_put.Fat32(image) as card:
        card.put(ANLZ, b"x")
        card.put("pioneer/usbanlz/p04a/0001519e/anlz0000.ext", b"")   # any case
    with card_put.Fat32(image) as card:
        assert card.get(ANLZ) == b""
    check_volume(image)


def test_create_long_name_and_new_directories(image):
    with card_put.Fat32(image) as card:
        card.put("PIONEER/USBANLZ/P04A/0001519E/Synthetic Hot Cues.EXT", b"a" * 9000)
        card.put("PIONEER/USBANLZ/P999/00000001/ANLZ0000.EXT", b"b" * 10)
    with card_put.Fat32(image) as card:
        assert card.get("PIONEER/USBANLZ/P04A/0001519E/synthetic hot cues.ext") == b"a" * 9000
        assert card.get("PIONEER/USBANLZ/P999/00000001/ANLZ0000.EXT") == b"b" * 10
        folder = card.walk(["PIONEER", "USBANLZ", "P04A", "0001519E"])
        names = [e["name"] for e in card.listing(folder)[2]]
    assert "Synthetic Hot Cues.EXT" in names and "ANLZ0000.EXT" in names
    check_volume(image)


def test_directory_grows_past_one_cluster(image):
    with card_put.Fat32(image) as card:
        for i in range(150):                   # 3 slots each: 450 > 128 per cluster
            card.put("PIONEER/many/a long file name %03d.bin" % i, bytes([i]) * 10)
    with card_put.Fat32(image) as card:
        for i in range(150):
            assert card.get("PIONEER/many/a long file name %03d.bin" % i) == bytes([i]) * 10
        assert len(card.chain(card.walk(["PIONEER", "many"]))) > 1
    check_volume(image)


def test_out_leaves_the_original(image, tmp_path):
    before = image.read_bytes()
    local = tmp_path / "local.EXT"
    local.write_bytes(b"new")
    copy = tmp_path / "copy.img"
    assert card_put.main([str(image), ANLZ, str(local), "--out", str(copy)]) == 0
    assert image.read_bytes() == before
    with card_put.Fat32(copy) as card:
        assert card.get(ANLZ) == b"new"


def test_get_and_ls(image, tmp_path, capsys):
    out = tmp_path / "got.EXT"
    assert card_put.main([str(image), ANLZ, "--get", str(out)]) == 0
    assert out.read_bytes().startswith(b"PMAI")
    assert card_put.main([str(image), "PIONEER/rekordbox", "--ls"]) == 0
    assert "export.pdb" in capsys.readouterr().out
    with card_put.Fat32(image) as card, pytest.raises(SystemExit):
        card.get("PIONEER/nothing/here.bin")
