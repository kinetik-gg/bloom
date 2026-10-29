# SND01 "sine" UI sounds -- provenance

Reviewed: 2026-10-30

UI sounds: SND01 "sine" by Yasuhiro Tsuchiya, from SND (snd.dev) by Dentsu Inc. CX Creative Center.

## Source

| Field | Value |
| --- | --- |
| Project | SND (<https://snd.dev/>) |
| Pack | SND01 "sine" |
| Author | Yasuhiro Tsuchiya |
| Publisher | Dentsu Inc. CX Creative Center |
| Archive | <https://snd.dev/assets/sounds/SND01_sine.zip> |
| Archive SHA-256 | `f662d9df0cf670b0d1a6f2d59d71494b7cc2f9631d100d60de1602ff72d937f2` |
| Archive format | 28 mono 16-bit 44.1 kHz WAV files |
| Licence | snd.dev Terms of Use (free use including commercial; credit appreciated; no redistribution of the unprocessed materials by themselves; no use as a trademark sound logo). The snd-lib project that distributes these sounds publishes its source code under the MIT licence. See `src/ui/kit/third_party/snd01-sine/LICENSE`. |
| Modified | Yes. Every file below is a processed derivative; the original files are not committed. |

The archive SHA-256 above was computed over the bytes downloaded from that URL on 2026-10-30 with
`sha256sum`.

## Terms of Use (snd.dev summary)

- Free use, commercial use included. Credit is appreciated.
- Redistributing the unprocessed materials by themselves is prohibited.
- Using the materials as a trademark sound logo is prohibited.

Bloom's use: only the sounds below, processed (not unmodified copies), embedded as application
resources and played for interface feedback. No original archive, no full copy of the pack and no
unprocessed file is stored here. The sounds are not used as a trademark or sound logo.

## Processing

`tools/ui/process_snd01_sine.sh <extracted pack directory> <output directory>` regenerates every
file from the archive. For each sound it resamples to 48 kHz, trims leading and trailing silence
(below -50 dB), scales to a mean level of -20 dB (event sounds) or -26 dB (interaction sounds),
limits peaks to about -2 dB, fades the last 3 ms, and writes 16-bit mono PCM WAV without metadata.
The result is quiet by design; the Preferences volume (default 40%) scales it further.

## Files

| File | Source sound | Class | Used for | SHA-256 |
| --- | --- | --- | --- | --- |
| `button.wav` | button | interaction | Button press | `cd28ba1fafaf0c48b01731677d5410ae1a8c9213799f18684d581db25b211f27` |
| `drag_start.wav` | swipe_01 | interaction | Drag entered the window | `84f6a465059327069bea8f304ed8552fddf8f527bfd159b2624bde8e2d525061` |
| `drop.wav` | tap_03 | interaction | Drop | `61fa411227abe549ad539c61381812d3f9775fd440c180733b38ba99dfa27d31` |
| `import_finished.wav` | notification | event | Import finished | `835fe1fc15d19e5b4be623902bfc990421d533a1759e5de591fcd15b277a2203` |
| `modal_close.wav` | transition_down | interaction | Dialog closed | `6d8df9438aabf7908a47e553796af6de34ea3a3dec4f8e96828411fb0a8e4a86` |
| `modal_open.wav` | transition_up | interaction | Dialog opened | `63a0f43b5a108da3fa97c794945ea3d08b877aa7810cdde51bed424d48a4e2e0` |
| `refused.wav` | disabled | event | Command refused; also a pressed disabled control at half level | `4bebeb78304b25da2ca7aa372d9d7ef6094b1c25cededbe10dd333649a4f7b7a` |
| `render_failed.wav` | caution | event | Render or export failed; import failed; warning dialogs | `c20f78fd47e7d13065b9875429cff7d2875e54908141a647218e76e4b8f73906` |
| `render_succeeded.wav` | celebration (Check me all) | event | Render or export finished | `edaaa68e1686da48058abb5a29767a562e7831857a6c16cb56e31d3698399d69` |
| `select.wav` | select | interaction | Dropdown or radio choice | `c7ed92158a2eb9054921f12f4be327adcfdc0785dd02b9f36e82fab7badcd030` |
| `toggle_off.wav` | toggle_off | interaction | Switch, checkbox or toggle turned off | `0ecd9a7fc25e9637f4e8c848d81780709e1bb251cb13009e6a61b9b8010bccae` |
| `toggle_on.wav` | toggle_on | interaction | Switch, checkbox or toggle turned on | `bbb766c791119d897d5107ae6d3a9958c670a22fc93c9f21b1265c9f90c10f62` |
| `tool_switch.wav` | tap_01 | interaction | Tool switched | `a12a3e31d20dd4adcb13fb5fbd54015e0c6c279d3388aba92d341474ac9bfaeb` |

Not bundled: the swipe, tap_02/04/05, type, ringtone_loop and progress_loop sounds, and the
remaining variants. The loading loop is not used (docs/ux/sound.md).
