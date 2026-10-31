# Translating FroggySoundTuner

Everything the program shows comes from `lang/<locale>.json`: one flat file of `"key": "text"`, the same
shape as TreeFrogUI's own language packs. `lang/en_US.json` is the master and is complete; any other file
may leave strings out (they stay English).

1. Copy `lang/en_US.json` to `lang/<your locale>.json` (the names TreeFrogUI uses: `de_DE`, `fr_FR`,
   `pt_BR`, `ja_JP` ...) and translate the values, never the keys.
2. Keep the button names in square brackets as they are: `[A]`, `[B]`, `[X]`, `[Y]`, `[Start]`,
   `[Select]`, `[L1]`, `[R1]`. The four direction names (`[Up]`, `[Down]`, `[Left]`, `[Right]`) are meant to
   be translated.
3. `keyboard.chars` is the letters shown on the name-entry grid. Put every letter your language needs
   in it (a space is allowed).
4. `_default.font_name` picks the font from `frogui/fonts/` (`alium_Nunito.ttf` for Latin and Cyrillic,
   `TreeFrogUnicode.ttf` for Chinese and Japanese); if the font cannot draw a character the program uses
   `TreeFrogUnicode.ttf` for everything. Write the text in plain characters: `TreeFrogUnicode.ttf` has no
   full-width brackets or colons.
5. `python3 tools/check_lang.py` checks the file; copy it to `roms/ports/soundtuner/lang/` on the card and
   set TreeFrogUI's language to see it. No rebuild is needed.
6. Start `_translation_note` with who reviewed the text.
