#!/usr/bin/env python3
"""Generates src/core/GsEfxData.inc from the text of the SC-88Pro owner's manual appendix
("Effect list" pages 176-182 and "Effect data table" pages 183-184).

usage: gen_gs_efx.py SC-88PRO_OM.txt out.inc
"""
import re, sys

src = open(sys.argv[1], encoding="utf-8").read()
pages = re.split(r"=====PAGE (\d+)=====", src)
page = {int(pages[i]): pages[i + 1] for i in range(1, len(pages), 2)}
efx_text = "\n".join(page[p] for p in range(178, 185))
tbl_text = "\n".join(page[p] for p in range(185, 187))

# ---------------------------------------------------------------- data table
table = [[None] * 14 for _ in range(128)]
prev = [None] * 14
for line in tbl_text.split("\n"):
    m = re.match(r"^([0-9A-F]{2}) (.*)$", line.strip())
    if not m: continue
    v = int(m.group(1), 16)
    toks = m.group(2).split()
    # join "L180(=R180)" etc. (single tokens already); ditto marks are '"'
    if len(toks) != 14: raise SystemExit("bad table row %s: %s" % (m.group(1), toks))
    for c, t in enumerate(toks):
        if t == '"': t = prev[c]
        table[v][c] = t
        prev[c] = t
UNITS = ["ms", "ms", "ms", "ms", "ms", "Hz", "Hz", "Hz", "Hz", "Hz", "Hz", "Hz", "", ""]

def fmt_table(col, s):
    if s == "--": return "--"
    if s in ("Bypass",) or s.startswith("L") or s.startswith("R") or s == "0" and col == 12:
        return s.replace("(=R180)", "").replace("(=L180)", "")
    u = UNITS[col]
    if u == "Hz":
        f = float(s)
        return ("%gk" % (f / 1000.0) if f >= 1000 else "%g" % f) + "Hz"
    return s + u

# ---------------------------------------------------------------- effect types
types = []
cur = None
lines = efx_text.split("\n")
i = 0
while i < len(lines):
    line = lines[i].strip()
    m = re.match(r"^(\d{1,2}) : (.+?) ([0-9A-F]{2}) ([0-9A-F]{2})$", line)
    if m:
        cur = {"num": int(m.group(1)), "name": m.group(2).strip().replace("→", "->"),
               "msb": int(m.group(3), 16), "lsb": int(m.group(4), 16), "params": []}
        types.append(cur)
        i += 1; continue
    m = re.match(r"^([+#] )?(.+?) (\S.*?)\s+([0-9A-F]{2})$", line)
    if cur and m and not line.startswith(("Parameter Value", "F0 ", "For Effect", "(dev")) and "**" not in line:
        ctrl = {"+ ": 1, "# ": 2}.get(m.group(1) or "", 0)
        body = m.group(2) + " " + m.group(3)
        addr = int(m.group(4), 16)
        cur["params"].append((ctrl, body, addr))
    i += 1

