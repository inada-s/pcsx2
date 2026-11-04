// macOS twin of pcsx2ctl.ps1 (same -Seq syntax), run by zrig.ctl: osascript -l JavaScript pcsx2ctl.js ARGS
//   -pid N -data DIR -seq S [-shot out.png] [-hold 120] [-gap 350]   send to that pcsx2 (DIR = its -datapath)
//   -check -ini PCSX2.ini -seq S                                    binding check only (rc 2 = unbound)
// Keys and the mouse go through CGEvent: the calling process needs Accessibility (rc 4 without it, nothing sent).
// Qt on macOS reads the Command key as Control: `Control+K` sends Command+K.
// Mouse: the render surface is taken as the window below its title bar (ZCTL_TITLE_PT, default 28), scaled to the
// bar layout's width.
ObjC.import('CoreGraphics'); ObjC.import('AppKit'); ObjC.import('Foundation'); ObjC.import('stdlib');
ObjC.bindFunction('CGPreflightPostEventAccess', ['bool', []]);

function run(argv) {
  const o = { hold: 120, gap: 350 };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i].replace(/^-+/, '');
    if (a === 'check') o.check = true; else o[a] = argv[++i];
  }
  const out = [];
  const say = (s) => out.push(s);
  const done = (rc) => { if (out.length) console.log(out.join('\n')); $.exit(rc); };
  const sleep = (ms) => $.NSThread.sleepForTimeInterval(ms / 1000);
  const read = (p) => { const s = $.NSString.stringWithContentsOfFileEncodingError(p, $.NSUTF8StringEncoding, null); return s.isNil() ? null : s.js; };
  const exists = (p) => $.NSFileManager.defaultManager.fileExistsAtPath(p);

  const tokens = (o.seq || '').split(',').map((t) => t.trim()).filter((t) => t);
  const isKey = (t) => !/^w\d+$|^(mv|ck|bar|shot):/.test(t);

  let ini = o.ini;
  if (!o.check && o.data) ini = [o.data + '/PCSX2/inis/PCSX2.ini', o.data + '/inis/PCSX2.ini'].find(exists);
  const names = [...new Set(tokens.filter(isKey).map((t) => t.replace(/\*\d+$/, '')))];
  if (names.length) {
    if (!o.check && !o.data) say('keycheck: no -data for pid ' + o.pid + ', unchecked');
    else {
      const text = ini ? read(ini) : null;
      const bound = text ? text.split(/\r?\n/).filter((l) => /^\w+ = /.test(l)) : [];
      const alias = { Back: 'Backspace', Enter: 'Return', Next: 'PageDown', Prior: 'PageUp' };
      const un = [];
      for (const kk of names) {
        const parts = kk.split('+'); const k = parts.pop();
        const m = /^D(\d)$/.exec(k);
        const want = parts.map((p) => 'Keyboard/' + p).concat('Keyboard/' + (alias[k] || (m ? m[1] : k))).join(' & ');
        const esc = want.replace(/[.*+?^${}()|[\]\\\/]/g, '\\$&');
        if (!text) un.push(kk + ' (no PCSX2.ini: ' + (ini || o.data) + ')');
        else if (!bound.some((l) => new RegExp('(^| )' + esc + '( |$|&)').test(l))) un.push(kk + ' (' + want + ') in ' + ini);
      }
      if (un.length) { un.forEach((u) => say('FAIL key not bound: ' + u)); done(2); }
    }
  }
  if (o.check) { say('keycheck ok: ' + (o.seq || '')); done(0); }

  const pid = parseInt(o.pid, 10);
  const app = $.NSRunningApplication.runningApplicationWithProcessIdentifier(pid);
  if (app.isNil()) { say('FAIL no process ' + o.pid); done(1); }
  if ((tokens.length || o.shot) && tokens.some((t) => !/^w\d+$|^shot:/.test(t)) && !$.CGPreflightPostEventAccess()) {
    say('FAIL no Accessibility for this process (System Settings > Privacy & Security > Accessibility): nothing sent');
    done(4);
  }
  const front = () => { app.activateWithOptions($.NSApplicationActivateIgnoringOtherApps); sleep(150); };

  const win = () => {
    const list = ObjC.castRefToObject($.CGWindowListCopyWindowInfo($.kCGWindowListOptionAll | $.kCGWindowListExcludeDesktopElements, 0)).js;
    let best = null, area = -1;
    for (const w of list) {
      if (w.objectForKey('kCGWindowOwnerPID').js !== pid || w.objectForKey('kCGWindowLayer').js !== 0) continue;
      const b = w.objectForKey('kCGWindowBounds').js;
      const r = { id: w.objectForKey('kCGWindowNumber').js, x: b.X.js, y: b.Y.js, w: b.Width.js, h: b.Height.js };
      if (r.w * r.h > area) { area = r.w * r.h; best = r; }
    }
    return best;
  };

  const codes = {
    A: 0, S: 1, D: 2, F: 3, H: 4, G: 5, Z: 6, X: 7, C: 8, V: 9, B: 11, Q: 12, W: 13, E: 14, R: 15, Y: 16, T: 17,
    O: 31, U: 32, I: 34, P: 35, L: 37, J: 38, K: 40, N: 45, M: 46,
    D1: 18, D2: 19, D3: 20, D4: 21, D5: 23, D6: 22, D7: 26, D8: 28, D9: 25, D0: 29,
    Return: 36, Enter: 36, Tab: 48, Space: 49, Back: 51, Backspace: 51, Escape: 53, Delete: 117,
    Home: 115, End: 119, PageUp: 116, Prior: 116, PageDown: 121, Next: 121, Left: 123, Right: 124, Down: 125, Up: 126,
    F1: 122, F2: 120, F3: 99, F4: 118, F5: 96, F6: 97, F7: 98, F8: 100, F9: 101, F10: 109, F11: 103, F12: 111,
  };
  const mods = { Shift: [56, $.kCGEventFlagMaskShift], Control: [55, $.kCGEventFlagMaskCommand] };
  const key = (code, down, flags) => {
    const e = $.CGEventCreateKeyboardEvent(null, code, down);
    $.CGEventSetFlags(e, flags);
    $.CGEventPostToPid(pid, e);
  };

  const barLayout = () => {
    const log = o.data ? read(o.data + '/PCSX2/logs/emulog.txt') : null;
    const lines = log ? log.split('\n').filter((l) => l.includes('replay bar: layout ')) : [];
    if (!lines.length) return null;
    const f = lines[lines.length - 1].split('replay bar: layout ')[1].trim().split(/\s+/);
    const [w, h] = f[0].split('x').map(Number); const [y0, y1] = f[2].split('-').map(Number);
    const b = { w, h, y0, y1 };
    for (let i = 3; i + 1 < f.length; i += 2) b[f[i].split(':')[0]] = f[i + 1].split('-').map(Number);
    return b;
  };
  const title = parseFloat($.NSProcessInfo.processInfo.environment.objectForKey('ZCTL_TITLE_PT').js || '28');
  const surface = (w, h) => {
    const r = win();
    const sw = r.w, sh = r.h - title;
    const k = w ? sw / w : 1;
    return { x: r.x, y: r.y + title, k, w: w || sw, h: h || sh, ptw: sw, pth: sh };
  };
  const moveTo = (s, x, y) => {
    const p = { x: s.x + x * s.k, y: s.y + y * s.k };
    const e = $.CGEventCreateMouseEvent(null, $.kCGEventMouseMoved, p, $.kCGMouseButtonLeft);
    $.CGEventPost($.kCGHIDEventTap, e);
    return p;
  };
  const click = (p) => {
    $.CGEventPost($.kCGHIDEventTap, $.CGEventCreateMouseEvent(null, $.kCGEventLeftMouseDown, p, $.kCGMouseButtonLeft)); sleep(+o.hold);
    $.CGEventPost($.kCGHIDEventTap, $.CGEventCreateMouseEvent(null, $.kCGEventLeftMouseUp, p, $.kCGMouseButtonLeft)); sleep(+o.gap);
  };
  const mouse = (t) => {
    const a = t.split(':');
    let p;
    if (a[0] === 'mv' || a[0] === 'ck') { p = moveTo(surface(0, 0), +a[1], +a[2]); sleep(100); }
    else if (a[1] === 'show') {
      const s = surface(0, 0);
      for (const dx of [0, 8, 16]) { moveTo(s, s.ptw / 2 + dx, s.pth * 0.9); sleep(150); }
      say('bar:show surface ' + s.ptw + 'x' + s.pth + ' pt'); return;
    } else {
      const b = barLayout();
      if (!b || !b[a[1]]) { say('FAIL no bar layout / element ' + a[1]); done(3); }
      const fr = a.length > 2 ? +a[2] : 0.5;
      const x = b[a[1]][0] + (b[a[1]][1] - b[a[1]][0]) * fr, y = (b.y0 + b.y1) / 2;
      const s = surface(b.w, b.h);
      moveTo(s, x - 6, y); sleep(120); p = moveTo(s, x, y); sleep(150);
      say(t + ' -> x ' + Math.round(x) + ' y ' + Math.round(y) + ' of ' + b.w + 'x' + b.h);
    }
    if (a[0] !== 'mv') click(p);
  };

  const shot = (file) => {
    const r = win();
    if (!r) { say('FAIL no window for pid ' + pid); done(1); }
    const t = $.NSTask.launchedTaskWithLaunchPathArguments('/usr/sbin/screencapture', $(['-x', '-o', '-l', String(r.id), file]));
    t.waitUntilExit;
    if (t.terminationStatus !== 0 || !exists(file)) { say('no shot ' + file); done(1); }
    say('shot ' + file + ' window ' + r.w + 'x' + r.h);
  };

  if (tokens.length) front();
  for (const tok of tokens) {
    let m;
    if ((m = /^w(\d+)$/.exec(tok))) { sleep(+m[1]); continue; }
    if ((m = /^shot:(.+)$/.exec(tok))) { shot(m[1]); continue; }
    if (/^(mv|ck|bar):/.test(tok)) { front(); mouse(tok); continue; }
    let t = tok, n = 1;
    if ((m = /^(.+)\*(\d+)$/.exec(t))) { t = m[1]; n = +m[2]; }
    const parts = t.split('+'); const k = parts.pop();
    if (!(k in codes) || parts.some((p) => !(p in mods))) { say('FAIL no macOS key code for ' + tok); done(2); }
    const flags = parts.reduce((f, p) => f | mods[p][1], 0);
    for (let i = 0; i < n; i++) {
      for (const p of parts) { key(mods[p][0], true, flags); sleep(30); }
      key(codes[k], true, flags); sleep(+o.hold);
      key(codes[k], false, flags);
      for (const p of parts) key(mods[p][0], false, 0);
      sleep(+o.gap);
    }
    say('sent ' + tok);
  }
  if (o.shot) shot(o.shot);
  done(0);
}
