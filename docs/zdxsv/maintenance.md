# Maintenance: upstream merges and releases

Procedures. Release and CI facts: [tools.md](tools.md); fork rules: `AGENTS.md`.

## Merging upstream PCSX2

The fork keeps upstream history: upstream comes in as a merge commit, never a
rebase of the fork.

1. From an up-to-date `zdxsv-master`, a branch for the merge:
   ```
   git fetch https://github.com/PCSX2/pcsx2.git master
   git switch -c <branch> origin/zdxsv-master
   git merge FETCH_HEAD
   ```
2. Conflicts are expected where the fork changes upstream files:
   - seam lines: an `#include` of a hook header and its guarded one-line call
     (`*Hooks.h`, [features.md](features.md) Structure). Take upstream's file,
     put the seam lines back at the same point of the logic;
   - files changed in place (marked so in the Structure table): redo the
     change on upstream's version;
   - build and CI files (`CMakeLists.txt`, `.vcxproj`, `.sln`, deps scripts,
     workflows): take upstream's, keep the zdxsv entries and the zdxsv-only
     workflow changes ([tools.md](tools.md), CI);
   - `AGENTS.md`: keep the Fork Rules section.
3. An upstream change of a function a hook calls breaks the build in
   `pcsx2/Zdxsv/`: fix it there, not in the upstream file.
4. An upstream change to an x86_64 recompiler may need the same change in the
   ARM64 cores ([arm64.md](arm64.md)).
5. Build: Windows CMake and MSBuild, macOS arm64;
   `.github/workflows/scripts/common/check-vs-projects.py`.
6. Sync: changes to the EE, IOP, VU, SPU2, DEV9 or save states can change
   what a frame computes. Run the rollback, delta state and replay tests of
   [tests/zdxsv/README.md](../../tests/zdxsv/README.md) before the PR. Players
   of different releases battle each other: a release that does not sync with
   the last one raises the lobby's `min_pcsx2_version`.
7. One PR with only the merge and its conflict resolutions.

## Cutting a release

1. `zdxsv-master` builds green; the guide (`guide/`) describes the
   visible changes.
2. The repository variables of [tools.md](tools.md) (Releases and updates) are set.
3. Optional trial: tag `zdxsv-X.Y.Z-rc1` and push it. It becomes a
   prerelease the updater skips; install its `.zip` and play.
4. Tag the commit and push the tag:
   ```
   git tag zdxsv-X.Y.Z <commit>
   git push origin zdxsv-X.Y.Z
   ```
   `zdxsv_release.yml` builds and publishes the release.
5. Check the release: `SHA256SUMS.txt`,
   `gh attestation verify <file> -R inada-s/pcsx2`; an install of the previous
   release offers the update.

## Rolling back a release

The updater offers only a published, non-prerelease `zdxsv-X.Y.Z` that is
strictly newer than the running build: it never downgrades.

1. Stop the bad release spreading: mark it a prerelease (or delete it) on
   GitHub. Copies already updated stay on it.
2. Move those copies off it: revert the bad change on `zdxsv-master` and
   release it as the next version, `zdxsv-X.Y.(Z+1)`.
3. To keep the bad version out of the lobby once the fix is out: the server's
   `min_pcsx2_version` setting, set to `zdxsv-X.Y.(Z+1)`. The lobby refuses an
   older `version` in the platform info ([lobby.md](lobby.md)); the player
   sees `The lobby refused this PCSX2 as outdated` and updates.
