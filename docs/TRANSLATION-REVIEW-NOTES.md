# Translation review notes

All ten translations (German, Spanish, French, Italian, Polish, Brazilian Portuguese, Russian, Turkish,
Japanese, Simplified Chinese) were written with Claude and **none has been reviewed by a native speaker**.
Each file says so in `_translation_note`. English is the master. If you read one of these languages, you
can help by reading its file (`lang/<locale>.json`) next to `lang/en_US.json`, or by running the program
with TreeFrogUI set to that language.

## How to report

Open a **Translation review** issue (there is a form), or send a pull request that edits the file and
starts `_translation_note` with who reviewed it (for example "Reviewed by NAME, native speaker, 2026-11").
No rebuild is involved; see [TRANSLATING.md](TRANSLATING.md) for the rules (keep `[A]`, `[B]`, `[X]`, `[Y]`,
`[Start]`, `[Select]`, `[L1]`, `[R1]` as they are).

## What has been checked (and by what)

| Check | Result |
|--|--|
| Every pack has every key of the English master, no extra keys | passes `tools/check_lang.py` |
| Button names kept as written | passes `tools/check_lang.py` |
| Every character can be drawn by the font the program picks for that language | checked; Polish and Turkish use `alium_Nunito.ttf`, Japanese and Chinese `TreeFrogUnicode.ttf` |
| Every screen and pop-up rendered in every language and looked at for clipping | done for the 854x480 panel; none clipped |
| Read-through of the most visible strings for mistakes and tone | no mistakes found, which is a weak test: it was done by the same model that wrote them |

## Things a reviewer is asked to look at

- **Spanish:** "Ajustes" is used for *Presets* and is also the general word for settings. "Preajustes"
  may be clearer.
- **Polish:** "Ustawienia" is used for *Presets*; TreeFrogUI already uses it for its Settings tab.
  "Presety" may avoid the clash.
- **Japanese:** On/Off is written オン/オフ; TreeFrogUI writes ON/OFF. Preset names are in katakana
  (プリセット); *Speaker Fix* is スピーカー補正.
- **German:** *Presets* is "Voreinstellungen", *Equalizer* and *Bass* are kept as loanwords.
- **French:** the colon is written with a normal space before it ("Niveau : 001" in the games), not the
  non-breaking space French typography uses.
- **Chinese and Japanese punctuation is plain ASCII** (`( ) : ,`), not full-width. This is deliberate: the
  font TreeFrogUI ships (`TreeFrogUnicode.ttf`) has no full-width brackets, colons or plus signs, and a
  character it cannot draw appears as a gap.
- **Name entry** (the letter grid for naming a preset): Japanese offers kana only (no kanji), Chinese a
  few useful characters plus Latin letters, Russian the full Cyrillic alphabet, the other languages
  their accented letters. A language with letters missing from `keyboard.chars` cannot type them.
- **Arabic** is not offered: right-to-left text is not supported (TreeFrogUI does not support it either).

## Status

| Language | Locale | Reviewed by a native speaker |
|--|--|--|
| German | de_DE | no |
| Spanish | es_ES | no |
| French | fr_FR | no |
| Italian | it_IT | no |
| Polish | pl_PL | no |
| Portuguese (Brazil) | pt_BR | no |
| Russian | ru_RU | no |
| Turkish | tr_TR | no |
| Japanese | ja_JP | no (the layout was checked on a device by someone who does not read Japanese) |
| Chinese (Simplified) | zh_CN | no |

TreeFrogUI's own Russian and Chinese packs are mostly English starter files, so these may be the first real
translations of a TreeFrogUI program into those languages; corrections there are especially welcome.
