# Crash reports

A build with the Sentry Native SDK and a DSN sends crash reports to the Sentry
project `gdxsv-project/pcsx2-zdxsv`. Only the official release build has a DSN
([options.md](options.md), Build options). A build without one behaves like upstream PCSX2:
no setting, no upload.

## Switch

`ZdxsvCrashReports` (Settings → zdxsv, and a "Send Crash Reports" checkbox on the first page of the setup wizard) is on by default. Off: the SDK does not
start, so upstream's crash handler stays in place and nothing leaves the machine.

Turning the setting off stops uploads at once. Turning it on takes effect at the next start,
because the SDK must start before the page fault handler is installed.

## What is sent

A minidump (the stacks and the memory they point to, which can hold game session data), the
release (`pcsx2-zdxsv@<git rev>`), the environment, the operating system and a random installation
ID (the event's user ID, kept in the database folder `sentry` in the data directory). A copy of the settings file `PCSX2.ini`, attached
from the database folder: written when crash reports start and each time the settings are saved, without the sections
`[Folders]`, `[GameList]` and `[Achievements]` and without any line whose value is an absolute path
(sign-in tokens are kept in other files). The last 100 log lines (info level and above), kept in memory
whatever the log settings and written at the crash as `emulog-tail.txt` in the database folder;
not on macOS, where crashpad runs no in-process crash callback. Sentry also gets
the sender's IP address and derives a location from it. Sessions, tracing, the rest of the log and per-game
settings files are not sent.

## Behavior per platform

The crashpad backend runs as a separate process, `crashpad_handler`, shipped next to the executable.

- **Windows**: page faults stay with PCSX2's vectored handler. A crash reaches crashpad's unhandled
  exception filter, which replaces upstream's, so no local `crash-*.dmp` is written while reports are on.
- **Linux**: the page fault handler replaces crashpad's `SIGSEGV` and `SIGBUS` handlers and passes
  a real crash to upstream's handler, which aborts. Crashpad reports that abort, so these issues read as
  `SIGABRT`. Not tested: no release is built for Linux.
- **macOS**: the page fault handler takes `EXC_BAD_ACCESS` on the emulator threads' Mach exception ports
  and declines a real crash; the process dies of the signal (`SIGSEGV` or `SIGBUS`, no abort) and crashpad,
  on the task's `EXC_CRASH` port, reports it with that signal.
- **Windows ARM64**: the dependency script does not build the SDK, so there are no reports.

## Symbols

`zdxsv_release.yml` uploads the release binaries and PDBs (Windows) and the macOS app with the dSYM of each architecture (built with `-g`; `dsymutil` in the build job) with a pinned `sentry-cli` release binary,
checked against its SHA-256 (update both in the workflow together). It needs the
repository secret `SENTRY_AUTH_TOKEN` and warns when it or the variable `ZDXSV_SENTRY_DSN` is empty.
