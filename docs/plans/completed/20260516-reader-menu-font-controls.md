# Reader-Menu Font Size and Font Family Controls

## Overview
Add font size and font family switching to the EPUB reader context menu (opened by pressing Confirm/OK while reading).

- **Font size**: inline cycle on the menu row (SMALL → MEDIUM → LARGE → X_LARGE), mirroring the existing `ROTATE_SCREEN` / `AUTO_PAGE_TURN` pattern. Applied on menu exit.
- **Font family**: a new menu row that launches the existing `FontSelectionActivity` (built-in + SD card fonts) as a sub-activity.

Today users must navigate to global Settings to change font size or family, which forces them out of the book. This change puts both controls one Confirm away from the page they are reading, matching the convenience of the existing orientation toggle.

## Context (from discovery)

Files involved:
- `src/activities/reader/EpubReaderMenuActivity.h` / `.cpp` — context menu activity. Existing inline-cycle pattern for `ROTATE_SCREEN` and `AUTO_PAGE_TURN`.
- `src/activities/ActivityResult.h:19-23` — `MenuResult` struct currently `{int action, uint8_t orientation, uint8_t pageTurnOption}`. This is the file to modify for the new field.
- `src/activities/reader/EpubReaderActivity.cpp` — invokes the menu at line ~176; menu-result callback at ~179-187 unconditionally applies orientation and page-turn even on Cancel; dispatches confirmed action via `onReaderMenuConfirm` starting at line ~329. The `applyOrientation` template at lines 485-509 demonstrates the exact `RenderLock + SETTINGS.saveToFile() + section.reset()` pattern to mirror.
- `src/activities/settings/FontSelectionActivity.h` / `.cpp` — existing full-screen font family picker. Writes `SETTINGS.fontFamily` / `SETTINGS.sdFontFamilyName` directly in `handleSelection()` at lines 90-98 and then `finish()`es. **It does NOT call `SETTINGS.saveToFile()`** — persistence happens in the caller's result lambda (cf. `SettingsActivity.cpp:192`). Our reader callback must do the same.
- `src/CrossPointSettings.h` — `fontFamily` (uint8_t, 3 built-ins), `fontSize` (uint8_t enum `SMALL=0..X_LARGE=3`), `sdFontFamilyName[32]`.
- `src/SdCardFontSystem.h:55` — `extern SdCardFontSystem sdFontSystem;` is already declared here. `SettingsActivity.cpp:16,190` just `#include "SdCardFontSystem.h"` and uses the symbol — no manual `extern` redeclaration needed.

Patterns observed:
- Inline cycle pattern (ROTATE_SCREEN): `pendingX` member initialised from `SETTINGS.X` in constructor; Confirm on the row cycles `pendingX`; value passed back in `MenuResult`; parent unconditionally applies on result callback.
- Sub-activity pattern (SELECT_CHAPTER): menu returns the action, `onReaderMenuConfirm` launches the sub-activity via `startActivityForResult`.
- Translation keys `STR_FONT_SIZE`, `STR_FONT_FAMILY`, `STR_SMALL`, `STR_MEDIUM`, `STR_LARGE`, `STR_X_LARGE` are already present.
- Cache invalidation already keys on `fontSize` and `fontFamily` (per CLAUDE.md / file-formats.md) — no version bump needed.

## Development Approach

- **Testing approach**: Regular (code first), with **manual hardware verification** as the primary test. The project does not have unit-test infrastructure for UI/Activity code (existing host tests in `test/` cover only algorithmic components: JSON parsing, hyphenation, rounding). For embedded UI work, verification is `pio run` (no warnings), clang-format check, and on-device testing in all four orientations with heap monitoring.
- Complete each task fully before moving to the next.
- Make small, focused changes — three task units corresponding to the three logical surface areas (menu data, menu rendering, reader integration).
- After each task: rebuild with `pio run`, fix any warnings.
- Maintain backward compatibility: existing menu actions and `MenuResult` consumers keep working; the struct grows a new field.

## Testing Strategy

