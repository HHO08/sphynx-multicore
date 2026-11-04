# Sphynx — multi-system fork of PPSSPP v1.18.1

> Working name of the project. **This repository is a fork of
> [hrydgard/ppsspp](https://github.com/hrydgard/ppsspp) pinned at tag `v1.18.1`.**
> Upstream is licensed GPL-2.0-or-later; this fork keeps that licence and all
> upstream copyright notices. "PPSSPP" is the name of the upstream project and
> is not used as the name of this product.

## The one hard rule

`Core/`, `GPU/`, `HLE/` and `UI/` — the PSP emulator itself — are **untouched**.
Every change made by this project is additive and belongs either to
`extcores/` (new, empty for now) or is marked with a `SPHYNX:` comment.

Find every deviation from upstream with:

```bash
grep -rn "SPHYNX:" .
```

## What is done so far (stage 0 — build foundation)

| item | status |
|---|---|
| Fork pinned at `v1.18.1` (`0f50225`) | ✔ |
| Rebranding: app label "Sphynx", own `applicationId` (`org.sphynx.emulator`) so it installs next to real PPSSPP | ✔ |
| ARM-only, per-ABI release builds (`-PsphynxAbi=arm64-v8a` / `armeabi-v7a`), x86_64 dropped | ✔ |
| CI workflow producing **signed** release APKs + size gate + SHA-256 | ✔ `.github/workflows/sphynx-apk.yml` |
| Emulator cores (the 35 systems) | ⛔ not started — blocked on the decisions listed in the project docs |
| App icon | ⏳ pending (must not resemble the PPSSPP icon) |

## Building

### Locally

```bash
git clone --recursive https://github.com/HHO08/sphynx-multicore.git
cd sphynx-multicore

# one ABI at a time (ARM only)
./gradlew assembleNormalRelease -PsphynxAbi=arm64-v8a
./gradlew assembleNormalRelease -PsphynxAbi=armeabi-v7a
```

Signing: `android/build.gradle` reads the upstream `RELEASE_STORE_FILE`,
`RELEASE_STORE_PASSWORD`, `RELEASE_KEY_ALIAS`, `RELEASE_KEY_PASSWORD` Gradle
properties. Put them in `~/.gradle/gradle.properties` — never in the repo.

### On GitHub Actions

Push to `sphynx-multicore-1.18.1` or run the **Sphynx APK** workflow manually.
Required secrets: `SPHYNX_KEYSTORE_BASE64`, `SPHYNX_KEYSTORE_PASSWORD`,
`SPHYNX_KEY_ALIAS`, `SPHYNX_KEY_PASSWORD`. Output: two artifacts
(`sphynx-arm64-v8a-release`, `sphynx-armeabi-v7a-release`), each with the APK and
`SHA256SUMS.txt`.

## Applying the rebrand to a fresh upstream checkout

```bash
git clone --branch v1.18.1 --recursive https://github.com/hrydgard/ppsspp.git ppsspp-clean
python3 patches/apply_sphynx_overlay.py ppsspp-clean
```

The patch is idempotent and refuses to guess: if an anchor is missing it stops
instead of producing a half-patched build.

## Project documents

* `docs/sphynx/00-تقرير-الجدوى-والمخاطر.md` — feasibility, size budget, licence
  audit, architecture, NDS/Dreamcast root-cause analysis, roadmap.

## Licence

GPL-2.0-or-later, same as upstream. If GPLv3-only cores are adopted later
(melonDS, Gearsystem…), the distributed work must move to GPLv3 — see the
licence audit in the docs before adding any core.
