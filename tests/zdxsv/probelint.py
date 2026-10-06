"""Static check of the pcsx2 zdxsv probe writers. Run by launch.ps1 before
every rig; exit 1 = do not run.

  python probelint.py [--exe pcsx2.exe] [pcsx2 tree, default this repository] [file ...]

Rules, each from a run that read a bad probe:
- tag: two trace line formats on one FILE with the same tag (first token after the frame,
  `%s` suffix stripped): readers split on it and mix them (e.g. PS hold `P` = pad `P`).
- flush: a write that ends a line (`\\n` format or fputc('\\n')) to a FILE opened with fopen must be
  followed by fflush/fclose of that FILE before the next `return` / its function's end, unless the FILE is setvbuf
  _IONBF (on Windows _IOLBF = full buffering): the rig kills pcsx2 and buffered lines are lost (hits read as none).
- key: a dump path built from the GGPO frame (ProbeFrame(), s_net_frame): rollbacks rerun GGPO
  frames, key dumps on g_FrameCount = vsync.
- stale (--exe): a checked file newer than the exe, content changed since a lint saw it older
  (`<exe>.probelint` sha1 stamp; a `git stash` mtime bump alone passes): the run would read the old probe.
Files: pcsx2/Zdxsv*.cpp, pcsx2/x86/ix86-32/iR5900.cpp, pcsx2/DEV9/Zdxsv/*.cpp (or the given files).
"""
import glob
import hashlib
import json
import os
import re
import sys

WRITE = re.compile(r"\b(?:std::)?(fprintf|fputc|fputs|fwrite)\s*\(")
OPEN = re.compile(r"\b(?:std::)?FILE\s*\*\s*(\w+)\s*[=;]|(\w+)\s*=\s*(?:std::)?fopen\s*\(")
STR = re.compile(r'"((?:[^"\\]|\\.)*)"')
CTRL = re.compile(r"^\s*(if|else|for|while|switch|do|namespace|struct|class|enum|try|catch|case)\b")