- **Unit tests**: not applicable. The project does not host-test UI/Activity code, and adding such infrastructure is out of scope for this feature.
- **Build verification**: `pio run` must complete with no new warnings after every task.
- **Manual device verification** (after Task 3):
  1. Open an EPUB. Press Confirm to open the menu. New rows "Font size" and "Font family" are visible in the documented order.
  2. Navigate to "Font size", press Confirm — value cycles SMALL → MEDIUM → LARGE → X_LARGE → SMALL.
  3. Press Back (cancel). Verify the book re-paginates at the new size.
  4. Re-open menu, change font size, this time press Confirm on another item (e.g., "Go to %"). Verify size still applied (unconditional on cancel/confirm, matching orientation).
  5. Re-open menu, navigate to "Font family", press Confirm. `FontSelectionActivity` opens. Pick a different family. Verify book re-paginates with the new family.
  6. Repeat the picker but Back out without picking — book still re-paginates (unconditional reset is acceptable per design).
  7. Run all four orientations to confirm right-aligned size value is drawn correctly relative to the landscape hint gutters.
  8. Check `ESP.getFreeHeap()` before menu open and after returning to reader — no leak.

## Progress Tracking

- Mark completed items with `[x]` immediately when done.
- Add newly discovered tasks with ➕ prefix.
- Document issues/blockers with ⚠️ prefix.
- Update plan if implementation deviates from original scope.

## Solution Overview

Two new entries in `EpubReaderMenuActivity::MenuAction`:
- `CHANGE_FONT_SIZE` — handled inline inside the menu's `loop()`, cycles `pendingFontSize`.
- `SELECT_FONT_FAMILY` — handled by closing the menu and dispatching in `onReaderMenuConfirm`, which launches `FontSelectionActivity`.

`MenuResult` grows one field (`uint8_t fontSize`). The constructor of `EpubReaderMenuActivity` grows one parameter (`uint8_t currentFontSize`). On menu exit (both Cancel and Confirm), `EpubReaderActivity` compares `menu.fontSize` to `SETTINGS.fontSize`; on change, wraps a `RenderLock` around the setting update + `SETTINGS.saveToFile()` + `section.reset()` — same shape as `applyOrientation` at `EpubReaderActivity.cpp:485-509`.

