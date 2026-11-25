#pragma once
// UI languages. Keys are the English texts; translations live in lang_table.inc
// (generated from tools/translations.py). Arabic is shaped (joined letter forms)
// and put in visual right-to-left order, because ImGui draws glyphs one by one.
#include <string>

namespace i18n {

enum Lang { EN, TR, FR, DE, AR, ZH_HANT, ZH_HANS, LANG_COUNT };

void SetLanguage(int lang);
int Language();
const char* LanguageCode(int lang);    // "en", "tr", ...
int LanguageFromCode(const char* code);  // EN if unknown
const char* LanguageName(int lang);    // native name, ready to draw

// Display-ready translation (shaped for Arabic). Unknown keys come back unchanged.
const char* T(const char* key);
// Logical (unshaped) translation, for building text that is shaped later.
const char* TL(const char* key);
// Translates the format, formats it, then makes it display-ready.
std::string F(const char* keyFormat, ...);
// Makes an already translated, logical UTF-8 string display-ready (no-op unless Arabic).
std::string Shape(const std::string& logical);
// Display-ready text wrapped to `width` pixels with the current ImGui font
// (lines are broken in logical order, then each line is shaped).
std::string Wrap(const std::string& logical, float width);

// Registers extra translations at run time (mod manifests): key -> text for one language.
void AddTranslation(int lang, const std::string& key, const std::string& text);

}  // namespace i18n
