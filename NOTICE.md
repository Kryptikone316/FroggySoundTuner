# Notice

**FroggySoundTuner** (`src/soundtuner.c`, `src/sndapi.h`, the scripts, the translation files and the
documentation) is licensed GPL 3 or later; see `LICENSE`.

## Parts taken from elsewhere

| Files | Where from | Terms here |
|--|--|--|
| `src/hwdisp.*` | The display glue lifted from TreeFrogUI's `pico286-sf3000.patch` (the vendor display driver called through `dlopen`) | GPL-compatible terms, see below |
| `src/joykey.*` | The gamepad reader lifted from TreeFrogUI's `apps/image_viewer` (shared memory written by the system's volume process) | GPL-compatible terms, see below |
| `src/hwaudio.*` | The audio calling convention confirmed from picoarch's own source (`sf3000_audio_thread_fn`) | GPL-compatible terms, see below |
| `src/tfenv.h` | Reads `/tmp/tfdevice.env`, which TreeFrogUI writes | GPL-compatible terms, see below |

TreeFrogUI's front end is licensed CC BY-NC-SA 4.0, picoarch BSD 3-Clause (libpicofe: GPL v2 or later,
LGPL v2.1 or later or the MAME licence). The `pico286` patch and the `image_viewer` app carry no
licence text of their own in that repository. The author of TreeFrogUI agreed, on its Discord, that this
display, audio and gamepad glue could be distributed under GPL-compatible terms in the ports made for
TreeFrogUI that use it; FroggySoundTuner is one of them. If you hold rights in these files and would like this
changed, please open an issue.

## Not included

- **The sound chip's SDK headers.** The two controls this program uses (`SET_EQ_BAND` and `SET_EQ_ONOFF`
  on `/dev/sndC0i2so`) are defined in `src/sndapi.h` from what the driver does; no vendor header is copied.
- **Fonts.** The program loads TreeFrogUI's fonts from the card (`frogui/fonts/`): `alium_Nunito.ttf`
  (Nunito, SIL OFL 1.1) and `TreeFrogUnicode.ttf` (WenQuanYi Zen Hei, GPL v2 with the font embedding
  exception). None is distributed here.
- **FreeType.** The device's own `libfreetype.so.6` is used; it is not distributed here.

## Translations

The ten translation files are drafts written with Claude (Anthropic) and not yet reviewed by native
speakers, released under the same licence as the program.
