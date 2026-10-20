"""Extract the actor-script opcode table from the IDA listing.

Usage: python -I tools/opcodes.py re/BTHORNE.EXE.asm > re/opcodes.txt

For each opcode prints the handler and a first-pass estimate of how many
operand bytes it consumes, by scanning the handler (and the subroutines it
calls) for script-pointer (bx) updates. Handlers that load bx from elsewhere
are marked as control flow.
"""
import re
import sys

TABLE = "funcs_160A1"


def load(path):
    lines = open(path, encoding="latin-1").read().splitlines()
    procs = {}
    cur = None
    for ln in lines:
        m = re.match(r"^(sub_\w+|nullsub_\d+)\s+proc", ln)
        if m:
            cur = m.group(1)
            procs[cur] = []
            continue
        if cur and re.match(r"^\w+\s+endp", ln):
            cur = None
            continue
        if cur:
            procs[cur].append(ln.split(";")[0].strip())
    return lines, procs


def table(lines):
    out = []
    start = next(i for i, l in enumerate(lines) if l.startswith(TABLE))
    for ln in lines[start:start + 400]:
        code = ln.split(";")[0].strip()
        if out and code and not re.search(r"\bdw\b", code):
            break
        out += re.findall(r"offset (\w+)", ln)
    return out


def bx_effect(name, procs, depth=0, seen=None):
    """Return (operand bytes, notes) for straight-line bx updates."""
    seen = seen or set()
    if name in seen or name not in procs or depth > 3:
        return 0, set()
    seen.add(name)
    n, notes = 0, set()
    for ins in procs[name]:
        if re.match(r"inc\s+bx$", ins):
            n += 1
        elif m := re.match(r"add\s+bx,\s*([0-9][0-9A-Fa-f]*h?)$", ins):
            v = m.group(1)
            n += int(v[:-1], 16) if v.endswith("h") else int(v)
        elif re.match(r"add\s+bx,", ins):
            notes.add("bx-add-reg")
        elif re.match(r"(mov|pop|lea|sub)\s+bx\b", ins) or "xchg" in ins and "bx" in ins:
            notes.add("bx-write")
        elif m := re.match(r"call\s+(sub_\w+)", ins):
            k, nn = bx_effect(m.group(1), procs, depth + 1, seen)
            n += k
            notes |= nn
        elif m := re.match(r"jmp\s+(sub_\w+)", ins):
            k, nn = bx_effect(m.group(1), procs, depth + 1, seen)
            n += k
            notes |= nn
        if re.match(r"(pop\s+ax|add\s+sp)", ins) and "retn" not in ins:
            pass
    return n, notes


def main(path):
    lines, procs = load(path)
    for op, h in enumerate(table(lines)):
        n, notes = bx_effect(h, procs)
        print(f"{op:3d} 0x{op:02X}  {h:12s} operands~{n}  {' '.join(sorted(notes))}")


if __name__ == "__main__":
    main(sys.argv[1])
