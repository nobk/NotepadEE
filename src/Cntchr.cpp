#include "ExtendExpr.h"
#include "Cntchr.h"

#include <UniConversion.h>

#include <array>
#include <bitset>
#include <variant>
#include <vector>
#include <format>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <thread>
#include <atomic>
#include <stop_token>
#include <cstring>
#include <windows.h>

#if defined(_MSVC_LANG) && _MSVC_LANG >= 202002L
#define FORCE_INLINE    __forceinline
#else
#define FORCE_INLINE    [[gnu::always_inline]]
#endif

// UTF-8 辅助函数：解码下一个码点并前进指针
FORCE_INLINE static char32_t cntchr_utf8_next(const char*& it) noexcept {
	const auto ptr = reinterpret_cast<const unsigned char*>(it);
	const int bytes = Scintilla::Internal::UTF8BytesOfLead(*ptr);
	const char32_t cp = Scintilla::Internal::UnicodeFromUTF8(ptr);
#if 0 // 对比了一下，Scintilla::Internal查表方式稍微快一点
	const unsigned char lead = ptr[0];
	char32_t cp;
	int bytes;
	if (lead < 0x80) {
		cp = lead;
		bytes = 1;
	} else if (lead < 0xE0) {
		cp = ((lead & 0x1Fu) << 6) | (ptr[1] & 0x3Fu);
		bytes = 2;
	} else if (lead < 0xF0) {
		cp = ((lead & 0x0Fu) << 12) | ((ptr[1] & 0x3Fu) << 6) | (ptr[2] & 0x3Fu);
		bytes = 3;
	} else {
		cp = ((lead & 0x07u) << 18) | ((ptr[1] & 0x3Fu) << 12) | ((ptr[2] & 0x3Fu) << 6) | (ptr[3] & 0x3Fu);
		bytes = 4;
	}
#endif
	it += bytes;
	return cp;
}

// UTF-8 辅助函数：解码下一个码点（带边界校验，替换 utf8::next）
// 返回 false 表示无效序列，此时 it 前进 1 字节
FORCE_INLINE bool utf8_next_safe(const char*& it, const char* end, char32_t& cp) noexcept {
	if (it >= end) return false;
	const auto ptr = reinterpret_cast<const unsigned char*>(it);
	const size_t remaining = end - it;
	const int utf8Status = Scintilla::Internal::UTF8Classify(ptr, remaining);
	if (utf8Status & Scintilla::Internal::UTF8MaskInvalid) {
		cp = static_cast<unsigned char>(*it); // 无效字节值
		it++;
		return false;
	}
	const int bytes = utf8Status & Scintilla::Internal::UTF8MaskWidth;
	cp = Scintilla::Internal::UnicodeFromUTF8(ptr);
	it += bytes;
	return true;
}

