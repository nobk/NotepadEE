#pragma once
#include <cstddef>
#include <atomic>

/**
 * @brief Count character types in UTF-8 text.
 *
 * @param utf8Text         UTF-8 encoded text (does not need to be null-terminated)
 * @param length           Byte length of utf8Text
 * @param chinese          [out] CJK unified ideographs and related
 * @param punctuation      [out] Unicode punctuation (Pc,Pd,Ps,Pe,Pi,Pf,Po) + math symbols (Sm)
 * @param nonChineseNonSpace [out] everything else not counted above
 * @param space            [out] Unicode whitespace (Zs,Zl,Zp) + C0/C1 controls (Cc)
 * @param cancel           [in] optional atomic flag; when set to true, the function
 *                          may return early with undefined results. Pass nullptr if
 *                          cancellation is not needed.
 */
void CountCharacterTypes(const char* utf8Text, size_t length,
	size_t* chinese,
	size_t* punctuation,
	size_t* nonChineseNonSpace,
	size_t* space,
	const std::atomic<bool>* cancel = nullptr);
