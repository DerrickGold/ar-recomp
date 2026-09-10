#ifndef AR_LOCALIZATION_LANGUAGE_KEYBOARD_H
#define AR_LOCALIZATION_LANGUAGE_KEYBOARD_H

#include "localization/language_pack.h"
#include "localization/language_keyboard_shape.h"

/* Validates every authored page, not only the currently selected key. Other
 * routes (including JP's reference-only kana screens) are left untouched. */
bool ArLanguageKeyboard_ValidateMessage(const ArLanguagePack *pack,
                                        const ArLanguageMessage *body,
                                        const char *semantic_id,
                                        ArLanguagePackError *error);

/* The shared runtime key reader. Input is normalized presentation text, with
 * its final rows holding the keyboard. One extra header line is reserved for
 * the runtime page indicator. ASCII spaces/tabs must separate graphemes so
 * the input's key count agrees with the renderer's gutter-delimited cells. */
bool ArLanguageKeyboard_KeyRange(const char *utf8, size_t bytes,
                                  unsigned row, unsigned column,
                                  uint32_t *start, uint32_t *end);

#endif
