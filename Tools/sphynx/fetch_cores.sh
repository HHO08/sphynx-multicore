#!/usr/bin/env bash
# ============================================================================
#  Sphynx — Tools/sphynx/fetch_cores.sh
#
#  Fetches the prebuilt libretro cores for one Android ABI and stages them as
#  APK native libraries.
#
#  Why prebuilt: libretro's buildbot compiles and signs these core binaries for
#  Android armv7/arm64 in exactly the same way RetroArch ships them, which is
#  both faster and more reproducible than building 23 C/C++ projects from
#  source inside a 10-minute CI job.  The licence of every core is unaffected:
#  the binaries are the core authors' own builds, and Sphynx is GPL-2.0+ like
#  most of them.
#
#  Every file is verified before it is accepted:
#    * must be an ELF shared object for ARM
#    * must export the libretro entry points the host requires
#    * must not reference libraries the app does not provide
#
#  Usage:  Tools/sphynx/fetch_cores.sh [ABI] [DEST_DIR]
#          ABI defaults to armeabi-v7a (the only ABI Sphynx ships).
#
#  SPDX-License-Identifier: GPL-2.0-or-later
# ============================================================================
set -euo pipefail

ABI="${1:-armeabi-v7a}"
DEST="${2:-android/src/normal/jniLibs/${ABI}}"
BASE="https://buildbot.libretro.com/nightly/android/latest/${ABI}"
CACHE="${SPHYNX_CORE_CACHE:-build/cores-cache/${ABI}}"

# The 23 cores that cover the 35 systems of the project.
CORES=(
  fceumm              # NES + Famicom Disk System
  gambatte            # Game Boy / Game Boy Color
  mgba                # Game Boy Advance
  mupen64plus_next_gles2  # Nintendo 64 (GLES2 build: right choice for weak ARM)
  melonds             # Nintendo DS
  bsnes               # SNES (GPLv3; Snes9x is non-commercial and cannot be used)
  gearsystem          # SG-1000 / Master System / Game Gear (GPLv3)
  picodrive           # Genesis / 32X / Sega CD / Sega Pico
  mednafen_saturn     # Sega Saturn
  flycast             # Dreamcast
  opera               # 3DO
  puae                # Amiga
  fbneo               # Arcade + Neo Geo CD
  bluemsx             # MSX / MSX2 + ColecoVision
  mednafen_wswan      # WonderSwan / WonderSwan Color
  pcsx_rearmed        # PlayStation
  np2kai              # PC-88 / PC-98   (licence still under review — bundled, flagged)
  mednafen_pcfx       # PC-FX
  mednafen_pce_fast   # TurboGrafx-16 / PC Engine
  mednafen_pce        # TurboGrafx-CD / PC Engine CD
  dosbox_pure         # MS-DOS
  scummvm             # ScummVM
  mednafen_ngp        # Neo Geo Pocket / Color
)

REQUIRED_SYMS=(retro_init retro_deinit retro_api_version retro_get_system_info
               retro_get_system_av_info retro_set_environment retro_set_video_refresh
               retro_set_audio_sample retro_set_audio_sample_batch retro_set_input_poll
               retro_set_input_state retro_run retro_reset retro_load_game
               retro_unload_game retro_serialize retro_unserialize retro_serialize_size)

# Libraries provided by Android itself.  Anything else means the core would
# fail to dlopen on a device.
ALLOWED_NEEDED="libc.so libm.so libdl.so libz.so liblog.so libEGL.so libGLESv2.so libandroid.so libjnigraphics.so"

mkdir -p "$CACHE" "$DEST"
manifest="$DEST/cores-manifest.txt"
: > "$manifest"

log() { printf '%s\n' "$*"; }
fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

# Resolve a strip binary that understands ARM ELF (NDK's llvm-strip if present).
STRIP=""
for cand in "${ANDROID_NDK_HOME:-}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" \
            "${ANDROID_NDK_ROOT:-}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" \
            "$(command -v llvm-strip || true)" \
            "$(command -v aarch64-linux-gnu-strip || true)" \
            "$(command -v arm-linux-gnueabihf-strip || true)"; do
  if [ -n "$cand" ] && [ -x "$cand" ]; then STRIP="$cand"; break; fi
done
log "strip tool: ${STRIP:-<none, keeping core binaries as published>}"

total=0
for core in "${CORES[@]}"; do
  zip="$CACHE/${core}.zip"
  so="$CACHE/${core}.so"
  if [ ! -s "$zip" ]; then
    log "-> downloading ${core}"
    curl -sfL --retry 3 --retry-delay 2 -o "$zip" "$BASE/${core}_libretro_android.so.zip" \
      || fail "download failed: ${core}"
  fi
  if [ ! -s "$so" ]; then
    unzip -o -q "$zip" -d "$CACHE/unpacked-${core}" || fail "unzip failed: ${core}"
    found=$(find "$CACHE/unpacked-${core}" -name '*.so' -type f | head -1)
    [ -n "$found" ] || fail "no .so inside ${core}.zip"
    mv "$found" "$so"
  fi

  # --- verify: ARM ELF --------------------------------------------------
  arch=$(readelf -h "$so" | awk -F: '/Machine/ {gsub(/^ +| +$/, "", $2); print $2}')
  case "$arch" in
    *ARM*) : ;;
    *) fail "${core}: wrong architecture '${arch}' for ${ABI}" ;;
  esac

  # --- verify: required entry points ------------------------------------
  syms=$(nm -D --defined-only "$so" | awk '{print $NF}')
  for s in "${REQUIRED_SYMS[@]}"; do
    grep -qx "$s" <<<"$syms" || fail "${core}: missing symbol ${s}"
  done

  # --- verify: no unexpected shared library dependencies -----------------
  needed=$(readelf -d "$so" | awk -F'[][]' '/NEEDED/ {print $2}')
  for n in $needed; do
    case " $ALLOWED_NEEDED " in
      *" $n "*) : ;;
      *) fail "${core}: needs unavailable library ${n}" ;;
    esac
  done

  # --- strip if possible (published cores are usually already stripped) --
  if [ -n "$STRIP" ]; then "$STRIP" --strip-unneeded "$so" 2>/dev/null || true; fi

  out="$DEST/libsphynx_core_${core}.so"
  cp -f "$so" "$out"
  size=$(stat -c%s "$out")
  total=$((total + size))
  printf '%s\t%s\t%s\n' "$core" "$(basename "$out")" "$size" >> "$manifest"
  log "   ok ${core} -> $(basename "$out") ($((size / 1024)) KiB)"
done

log ""
log "ABI ${ABI}: $(wc -l < "$manifest") cores, $((total / 1048576)) MiB raw"
log "manifest: $manifest"
