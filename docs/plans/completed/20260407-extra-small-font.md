# Add EXTRA_SMALL Font Size

## Overview
Add an "X Small" font size option to the reader, giving users a fifth size choice below the current "Small". This enables higher text density for users who prefer it.

- Bookerly/NotoSans/Lexica: new 10pt size (below existing 12pt Small)
- OpenDyslexic: maps to existing 8pt (same as Small — 6pt defeats the dyslexia-readability purpose)
- Requires generating new 10pt font data from source TTF/OTF files
- Existing users' font size preference migrated so they don't silently shift down one size

**Acceptance criteria:**
1. All 5 font sizes selectable in settings UI
2. Existing users' font size preserved after OTA (migration works)
3. OpenDyslexic X Small renders same as Small (both 8pt)
4. Builds cleanly with 0 errors

## Context (from discovery)
- Font size enum: `CrossPointSettings.h:97` — currently `SMALL=0..EXTRA_LARGE=3`
- Font include aggregator: `lib/EpdFont/builtinFonts/all.h` — must add new includes here
- Font conversion: `lib/EpdFont/scripts/convert-builtin-fonts.sh` — size arrays per family
- Font ID generation: `lib/EpdFont/scripts/build-font-ids.sh` → `src/fontIds.h`
- Font globals: `src/main.cpp:36-133`, registered at lines 215-252
- Size→ID mapping: `CrossPointSettings.cpp:306-358` (`getReaderFontId()`)
- Settings UI: `src/SettingsList.h:44-46` — enum options list
- JSON migration pattern: `src/JsonSettingsIO.cpp:140-144` — absent-field detection
- Generic settings loop: `src/JsonSettingsIO.cpp:146-189` — reads and clamps all settings
- 21 translation YAML files in `lib/I18n/translations/`
- Source fonts available in `lib/EpdFont/builtinFonts/source/` (TTF/OTF for all families)
- `#ifndef OMIT_FONTS` guard: `src/main.cpp:43-133` (declarations), lines 232-251 (registration)

## Development Approach
- **testing approach**: Regular (no unit test framework — verification is `pio run` build + device test)
- complete each task fully before moving to the next
- make small, focused changes
- **CRITICAL: update this plan file when scope changes during implementation**
- run `pio run` after each code-change task to verify compilation

## Progress Tracking
- mark completed items with `[x]` immediately when done
- add newly discovered tasks with ➕ prefix
- document issues/blockers with ⚠️ prefix

## Solution Overview
Insert `EXTRA_SMALL` at position 0 in the `FONT_SIZE` enum, shifting all existing values up by 1. Generate 10pt font data for Bookerly, NotoSans, and Lexica (12 new `.h` files). Add migration in JSON settings loader to increment `fontSize` by 1 for existing settings files, detected by absence of a `fontSizeV2` marker key.

## Font Size Mapping After Change

| Font Family    | X_SMALL (new) | SMALL | MEDIUM | LARGE | X_LARGE |
|----------------|---------------|-------|--------|-------|---------|
| Bookerly       | 10pt          | 12pt  | 14pt   | 16pt  | 18pt    |
| NotoSans       | 10pt          | 12pt  | 14pt   | 16pt  | 18pt    |
| OpenDyslexic   | 8pt (=SMALL)  | 8pt   | 10pt   | 12pt  | 14pt    |
| Lexica         | 10pt          | 12pt  | 14pt   | 16pt  | 18pt    |

## Implementation Steps

### Task 1: Generate 10pt font data

**Files:**
- Modify: `lib/EpdFont/scripts/convert-builtin-fonts.sh`
- Modify: `lib/EpdFont/scripts/build-font-ids.sh`
- Modify: `lib/EpdFont/builtinFonts/all.h`
- Create: `lib/EpdFont/builtinFonts/bookerly_10_*.h` (4 files)
- Create: `lib/EpdFont/builtinFonts/notosans_10_*.h` (4 files)
- Create: `lib/EpdFont/builtinFonts/lexica_10_*.h` (4 files)
- Modify: `src/fontIds.h` (regenerated)

- [x] add `10` to `BOOKERLY_FONT_SIZES` array in `convert-builtin-fonts.sh`
- [x] add `10` to `NOTOSANS_FONT_SIZES` array in `convert-builtin-fonts.sh`
- [x] add `10` to `LEXICA_FONT_SIZES` array in `convert-builtin-fonts.sh`
- [x] add `BOOKERLY_10_FONT_ID`, `NOTOSANS_10_FONT_ID`, `LEXICA_10_FONT_ID` entries to `build-font-ids.sh`
- [x] run `convert-builtin-fonts.sh` to generate 12 new font header files
- [x] run `build-font-ids.sh > ../../src/fontIds.h` to regenerate font IDs
- [x] add 12 new `#include` directives to `lib/EpdFont/builtinFonts/all.h` for the 10pt font headers
- [x] verify all 12 new `.h` files exist in `lib/EpdFont/builtinFonts/`

