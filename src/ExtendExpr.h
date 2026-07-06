#pragma once
#include <string>
#include <atomic>
#include <cstddef>

bool CallExtendExpr(bool bShiftDown, bool noEnter);
size_t RemoveUnnecessaryLeadingCharacters(const char* utf8Text, size_t length) noexcept;
std::string RunTests();
std::string RunValidationBenchmark();
std::string RunPerformanceTest();
std::string getBitsetInfo();
std::string RunTrimPerformanceTest(bool old = false);

bool EvaluateJSExpression(const char* expr, int codepage, std::string& result);
bool EvaluateJSExpressionCached(const char* expr, int codepage, std::string& result);

//=============================================================================
//
// 异步选中文本字数统计（仅 Notepad4.cpp 主线程读取结果）
//
//=============================================================================
void StartSelCharCountAsync(void* hwndMain) noexcept;

// 判断当前语言菜单设置是否应该启用字数统计（中文简体/繁体 + 系统语言为中文时）
bool IsSelCharCountLangActive(unsigned int languageMenu) noexcept;

#define SELCHARCOUNT_FORMATTED_SIZE 64

struct SelCharCountResult {
	std::atomic<unsigned int>	currentSeqNo{0};		// 当前最新请求的序号
	std::atomic<unsigned int>	completedSeqNo{0};		// 已完成的最新序号
	ptrdiff_t					selStart{0};			// 快照时的选中起始
	ptrdiff_t					selEnd{0};				// 快照时的选中结束
	wchar_t						tchFormatted[SELCHARCOUNT_FORMATTED_SIZE]; // 格式化后的状态栏文本
};

extern SelCharCountResult g_selCharCountResult;

//=============================================================================
//
// 状态栏求值（Eval）：根据键盘输入或选中文本计算表达式
//
//=============================================================================

// 求值结果缓冲区（主线程使用，无需原子操作）
extern wchar_t g_wchEvalResult[64];

//=============================================================================
//
// INI 配置变量（从 Notepad4.ini 的 [Extend Expression] 节加载）
//
//=============================================================================
extern bool		bEnableExtendExpr;		// 主功能开关
extern bool		bJITEval;				// JIT 求值开关
extern bool		bSelEval;				// 选区求值开关
extern int		nSignificantDigits;		// 输出有效数字位数（对应 G 格式的精度）
void LoadExtendExprSettings() noexcept;

// 在 SCN_UPDATEUI 中统一调用（仅一行），根据 updated 标志自动选择：
//   SC_UPDATE_CONTENT   → 若最后输入的字符是数字/')'，从光标到行首求值
//   SC_UPDATE_SELECTION → 若有选中文本，直接尝试一次 tecpp_expr
void UpdateEvalFromUI(unsigned int updated) noexcept;
