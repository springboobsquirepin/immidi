"""Per-kit XG drum note lists for tools/gen_ins.py and tools/gen_ins_tmidi.py.

Source: the YAMAHA MU1000/MU2000 instrument definition by kuzu / openmidiproject (2008), based on the
"MU1000/MU2000 List Book" (reference/openmidiproject/MU1000_MU2000.ins; a reference, not shipped).
Its kit lists are the MU Basic standard kit plus each kit's own sounds. The names here lose the list
book's model marks (*, **, ***, ****, +++), the display abbreviations of the Natural kits are spelled
out, and typos are fixed.
"""
import os
import re

REF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "reference", "openmidiproject", "MU1000_MU2000.ins")

TYPOS = [("Cynbal", "Cymbal"), ("Tom Toom 5", "Tom Room 5"), ("Room Tom 2 Q", "Tom Room 2 Q"), ("Room Tom 3 Q", "Tom Room 3 Q"),
         ("Xizocha", "Xiaocha"), ("Ziaocha", "Xiaocha"), ("HiHat Pedal", "Hi-Hat Pedal"), ("KickDrySoft3", "Kick Dry Soft 3"),
         ("Ride Cymbal 2V", "Ride Cymbal 2 V"), ("Chinese Cym-", "Chinese Cymbal"), ("Gate Tekno", "Gate Techno"),
         # the Natural kits' display abbreviations
         ("BrshSwrl", "Brush Swirl "), ("BrushSlap", "Brush Slap"), ("SnareRoll", "Snare Roll"), ("KickLight", "Kick Light"),
         ("SideStick", "Side Stick"), ("FloorTom", "Floor Tom "), ("HiHatClose", "Hi-Hat Closed "), ("HiHatPedal", "Hi-Hat Pedal "),
         ("HiHatOpen", "Hi-Hat Open "), ("CrashCym", "Crash Cymbal "), ("RideCymCup", "Ride Cymbal Cup "), ("RideCym", "Ride Cymbal"),
         ("ChineseCym", "Chinese Cymbal "), ("SplashCym", "Splash Cymbal")]


def clean(name):
    n = re.sub(r"\s*\*+\s*$", "", name)
    n = re.sub(r"^\+*\s*", "", n)
    for a, b in TYPOS:
        n = n.replace(a, b)
    return re.sub(r"\s+", " ", n).strip()


def load(path=REF):
    """{list name: {note: name}} with BasedOn resolved (names as in the file)."""
    lists, based, cur, sec = {}, {}, None, None
    for line in open(path, encoding="latin-1").read().splitlines():
        line = line.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("."):
            sec, cur = line.lower(), None
            continue
        m = re.match(r"^\[(.*)\]$", line)
        if m:
            cur = m.group(1).strip() if sec == ".note names" else None
            if cur:
                lists[cur] = {}
            continue
        if cur is None:
            continue
        if line.lower().startswith("basedon="):
            based[cur] = line[8:].strip()
            continue
        m = re.match(r"^(\d+)=(.*)$", line)
        if m:
            lists[cur][int(m.group(1))] = m.group(2).strip()
    full = {}
    for k in lists:
        d, chain = {}, [k]
        while chain[-1] in based:
            chain.append(based[chain[-1]])
        for c in reversed(chain):
            d.update(lists.get(c, {}))
        full[k] = d
    return full


P = "YAMAHA MU1000/MU2000 "
# (bank MSB, program 0-127) -> (kit name, list of the source file). Bank 127: the XG kits (MU Basic
# based; the Standard Kit is MU Basic's own); bank 126: SFX kits.
XG_KITS = {
    (127, 0): ("Standard Kit", "#128 StandKit Mu Basic"), (127, 1): ("Standard Kit 2", "#002 StndKit2"),
    (127, 2): ("Dry Kit", "#003 Dry Kit ***"), (127, 3): ("Bright Kit", "#004 BriteKit ***"),
    (127, 4): ("Skim Kit", "#005 Skim Kit ****"), (127, 5): ("Slim Kit", "#006 Slim Kit ****"),
    (127, 6): ("Rogue Kit", "#007 RogueKit ****"), (127, 7): ("Hob Kit", "#008 Hob Kit ****"),
    (127, 8): ("Room Kit", "#009 Room Kit"), (127, 9): ("Dark Room Kit", "#010 DarkRKit ***"),
    (127, 16): ("Rock Kit", "#017 Rock Kit"), (127, 17): ("Rock Kit 2", "#018 RockKit2 ***"),
    (127, 24): ("Electro Kit", "#025 ElctrKit"), (127, 25): ("Analog Kit", "#026 AnalgKit"),
    (127, 26): ("Analog Kit 2", "#027 AnlgKit2 ***"), (127, 27): ("Dance Kit", "#028 DanceKit ***"),
    (127, 28): ("Hip Hop Kit", "#029 HipHpKit ***"), (127, 29): ("Jungle Kit", "#030 JunglKit *"),
    (127, 30): ("Apogee Kit", "#031 ApogeeKt *"), (127, 31): ("Perigee Kit", "#032 PergeeKt"),
    (127, 32): ("Jazz Kit", "#033 Jazz Kit"), (127, 33): ("Jazz Kit 2", "#034 JazzKit2"),
    (127, 40): ("Brush Kit", "#041 BrushKit"), (127, 41): ("Brush Kit 2", "#042 BrshKit2"),
    (127, 48): ("Symphony Kit", "#049 SymphKit"), (127, 56): ("Natural Kit", "#057 Ntrl Kit +++"),
    (127, 57): ("Natural Funk Kit", "#058 NtFunkKt +++"), (127, 64): ("Tramp Kit", "#065 TrampKit"),
    (127, 65): ("Amber Kit", "#066 AmberKit"), (127, 66): ("Coffin Kit", "#067 CoffinKt"),
}
# MU1000/MU2000 only
MU_KITS = {
    (127, 126): ("MU100 Native Kit", "#127 StndKit#"),
    (126, 0): ("SFX Kit 1", "#001 SFX Kit 1"), (126, 1): ("SFX Kit 2", "#002 SFX Kit 2"),
    (126, 16): ("Techno Kit K/S", "#017 Techno Kit K/S ****"), (126, 17): ("Techno Kit Hi", "#018 Techno Kit Hi ****"),
    (126, 18): ("Techno Kit Lo", "#019 Techno Kit Lo ****"), (126, 32): ("Sakura Kit", "#033 Sakura Kit ****"),
    (126, 33): ("Small Latin Kit", "#034 Small Latin Kit ****"), (126, 34): ("China Kit", "#035 +++China Kit"),
}


def kit_notes(full, key):
    """{note: clean name} of a kit."""
    name, src = XG_KITS.get(key) or MU_KITS[key]
    return {n: clean(v) for n, v in full[P + src].items()}


def own_notes(full, key, base):
    """The kit's notes that differ from `base` ({note: name}); notes the base has but the kit lacks
    would need an empty entry, which no XG kit of bank 127 has."""
    notes = kit_notes(full, key)
    missing = [n for n in base if n not in notes]
    if missing:
        raise SystemExit("%s lacks notes %s of its base list" % (XG_KITS.get(key, MU_KITS.get(key))[0], missing))
    return {n: v for n, v in notes.items() if base.get(n) != v}