def args_of(src, start):
    """Text of the call's argument list starting at the '(' at src[start]."""
    depth = 0
    for i in range(start, len(src)):
        c = src[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return src[start + 1:i]
        elif c == '"':
            m = STR.match(src, i)
            if m:
                pass
    return src[start + 1:]


def split_args(a):
    out, depth, cur, i = [], 0, "", 0
    while i < len(a):
        c = a[i]
        if c == '"':
            m = STR.match(a, i)
            if m:
                cur += m.group(0)
                i = m.end()
                continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += c
        i += 1
    out.append(cur.strip())
    return out


def func_end(lines, i):
    """Index of the closing brace of the function containing line i (Allman braces, tab indent)."""
    ind = len(lines[i]) - len(lines[i].lstrip("\t"))
    for j in range(i, -1, -1):
        s = lines[j]
        if s.strip() == "{":
            k = len(s) - len(s.lstrip("\t"))
            prev = lines[j - 1].strip() if j else ""
            if k < ind and prev and not prev.endswith(";") and not CTRL.match(prev) and prev.endswith(")") or \
                    k < ind and re.search(r"\)\s*(const)?\s*$", prev) and not CTRL.match(prev):
                for e in range(i + 1, len(lines)):
                    if lines[e].rstrip() == "\t" * k + "}":
                        return e
                return len(lines) - 1
            ind = min(ind, k)
    return len(lines) - 1


def lint(path):
    errs = []
    text = open(path, encoding="utf-8", errors="replace").read()
    lines = text.split("\n")
    rel = path.replace("\\", "/")
    files = {m.group(1) or m.group(2) for m in OPEN.finditer(text)}
    files.discard(None)
    unbuf = {f for f in files if re.search(r"setvbuf\s*\(\s*%s\s*,[^;]*_IONBF" % re.escape(f), text)}
    # FILE* x = [] { ... setvbuf(f, .., _IONBF ..) ... }();
    unbuf |= {m.group(1) for m in re.finditer(r"FILE\s*\*\s*(\w+)\s*=\s*\[[^\]]*\]\s*\{(.*?)\}\(\);", text, re.S)
              if re.search(r"setvbuf\s*\([^;]*_IONBF", m.group(2))}
    tags = {}
    off = [0]
    for s in lines:
        off.append(off[-1] + len(s) + 1)
    for n, s in enumerate(lines):
        if s.lstrip().startswith("//") or "probelint:" in s or (n and "// probelint:" in lines[n - 1]):
            continue  # `// probelint: <why>` on or above the line: reasoned opt-out
        for m in WRITE.finditer(s):
            a = split_args(args_of(text, off[n] + m.end() - 1))
            fn = m.group(1)
            fp = a[0] if fn == "fprintf" else a[-1]
            fp = fp.strip()
            if fp not in files:
                continue
            fmt = "".join(STR.findall(a[0 if fn == "fputc" else 1])) if fn != "fwrite" else ""
            if fn == "fprintf":
                t = re.match(r"(?:%u|%d|0) ([A-Za-z][A-Za-z0-9]*)", fmt)
                if t:
                    stem = t.group(1)
                    tags.setdefault((fp, stem), []).append((n + 1, fmt))
            ends = fmt.endswith("\\n") or (fn == "fputc" and "'\\n'" in a[0]) or (fn == "fputs" and fmt.endswith("\\n"))
            if ends and fp not in unbuf:
                seg = "\n".join(lines[n:func_end(lines, n) + 1])
                ret = re.search(r"\breturn\b", seg[m.end():])
                seg = seg[:m.end() + ret.start()] if ret else seg
                if not re.search(r"\b(fflush|fclose)\s*\(\s*%s\s*\)" % re.escape(fp), seg):
                    errs.append("%s:%d: flush: line written to %s, no fflush(%s) before the function ends (rig kill loses it)"
                                % (rel, n + 1, fp, fp))
        if "fopen" in s and re.search(r"ProbeFrame\s*\(|s_net_frame", s):
            errs.append("%s:%d: key: dump path from the GGPO frame (rollback reruns it); key on g_FrameCount" % (rel, n + 1))
    for (fp, stem), sites in tags.items():
        if len({f for _, f in sites}) > 1:
            errs.append("%s:%d: tag: `%s` on %s has %d formats (lines %s): readers mix them, give each its own tag"
                        % (rel, sites[1][0], stem, fp, len(sites), ", ".join(str(l) for l, _ in sites)))
    return errs


def stale(exe, paths):
    """Files newer than the exe whose content differs from the last one seen older than it.
    mtime alone refused an unchanged file: `git stash` of an edit restores the built content with a
    new mtime (a launch was refused and the routes ran blind). <exe>.probelint keeps each file's sha1 from a
    lint where it was older than this exe build."""
    stamp, mt = exe + ".probelint", os.path.getmtime(exe)
    try:
        with open(stamp, encoding="utf-8") as f:
            old = json.load(f)
        seen = old["files"] if old.get("exe_mtime") == mt else {}
    except (OSError, ValueError, KeyError):
        seen = {}
    out, keep = [], dict(seen)
    for p in paths:
        key = os.path.abspath(p).replace("\\", "/").lower()
        with open(p, "rb") as f:
            h = hashlib.sha1(f.read()).hexdigest()
        if os.path.getmtime(p) <= mt:
            keep[key] = h
        elif seen.get(key) != h:
            out.append(p)
    if keep != seen:
        try:
            with open(stamp, "w", encoding="utf-8") as f:
                json.dump({"exe_mtime": mt, "files": keep}, f)
        except OSError:
            pass
    return out


def main(argv):
    exe = None
    if argv[:1] == ["--exe"]:
        exe, argv = argv[1], argv[2:]
    tree = argv[0] if argv else os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    paths = argv[1:] or sorted(glob.glob(os.path.join(tree, "pcsx2", "Zdxsv*.cpp")) +
                               [os.path.join(tree, "pcsx2", "x86", "ix86-32", "iR5900.cpp")] +
                               glob.glob(os.path.join(tree, "pcsx2", "DEV9", "Zdxsv", "*.cpp")))
    errs = [e for p in paths if os.path.exists(p) for e in lint(p)]
    if exe and os.path.exists(exe):
        errs += ["%s: stale: newer than %s, rebuild (the run would read the old probe)" % (p.replace("\\", "/"), exe)
                 for p in stale(exe, [p for p in paths if os.path.exists(p)])]
    for e in errs:
        print("probelint: " + e)
    if errs:
        print("probelint: %d error(s); fix the probe before a rig run (probelint.py)" % len(errs))
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