namespace {
/* 以下代码由Python脚本生成
```python
import unicodedata
import sys

# 强制设置输出编码为UTF-8
if sys.stdout.encoding != 'utf-8':
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

def print_unicode_categories():
    # 定义目标类别
    target_categories = {'Sm', 'So'}

    # 存储结果
    sm_chars = []
    so_chars = []

    # 遍历所有可能的Unicode码点
    for i in range(0, 0x110000):
        try:
            char = chr(i)
            category = unicodedata.category(char)

            if category == 'Sm':
                sm_chars.append((i, char))
            elif category == 'So':
                so_chars.append((i, char))

        except ValueError:
            continue  # 跳过无效码点

    # 输出 Sm 类别字符
    print("=== Sm (Mathematical Symbols) ===")
    for codepoint, char in sm_chars:
        print(f"U+{codepoint:04X} {char} - {unicodedata.name(char, 'No Name')}")

    print("\n=== So (Other Symbols) ===")
    for codepoint, char in so_chars:
        print(f"U+{codepoint:04X} {char} - {unicodedata.name(char, 'No Name')}")

if __name__ == "__main__":
    print_unicode_categories()
```=

```python
import unicodedata
import sys
LineWidth = 102
print('*''/\n')
# 打印 Python 和 Unicode 版本以供参考
print(f"// Python version: {sys.version}")
print(f"// Unicode version: {unicodedata.unidata_version}")

# 定义空白符号的 Unicode 类别
whitespace_categories = {'Zs', 'Zl', 'Zp', 'Cc'}

# 定义标点符号的 Unicode 类别
punctuation_categories = {'Pc', 'Pd', 'Ps', 'Pe', 'Pi', 'Pf', 'Po', 'Sm'}

# 收集空白符号和标点符号的码点
whitespace_codepoints = []
punctuation_codepoints = []

# 遍历全部UNICODE找属于空白或标点符号的码点
for i in range(0, 0x110000):
    try:
        char = chr(i)
        category = unicodedata.category(char)
        if category in whitespace_categories:
            whitespace_codepoints.append(i)
        elif category in punctuation_categories:
            punctuation_codepoints.append(i)
    except ValueError:
        continue  # 跳过无效码点

# 合并连续区间（只有3个及以上连续数才合并为范围）
def merge_consecutive(codepoints):
    if not codepoints:
        return []

    # 排序
    codepoints.sort()

    ranges = []
    start = codepoints[0]
    end = codepoints[0]

    for i in range(1, len(codepoints)):
        if codepoints[i] == end + 1:
            end = codepoints[i]
        else:
            # 只有连续3个及以上才合并为范围
            if end - start >= 2:
                ranges.append((start, end))   # 范围
            else:
                # 单独输出每个数
                for j in range(start, end + 1):
                    ranges.append((j, None))
            start = codepoints[i]
            end = codepoints[i]

    # 处理最后一段
    if end - start >= 2:
        ranges.append((start, end))
    else:
        for j in range(start, end + 1):
            ranges.append((j, None))

    return ranges

# 生成 C++ 宏的辅助函数（支持 Range 格式）
def generate_switch_code(codepoints, func_name):
    nlen = len(codepoints)

    # 合并连续区间
    ranges = merge_consecutive(codepoints)

    # 构建输出行
    case_lines = []
    current_line = "    "

    for i, (start, end) in enumerate(ranges):
        if end is None:
            value = f"0x{start:02X}"
        else:
            value = f"Rg(0x{start:02X}, 0x{end:02X})"

        # 检查是否需要换行
        if len(current_line) + len(value) + 2 > LineWidth:  # 字符限制
            case_lines.append(current_line + "\\\n")
            current_line = "    "

        current_line += value + ", "

    # 添加最后一行
    if current_line != "    ":
        case_lines.append(current_line.rstrip(', ') + ", \\\n")

        # 添加注释行
    if case_lines:
        case_lines[-1] = case_lines[-1].rstrip(', \\\n') + f" // {nlen} total {func_name} code points"

    uset_code = f"#define {func_name.upper()}_CASES \\\n{''.join(case_lines)}"
    return uset_code

# 生成空白符号的 C++ 函数
whitespace_func = generate_switch_code(whitespace_codepoints, "whitespace")

# 生成标点符号的 C++ 函数
punctuation_func = generate_switch_code(punctuation_codepoints, "punctuation")

# 将结果输出
print(whitespace_func)
print(punctuation_func)

```=
*/

//To regenerate: run "python tools/GenerateCodePointRanges.py" from the repo root.

//++Autogenerated
// Generated: Python 3.14.6, Unicode 16.0.0
#define WHITESPACE_CASES \
    Rg(0x00, 0x20), Rg(0x7F, 0xA0), 0x1680, Rg(0x2000, 0x200A), 0x2028, 0x2029, 0x202F, 0x205F, \
    0x3000 // 84 total whitespace code points
#define PUNCTUATION_CASES \
    Rg(0x21, 0x23), Rg(0x25, 0x2F), Rg(0x3A, 0x40), Rg(0x5B, 0x5D), 0x5F, Rg(0x7B, 0x7E), 0xA1, 0xA7, \
    0xAB, 0xAC, 0xB1, 0xB6, 0xB7, 0xBB, 0xBF, 0xD7, 0xF7, 0x37E, 0x387, 0x3F6, Rg(0x55A, 0x55F), \
    0x589, 0x58A, 0x5BE, 0x5C0, 0x5C3, 0x5C6, 0x5F3, 0x5F4, Rg(0x606, 0x60A), 0x60C, 0x60D, 0x61B, \
    Rg(0x61D, 0x61F), Rg(0x66A, 0x66D), 0x6D4, Rg(0x700, 0x70D), Rg(0x7F7, 0x7F9), Rg(0x830, 0x83E), \
    0x85E, 0x964, 0x965, 0x970, 0x9FD, 0xA76, 0xAF0, 0xC77, 0xC84, 0xDF4, 0xE4F, 0xE5A, 0xE5B, \
    Rg(0xF04, 0xF12), 0xF14, Rg(0xF3A, 0xF3D), 0xF85, Rg(0xFD0, 0xFD4), 0xFD9, 0xFDA, \
    Rg(0x104A, 0x104F), 0x10FB, Rg(0x1360, 0x1368), 0x1400, 0x166E, 0x169B, 0x169C, \
    Rg(0x16EB, 0x16ED), 0x1735, 0x1736, Rg(0x17D4, 0x17D6), Rg(0x17D8, 0x17DA), Rg(0x1800, 0x180A), \
    0x1944, 0x1945, 0x1A1E, 0x1A1F, Rg(0x1AA0, 0x1AA6), Rg(0x1AA8, 0x1AAD), 0x1B4E, 0x1B4F, \
    Rg(0x1B5A, 0x1B60), Rg(0x1B7D, 0x1B7F), Rg(0x1BFC, 0x1BFF), Rg(0x1C3B, 0x1C3F), 0x1C7E, 0x1C7F, \
    Rg(0x1CC0, 0x1CC7), 0x1CD3, Rg(0x2010, 0x2027), Rg(0x2030, 0x205E), Rg(0x207A, 0x207E), \
    Rg(0x208A, 0x208E), 0x2118, Rg(0x2140, 0x2144), 0x214B, Rg(0x2190, 0x2194), 0x219A, 0x219B, \
    0x21A0, 0x21A3, 0x21A6, 0x21AE, 0x21CE, 0x21CF, 0x21D2, 0x21D4, Rg(0x21F4, 0x22FF), \
    Rg(0x2308, 0x230B), 0x2320, 0x2321, 0x2329, 0x232A, 0x237C, Rg(0x239B, 0x23B3), \
    Rg(0x23DC, 0x23E1), 0x25B7, 0x25C1, Rg(0x25F8, 0x25FF), 0x266F, Rg(0x2768, 0x2775), \
    Rg(0x27C0, 0x27FF), Rg(0x2900, 0x2AFF), Rg(0x2B30, 0x2B44), Rg(0x2B47, 0x2B4C), \
    Rg(0x2CF9, 0x2CFC), 0x2CFE, 0x2CFF, 0x2D70, Rg(0x2E00, 0x2E2E), Rg(0x2E30, 0x2E4F), \
    Rg(0x2E52, 0x2E5D), Rg(0x3001, 0x3003), Rg(0x3008, 0x3011), Rg(0x3014, 0x301F), 0x3030, 0x303D, \
    0x30A0, 0x30FB, 0xA4FE, 0xA4FF, Rg(0xA60D, 0xA60F), 0xA673, 0xA67E, Rg(0xA6F2, 0xA6F7), \
    Rg(0xA874, 0xA877), 0xA8CE, 0xA8CF, Rg(0xA8F8, 0xA8FA), 0xA8FC, 0xA92E, 0xA92F, 0xA95F, \
    Rg(0xA9C1, 0xA9CD), 0xA9DE, 0xA9DF, Rg(0xAA5C, 0xAA5F), 0xAADE, 0xAADF, 0xAAF0, 0xAAF1, 0xABEB, \
    0xFB29, 0xFD3E, 0xFD3F, Rg(0xFE10, 0xFE19), Rg(0xFE30, 0xFE52), Rg(0xFE54, 0xFE66), 0xFE68, \
    0xFE6A, 0xFE6B, Rg(0xFF01, 0xFF03), Rg(0xFF05, 0xFF0F), Rg(0xFF1A, 0xFF20), Rg(0xFF3B, 0xFF3D), \
    0xFF3F, Rg(0xFF5B, 0xFF65), 0xFFE2, Rg(0xFFE9, 0xFFEC), Rg(0x10100, 0x10102), 0x1039F, 0x103D0, \
    0x1056F, 0x10857, 0x1091F, 0x1093F, Rg(0x10A50, 0x10A58), 0x10A7F, Rg(0x10AF0, 0x10AF6), \
    Rg(0x10B39, 0x10B3F), Rg(0x10B99, 0x10B9C), 0x10D6E, 0x10D8E, 0x10D8F, 0x10EAD, \
    Rg(0x10F55, 0x10F59), Rg(0x10F86, 0x10F89), Rg(0x11047, 0x1104D), 0x110BB, 0x110BC, \
    Rg(0x110BE, 0x110C1), Rg(0x11140, 0x11143), 0x11174, 0x11175, Rg(0x111C5, 0x111C8), 0x111CD, \
    0x111DB, Rg(0x111DD, 0x111DF), Rg(0x11238, 0x1123D), 0x112A9, 0x113D4, 0x113D5, 0x113D7, 0x113D8, \
    Rg(0x1144B, 0x1144F), 0x1145A, 0x1145B, 0x1145D, 0x114C6, Rg(0x115C1, 0x115D7), \
    Rg(0x11641, 0x11643), Rg(0x11660, 0x1166C), 0x116B9, Rg(0x1173C, 0x1173E), 0x1183B, \
    Rg(0x11944, 0x11946), 0x119E2, Rg(0x11A3F, 0x11A46), Rg(0x11A9A, 0x11A9C), Rg(0x11A9E, 0x11AA2), \
    Rg(0x11B00, 0x11B09), 0x11BE1, Rg(0x11C41, 0x11C45), 0x11C70, 0x11C71, 0x11EF7, 0x11EF8, \
    Rg(0x11F43, 0x11F4F), 0x11FFF, Rg(0x12470, 0x12474), 0x12FF1, 0x12FF2, 0x16A6E, 0x16A6F, 0x16AF5, \
    Rg(0x16B37, 0x16B3B), 0x16B44, Rg(0x16D6D, 0x16D6F), Rg(0x16E97, 0x16E9A), 0x16FE2, 0x1BC9F, \
    0x1D6C1, 0x1D6DB, 0x1D6FB, 0x1D715, 0x1D735, 0x1D74F, 0x1D76F, 0x1D789, 0x1D7A9, 0x1D7C3, \
    Rg(0x1DA87, 0x1DA8B), 0x1E5FF, 0x1E95E, 0x1E95F, 0x1EEF0, 0x1EEF1 // 1805 total punctuation code points
//--Autogenerated

#define WHITESPACE_CASES_OLD \
        0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0008, 0x0009, \
        0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x000F, 0x0010, 0x0011, 0x0012, 0x0013, \
        0x0014, 0x0015, 0x0016, 0x0017, 0x0018, 0x0019, 0x001A, 0x001B, 0x001C, 0x001D, \
        0x001E, 0x001F, 0x0020, 0x007F, 0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, \
        0x0086, 0x0087, 0x0088, 0x0089, 0x008A, 0x008B, 0x008C, 0x008D, 0x008E, 0x008F, \
        0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097, 0x0098, 0x0099, \
        0x009A, 0x009B, 0x009C, 0x009D, 0x009E, 0x009F, 0x00A0, 0x1680, 0x2000, 0x2001, \
        0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007, 0x2008, 0x2009, 0x200A, 0x2028, \
        0x2029, 0x202F, 0x205F, 0x3000 // 84 total whitespace code points

// ==========================================
// 基础定义与工具
// ==========================================

constexpr const int UnicodeMax = 0x10FFFF;

struct Rg {
    char32_t start;
    char32_t end;
};

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };

