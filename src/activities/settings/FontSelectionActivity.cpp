#include "FontSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

FontSelectionActivity::FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const SdCardFontRegistry* registry)
    : Activity("FontSelect", renderer, mappedInput), registry_(registry) {}

void FontSelectionActivity::onEnter() {
  Activity::onEnter();

  // Build combined font list: built-in + SD card fonts
  fonts_.clear();
  fonts_.reserve(CrossPointSettings::BUILTIN_FONT_COUNT + (registry_ ? registry_->getFamilyCount() : 0));

  fonts_.push_back({I18N.get(StrId::STR_NOTO_SERIF), true, 0});
  fonts_.push_back({I18N.get(StrId::STR_NOTO_SANS), true, 1});
  fonts_.push_back({I18N.get(StrId::STR_OPEN_DYSLEXIC), true, 2});

  if (registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      fonts_.push_back({families[i].name, false, static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i)});
    }
  }

  // Cursor lands on the previously-used font for one-press toggle.
  // Falls back to current font when there's no valid different previous (first run, SD font removed, etc.).
  const int currentIndex = resolveFontIndex(SETTINGS.fontFamily, SETTINGS.sdFontFamilyName);
  const int previousIndex = resolveFontIndex(SETTINGS.previousFontFamily, SETTINGS.previousSdFontFamilyName);
  if (previousIndex >= 0 && previousIndex != currentIndex) {
    selectedIndex_ = previousIndex;
  } else {
    selectedIndex_ = currentIndex >= 0 ? currentIndex : 0;
  }

  requestUpdate();
}

int FontSelectionActivity::resolveFontIndex(uint8_t builtinIndex, const char* sdName) const {
  if (sdName[0] != '\0') {
    if (!registry_) return -1;
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      if (families[i].name == sdName) {
        return CrossPointSettings::BUILTIN_FONT_COUNT + i;
      }
    }
    return -1;
  }
  if (builtinIndex < CrossPointSettings::BUILTIN_FONT_COUNT) {
    return builtinIndex;
  }
  return -1;
}

void FontSelectionActivity::onExit() { Activity::onExit(); }

void FontSelectionActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int listSize = static_cast<int>(fonts_.size());
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);

  buttonNavigator_.onNextRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
    requestUpdate();
  });

  buttonNavigator_.onPreviousRelease([this, listSize] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
    requestUpdate();
  });

  buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
    selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
    requestUpdate();
  });
}

void FontSelectionActivity::handleSelection() {
  const auto& font = fonts_[selectedIndex_];

  // Determine the new (builtin, sdName) pair without mutating SETTINGS yet,
  // so we can compare against the current values to decide whether to roll previous.
  uint8_t newBuiltin = SETTINGS.fontFamily;
  const char* newSdName = "";
  if (font.settingIndex < CrossPointSettings::BUILTIN_FONT_COUNT) {
    newBuiltin = font.settingIndex;
  } else if (registry_) {
    const int sdIdx = font.settingIndex - CrossPointSettings::BUILTIN_FONT_COUNT;
    const auto& families = registry_->getFamilies();
    if (sdIdx < static_cast<int>(families.size())) {
      newSdName = families[sdIdx].name.c_str();
    } else {
      finish();
      return;
    }
  } else {
    finish();
    return;
  }

  // Roll previous only when the selection actually changes — otherwise repeated confirms
  // would clobber the toggle target.
  const bool sdChanged = strcmp(newSdName, SETTINGS.sdFontFamilyName) != 0;
  const bool builtinChanged = (newSdName[0] == '\0') && (SETTINGS.sdFontFamilyName[0] != '\0' || newBuiltin != SETTINGS.fontFamily);
  if (sdChanged || builtinChanged) {
    SETTINGS.previousFontFamily = SETTINGS.fontFamily;
    strncpy(SETTINGS.previousSdFontFamilyName, SETTINGS.sdFontFamilyName, sizeof(SETTINGS.previousSdFontFamilyName) - 1);
    SETTINGS.previousSdFontFamilyName[sizeof(SETTINGS.previousSdFontFamilyName) - 1] = '\0';
  }

  if (newSdName[0] == '\0') {
    SETTINGS.fontFamily = newBuiltin;
    SETTINGS.sdFontFamilyName[0] = '\0';
  } else {
    strncpy(SETTINGS.sdFontFamilyName, newSdName, sizeof(SETTINGS.sdFontFamilyName) - 1);
    SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
  }
  finish();
}

void FontSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_FAMILY));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // Determine which font index is currently active (to mark as "Selected")
  int currentFontIndex = 0;
  if (SETTINGS.sdFontFamilyName[0] != '\0' && registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      if (families[i].name == SETTINGS.sdFontFamilyName) {
        currentFontIndex = CrossPointSettings::BUILTIN_FONT_COUNT + i;
        break;
      }
    }
  } else {
    currentFontIndex = SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily : 0;
  }

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(fonts_.size()), selectedIndex_,
      [this](int index) { return fonts_[index].name; }, nullptr, nullptr,
      [this, currentFontIndex](int index) -> std::string { return index == currentFontIndex ? tr(STR_SELECTED) : ""; },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