# parameter body parsing: "<name> <value description> <hex spec>"
def parse_param(ctrl, body, addr):
    body = body.replace("990m/1sec*", "990m/1sec *").replace("  ", " ")
    # hex spec at the end
    m = re.search(r"\*(\d+)$", body)
    if m:
        spec = ("table", int(m.group(1)) - 1)
        rest = body[:m.start()].strip()
    else:
        m = re.search(r"((?:[0-9A-F]{2,3}/)+[0-9A-F]{2,3})$", body)
        if m:
            spec = ("enum", [int(x, 16) for x in m.group(1).split("/")])
            rest = body[:m.start()].strip()
        else:
            m = re.search(r"([0-9A-F]{2}) ?- ?([0-9A-F]{2})$", body)
            if not m: raise SystemExit("unparsed param: " + body)
            spec = ("range", int(m.group(1), 16), int(m.group(2), 16))
            rest = body[:m.start()].strip()
    # split name from value description: value starts at first token that looks like a value
    toks = rest.split(" ")
    k = 1
    valre = re.compile(r"^(L63|D>|A>|180/L168|[-+]?\d[\d.]*[mk%]?(sec)?(\s*-\s|/)|[A-Za-z0-9.+-]+/)")
    while k < len(toks) and not valre.match(" ".join(toks[k:])): k += 1
    name = " ".join(toks[:k]).strip()
    desc = " ".join(toks[k:]).strip()
    p = {"ctrl": ctrl, "name": name, "desc": desc, "index": addr - 2}
    if spec[0] == "table":
        p["kind"] = "TABLE"; p["table"] = spec[1]
        # default: middle value when "a - def - b"
        parts = [x.strip() for x in re.split(r" - ", desc)]
        p["default_text"] = parts[1] if len(parts) == 3 else None
    elif spec[0] == "enum":
        labels = re.sub(r"\s*/\s*", "/", desc)
        labels = labels.replace("1/1.5,1/2,1/4,1/100", "1/1.5,1/2,1/4,1/100")
        if labels.startswith("1/1.5"): lab = ["1:1.5", "1:2", "1:4", "1:100"]
        elif labels.startswith("0/+6"): lab = ["0dB", "+6dB", "+12dB", "+18dB"]
        else: lab = labels.split("/")
        if len(lab) != len(spec[1]):
            lab = (lab + ["?"] * len(spec[1]))[:len(spec[1])]
        p["kind"] = "ENUM"; p["values"] = spec[1]; p["labels"] = lab
    else:
        lo, hi = spec[1], spec[2]
        p["lo"] = lo; p["hi"] = hi
        if "/" in desc and not re.match(r"^[-+]?\d", desc):
            p["kind"] = "ENUM"; p["values"] = list(range(lo, hi + 1))
            p["labels"] = (re.sub(r"\s*/\s*", "/", desc).split("/") + ["?"] * 8)[:hi - lo + 1]
        elif desc.startswith("L63"): p["kind"] = "PAN"
        elif desc.startswith("D>"): p["kind"] = "BALANCE_DE"
        elif desc.startswith("A>"): p["kind"] = "BALANCE_AB"
        else:
            nums = re.findall(r"[-+]?\d+(?:\.\d+)?", desc)
            if not nums:
                print("WARN no numbers:", repr(name), repr(desc), hex(lo), hex(hi)); nums = [str(lo), str(hi)]
            p["kind"] = "LINEAR"
            p["dlo"] = float(nums[0]); p["dhi"] = float(nums[-1])
            p["unit"] = "%" if "%" in desc else ("ms" if "m" in desc and "Gate" in name else ("dB" if lo == 0x34 and hi == 0x4C else ""))
            if lo == 0x34 and hi == 0x4C: p["unit"] = "dB"
            if lo == 0x00 and hi == 0x5A: p["unit"] = "deg"
            if lo == 0x0E and hi == 0x72: p["unit"] = "cent"
            if lo == 0x28 and hi == 0x4C: p["unit"] = "semi"
            if len(nums) == 3: p["ddef"] = float(nums[1])
    return p

out = []
out.append("// Generated by tools/gen_gs_efx.py from the Roland SC-88Pro owner's manual appendix.")
out.append("// Do not edit by hand.\n")
out.append("static const char* const kGsEfxDataTable[128][14] = {")
for v in range(128):
    out.append("    {" + ", ".join('"%s"' % fmt_table(c, table[v][c]) for c in range(14)) + "},")
out.append("};\n")

def cstr(s): return '"' + s.replace('"', '\\"') + '"'
def default_hex(p):
    k = p["kind"]
    if k == "TABLE":
        d = p.get("default_text")
        if not d: return -1
        d2 = d.replace("m", "").replace("k", "000") if not d.startswith(("L", "R")) else d
        col = p["table"]
        for v in range(128):
            t = table[v][col]
            try:
                if abs(float(t) - float(d2)) < 1e-6: return v
            except ValueError:
                if t == d2: return v
        return -1
    if k == "LINEAR" and "ddef" in p:
        f = (p["ddef"] - p["dlo"]) / (p["dhi"] - p["dlo"]) if p["dhi"] != p["dlo"] else 0
        return int(round(p["lo"] + f * (p["hi"] - p["lo"])))
    if k in ("PAN", "BALANCE_DE", "BALANCE_AB"):
        return 64
    return -1

out.append("static const GsEfxParamDef kGsEfxParams[] = {")
index_of = []
n = 0
for t in types:
    first = n
    for ctrl, body, addr in t["params"]:
        p = parse_param(ctrl, body, addr)
        k = p["kind"]
        labels = "|".join(p.get("labels", []))
        vals = p.get("values", [])
        valmask = ",".join(str(v) for v in vals)
        out.append("    {%d, %s, %d, GsEfxKind::%s, %d, %d, %g, %g, %d, %s, %s, %s, %d}," % (
            p["index"], cstr(p["name"]), p["ctrl"], k, p.get("lo", 0), p.get("hi", 127),
            p.get("dlo", 0), p.get("dhi", 0), p.get("table", -1), cstr(p.get("unit", "")), cstr(labels),
            cstr(valmask), default_hex(p)))
        n += 1
    index_of.append((t, first, n - first))
out.append("};\n")
out.append("static const GsEfxTypeDef kGsEfxTypes[] = {")
for t, first, cnt in index_of:
    out.append("    {0x%02X, 0x%02X, %s, %d, %d}," % (t["msb"], t["lsb"], cstr(t["name"]), first, cnt))
out.append("};\n")
open(sys.argv[2], "w", encoding="utf-8").write("\n".join(out) + "\n")
print("types", len(types), "params", n)
nums = [t["num"] for t in types]
print("missing", sorted(set(range(1, 65)) - set(nums)))
