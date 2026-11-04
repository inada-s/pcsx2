# Crash reports

A build with the Sentry Native SDK and a DSN sends crash reports to the Sentry
project `gdxsv-project/pcsx2-zdxsv`. Only the official release build has a DSN
([options.md](options.md), Build options). A build without one behaves like upstream PCSX2:
no question, no setting, no upload.

## Consent

The first start asks the player once and stores the answer in `ZdxsvCrashReports`.
With no answer, or with the answer No, the SDK does not start, so upstream's crash handler stays
in place and nothing leaves the machine. Batch and no-GUI starts never ask: without a stored answer they
do not send, with a stored Yes they do.

Turning the setting off stops uploads at once. Turning it on takes effect at the next start,
because the SDK must start before the page fault handler is installed.

## What is sent

A minidump (the stacks and the memory they point to, which can hold game session data), the
release (`pcsx2-zdxsv@<git rev>`), the environment, the operating system and a random installation
ID (the event's user ID, kept in the database folder `sentry` in the data directory). Sentry also gets
the sender's IP address and derives a location from it. Sessions, tracing, logs and settings files are
not sent.

## Behavior per platform

The crashpad backend runs as a separate process, `crashpad_handler`, shipped next to the executable.

- **Windows**: page faults stay with PCSX2's vectored handler. A crash reaches crashpad's unhandled
  exception filter, which replaces upstream's, so no local `crash-*.dmp` is written while reports are on.
- **Linux, macOS**: the page fault handler replaces crashpad's `SIGSEGV` and `SIGBUS` handlers and passes
  a real crash to upstream's handler, which aborts. Crashpad reports that abort, so these issues read as
  `SIGABRT`. Not tested: no release is built for these platforms.
- **Windows ARM64**: the dependency script does not build the SDK, so there are no reports.

## Symbols

`zdxsv_release.yml` uploads the release binaries and PDBs with `sentry-cli`. It needs the
repository secret `SENTRY_AUTH_TOKEN` and warns when it or the variable `ZDXSV_SENTRY_DSN` is empty.
