#pragma once
#include <string>
#include <atomic>
#include <cstddef>

bool CallExtendExpr(bool bShiftDown, bool noEnter);
std::string& RemoveUnnecessaryLeadingCharacters(std::string& str) noexcept;
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
