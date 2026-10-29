#!/usr/bin/env bash
# Rebuilds src/ui/sound/assets/*.wav from the SND01 "sine" pack (see PROVENANCE.md there).
#
# usage: process_snd01_sine.sh <extracted SND01_sine directory> <output directory>
#
# The pack is never committed: only the processed result is. Every sound is trimmed of leading and
# trailing silence, resampled to 48 kHz, level-matched by its mean level (events louder than
# interaction sounds, both quiet), peak-limited, and written as 16-bit mono PCM WAV.
set -euo pipefail
src=${1:?extracted SND01_sine directory}
out=${2:?output directory}
mkdir -p "$out"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# name:source:class  (e = event, i = interaction)
sounds=(
  render_succeeded:celebration:e
  render_failed:caution:e
  import_finished:notification:e
  refused:disabled:e
  button:button:i
  select:select:i
  toggle_on:toggle_on:i
  toggle_off:toggle_off:i
  modal_open:transition_up:i
  modal_close:transition_down:i
  tool_switch:tap_01:i
  drag_start:swipe_01:i
  drop:tap_03:i
)
event_db=-20
interaction_db=-26
trim="silenceremove=start_periods=1:start_threshold=-50dB,areverse,silenceremove=start_periods=1:start_threshold=-50dB,areverse"

for entry in "${sounds[@]}"; do
  IFS=: read -r name source cls <<<"$entry"
  target=$interaction_db
  [[ $cls == e ]] && target=$event_db
  ffmpeg -y -v error -i "$src/$source.wav" -af "aresample=48000,${trim}" -c:a pcm_f32le "$tmp/$name.wav"
  mean=$(ffmpeg -nostats -v info -i "$tmp/$name.wav" -af volumedetect -f null - 2>&1 |
    sed -n 's/.*mean_volume: \(-\?[0-9.]*\) dB.*/\1/p')
  gain=$(awk -v t="$target" -v m="$mean" 'BEGIN{printf "%.2f", t-m}')
  dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$tmp/$name.wav")
  fade=$(awk -v d="$dur" 'BEGIN{s=d-0.003; if (s<0) s=0; printf "%.5f", s}')
  ffmpeg -y -v error -i "$tmp/$name.wav" \
    -af "volume=${gain}dB,alimiter=limit=0.79:attack=0.1:release=5:level=disabled,afade=t=out:st=${fade}:d=0.003" \
    -ar 48000 -ac 1 -c:a pcm_s16le -map_metadata -1 -fflags +bitexact -flags:a +bitexact "$out/$name.wav"
done