// 页面类型简化：Empty/Full 由查找表隐含
enum class PageKind : uint8_t {
    Single,       // 1个码点，存储在 PageInfo 中
    Small,        // 2-3个码点，存储在 PageInfo 中
    Bitmap        // >3个码点，存储在全局位图池中
};

// ==========================================
// 范围解析核心
// ==========================================

template <auto... Args>
class CodePointRangeSetBase {
protected:
    using ArgVariant = std::variant<int, Rg>;
    static constexpr std::array<ArgVariant, sizeof...(Args)> args = {Args...};

    static consteval std::array<Rg, sizeof...(Args)> convert_to_ranges() {
        std::array<Rg, sizeof...(Args)> result{};
        for (size_t i = 0; i < sizeof...(Args); ++i) {
            result[i] = std::visit(overloaded{
                [](int v) {
                    if (v < 0 || v > UnicodeMax) throw std::domain_error("Out of bounds");
                    return Rg{static_cast<char32_t>(v), static_cast<char32_t>(v)};
                },
                [](const Rg& r) {
                    if (r.start > r.end || r.end > UnicodeMax) throw std::domain_error("Invalid Range");
                    return r;
                }
            }, args[i]);
        }
        return result;
    }

    static constexpr std::array<Rg, sizeof...(Args)> ranges = convert_to_ranges();

    static constexpr char32_t max_code_point = []() consteval {
        if constexpr (sizeof...(Args) == 0) return 0;
        char32_t max = 0;
        for (const auto& r : ranges) if (r.end > max) max = r.end;
        return max;
    }();
};

// ==========================================
// 核心实现模版 页面大小可变稀疏位图集合
// ==========================================

template <auto... Args>
class CodePointRangeSet : public CodePointRangeSetBase<Args...> {
    using Base = CodePointRangeSetBase<Args...>;

public:
    // 极简的 4字节 页面描述符
    struct CompactPageInfo {
        // 原始数据容器
        uint32_t raw;

        // 类型枚举 (2 bits)
        enum Type : uint32_t {
            Bitmap = 0, // 00
            Single = 1, // 01
            Double = 2, // 10
            Triple = 3  // 11
        };

        constexpr Type get_type() const {
            return static_cast<Type>(raw >> 30);
        }

        // --- 判断是否为位图模式 ---
        FORCE_INLINE constexpr bool is_bitmap() const {
            return get_type() == Bitmap;
        }

        // --- Bitmap 模式 ---
        // 获取位图在全局数组中的起始索引
        FORCE_INLINE constexpr uint32_t get_bitmap_index() const {
            return raw & 0x3FFFFFFF; // 取低30位 (支持 10亿+ 个位图字，足够了)
        }

        // --- Sparse 模式 (Single/Double/Triple) ---
        // 检查 offset 是否存在于压缩的数据中
        FORCE_INLINE constexpr bool contains_sparse(uint8_t offset) const {
            // 根据类型解码
            const uint32_t t = raw >> 30;

            // 技巧：去掉if改为无分支判断，以便流水线并行以及AVX2优化（g++ clang++）
            // 这里的逻辑：将 offset 放在不同的字节位置进行比较

            // 检查第1个字节 (Single, Double, Triple 都有)
            const uint32_t r1 = static_cast<uint32_t>(((raw >> 0) & 0xFF) == offset);

            // 检查第2个字节 (仅 Double, Triple)
            const uint32_t r2 = static_cast<uint32_t>(t >= Double) & static_cast<uint32_t>(((raw >> 8) & 0xFF) == offset);

            // 检查第3个字节 (仅 Triple)
            const uint32_t r3 = static_cast<uint32_t>(t == Triple) & static_cast<uint32_t>(((raw >> 16) & 0xFF) == offset);

            return (r1 | r2 | r3);
        }

        // --- 构建辅助函数 (编译期调用) ---
        static constexpr CompactPageInfo make_bitmap(size_t idx) {
            return { (static_cast<uint32_t>(Bitmap) << 30) | static_cast<uint32_t>(idx) };
        }

        static constexpr CompactPageInfo make_sparse(const uint8_t* start, size_t count) {
            uint32_t d = 0;
            uint32_t type_bits = 0;

            if (count == 1) type_bits = Single;
            else if (count == 2) type_bits = Double;
            else if (count == 3) type_bits = Triple;

            for (size_t i = 0; i < count; ++i) {
                d |= (static_cast<uint32_t>(start[i]) << (i * 8));
            }

            return { (type_bits << 30) | d };
        }
    };

private:
    // 常量定义
    static constexpr size_t PageSize = 256;
    static constexpr size_t FULL_PAGE_COUNT = PageSize;
    static constexpr size_t WORDS_PER_PAGE = (PageSize + 63) / 64; // 位图需要的 uint64_t 数量
    static constexpr size_t SMALL_THRESHOLD = 3; // <= 3 使用内嵌数组

    // 1. 预计算：统计每个页面的码点数量
    static constexpr size_t TOTAL_PAGES_NEEDED = (Base::max_code_point / PageSize) + 1;

    using PageCountArray = std::array<uint16_t, TOTAL_PAGES_NEEDED>;

    static consteval PageCountArray calculate_page_counts() {
        PageCountArray counts{};
        for (const auto& r : Base::ranges) {
            uint32_t start_page = r.start / PageSize;
            uint32_t end_page = r.end / PageSize;

            for (uint32_t p = start_page; p <= end_page; ++p) {
                uint32_t p_start = p * PageSize;
                uint32_t p_end = p_start + PageSize - 1;
                uint32_t act_start = std::max((uint32_t)r.start, p_start);
                uint32_t act_end = std::min((uint32_t)r.end, p_end);

                if (act_start <= act_end) {
                    counts[p] += static_cast<uint16_t>(act_end - act_start + 1);
                }
            }
        }
        return counts;
    }

    static constexpr PageCountArray page_counts = calculate_page_counts();

    // 2. 预计算：获取页面内的偏移量 (仅对非Full/Empty页面)
    // 返回 pair<偏移数组, 实际数量>
    static constexpr auto get_offsets_for_page(uint32_t page_idx) {
        std::array<uint8_t, PageSize> offsets{};
        uint16_t count = 0;

        uint32_t p_start = page_idx * PageSize;
        uint32_t p_end = p_start + PageSize - 1;

        for (const auto& r : Base::ranges) {
            if (r.end < p_start || r.start > p_end) continue;
            uint32_t s = std::max((uint32_t)r.start, p_start);
            uint32_t e = std::min((uint32_t)r.end, p_end);
            for (uint32_t cp = s; cp <= e; ++cp) {
                offsets[count++] = static_cast<uint8_t>(cp - p_start);
            }
        }
        return std::pair{offsets, count};
    }

    // 3. 构建数据结构
    // 统计不同类型的页面数量，用于分配数组大小
    struct MetaCounts {
        size_t non_empty_desc = 0; // 需要 PageInfo 的数量 (Single + Small + Bitmap)
        size_t bitmap_words = 0;   // 位图池需要的 uint64_t 数量
    };

    static consteval MetaCounts count_meta() {
        MetaCounts mc{};
        for (uint16_t c : page_counts) {
            if (c == 0 || c == FULL_PAGE_COUNT) continue; // Empty 或 Full 不需要描述符

            mc.non_empty_desc++;
            if (c > SMALL_THRESHOLD) {
                mc.bitmap_words += WORDS_PER_PAGE;
            }
        }
        return mc;
    }

    static constexpr MetaCounts meta = count_meta();

