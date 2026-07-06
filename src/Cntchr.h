#pragma once
#include <cstddef>
#include <stop_token>
#include <utility>

/**
 * @brief Count character types in UTF-8 text.
 *
 * @param utf8Text         UTF-8 encoded text (does not need to be null-terminated)
 * @param length           Byte length of utf8Text
 * @param chinese          [out] CJK unified ideographs and related
 * @param punctuation      [out] Unicode punctuation (Pc,Pd,Ps,Pe,Pi,Pf,Po) + math symbols (Sm)
 * @param nonChineseNonSpace [out] everything else not counted above
 * @param space            [out] Unicode whitespace (Zs,Zl,Zp) + C0/C1 controls (Cc)
 * @param token            [in] std::stop_token for cooperative cancellation;
 *                          when stop is requested the function returns early.
 *                          Default-constructed token (never stop-requested) if omitted.
 */
void CountCharacterTypes(const char* utf8Text, size_t length,
	size_t* chinese,
	size_t* punctuation,
	size_t* nonChineseNonSpace,
	size_t* space,
	std::stop_token token = std::stop_token{});

// 小于此字节数的文本直接串行处理，不创建线程
inline constexpr size_t MIN_PARALLEL_THRESHOLD = 20 * 1024;

/**
 * @brief Trim Unicode whitespace from both ends of UTF-8 text.
 *
 * Uses is_unicode_whitespace() to correctly handle the full Unicode whitespace set.
 * Does NOT modify the original buffer.
 *
 * @param utf8Text  UTF-8 encoded text
 * @param length    Byte length of utf8Text
 * @return          std::pair{trimmed_start_offset, trimmed_length}
 *                  When the result is empty, returns {length, 0}.
 */
std::pair<size_t, size_t> TrimWhitespace(const char* utf8Text, size_t length) noexcept;