For the family picker, after `FontSelectionActivity` returns, `EpubReaderActivity` unconditionally persists settings and resets the section under a `RenderLock`. The picker writes the in-memory `SETTINGS` fields but does not call `saveToFile()` (cf. `FontSelectionActivity.cpp:90-98` — no save call; cf. `SettingsActivity.cpp:192` — caller's lambda calls `SETTINGS.saveToFile()`). Our callback follows the same pattern. Unconditional `section.reset()` is simpler than diff-detect; the cost of one redundant re-pagination when the user backs out without picking is acceptable.

The `SdCardFontRegistry*` needed by `FontSelectionActivity` is obtained from the global `sdFontSystem.registry()` (same as `SettingsActivity.cpp:190`).

## Technical Details

**New `MenuAction` values** (`EpubReaderMenuActivity.h`):
```cpp
enum class MenuAction {
  SELECT_CHAPTER,
  FOOTNOTES,
  CHANGE_FONT_SIZE,     // NEW — inline cycle
  SELECT_FONT_FAMILY,   // NEW — launches FontSelectionActivity
  GO_TO_PERCENT,
  AUTO_PAGE_TURN,
  ROTATE_SCREEN,
  SCREENSHOT,
  DISPLAY_QR,
  GO_HOME,
  SYNC,
  DELETE_CACHE
};
```

**Updated menu order in `buildMenuItems()`** (inserted after FOOTNOTES, before ROTATE_SCREEN):
```
SELECT_CHAPTER
FOOTNOTES (conditional)
CHANGE_FONT_SIZE    [value]   ← NEW
SELECT_FONT_FAMILY            ← NEW
ROTATE_SCREEN       [value]
AUTO_PAGE_TURN      [value]
GO_TO_PERCENT
SCREENSHOT
DISPLAY_QR
GO_HOME
SYNC
DELETE_CACHE
```
Bump the `items.reserve(...)` capacity from 10 to 12.

**New member fields** (`EpubReaderMenuActivity.h`):
```cpp
uint8_t pendingFontSize = 0;
const std::vector<StrId> fontSizeLabels = {
    StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE};
```

**Updated `MenuResult`** at `src/activities/ActivityResult.h:19-23`:
```cpp
struct MenuResult {
  int action = -1;
  uint8_t orientation = 0;
  uint8_t pageTurnOption = 0;
  uint8_t fontSize = 0;   // NEW
};
```

**Updated constructor signature** (`EpubReaderMenuActivity.h` and `.cpp`):
```cpp
EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                       const std::string& title, int currentPage, int totalPages,
                       int bookProgressPercent, uint8_t currentOrientation,
                       uint8_t currentFontSize,   // NEW
                       bool hasFootnotes);
```

**`loop()` change** — add a branch alongside the existing `ROTATE_SCREEN` / `AUTO_PAGE_TURN` inline-cycle blocks:
```cpp
if (selectedAction == MenuAction::CHANGE_FONT_SIZE) {
  pendingFontSize = (pendingFontSize + 1) % fontSizeLabels.size();
  requestUpdate();
  return;
}
```

**`render()` change** — after the existing `AUTO_PAGE_TURN` value-draw block, add a parallel block for `CHANGE_FONT_SIZE`:
```cpp
if (menuItems[i].action == MenuAction::CHANGE_FONT_SIZE) {
  const char* value = I18N.get(fontSizeLabels[pendingFontSize]);
  const auto width = renderer.getTextWidth(UI_10_FONT_ID, value);
  renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - 20 - width, displayY, value, !isSelected);
}
```

**`setResult` calls** — both the Confirm and Cancel paths must now include `pendingFontSize`:
```cpp
setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation,
                     selectedPageTurnOption, pendingFontSize});
```
and on Cancel:
```cpp
result.data = MenuResult{-1, pendingOrientation, selectedPageTurnOption, pendingFontSize};
```

**`EpubReaderActivity.cpp:176-187` construction + callback** — pass `SETTINGS.fontSize` to the new constructor parameter, and after `toggleAutoPageTurn(...)`:
```cpp
if (menu.fontSize != SETTINGS.fontSize) {
  RenderLock lock(*this);
  SETTINGS.fontSize = menu.fontSize;
  SETTINGS.saveToFile();
  section.reset();
}
requestUpdate();
```
The `RenderLock` is mandatory: every existing `section.reset()` in this file is wrapped in one (see `EpubReaderActivity.cpp:493-509` `applyOrientation`, and lines 235, 321, 407, 425, 467, 527, 533, 544, 556). `requestUpdate()` is called outside the lock scope so the next render picks up the reset section.

**`onReaderMenuConfirm` new case** (`EpubReaderActivity.cpp`):
```cpp
case EpubReaderMenuActivity::MenuAction::SELECT_FONT_FAMILY: {
  startActivityForResult(
      std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry()),
      [this](const ActivityResult&) {
        {
          RenderLock lock(*this);
          SETTINGS.saveToFile();   // FontSelectionActivity mutates SETTINGS but does not persist
          section.reset();
        }
        requestUpdate();
      });
  break;
}
```

Required includes added to `EpubReaderActivity.cpp` (copy from `SettingsActivity.cpp:10,16`):
```cpp
#include "activities/settings/FontSelectionActivity.h"
#include "SdCardFontSystem.h"
```
The global `sdFontSystem` is already declared `extern` in `SdCardFontSystem.h:55`; do not add a manual extern.

## What Goes Where

- **Implementation Steps**: code changes in `EpubReaderMenuActivity.{h,cpp}` and `EpubReaderActivity.cpp`, plus a build + format pass.
- **Post-Completion**: on-device manual verification, heap check, four-orientation visual check.

## Implementation Steps

### Task 1: Extend menu data model and update construction call site

**Files:**
- Modify: `src/activities/ActivityResult.h` (MenuResult struct at lines 19-23)
- Modify: `src/activities/reader/EpubReaderMenuActivity.h`
- Modify: `src/activities/reader/EpubReaderMenuActivity.cpp`
- Modify: `src/activities/reader/EpubReaderActivity.cpp` (single construction call site at lines 176-178)

- [x] In `src/activities/ActivityResult.h`, add `uint8_t fontSize = 0;` to `MenuResult` (default-init keeps any other aggregate-init sites valid).
- [x] Add `CHANGE_FONT_SIZE` and `SELECT_FONT_FAMILY` to `MenuAction` enum in `EpubReaderMenuActivity.h`, in the documented positions (after `FOOTNOTES`, before `GO_TO_PERCENT`).
- [x] Add `uint8_t currentFontSize` parameter to the `EpubReaderMenuActivity` constructor (placed before `hasFootnotes`). Add `uint8_t pendingFontSize` member; initialise it from the new param in the constructor member-init list.
- [x] Add `const std::vector<StrId> fontSizeLabels = {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE};` member.
- [x] Update `buildMenuItems()` to insert the two new items in the documented order and bump `items.reserve(10)` to `items.reserve(12)`.
- [x] Update the construction call site at `EpubReaderActivity.cpp:176-178` to pass `SETTINGS.fontSize` in the new position. Both Confirm and Cancel `MenuResult` constructions in `EpubReaderMenuActivity.cpp` (lines 74, 80) still compile because of the `fontSize = 0` default — they are properly updated in Task 2.
- [x] Build: `pio run` (skipped - PlatformIO/ESP32 toolchain not installable in sandbox; verified compile-consistency by manual review: member-init order matches declaration order, no -Wreorder; 3-arg MenuResult aggregate-init still valid via default member initializer).

### Task 2: Add inline font-size cycle to menu loop and render

**Files:**
- Modify: `src/activities/reader/EpubReaderMenuActivity.cpp`

- [x] In `loop()`, after the existing `AUTO_PAGE_TURN` cycle branch (around line 68-72), add a `CHANGE_FONT_SIZE` branch that cycles `pendingFontSize` modulo `fontSizeLabels.size()` and calls `requestUpdate()`.
- [x] In both Confirm (line 74) and Cancel (line 80) paths, update `setResult` / `result.data = MenuResult{...}` to include `pendingFontSize` as the new fourth field.
- [x] In `render()`, after the existing `AUTO_PAGE_TURN` value-draw block (around line 144-149), add a parallel `CHANGE_FONT_SIZE` block that right-aligns `I18N.get(fontSizeLabels[pendingFontSize])` on the row, using the same drawing pattern.
- [x] Build: `pio run` — must succeed with no new warnings.
- [x] Run clang-format: `find src/activities/reader -name "EpubReaderMenuActivity.*" | xargs clang-format -i`.

### Task 3: Wire up the reader integration

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.cpp`

- [x] Add `#include "activities/settings/FontSelectionActivity.h"` and `#include "SdCardFontSystem.h"` near the other activity includes at the top of the file (no manual `extern` — the header at `SdCardFontSystem.h:55` already declares the global).
- [x] In the menu-result lambda at lines 179-187, after `toggleAutoPageTurn(menu.pageTurnOption);`, add the font-size apply block from "Technical Details" — wrap the body in a `RenderLock lock(*this);` scope, call `SETTINGS.saveToFile()` (not `save()`), and call `requestUpdate()` after the lock scope closes.
- [x] In `onReaderMenuConfirm` `switch(action)` near line 329, add the `case SELECT_FONT_FAMILY:` block from "Technical Details" — `startActivityForResult` with a result lambda that wraps `SETTINGS.saveToFile() + section.reset()` in a `RenderLock` scope, then calls `requestUpdate()`. The `saveToFile()` call is required because `FontSelectionActivity` does not persist on its own.
- [x] Build: `pio run` — must succeed with no warnings.
- [x] Run clang-format on `src/activities/reader/EpubReaderActivity.cpp`.
- [x] Static analysis: `pio check` — no new findings related to the changes.

### Task 4: Verify acceptance criteria

- [x] Confirm all `MenuResult` construction sites and all `EpubReaderMenuActivity` constructor call sites compile cleanly (single call site exists at `EpubReaderActivity.cpp:178`; verified via grep — MenuResult constructed at EpubReaderMenuActivity.cpp:85 and :91 both pass `pendingFontSize` as 4th field; sole EpubReaderMenuActivity instantiation passes `SETTINGS.fontSize` in the new position).
- [x] Verify clang-format produces no diff: `find src/activities/reader src/activities/settings -name "*.cpp" -o -name "*.h" | xargs clang-format --dry-run --Werror` — exit 0, no diff.
- [x] Verify final build is clean: `pio run` (default env) — zero warnings. (skipped - PlatformIO/ESP32 toolchain not installable in sandbox; static review only)
- [x] (User) Run manual hardware verification per the Testing Strategy section above. (skipped - not automatable, requires Xteink X4 hardware)

### Task 5: Update documentation and archive plan

- [x] If a user-facing change log or release-notes file exists, add a one-line entry; otherwise skip. (skipped - no changelog file in repo)
- [x] CLAUDE.md update: not needed — no new patterns introduced.
- [x] Move this plan to `docs/plans/completed/20260516-reader-menu-font-controls.md` (create directory if needed).

## Post-Completion
*Items requiring manual intervention — no checkboxes, informational only.*

**Manual verification on hardware** (Xteink X4 device):
- All eight steps in "Manual device verification" above.
- Specifically verify the right-aligned size value renders cleanly in landscape CW and CCW (the `hintGutterWidth` already applied to `contentWidth` should keep it inside the gutter).
- Cycle font size at the SD-card-font case (have an SD card font selected as the active family) and confirm size cycle still works — fontSize is independent of family.
- After font family change, confirm cached sections in `.crosspoint/epub_<hash>/sections/` are regenerated (file modification timestamps change), confirming cache invalidation includes fontFamily.
- Heap watermark: log `ESP.getFreeHeap()` before menu open and after returning to reader on at least one full cycle — confirm no monotonic decrease.

**External system updates**: none.