    // 1. 定义查找表使用的类型
    // 如果描述符数量 < 126 (保留 -1 和 -2 的位置)，使用 int8_t，否则使用 int16_t
    using LookupType = std::conditional_t<
        (meta.non_empty_desc < 126),
        int8_t,
        int16_t
    >;

    static constexpr auto page_stats = []() consteval { // 统计各种页面数量
        struct S{ size_t empty, full, single, small, bitmap; } s{};
        for (size_t i = 0; i < page_counts.size(); ++i) {
            uint16_t c = page_counts[i];
            if (c == 0)                     ++s.empty;
            else if (c == PageSize)         ++s.full;
            else if (c == 1)                ++s.single;
            else if (c <= SMALL_THRESHOLD)  ++s.small;
            else                            ++s.bitmap;
        }
        return s;
    }();

private:
#pragma warning(push)
#pragma warning(disable:4324)
    // 实际数据容器
    struct alignas(64) DataBlock {
        // 查找表：映射 PageIndex -> DescriptorIndex
        // -1: Empty, -2: Full, >=0: index in descriptors
        std::array<LookupType, TOTAL_PAGES_NEEDED> lookup;

        // 页面描述符数组
        std::array<CompactPageInfo, (meta.non_empty_desc > 0 ? meta.non_empty_desc : 1)> descriptors;

        // 位图数据池
        std::array<uint64_t, (meta.bitmap_words > 0 ? meta.bitmap_words : 1)> bitmaps;
    };
#pragma warning(pop)

    static constexpr DataBlock build_data() {
        DataBlock db{};
        db.lookup.fill(static_cast<LookupType>(-1)); // 初始化为 -1 Empty

        size_t desc_idx = 0;
        size_t bitmap_idx = 0;

        for (uint32_t i = 0; i < page_counts.size(); ++i) {
            uint16_t count = page_counts[i];

            if (count == 0) {
                db.lookup[i] = -1; // Empty
                continue;
            }
            if (count == FULL_PAGE_COUNT) {
                db.lookup[i] = -2; // Full
                continue;
            }

            // 1. 在查找表中记录当前页对应的描述符位置
            db.lookup[i] = static_cast<LookupType>(desc_idx);
            //auto& page = db.descriptors[desc_idx];

            auto [offsets, actual_count] = get_offsets_for_page(i);

            if (actual_count <= SMALL_THRESHOLD) {
                // Single, Double, Triple 都走这里
                // make_sparse 会自动根据 count 决定 Type
                db.descriptors[desc_idx] = CompactPageInfo::make_sparse(
                    offsets.data(), // 假设这是指向 array 的指针
                    actual_count
                );
            }
            else {
                // Bitmap 模式 (Count >= 4)
                db.descriptors[desc_idx] = CompactPageInfo::make_bitmap(bitmap_idx);

                // 填充位图
                for(int k=0; k<actual_count; ++k) {
                    uint8_t off = offsets[k];
                    size_t word_pos = bitmap_idx + (off >> 6);
                    db.bitmaps[word_pos] |= (1ULL << (off & 0x3F));
                }
                bitmap_idx += WORDS_PER_PAGE;
            }
            desc_idx++;
        }
        return db;
    }

    static constexpr DataBlock data = build_data();

public:
    FORCE_INLINE static constexpr bool contains(char32_t cp) noexcept {
        if (cp > Base::max_code_point) return false;

        const size_t p_idx = cp / PageSize;

        const int16_t lookup_val = data.lookup[p_idx];

        if (lookup_val == -2) return true;  // in Full page
        if (lookup_val == -1) return false; // in Empty page

        const uint8_t offset = cp % PageSize;

        const auto& page = data.descriptors[lookup_val];

        if (page.is_bitmap()) {
            // 位图模式查询
            size_t word_idx = page.get_bitmap_index() + (offset >> 6); // offset / 64
            return (data.bitmaps[word_idx] >> (offset & 0x3F)) & 1ULL; // offset % 64
        }
        // 稀疏模式查询 (Single/Double/Triple)
        return page.contains_sparse(offset);
    }

    // 调试/统计接口
    static constexpr size_t get_memory_usage() { return sizeof(DataBlock); }
    static constexpr size_t get_page_size() { return PageSize; }
    static constexpr size_t get_max_code_point() { return Base::max_code_point; }

    // 编译期可直接访问的统计量
    static constexpr size_t stat_empty   = page_stats.empty;
    static constexpr size_t stat_full    = page_stats.full;
    static constexpr size_t stat_single  = page_stats.single;
    static constexpr size_t stat_small   = page_stats.small;
    static constexpr size_t stat_bitmap  = page_stats.bitmap;
    static constexpr auto page_stat = page_stats;

private:
    // 编译期完整性验证：
    // 1) 对于 Base::ranges 中的每一个范围，所有码点都应被 contains() 包含
    // 2) 对于 0..Base::max_code_point 范围内，若 contains(cp) 为真，则该 cp 必须落在某个 Base::ranges 中
    static consteval bool validate_integrity() {
        // 检查所有 ranges 内的码点都被 contains() 覆盖
        for (const auto& r : Base::ranges) {
            for (char32_t cp = r.start; cp <= r.end; ++cp) {
                if (!CodePointRangeSet::contains(cp)) return false;
            }
        }

        // 检查 contains() 没有包含不属于 ranges 的码点
        if constexpr (Base::max_code_point > 0) {
            for (char32_t cp = 0; cp <= Base::max_code_point; ++cp) {
                if (CodePointRangeSet::contains(cp)) {
                    bool in_range = false;
                    for (const auto& r : Base::ranges) {
                        if (cp >= r.start && cp <= r.end) { in_range = true; break; }
                    }
                    if (!in_range) return false;
                }
            }
        }

        return true;
    }
#if defined(_MSC_VER) && !defined(__clang__)
    // 严格校验：确保集合完整性，防止非法数据进入编译期常量
    static_assert(validate_integrity(), "CodePointRangeSet data validation failed: contains()/ranges mismatch");
#elif defined(__clang__)
    // Note: Compile-time integrity check skipped in Clang to avoid complexity limits.
#endif
};

// 定义具体集合类型 - 使用页面大小可变稀疏位图集合实现
using WhitespaceSet = CodePointRangeSet<WHITESPACE_CASES>;
using PunctuationSet = CodePointRangeSet<PUNCTUATION_CASES>;
using WhitespaceSetOld = CodePointRangeSet<WHITESPACE_CASES_OLD>;
using ChineseCharSet = CodePointRangeSet<
    Rg(0x2E80, 0x2EF3), //U+2E80 ⺀ - CJK RADICAL REPEAT ... U+2EF3 ⻳ - CJK RADICAL C-SIMPLIFIED TURTLE
    Rg(0x2F00, 0x2FD5), //U+2F00 ⼀ - KANGXI RADICAL ONE ... U+2FD5 ⿕ - KANGXI RADICAL FLUTE
    Rg(0x31C0, 0x31E3), //U+31C0 ㇀ - CJK STROKE T ... U+31E3 ㇣ - CJK STROKE Q
    Rg(0x4E00, 0x9FFF),     // 基本汉字
    Rg(0x3400, 0x4DBF),     // 扩展A
    Rg(0x20000, 0x2EBEF),   // 扩展B-F
    Rg(0x30000, 0x3134F),   // 扩展G
    Rg(0x31350, 0x323AF),   // 扩展H
    Rg(0xF900, 0xFAFF),     // 兼容汉字
    Rg(0x2F800, 0x2FA1F)    // 兼容扩展
>;

FORCE_INLINE constexpr bool is_unicode_whitespace(char32_t code_point) {
    return WhitespaceSet::contains(code_point);
}

FORCE_INLINE constexpr bool is_unicode_punctuation(char32_t code_point) {
    return PunctuationSet::contains(code_point);
}