### Task 2: Update settings enum and font ID mapping

**Files:**
- Modify: `src/CrossPointSettings.h`
- Modify: `src/CrossPointSettings.cpp`

- [x] update `FONT_SIZE` enum to `{ EXTRA_SMALL = 0, SMALL = 1, MEDIUM = 2, LARGE = 3, EXTRA_LARGE = 4, FONT_SIZE_COUNT }`
- [x] add `case EXTRA_SMALL` in `getReaderFontId()` for BOOKERLY → `BOOKERLY_10_FONT_ID`
- [x] add `case EXTRA_SMALL` in `getReaderFontId()` for NOTOSANS → `NOTOSANS_10_FONT_ID`
- [x] add `case EXTRA_SMALL` in `getReaderFontId()` for OPENDYSLEXIC → `OPENDYSLEXIC_8_FONT_ID` (same as SMALL)
- [x] add `case EXTRA_SMALL` in `getReaderFontId()` for LEXICA → `LEXICA_10_FONT_ID`
- [x] run `pio run` — must compile cleanly before next task

### Task 3: Register 10pt font objects

**Note:** All 10pt font globals and `insertFont()` calls must be placed inside the existing `#ifndef OMIT_FONTS` guards (declarations: lines 43-133, registration: lines 232-251).

**Files:**
- Modify: `src/main.cpp`

- [x] declare 10pt `EpdFont` globals for Bookerly (regular, bold, italic, bolditalic) inside `#ifndef OMIT_FONTS`
- [x] declare `EpdFontFamily bookerly10FontFamily` inside `#ifndef OMIT_FONTS`
- [x] declare 10pt `EpdFont` globals for NotoSans (regular, bold, italic, bolditalic) inside `#ifndef OMIT_FONTS`
- [x] declare `EpdFontFamily notosans10FontFamily` inside `#ifndef OMIT_FONTS`
- [x] declare 10pt `EpdFont` globals for Lexica (regular, bold, italic, bolditalic) inside `#ifndef OMIT_FONTS`
- [x] declare `EpdFontFamily lexica10FontFamily` inside `#ifndef OMIT_FONTS`
- [x] register all three 10pt font families in `setupDisplayAndFonts()` with their font IDs, inside `#ifndef OMIT_FONTS`
- [x] run `pio run` — must compile cleanly before next task
- ➕ expanded app partitions in `partitions.csv` from 0x640000 to 0x6A0000 each (SPIFFS shrunk from 0x360000 to 0x2A0000) to accommodate ~534KB of new font data

### Task 4: Add i18n string and update settings UI

**Files:**
- Modify: `lib/I18n/translations/english.yaml`
- Modify: `src/SettingsList.h`

- [x] add `STR_X_SMALL: "X Small"` to `english.yaml` next to `STR_SMALL` (non-English languages fall back to English)
- [x] prepend `StrId::STR_X_SMALL` to font size options in `SettingsList.h:44-46`
- [x] run i18n generator: `python scripts/gen_i18n.py lib/I18n/translations lib/I18n/`
- [x] run `pio run` — must compile cleanly before next task

### Task 5: Add settings migration

**Note:** Migration must be placed **after** the generic settings loop (after line 189 in `JsonSettingsIO.cpp`), because `s.fontSize` is loaded from JSON inside that loop. Placing it before the loop would increment the default value instead of the loaded value.

**Files:**
- Modify: `src/JsonSettingsIO.cpp`

- [x] add migration block after the generic settings loop (after line 189): if `doc["fontSizeV2"]` is null, increment `s.fontSize` by 1 (capped at `EXTRA_LARGE`), set `*needsResave = true`
- [x] add `fontSizeV2` to settings save (write `doc["fontSizeV2"] = 1` in `saveSettings()`)
- [x] run `pio run` — must compile cleanly before next task

### Task 6: Build verification and acceptance

- [x] run `pio run -t clean && pio run` — must compile with 0 errors
- [x] verify no new warnings related to font changes
- [x] check binary size increase is reasonable (~534KB for 12 new font header files across 3 families; all in .rodata/Flash, DRAM increase only 88 bytes)
- [x] verify all acceptance criteria from Overview are addressed in implementation
- [x] move this plan to `docs/plans/completed/`

## Post-Completion

**Manual verification:**
- test on device: cycle through all 5 font sizes in settings
- verify X Small renders legibly on 800x480 e-ink display
- verify existing users' font size preference is preserved after OTA update (migration)
- verify OpenDyslexic X Small shows same size as Small (both 8pt)
- test in all 4 orientations
- check cache invalidation works (changing to X Small triggers section re-render)

**i18n follow-up:**
- non-English translations for `STR_X_SMALL` can be contributed later (English fallback works meanwhile)
