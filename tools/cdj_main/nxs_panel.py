"""NXS physical contacts, decoded by MAIN 042f5810 (not the legacy panel).

Names are from service tables 040b0eb4/040b0fac/040b0fdc and the
name-pointer table at unpacked image offset 003c32f0. See NXS_PANEL_MAP.md.
"""
from tools.cdj_main import panel_control as legacy

KEY_NAMES = {pair: name for pair, name in legacy.FIRMWARE_KEY_NAMES.items()
             if pair[0] in (15, 16, 17, 20)}
KEY_NAMES.update({
    (17, 1): "LOOP MODE", (17, 3): "SLIP",
    (18, 0): "QUANTIZE", (18, 1): "REC MODE",
    (18, 2): "PREVIOUS |<<", (18, 3): "NEXT >>|",
    (18, 4): "REV <<", (18, 5): "FWD >>",
    (18, 6): "< CALL", (18, 7): "CALL >",
    (19, 0): "REKORDBOX", (19, 1): "LINK", (19, 2): "USB",
    (19, 3): "SD", (19, 4): "DISC", (19, 5): "TIME/ACUE",
    (19, 6): "DELETE", (19, 7): "MEMORY", (20, 6): "EJECT",
    (21, 0): "JOG MODE", (21, 1): "SYNC", (21, 2): "MASTER",
    (21, 3): "TEMPO RANGE", (21, 4): "MASTER TEMPO",
    (21, 5): "TEMPO RESET",
})
BUTTON_BITS = sorted(set(KEY_NAMES) | {(15, 6), (15, 7), (17, 3)})

# Derive names from the NXS service table, never the legacy source-key map.
BUTTON_NAMES = {name.lower(): (byte, 1 << bit)
                for (byte, bit), name in KEY_NAMES.items()}

# REV is a physical active-low switch: zero requests reverse playback.
# The stock NXS firmware then correctly stops a freshly loaded track at its
# start boundary. Keep the neutral level high before applying live controls.
ACTIVE_LOW_CONTACTS = {(15, 0x02)}


def contact_level(byte: int, mask: int, active: bool) -> bool:
    """Map a named control's active state to its literal electrical level."""
    return not active if (byte, mask) in ACTIVE_LOW_CONTACTS else active


def neutral_frame() -> bytes:
    frame = bytearray(legacy.PANEL_PAYLOAD_LEN)
    for byte, mask in ACTIVE_LOW_CONTACTS:
        frame[byte] |= mask
    return bytes(frame)


def button_mask(name):
    """Resolve a friendly NXS name or an explicit BYTE.BIT / BYTE:MASK."""
    key = name.strip().lower().replace('_', ' ').replace('-', ' ')
    key = {'enter': 'encoder push', 'back': 'return', 'info': 'information'}.get(key, key)
    if key in BUTTON_NAMES:
        return BUTTON_NAMES[key]
    if ':' in key or '.' in key:
        return legacy.button_mask(key)
    raise ValueError(f'unknown NXS button {name!r}; use {sorted(BUTTON_NAMES)}')


def input_ids():
    return [f"{byte}.{bit}" for byte, bit in BUTTON_BITS] + [
        f"field{i}" for i in range(len(legacy.ANALOG_FIELDS))]


def deck_input(legacy_id):
    """Resolve a legacy faceplate position by its function, not its old bit."""
    base, separator, suffix = legacy_id.partition("-")
    if "." not in base:
        return legacy_id
    pair = tuple(map(int, base.split(".")))
    name = legacy.FIRMWARE_KEY_NAMES.get(pair)
    if name is None:
        return legacy_id
    target = next((pair for pair, candidate in KEY_NAMES.items() if candidate == name), None)
    if target is None:
        return None  # no verified NXS contact for this legacy-named position
    return f"{target[0]}.{target[1]}" + (separator + suffix if separator else "")