FORCE_INLINE constexpr bool is_chinese_character(char32_t code_point) {
    return ChineseCharSet::contains(code_point);
}

// ===== 边界验证（精简）因为类的尾部已经使用编译时函数validate_integrity()自查完成 =====
// 保留少量几行代表性断言以防破坏最关键的范围边界。

// 中文字符主要区间代表性检查
static_assert(is_chinese_character(0x4E00), "0x4E00 should be included");
static_assert(is_chinese_character(0x9FFF), "0x9FFF should be included");
static_assert(!is_chinese_character(0x4DFF), "0x4DFF should NOT be included"); // off-by-one

// 空白字符代表性检查
static_assert(is_unicode_whitespace(0x0020), "Space (0x20) should be whitespace");
static_assert(!is_unicode_whitespace(0x0021), "0x21 should NOT be whitespace");

// 极端边界
static_assert(!is_unicode_whitespace(0x10FFFF), "Unicode max should be outside whitespace set");

/* 内存空间占用情况及性能数据
WhitespaceSet   BitsetSize  192 B, Max Codepoint 0x003000, PageSize 256
Pages: Empty  45, Full   0, Single   2, Small   0, Bitmap   2

PunctuationSet  BitsetSize 2432 B, Max Codepoint 0x01eef1, PageSize 256
Pages: Empty 410, Full   3, Single  13, Small  19, Bitmap  50

ChineseCharSet  BitsetSize 1088 B, Max Codepoint 0x0323af, PageSize 256
Pages: Empty 416, Full 381, Single   0, Small   0, Bitmap   7

--- Unicode Analysis Benchmark ---
Data Size:      10.00 MB
Total Codepoints: 5519190
Iterations:      300
----------------------------------
Total Time:      1215.88 ms
Avg Time/Pass:   4.0529 ms
Throughput:      2467.35 MB/s
----------------------------------
Counts - Hanzi: 1655568, Punct: 827717, Space: 826997, Other: 2208908 */

} // namespace

