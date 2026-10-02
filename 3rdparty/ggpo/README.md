# GGPO

Rollback netcode library, imported for zdxsv from inada-s/flycast `gdxsv-master`
(e7d5aaf6f, `core/deps/ggpo`). Upstream: https://github.com/pond3r/ggpo (MIT, `LICENSE`).

PCSX2-side additions (the flycast sources are otherwise unchanged):
- `include/ggpo_log.h`, `lib/ggpo/log/`: log sink replacing flycast's `log/Log.h`.
- `lib/ggpo/sleep.h`: `sleep_us` replacing flycast's `sleep.h`.
- `backends/synctest.cpp`: sync errors also go to the log sink (error level).
