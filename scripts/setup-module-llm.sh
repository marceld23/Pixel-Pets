#!/usr/bin/env bash
#
# Configure the M5Stack Module-LLM (Axera AX620E, Ubuntu 22.04 aarch64) for
# Pixel Pets / Muffin. One-time setup: install Whisper-Base + Silero-VAD +
# Qwen3-0.6B, raise the framework stack to lib-llm 1.8, disable TTS
# services, swap the wake-up WAV for silence, and verify the resulting
# service tree is healthy.
#
# Idempotent: re-runs are safe (dpkg installs are no-ops at the same
# version, the wake-WAV swap preserves an existing .bak instead of
# overwriting it).
#
# Prerequisites:
#   - USB-C plugged into the *Module-LLM* (not the CoreS3).
#   - `adb` on PATH, or ADB env var pointing at the binary.
#       Windows:  winget install Google.PlatformTools
#       macOS:    brew install android-platform-tools
#       Linux:    apt install adb
#   - The .deb files listed in pkgs/MANIFEST.txt present under pkgs/.
#     Download from https://repo.llm.m5stack.com/m5stack-apt-repo if
#     missing — see docs/setup-muffin.md for direct curl links.
#
# Usage:
#   scripts/setup-module-llm.sh                 # uses `adb` from PATH
#   ADB=/full/path/to/adb scripts/setup-module-llm.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MANIFEST="${REPO_ROOT}/pkgs/MANIFEST.txt"
PKGS_DIR="${REPO_ROOT}/pkgs"
ADB="${ADB:-adb}"

if [ ! -f "$MANIFEST" ]; then
  echo "error: MANIFEST.txt not found at $MANIFEST" >&2
  exit 1
fi

if ! command -v "$ADB" >/dev/null 2>&1; then
  echo "error: '$ADB' not found on PATH" >&2
  echo "       set ADB=/path/to/adb or install — see header of this script" >&2
  exit 1
fi

# Git Bash on Windows auto-converts Unix paths in command arguments to
# Windows paths, which mangles `adb push file /root/pkgs/`. Disabling
# the conversion is harmless on macOS / Linux.
export MSYS_NO_PATHCONV=1

echo "==> 1/7 Verifying ADB connection to the Module-LLM"
if ! "$ADB" devices | tail -n +2 | grep -q "device$"; then
  echo "error: no ADB device detected." >&2
  echo "       Plug USB-C into the Module-LLM (not the CoreS3) and try again." >&2
  echo "       Expected: \`$ADB devices\` shows 'axera-ax620e   device'." >&2
  exit 1
fi

echo "==> 2/7 Pushing files to /root/pkgs/ on the module"
"$ADB" shell "mkdir -p /root/pkgs" >/dev/null
mapfile -t FILES < <(grep -v '^#' "$MANIFEST" | grep -v '^[[:space:]]*$')
for f in "${FILES[@]}"; do
  src="${PKGS_DIR}/${f}"
  if [ ! -f "$src" ]; then
    echo "error: $src missing (declared in pkgs/MANIFEST.txt)" >&2
    echo "       see docs/setup-muffin.md → 'Reference: M5Stack APT repository' for download instructions" >&2
    exit 1
  fi
  echo "    push $f"
  "$ADB" push "$src" /root/pkgs/ >/dev/null
done

echo "==> 3/7 Installing framework (lib-llm + llm-sys + llm-llm)"
"$ADB" shell "
  set -e
  dpkg -i /root/pkgs/lib-llm_1.8-m5stack1_arm64.deb
  dpkg -i /root/pkgs/llm-sys_1.6-m5stack1_arm64.deb
  dpkg -i /root/pkgs/llm-llm_1.8-m5stack1_arm64.deb
  systemctl daemon-reload
  systemctl restart llm-sys llm-llm
"

echo "==> 4/7 Installing Whisper + VAD service binaries + their model files"
"$ADB" shell "
  set -e
  dpkg -i /root/pkgs/llm-vad_1.5.deb
  dpkg -i /root/pkgs/llm-whisper_1.5.deb
  dpkg -i --force-depends /root/pkgs/llm-model-silero-vad_0.4.deb
  dpkg -i --force-depends /root/pkgs/llm-model-whisper-base_0.4.deb
  systemctl daemon-reload
  systemctl start llm-vad llm-whisper
"

echo "==> 5/7 Installing Qwen3-0.6B LLM model"
"$ADB" shell "
  dpkg -i --force-depends /root/pkgs/llm-model-qwen3-0.6B-ax630c_0.4.deb
"

echo "==> 6/7 Disabling TTS services (CoreS3 has its own sounds)"
# `|| true` because re-running on a system where they're already
# disabled would otherwise return a non-zero exit code.
"$ADB" shell "
  systemctl stop llm-tts llm-melotts || true
  systemctl disable llm-tts llm-melotts || true
"

echo "==> 7/7 Silencing the default wake-up WAV"
# Preserve the original on the first run; never overwrite an existing
# .bak (otherwise re-runs would replace the .bak with our silent file).
"$ADB" shell "
  if [ ! -f /opt/m5stack/data/audio/wakeup_en_us.wav.bak ]; then
    cp /opt/m5stack/data/audio/wakeup_en_us.wav /opt/m5stack/data/audio/wakeup_en_us.wav.bak
    echo '    backed up wakeup_en_us.wav -> wakeup_en_us.wav.bak'
  else
    echo '    .bak already present, leaving it alone'
  fi
"
"$ADB" push "${PKGS_DIR}/audio/silent_wakeup.wav" /opt/m5stack/data/audio/wakeup_en_us.wav >/dev/null

echo
echo "==> Verification — service tree health"
EXPECTED=(llm-sys llm-llm llm-audio llm-kws llm-vad llm-whisper)
all_ok=true
for svc in "${EXPECTED[@]}"; do
  status=$("$ADB" shell "systemctl is-active $svc" | tr -d '\r\n')
  if [[ "$status" != "active" ]]; then
    printf '  \xe2\x9c\x97 %s: %s (expected active)\n' "$svc" "$status"
    all_ok=false
  else
    printf '  \xe2\x9c\x93 %s: active\n' "$svc"
  fi
done

echo
echo "==> Installed package versions"
"$ADB" shell "dpkg -l | grep -E 'lib-llm|llm-llm|llm-sys|llm-vad|llm-whisper|qwen3' | awk '{print \"  \" \$2, \$3}'"

echo
if $all_ok; then
  echo "Module-LLM setup complete. Plug USB-C back into the CoreS3 and call \"Muffin\"."
else
  echo "One or more services are not active. Inspect with:" >&2
  echo "  $ADB shell 'journalctl -u <service> -n 40 --no-pager'" >&2
  exit 1
fi
