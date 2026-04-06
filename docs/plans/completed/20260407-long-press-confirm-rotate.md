# Long-Press Confirm Button to Toggle Screen Orientation

## Overview
- Add long-press (>=1000ms) on the Confirm (OK) button to toggle between Portrait and Landscape CCW orientations while reading an EPUB
- Short-press Confirm (<1000ms) continues to open the reader menu (existing behavior unchanged)
- Provides a quick shortcut for the most common orientation change without navigating the menu

## Context (from discovery)
- Files involved: `EpubReaderActivity.cpp` (loop + applyOrientation), `EpubReaderActivity.h` (member flag)
- Existing pattern: Back button long-press (>=1000ms) goes to file browser (`ReaderUtils::GO_HOME_MS`, line 163)
- `applyOrientation()` already handles the full cycle: save position, persist settings, update renderer, reset section for re-layout
- Orientation enum: `PORTRAIT=0`, `LANDSCAPE_CCW=3` (defined in `CrossPointSettings.h`)
- No new heap allocations needed; reuses existing infrastructure

## Development Approach
- **testing approach**: Regular (no unit test framework for activity code on ESP32-C3)
- complete each task fully before moving to the next
- make small, focused changes
- **CRITICAL: update this plan file when scope changes during implementation**
- maintain backward compatibility with existing Confirm and menu behavior

## Solution Overview
- Add a `rotateTriggered` one-shot guard flag to prevent repeated triggers during a single long-press
- In `loop()`, insert long-press Confirm detection *before* the existing `wasReleased(Confirm)` block
- Gate the existing menu-open to short-press only by checking `getHeldTime() < ROTATE_MS`
- Toggle orientation between Portrait and Landscape CCW via existing `applyOrientation()` method
- Add `ROTATE_MS` constant to `ReaderUtils` namespace alongside existing `GO_HOME_MS`

## Implementation Steps

### Task 1: Add ROTATE_MS constant to ReaderUtils

**Files:**
- Modify: `src/activities/reader/ReaderUtils.h`

- [x] Add `constexpr unsigned long ROTATE_MS = 1000;` in the `ReaderUtils` namespace, next to `GO_HOME_MS` (line 11)

### Task 2: Add rotateTriggered flag to EpubReaderActivity

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.h`

- [x] Add `bool rotateTriggered = false;` private member alongside other bool flags (near line 29)

### Task 3: Implement long-press Confirm rotation in loop()

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.cpp`

- [x] Insert long-press Confirm detection block *before* the existing `wasReleased(Confirm)` block (before line 139):
  - Check `isPressed(Button::Confirm) && getHeldTime() >= ReaderUtils::ROTATE_MS && !rotateTriggered`
  - Toggle: if current orientation is `PORTRAIT`, call `applyOrientation(LANDSCAPE_CCW)`, otherwise call `applyOrientation(PORTRAIT)`
  - Set `rotateTriggered = true` to prevent re-triggering while held
  - `return` to consume the input
- [x] Gate existing `wasReleased(Confirm)` menu-open (line 139) to also require `getHeldTime() < ReaderUtils::ROTATE_MS`
- [x] Add `rotateTriggered = false` reset inside the `wasReleased(Confirm)` block (or as a separate check on release) so the flag resets for the next press

### Task 4: Verify build compiles cleanly

- [x] Run `pio run` and confirm zero errors/warnings
- [x] Verify no new heap allocations introduced

## Post-Completion

**Manual verification** (hardware testing required):
- Confirm short-press Confirm still opens reader menu in all orientations
- Confirm long-press (>=1s) Confirm toggles Portrait <-> Landscape CCW
- Confirm releasing after long-press does NOT also open the menu
- Confirm repeated long-presses toggle back and forth correctly
- Confirm orientation persists after leaving and re-entering the reader
- Confirm reader menu ROTATE_SCREEN option still works independently
- Test with auto-page-turn active (should disable auto-turn on Confirm release, not rotate)