namespace {

// 核心处理函数（不含 SEH），提取出来以规避 MSVC C2712：
// SEH (__try/__except) 不能与具有析构函数的 C++ 对象共存于同一函数。
struct CharacterCounts {
    size_t chinese;
    size_t punctuation;
    size_t space;
    size_t all;
};

// 短于此长度的文本直接串行处理，避免线程开销
constexpr size_t MIN_PARALLEL_THRESHOLD = 20 * 1024;

// 串行处理短串（无需线程开销，减少延迟）
static CharacterCounts CountCharacterTypesSerial(const char* utf8Text, size_t length) {
    size_t total_c = 0, total_p = 0, total_s = 0, total_all = 0;
    const char* ptr = utf8Text;
    const char* const end = utf8Text + length;
    while (ptr < end) {
        const char32_t cp = cntchr_utf8_next(ptr);
        total_s += static_cast<size_t>(is_unicode_whitespace(cp));
        total_c += static_cast<size_t>(is_chinese_character(cp));
        total_p += static_cast<size_t>(is_unicode_punctuation(cp));
        total_all++;
    }
    return { total_c, total_p, total_s, total_all };
}

CharacterCounts CountCharacterTypesImpl(const char* utf8Text, size_t length,
    std::stop_token token = std::stop_token{}) {
#pragma warning(push)
#pragma warning(disable:4324)
    // 定义计数器结构体，强制 64 字节对齐 (Cache Line Size)
    // 这确保了不同线程修改不同的结构体实例时，不会发生缓存一致性冲突
    struct alignas(64) ThreadCounters {
        size_t chinese = 0;
        size_t punctuation = 0;
        size_t space = 0;
        size_t all = 0;
        // 结构体大小为 32 字节 (4个 size_t)，剩余 32 字节作为填充，防止与下一个结构体在同一行
    };
#pragma warning(pop)

    constexpr size_t BATCH_CODEPOINTS = 4096;

    // --- 并行处理长串 (使用 C++20 std::jthread + std::stop_token) ---
    const unsigned int num_threads = std::max(1u, std::thread::hardware_concurrency());
    const size_t chunk_size = length / num_threads;

    std::atomic<size_t> atomic_c{0}, atomic_p{0}, atomic_s{0}, atomic_all{0};

    {
        std::vector<std::jthread> threads;
        threads.reserve(num_threads);

        for (unsigned int t = 0; t < num_threads; ++t) {
            const size_t my_start_byte = t * chunk_size;
            const size_t my_end_byte = (t == num_threads - 1) ? length : (t + 1) * chunk_size;

            threads.emplace_back([&, token, t, num_threads, my_start_byte, my_end_byte]() {
                ThreadCounters local;
                alignas(64) char32_t buffer[BATCH_CODEPOINTS];

                const char* ptr = utf8Text + my_start_byte;
                const char* limit = utf8Text + my_end_byte;

                // 1. 除了 0 号线程外，所有线程都必须跳过开头的延续字节
                if (t != 0) {
                    while (ptr < limit && (static_cast<unsigned char>(*ptr) & 0xC0) == 0x80) {
                        ++ptr;
                    }
                }

                // 2. 除了最后一个线程外，所有线程都要读取完整的末尾字符
                if (t != num_threads - 1) {
                    while (limit < utf8Text + length && (static_cast<unsigned char>(*limit) & 0xC0) == 0x80) {
                        ++limit;
                    }
                }

                while (ptr < limit) {
                    // 每轮循环检查取消标志
                    if (token.stop_requested()) {
                        return;
                    }

                    const size_t remaining = limit - ptr;
                    size_t input_this_round = std::min(remaining, BATCH_CODEPOINTS);

                    while (input_this_round > 0 &&
                        (static_cast<unsigned char>(ptr[input_this_round]) & 0xC0) == 0x80) {
                        --input_this_round;
                    }

                    const std::string_view chunk(ptr, input_this_round);
                    const size_t count = Scintilla::Internal::UTF32Length(chunk);
                    Scintilla::Internal::UTF32FromUTF8(chunk, reinterpret_cast<unsigned int*>(buffer), count);

                    for (size_t i = 0; i < count; ++i) {
                        const char32_t cp = buffer[i];
                        local.space += is_unicode_whitespace(cp);
                        local.chinese += is_chinese_character(cp);
                        local.punctuation += is_unicode_punctuation(cp);
                        ++local.all;
                    }

                    ptr += input_this_round;
                }

                atomic_c.fetch_add(local.chinese, std::memory_order_relaxed);
                atomic_p.fetch_add(local.punctuation, std::memory_order_relaxed);
                atomic_s.fetch_add(local.space, std::memory_order_relaxed);
                atomic_all.fetch_add(local.all, std::memory_order_relaxed);
            });
        }
    } // 作用域结束 -> std::jthread 析构自动 join

    return {
        atomic_c.load(std::memory_order_relaxed),
        atomic_p.load(std::memory_order_relaxed),
        atomic_s.load(std::memory_order_relaxed),
        atomic_all.load(std::memory_order_relaxed)
    };
}

// 非 SEH 桥接：将原始指针转为 stop_token，短串直接串行，长串走并行
static CharacterCounts CountBridge(const char* utf8Text, size_t length,
    const std::stop_token* token_ptr) {
    const auto token = token_ptr ? *token_ptr : std::stop_token{};
    if (length < MIN_PARALLEL_THRESHOLD) {
        return CountCharacterTypesSerial(utf8Text, length);
    }
    return CountCharacterTypesImpl(utf8Text, length, token);
}

// SEH 安全包装：只接受原始指针，避免 MSVC C2712
//（__try/__except 与带析构函数的 std::stop_token 不能共存于同一函数）
static bool CountSehSafe(const char* utf8Text, size_t length,
    const std::stop_token* token_ptr, CharacterCounts& out) noexcept {
    __try {
        out = CountBridge(utf8Text, length, token_ptr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void CountCharacterTypes(const char* utf8Text, size_t length,
    size_t* chinese,
    size_t* punctuation,
    size_t* nonChineseNonSpace,
    size_t* space,
    std::stop_token token) {
    if (!utf8Text || !chinese || !punctuation || !nonChineseNonSpace || !space || length == 0) {
        *chinese = *punctuation = *nonChineseNonSpace = *space = 0;
        return;
    }

    size_t total_c = 0, total_p = 0, total_s = 0, total_all = 0;

    CharacterCounts counts;
    if (!CountSehSafe(utf8Text, length, &token, counts)) {
        *chinese = *punctuation = *nonChineseNonSpace = *space = 0;
        return;
    }

    // 如果被取消则返回零（无需进一步处理）
    if (token.stop_requested()) {
        *chinese = *punctuation = *nonChineseNonSpace = *space = 0;
        return;
    }

    total_c = counts.chinese;
    total_p = counts.punctuation;
    total_s = counts.space;
    total_all = counts.all;

    *chinese = total_c;
    *punctuation = total_p;
    *space = total_s;
    *nonChineseNonSpace = (total_c + total_p + total_s) <= total_all ?
        total_all - total_c - total_p - total_s :
        0;
}

std::string& RemoveUnnecessaryLeadingCharacters(std::string& str) noexcept {
    if (str.empty()) return str;

    // 状态机常量
    enum : uint8_t { ST_TRACKING = 0, ST_LOCKED = 1 };
    enum : uint8_t { IN_C = 0, IN_W = 1, IN_O = 2 };

    // NEXT_STATE[当前状态][输入类型]
    static constexpr uint8_t U8NEXT_STATE[2][3] = {
        /* ST_TRACKING: 遇到中文或空白保持追踪，遇到其他则锁定 */
        { ST_TRACKING, ST_TRACKING, ST_LOCKED },
        /* ST_LOCKED: 只有遇到中文才能解锁回到追踪状态 */
        { ST_TRACKING, ST_LOCKED,   ST_LOCKED }
    };

    // ACTION[当前状态][输入类型] -> 是否更新 cut_pos
    static constexpr uint8_t U8ACTION[2][3] = {
        /* ST_TRACKING: 中文(1), 空白(1), 其他(0) */
        { 1, 1, 0 },
        /* ST_LOCKED: 中文(1), 空白(0), 其他(0) */
        { 1, 0, 0 }
    };

    const char* const start_ptr = str.data();
    const char* const end_ptr = start_ptr + str.size();
    const char* it = start_ptr;

    size_t cut_pos = 0;
    uint8_t state = ST_TRACKING;

    // 使用 Scintilla 内置函数追求极致速度，前提是输入已校验
    while (it < end_ptr) {
        const uint32_t cp = cntchr_utf8_next(it); // it 会被移动到下一个码点位置

        // 1. 输入分类 (无分支逻辑)
        uint32_t is_c = static_cast<uint32_t>(is_chinese_character(cp));
        uint32_t is_w = static_cast<uint32_t>(is_unicode_whitespace(cp));

        // in: 0=Chinese, 1=Whitespace, 2=Other
        // 利用互斥性：is_c=1 则 in=0; is_c=0,is_w=1 则 in=1; 均为0 则 in=2
        uint32_t in = (is_c ^ 1) * (2 - is_w);

        // 2. 更新剪切位置 (无分支计算)
        // 计算当前码点结束相对于起始的偏移
        size_t cp_end_offset = it - start_ptr;
        uint8_t act = U8ACTION[state][in];

        // 如果 act 为 1，则更新 cut_pos 为当前码点的末尾
        cut_pos = cut_pos + act * (cp_end_offset - cut_pos);

        // 3. 状态转换
        state = U8NEXT_STATE[state][in];
    }

    if (cut_pos > 0) {
        str.erase(0, cut_pos);
    }

    return str;
}

std::string& RemoveUnnecessaryLeadingCharacters_old(std::string& str) {
    if (str.empty()) return str;

    std::string_view sv = str;
    const char* const original_start = sv.data();
    const char* last_chinese_end = nullptr;

    // 1. 寻找最后一个中文字符的结束位置
	{
		const char* const end_ptr = sv.data() + sv.size();
		const char* pos = sv.data();
		while (pos < end_ptr) {
			char32_t cp;
			if (utf8_next_safe(pos, end_ptr, cp)) {
				if (is_chinese_character(cp)) {
					last_chinese_end = pos; // 记录该中文字符之后的位置
				}
			}
			// 无效序列：utf8_next_safe 已前进 1 字节，继续
		}
	}

    // 2. 缩小 view 范围：如果有中文，从中文后开始；否则保持原状
    if (last_chinese_end) {
        size_t offset = last_chinese_end - original_start;
        sv.remove_prefix(offset);
    }

    // 3. 跳过开头的空白字符
	{
		const char* const end_ptr = sv.data() + sv.size();
		const char* space_pos = sv.data();
		while (space_pos < end_ptr) {
			char32_t cp;
			if (!utf8_next_safe(space_pos, end_ptr, cp)) {
				break; // 无效序列视为非空白，停止
			}
			if (!is_unicode_whitespace(cp)) {
				break; // 找到非空白，停止
			}
		}

		// 4. 计算偏移并更新原字符串
		size_t final_offset = space_pos - original_start;
		str.erase(0, final_offset);
	}
    return str;
}

#include <chrono>
#include <random>
#include <numeric>  // 用于计算总和

// 随机数种子（固定种子保证测试可复现）
constexpr int RANDOM_SEED = 42;

std::string RunPerformanceTest() {
    // 1. 生成测试数据 (约 10MB)
    const size_t TARGET_SIZE = 10 * 1024 * 1024;
    std::string test_data;
    test_data.reserve(TARGET_SIZE);

    std::mt19937 rng(RANDOM_SEED);
    // 定义权重：30% 汉字, 40% 英文/数字, 15% 标点, 15% 空格
    std::uniform_int_distribution<int> dist(0, 99);

    // 准备一些样本
    std::vector<std::string> sample_hz = {"你", "好", "世", "界", "编", "程", "之", "美"};
    std::vector<std::string> sample_en = {"H", "e", "l", "l", "o", "W", "o", "r", "l", "d", "1", "2", "3"};
    std::vector<std::string> sample_punc = {"，", "。", "！", "？", "；", "：", "（", "）"};
    std::vector<std::string> sample_space = {" ", "\n", "\t", "\r"};

    while (test_data.size() < TARGET_SIZE) {
        int roll = dist(rng);
        if (roll < 30) test_data += sample_hz[rng() % sample_hz.size()];
        else if (roll < 70) test_data += sample_en[rng() % sample_en.size()];
        else if (roll < 85) test_data += sample_punc[rng() % sample_punc.size()];
        else test_data += sample_space[rng() % sample_space.size()];
    }

    // 2. 预热 (Warm-up)
    size_t c, p, n, s;
    CountCharacterTypes(test_data.data(), test_data.size(), &c, &p, &n, &s);

    // 3. 正式测试 (跑 300 遍)
    const int ITERATIONS = 300;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < ITERATIONS; ++i) {
        CountCharacterTypes(test_data.data(), test_data.size(), &c, &p, &n, &s);
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> total_ms = end - start;

    double avg_ms = total_ms.count() / ITERATIONS;
    double throughput_mb_s = (static_cast<double>(TARGET_SIZE) / (1024 * 1024)) / (avg_ms / 1000.0);
    size_t total_chars = c + p + n + s;

    // 4. 使用 std::format 构造返回信息
    return std::format(
        "--- Unicode Analysis Benchmark ---\n"
        "Data Size:      {:.2f} MB\n"
        "Total Codepoints: {}\n"
        "Iterations:      {}\n"
        "----------------------------------\n"
        "Total Time:      {:.2f} ms\n"
        "Avg Time/Pass:   {:.4f} ms\n"
        "Throughput:      {:.2f} MB/s\n"
        "----------------------------------\n"
        "Counts - Hanzi: {}, Punct: {}, Space: {}, Other: {}\n",
        static_cast<double>(TARGET_SIZE) / (1024 * 1024),
        total_chars,
        ITERATIONS,
        total_ms.count(),
        avg_ms,
        throughput_mb_s,
        c, p, s, n
    );
}

// 辅助函数：位图/集合大小和最大码点查询（调试用）
std::string getBitsetInfo() {
    auto setstr = [](std::string_view name, size_t mem, size_t max_code_point,
        size_t pagesz, size_t empty, size_t full, size_t single, size_t small, size_t bitmap) {
            return std::format(
                "{:15} BitsetSize {:>4} B, Max Codepoint 0x{:06x}, PageSize {}\n"
                "Pages: Empty {:3}, Full {:3}, Single {:3}, Small {:3}, Bitmap {:3}\n\n",
                name, mem, max_code_point, pagesz, empty, full, single, small, bitmap);
    };

    return setstr("WhitespaceSet", WhitespaceSet::get_memory_usage(), WhitespaceSet::get_max_code_point(),
        WhitespaceSet::get_page_size(),
        WhitespaceSet::stat_empty,
        WhitespaceSet::stat_full,
        WhitespaceSet::stat_single,
        WhitespaceSet::stat_small,
        WhitespaceSet::stat_bitmap
    ) + setstr("PunctuationSet", PunctuationSet::get_memory_usage(), PunctuationSet::get_max_code_point(),
        PunctuationSet::get_page_size(),
        PunctuationSet::stat_empty,
        PunctuationSet::stat_full,
        PunctuationSet::stat_single,
        PunctuationSet::stat_small,
        PunctuationSet::stat_bitmap
    ) + setstr("ChineseCharSet", ChineseCharSet::get_memory_usage(), ChineseCharSet::get_max_code_point(),
        ChineseCharSet::get_page_size(),
        ChineseCharSet::stat_empty,
        ChineseCharSet::stat_full,
        ChineseCharSet::stat_single,
        ChineseCharSet::stat_small,
        ChineseCharSet::stat_bitmap
    ) + RunPerformanceTest();

}

#pragma warning(push)
#pragma warning(disable: 4566)  // Emoji char
std::string RunTests() {
    // 辅助：将本地编码（ACP，简体中文下为 GBK）字符串转为 UTF-8
    auto acp_to_utf8 = [](const char* s) -> std::string {
        int wlen = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
        if (wlen <= 0) return s;
        std::wstring ws(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_ACP, 0, s, -1, ws.data(), wlen);
        int u8len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), -1, nullptr, 0, nullptr, nullptr);
        if (u8len <= 0) return s;
        std::string u8(static_cast<size_t>(u8len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, ws.data(), -1, u8.data(), u8len, nullptr, nullptr);
        if (!u8.empty() && u8.back() == '\0') u8.pop_back();
        return u8;
    };

    // 辅助：将宽字符串（含 \U 转义的补充字符）转为 UTF-8
    auto wcs_to_utf8 = [](const wchar_t* ws) -> std::string {
        int u8len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
        if (u8len <= 0) return "";
        std::string u8(static_cast<size_t>(u8len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, ws, -1, u8.data(), u8len, nullptr, nullptr);
        if (!u8.empty() && u8.back() == '\0') u8.pop_back();
        return u8;
    };

    struct TestCase {
        std::string input;
        std::string expected;
        std::string desc;
    };
    std::vector<TestCase> cases = {
        // 1. 空白与普通字符
        {"", "", "Empty string"},
        {"   ", "", "Pure whitespaces"},
        {"abc", "abc", "No Chinese, no leading WS"},
        {"  abc", "abc", "No Chinese, leading WS"},

        // 2. 中文基本逻辑 (ACP → UTF-8 运行时转换)
        {acp_to_utf8("你好"), "", "Pure Chinese"},
        {acp_to_utf8("你好 "), "", "Chinese followed by WS at end"},
        {acp_to_utf8("你好 abc"), "abc", "Chinese followed by WS and text"},
        {acp_to_utf8("  你好 abc"), "abc", "Leading WS and Chinese"},

        // 3. 再次推进逻辑 (The "Jump" logic)
        {acp_to_utf8("你好 a 再见"), "", "Two Chinese blocks separated by 'a'"},
        {acp_to_utf8("你好 a 再见 b"), "b", "Two Chinese blocks, final stop at 'b'"},
        {acp_to_utf8("  abc 你好 def"), "def", "Locked by 'abc', then re-activated by Chinese"},
        {acp_to_utf8("你好  a   再见   "), "", "Multiple WS and multiple Chinese"},

        // 4. 复杂 UTF-8 混合
        {acp_to_utf8("你好 ") + wcs_to_utf8(L"\U0001F60A") + " abc",
         wcs_to_utf8(L"\U0001F60A") + " abc",
         "Chinese followed by Emoji (Emoji is 'Other')"},
        {wcs_to_utf8(L"\U0001F60A") + acp_to_utf8(" 你好 abc"),
         "abc",
         "Leading Emoji, then Chinese should clear everything before it"}
    };

    int passed = 0;
    std::string testResults;

    for (auto& tc : cases) {
        std::string target = tc.input;
        RemoveUnnecessaryLeadingCharacters(target);

        if (target == tc.expected) {
            testResults += std::format("[PASS] {}\n", tc.desc);
            passed++;
        } else {
            testResults += std::format(
                "[FAIL] {}\n  Input:    [{}]\n  Expected: [{}]\n  Actual:   [{}]\n",
                tc.desc, tc.input, tc.expected, target
            );
        }
    }

    testResults += std::format("\nResult: {}/{} passed.\n", passed, cases.size());
    return testResults;
}
#pragma warning(pop)

// 生成指定长度的随机混合字符串（包含中文、空格、字母、Emoji）
std::string GenerateRandomString(size_t length) {
    if (length == 0) return "";

    // 字符池：包含中文、字母、空格、Emoji
    const std::vector<char32_t> char_pool = {
        U'你', U'好', U'测', U'试', U'a', U'b', U'c', U' ', U'1', U'2',
        U'😊', U'🔥', U'🚀', U'@', U'#', U'$'
    };

    //std::random_device rd;
    std::mt19937_64 gen(RANDOM_SEED);
    std::uniform_int_distribution<size_t> dist(0, char_pool.size() - 1);

    std::u32string u32_str;
    for (size_t i = 0; i < length; ++i) {
        u32_str += char_pool[dist(gen)];
    }

    // 转换为 UTF-8 编码的 std::string
    std::string utf8_str;
    for (char32_t c : u32_str) {
        if (c < 0x80) {
            utf8_str += static_cast<char>(c);
        } else if (c < 0x800) {
            utf8_str += static_cast<char>(0xC0 | (c >> 6));
            utf8_str += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            utf8_str += static_cast<char>(0xE0 | (c >> 12));
            utf8_str += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            utf8_str += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x110000) {
            utf8_str += static_cast<char>(0xF0 | (c >> 18));
            utf8_str += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            utf8_str += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            utf8_str += static_cast<char>(0x80 | (c & 0x3F));
        }
    }

    return utf8_str;
}

// 性能测试函数：跑300遍不同长度字符串，返回格式化的测试结果
std::string RunTrimPerformanceTest(bool old) {
    // 定义测试的字符串长度范围（300个不同长度，均匀分布在 0-10000 字符）
    const int test_rounds = 1000;
    std::vector<size_t> test_lengths;
    for (int i = 0; i < test_rounds; ++i) {
        // 长度范围：0 ~ 10000 字符（可根据需要调整）
        test_lengths.push_back(static_cast<size_t>(i * 10000 / test_rounds));
    }

    // 存储每轮测试的结果
    std::vector<double> elapsed_times_ms;  // 每轮耗时（毫秒）
    std::vector<size_t> processed_bytes;   // 每轮处理的字节数
    std::vector<double> throughput_mb_s;   // 每轮吞吐量（MB/s）

    // 随机数生成器（确保每次测试字符串不同）
    std::random_device rd;
    std::mt19937 gen(rd());

    auto fn = old ? RemoveUnnecessaryLeadingCharacters_old: RemoveUnnecessaryLeadingCharacters;
    // 执行300轮测试
    for (size_t len : test_lengths) {
        // 生成随机测试字符串
        std::string test_str = GenerateRandomString(len);
        size_t bytes = test_str.size();  // 实际处理的字节数

        // 计时开始
        auto start = std::chrono::high_resolution_clock::now();

        // 执行核心处理函数
        fn(test_str);

        // 计时结束
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> elapsed = end - start;
        double time_ms = elapsed.count();

        // 计算吞吐量：MB/s = (字节数 / 1024 / 1024) / (耗时 / 1000)
        double throughput = (time_ms > 0) ? (bytes / 1024.0 / 1024.0) / (time_ms / 1000.0) : 0.0;

        // 保存本轮结果
        elapsed_times_ms.push_back(time_ms);
        processed_bytes.push_back(bytes);
        throughput_mb_s.push_back(throughput);
    }

    // 计算汇总统计
    double total_time_ms = std::accumulate(elapsed_times_ms.begin(), elapsed_times_ms.end(), 0.0);
    size_t total_bytes = std::accumulate(processed_bytes.begin(), processed_bytes.end(), (size_t)0);
    double avg_throughput = (total_time_ms > 0) ? (total_bytes / 1024.0 / 1024.0) / (total_time_ms / 1000.0) : 0.0;
    double max_throughput = *std::max_element(throughput_mb_s.begin()+1, throughput_mb_s.end());
    double min_throughput = *std::min_element(throughput_mb_s.begin()+1, throughput_mb_s.end());

    // 格式化测试结果（使用std::format，便于修改排版）
    std::string result;
    result += std::format("=== 性能测试报告 ===\n");
    result += std::format("测试轮数: {}\n", test_rounds);
    result += std::format("字符串长度范围: 0 ~ {} 字符\n", test_lengths.back());
    result += std::format("总处理耗时: {:.6f} 毫秒\n", total_time_ms);
    result += std::format("总处理字节数: {} 字节 ({:.2f} MB)\n", total_bytes, total_bytes / 1024.0 / 1024.0);
    result += std::format("\n=== 吞吐量统计 ===\n");
    result += std::format("平均吞吐量: {:.2f} MB/s\n", avg_throughput);
    result += std::format("最大吞吐量: {:.2f} MB/s\n", max_throughput);
    result += std::format("最小吞吐量: {:.2f} MB/s\n", min_throughput);

    // 可选：输出每轮详细数据（如需精简可注释）
    result += std::format("\n=== 每轮详细数据（前10轮示例）===\n");
    result += std::format("{:<10} {:<15} {:<15} {:<15}\n", "长度(字符)", "耗时(ms)", "字节数", "吞吐量(MB/s)");
    for (int i = 1; i < std::min(10, test_rounds); ++i) {
        result += std::format("{:<10} {:<15.6f} {:<15} {:<15.2f}\n",
                             test_lengths[i],
                             elapsed_times_ms[i],
                             processed_bytes[i],
                             throughput_mb_s[i]);
    }

    return result;
}

std::string RunValidationBenchmark() {
	// ===================== 可配置参数 =====================
	// 测试数据尺寸（单位：字节），最终显示为 KB（保留两位小数）
	const std::vector<size_t> TEST_SIZES_BYTES = {256, 512, 1024, 4096, 8192, 16384, 32768, 65536};
	// 每个尺寸的测试迭代次数
	const int TEST_ITERATIONS = 10000;
	// ASCII/CJK 混合比例（数值越大，ASCII 占比越高，范围 0-10）
	const int ASCII_CJK_RATIO = 3;
	// 输出格式相关配置
	const int COLUMN_WIDTH_SIZE = 10;    // Size(KB) 列宽度
	const int COLUMN_WIDTH_TPUT = 18;
	const int SEPARATOR_LENGTH = 60;     // 分隔线长度
	const int OUTPUT_PRECISION = 2;      // 浮点数输出精度
	const int SIZE_PRECISION = 2;        // Size(KB) 列的小数精度（核心调整项）
	// ============================================================================

	std::string result;  // 存储最终格式化结果

	// 1. 输出标题和表头（使用 std::format 替代 stringstream）
	result += "=== UTF-8 Validation Benchmark (Scintilla::Internal::UTF8IsValid) ===\n";
	result += std::format("{:<{}} {:<{}}\n",
							"Size(KB)", COLUMN_WIDTH_SIZE,
		"Throughput(MB/s)", COLUMN_WIDTH_TPUT);
		result += std::format("{}\n", std::string(SEPARATOR_LENGTH, '-'));

	// 2. 生成测试数据的lambda函数（逻辑不变，适配参数）
	auto generate_utf8 = [&](size_t bytes) {
		std::string res;
		res.reserve(bytes);
		std::mt19937 gen(RANDOM_SEED);  // 使用可配置的种子
		std::uniform_int_distribution<> dis(0, 10);

		// 将本地编码的中文字符串转为 UTF-8
		auto acp_to_u8 = [](const char* s) -> std::string {
			int wlen = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
			if (wlen <= 0) return s;
			std::wstring ws(static_cast<size_t>(wlen), L'\0');
			MultiByteToWideChar(CP_ACP, 0, s, -1, ws.data(), wlen);
			int u8len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), -1, nullptr, 0, nullptr, nullptr);
			if (u8len <= 0) return s;
			std::string u8(static_cast<size_t>(u8len), '\0');
			WideCharToMultiByte(CP_UTF8, 0, ws.data(), -1, u8.data(), u8len, nullptr, nullptr);
			if (!u8.empty() && u8.back() == '\0') u8.pop_back();
			return u8;
		};

		std::string ascii = "Hello World ";
		std::string cjk = acp_to_u8("你好世界");
		while (res.size() < bytes) {
			if (dis(gen) > ASCII_CJK_RATIO) res += ascii;
			else res += cjk;
		}
		if (res.size() > bytes) res.resize(bytes);
			return res;
	};

	// 3. 遍历测试尺寸，执行性能测试
	for (size_t size : TEST_SIZES_BYTES) {
		std::string test_data = generate_utf8(size);
		const char* data = test_data.data();
		const size_t len = test_data.size();

		// 累积结果，防止编译器消除或提升函数调用
		bool sink = false;
		auto start = std::chrono::high_resolution_clock::now();
		for (int i = 0; i < TEST_ITERATIONS; ++i) {
			// 每次循环读取一个 volatile 字节，制造内存屏障阻止 hoisting
			volatile char guard = data[i % len];
			(void)guard;
			sink ^= Scintilla::Internal::UTF8IsValid(std::string_view(data, len));
		}
		auto end = std::chrono::high_resolution_clock::now();
		// 用掉 sink 防止 DCE
		if (sink) result += "";

		double time_sec = std::chrono::duration<double>(end - start).count();
		double total_mb = (static_cast<double>(size) * TEST_ITERATIONS) / (1024.0 * 1024.0);
		double throughput = total_mb / time_sec;
		double size_kb = static_cast<double>(size) / 1024.0;
		result += std::format("{:<{}.{}f} {:<{}.{}f}\n",
								size_kb, COLUMN_WIDTH_SIZE, SIZE_PRECISION,
								throughput, COLUMN_WIDTH_TPUT, OUTPUT_PRECISION);
	}
	return result;
}
