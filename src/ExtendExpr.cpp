/**
* @file extendexpr.cpp
 * @author nobk @ Github
 * 用法说明: 扩展表达式处理模块实现
 * 多行扩展表达式格式：
 * ```命令行 参数...
 * 管道输入文本
 * ```=
 * 等号后按下回车执行，或者在表达式范围内任何位置按下Shift+Enter执行
 * 内部自定义特殊命令（以冒号开头）：
 *   :tts     管道输入文本进行TTS朗读
 *   :runtest 运行单元测试并输出结果
 * 其他命令行调用外部程序
 * ```python
import re
def thousands(num: str) -> str:
	# 1. 整数部分逆序后每 3 位插逗号，再逆回来
	int_part = re.sub(r'\B(?=(\d{3})+$)', r',', num.split('.')[0])
	dec_part = num.split('.')[1] if '.' in num else ''
	return int_part + ('.' + dec_part if dec_part else '')

tests = ['1234567', '-1234567.8901', '42']
for t in tests:
	print(t, '→', thousands(t))
 * ```=

 * 单行扩展表达式格式：
 * ```命令行 参数...```=
 * 执行方式同上，但不支持管道输入文本
 *
 * 数值表达式扩展：
 * 支持数学计算，通过tinyexpr++实现，支持常见数学函数和运算符，例如：
 *     5*5/5+5=10 , sqrt(cos(pi)^2)=1 , sin(30*pi/180)=0.5 , 5**5=3125 等等。
 *     详细语法见tinyexpr++ pdf文档。
 * 时间表达式计算：支持h/m/s时间格式的加减运算，如1h30m15.019s+15m=01h45m15.019s 等。
 *     或者冒号分隔符，如01:30:00-15:00.079=01:14:59.921 等。
 *     还可以混合使用，如15:00-1h30m=-01h15m00s 等。
 * 时间转换函数：toSec(时间表达式或时间表达式二元加减运算)=总秒数，如toSec(1h-1m)=3540，toSec(1h30m15.5s)=5415.5s，
 *             toTime(秒数s后缀可省略或数学计算表达式)=h/m/s时间格式，如 toTime(39.6/3+1)，toTime(5415.5s)=01h30m15.5s
 * 进制转换及偏移量函数：num(b|x|o|d,tinyexpr_or_val)，如输出16进制格式num(x, 0x1f03d - 3 * 0xff)=0x1ed40
 *     或者二进制格式num(b, 42 + 0x0a*3)=0b1001000，八进制格式num(o, 1000/8)=0o175 等
 */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include <windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <stack>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <atomic>
#include <stop_token>
#include <vector>

#include "ExtendExpr.h"
#include "tinyexpr-plusplus/tinyexpr.h"
#include "SciCall.h"
#include "Cntchr.h"
#include "resource.h"

#include "ctre/ctre.hpp"
#include "CmdProc.hpp"

#include <UniConversion.h>

#include <activscp.h>
#include <sapi.h>
#pragma comment(lib, "sapi.lib")

// 引用 Notepad4.cpp 中的 INI 文件路径
extern WCHAR szIniFile[MAX_PATH];

// [Extend Expression] INI 配置变量（默认值）
bool bEnableExtendExpr = true;
bool bJITEval = true;
bool bSelEval = true;
int  nSignificantDigits = 8;

// 这些函数，类型，模板相当于对其它.cpp隐藏了，类似C的static但用途更广，单独放一个文件避免多一层缩进
namespace {
#	include "ExtendExpr_impl.hpp"
};

static bool isTripleBacktickBefore(Sci_Position pos) noexcept {
	for (int i = 0; i < 3 && pos > 0; ++i) {
		const auto chr = SciCall_GetCharAt(pos);
		pos = SciCall_PositionBefore(pos);
		if ('`' != chr)
			return false;
	}
	return true;
}

// 统一的求值并输出函数
static bool evaluateAndOutput(Sci_Position contentStart, Sci_Position contentEnd,
					Sci_Position equalPos, Sci_Position returnPos,
					bool isCodeBlock, bool isEnterPressed) {
	if (contentStart >= contentEnd)
	return false;

	// 使用智能指针分配缓冲区
	const auto bufLen = static_cast<size_t>(contentEnd - contentStart + 1);
	auto buf = std::make_unique<char[]>(bufLen);
	if (!buf)
		return false;

	// 提取文本内容
	struct Sci_TextRangeFull tr = {{contentStart, contentEnd}, buf.get()};
	SciCall_GetTextRangeFull(&tr);

	// 创建 string_view，生命周期在 buf 有效期内
	std::string_view content(buf.get(), contentEnd - contentStart);

	// 根据类型调用不同的求值函数
	std::vector<char> result;
	result.reserve(isCodeBlock ? 4096 : 256);

	const bool success = (isCodeBlock) ?
					extendExprExtd(content, result) :
					extendExpr(content, result);

	if (success && !result.empty()) {
		result.push_back('\0');
		// 在等号后面直接插入结果
		const Sci_Position outputPos = SciCall_PositionAfter(equalPos);
		SciCall_ReplaceSel("");
		const Sci_Position selEnd = isEnterPressed ? outputPos : SciCall_PositionAfter(outputPos);
		SciCall_SetSel(outputPos, selEnd);
		SciCall_ReplaceSel(result.data());

		// 光标定位
		if (returnPos >= 0) {
			// Shift+Enter: 光标回到代码块内原先位置
			SciCall_SetSel(returnPos, returnPos);
		}
	}

	return success;
}

// 查找最后一个有效的代码块开始标记
static Sci_Position findLastBlockStart(Sci_Position cursorPos) noexcept {
	const Sci_Position docStart = SciCall_PositionFromLine(
	SciCall_LineFromPosition(0));

	const static char* startPattern = R"(```[^\s=])";
	struct Sci_TextToFindFull findStart = {{docStart, cursorPos}, startPattern, {0, 0}};

	Sci_Position blockStart = -1;
	while (SciCall_FindTextFull(SCFIND_REGEXP, &findStart) >= 0) {
		blockStart = findStart.chrgText.cpMin;
		findStart.chrg.cpMin = findStart.chrgText.cpMax;
	}

	if (blockStart < 0)
		return -1;

	// 验证光标是否在代码块内（没有遇到 ```= 结束标记）
	const Sci_Position searchFrom = blockStart + 3;
	const static char* endPattern = R"(```=)";
	struct Sci_TextToFindFull findEnd = {{searchFrom, cursorPos}, endPattern, {0, 0}};

	if (SciCall_FindTextFull(SCFIND_REGEXP, &findEnd) >= 0) {
		return -1; // 代码块已结束
	}

	return blockStart;
}

// Shift+Enter 在代码块内
static bool handleShiftEnterInBlock(Sci_Position iCurPos) {
	// 从文档开始到光标位置，找最后一个有效代码块开始标记
	const Sci_Position blockStart = findLastBlockStart(iCurPos);
	if (blockStart < 0)
		return false;

	// 向后找到配对的 ```=
	const Sci_Position searchStart = SciCall_PositionAfter(iCurPos);
	const Sci_Position docEnd = SciCall_PositionAfter(SciCall_GetLength() - 1);

	const static char* endPattern = R"(```=)";
	struct Sci_TextToFindFull findEnd = {{searchStart, docEnd}, endPattern, {0, 0}};

	if (SciCall_FindTextFull(SCFIND_REGEXP, &findEnd) < 0)
		return false;

	// 提取命令和输入内容（不包括 ``` 本身）
	Sci_Position contentStart = blockStart + 3;
	Sci_Position contentEnd =findEnd.chrgText.cpMin;

	return evaluateAndOutput(contentStart, contentEnd, findEnd.chrgText.cpMax, iCurPos, true, true);
}

// 主入口函数
bool CallExtendExpr(bool bShiftDown, bool noEnter) {
	if (!bEnableExtendExpr)
		return false;

	const Sci_Position iCurPos = SciCall_GetCurrentPos();
	const Sci_Position posSelStart = SciCall_GetSelectionStart();
	const Sci_Position posBegin = noEnter ? SciCall_PositionBefore(posSelStart) : posSelStart;

	// Shift+Enter 在代码块内
	if (bShiftDown) {
		return handleShiftEnterInBlock(iCurPos);
	}

	// 普通回车：光标必须紧邻等号后面
	const Sci_Position charBeforeCursor = SciCall_PositionBefore(posBegin);
	if (SciCall_GetCharAt(charBeforeCursor) != '=')
		return false;

	// 检查等号前面是否有 ```
	const Sci_Position posBeforeEqual = SciCall_PositionBefore(charBeforeCursor);

	if (isTripleBacktickBefore(posBeforeEqual)) {
		// 代码块格式: ```...```=
		const Sci_Position blockStart = findLastBlockStart(posBeforeEqual);
		if (blockStart < 0)
			return false;
		// 提取命令和输入内容（不包括 ``` 和 ```= 本身）
		const Sci_Position contentStart = blockStart + 3;
		const Sci_Position contentEnd = posBeforeEqual - 2;
		const Sci_Position putCurPos = noEnter ? posBegin : iCurPos;
		return evaluateAndOutput(contentStart, contentEnd, charBeforeCursor, putCurPos, true, !noEnter);
	} else {
		// 数学表达式: 1+1=
		const Sci_Position lineStart = SciCall_PositionFromLine(
		SciCall_LineFromPosition(charBeforeCursor));

		// 提取从行首到等号的内容（不包括等号）
		const Sci_Position contentStart = lineStart;
		const Sci_Position contentEnd = charBeforeCursor;

		return evaluateAndOutput(contentStart, contentEnd, charBeforeCursor, -1, false, !noEnter);
	}
}

//=============================================================================
//
// 异步选中文本字数统计
//
// 复制选中文本快照 → 取消旧任务 → 启动新线程 → 完成后 PostMessage
//
// 供 Notepad4.cpp 的 SCN_UPDATEUI / UpdateStatusbar / APPM_SELCHARCOUNT 使用
//
//=============================================================================

// 外部全局结果（ExtendExpr.h 中 extern 声明）
// 异步字数统计完成通知消息（与 Notepad4.h 中的定义保持一致）
#define APPM_SELCHARCOUNT	(WM_APP + 8)

SelCharCountResult g_selCharCountResult;

static std::stop_source s_stopSource;

// HeapAlloc/HeapFree 的 RAII 包装（使用 WIN32 API 明文实现）
struct HeapDeleter {
	void operator()(char* ptr) const noexcept {
		HeapFree(GetProcessHeap(), 0, ptr);
	}
};
using HeapBuffer = std::unique_ptr<char[], HeapDeleter>;

// 内存对齐辅助（alignment 必须为 2 的幂）
constexpr size_t AlignUp(size_t value, size_t alignment) noexcept {
	return (value + alignment - 1) & ~(alignment - 1);
}
void StartSelCharCountAsync(void* hwndMain) noexcept {
	const Sci_Position iSelStart = SciCall_GetSelectionStart();
	const Sci_Position iSelEnd = SciCall_GetSelectionEnd();

	if (iSelStart == iSelEnd) {
		// 无选中：清除之前的结果
		s_stopSource.request_stop();
		g_selCharCountResult.completedSeqNo.store(0, std::memory_order_relaxed);
		g_selCharCountResult.selStart = 0;
		g_selCharCountResult.selEnd = 0;
		g_selCharCountResult.tchFormatted[0] = L'\0';
		return;
	}

	// 矩形选择跳过
	if (SciCall_IsRectangularSelection()) {
		return;
	}

	// 1. 快照选中文本
	const Sci_Position iSelBytes = SciCall_GetSelTextLength();
	const size_t allocSize = AlignUp(static_cast<size_t>(iSelBytes) + 1, 16);
	HeapBuffer pszSnapshot(static_cast<char*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, allocSize)));
	if (!pszSnapshot) {
		return;
	}
	SciCall_GetSelText(pszSnapshot.get());

	// 2. 取消旧任务，递增序号
	s_stopSource.request_stop();
	const unsigned int seq = ++g_selCharCountResult.currentSeqNo;

	// --- 格式化和发送结果（同步/异步共用）---
	auto formatAndPost = [seq, iSelStart, iSelEnd, hwndMain](
		size_t chinese, size_t punctuation, size_t nonChineseNonSpace, size_t space) noexcept -> void
	{
		g_selCharCountResult.selStart = iSelStart;
		g_selCharCountResult.selEnd = iSelEnd;

		wchar_t tchC[32], tchP[32], tchO[32], tchS[32];
		swprintf(tchC, 32, L"%zu", chinese);
		swprintf(tchP, 32, L"%zu", punctuation);
		swprintf(tchO, 32, L"%zu", nonChineseNonSpace);
		swprintf(tchS, 32, L"%zu", space);

		if (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE) {
			swprintf(g_selCharCountResult.tchFormatted, SELCHARCOUNT_FORMATTED_SIZE,
				L"\x4E2D%s \x7B26%s \x5B57%s \x7A7A%s",
				tchC, tchP, tchO, tchS);
		} else {
			swprintf(g_selCharCountResult.tchFormatted, SELCHARCOUNT_FORMATTED_SIZE,
				L"C:%s P:%s O:%s S:%s", tchC, tchP, tchO, tchS);
		}

		g_selCharCountResult.completedSeqNo.store(seq, std::memory_order_release);
		PostMessage(static_cast<HWND>(hwndMain), APPM_SELCHARCOUNT, static_cast<WPARAM>(seq), 0);
	};

	// 3. 短串：在当前线程同步处理（避免线程开销）
	if (iSelBytes < static_cast<Sci_Position>(MIN_PARALLEL_THRESHOLD)) {
		size_t chinese = 0, punctuation = 0, nonChineseNonSpace = 0, space = 0;
		CountCharacterTypes(pszSnapshot.get(), iSelBytes,
			&chinese, &punctuation, &nonChineseNonSpace, &space);
		formatAndPost(chinese, punctuation, nonChineseNonSpace, space);
		return;
	}

	// 4. 长串：另起线程异步处理
	s_stopSource = std::stop_source{};
	std::stop_token token = s_stopSource.get_token();

	std::jthread([iSelBytes,
		pszSnapshot = std::move(pszSnapshot), token,
		formatAndPost = std::move(formatAndPost)]() mutable
	{
		size_t chinese = 0, punctuation = 0, nonChineseNonSpace = 0, space = 0;
		CountCharacterTypes(pszSnapshot.get(), iSelBytes,
			&chinese, &punctuation, &nonChineseNonSpace, &space,
			token);

		if (token.stop_requested()) {
			return;
		}

		formatAndPost(chinese, punctuation, nonChineseNonSpace, space);
	}).detach();
}

//=============================================================================
//
// IsSelCharCountLangActive() - 判断是否应启用字数统计
//
// 当语言菜单选中中文(简/繁) 或 语言菜单为"系统语言"且系统UI是中文时返回 true
//
//=============================================================================
bool IsSelCharCountLangActive(unsigned int languageMenu) noexcept {
	if (languageMenu == IDM_LANG_CHINESE_SIMPLIFIED || languageMenu == IDM_LANG_CHINESE_TRADITIONAL) {
		return true;
	}
	if (languageMenu == IDM_LANG_USER_DEFAULT) {
		const LANGID sysLang = GetUserDefaultUILanguage();
		return (PRIMARYLANGID(sysLang) == LANG_CHINESE);
	}
	return false;
}

//=============================================================================
//
// JS 表达式求值（复用 Bridge.cpp 的 Chakra 引擎模式，缓存引擎实例避免重复创建）
//
//=============================================================================

// https://github.com/chakra-core/ChakraCore/blob/master/lib/Common/Core/AtomLockGuids.h
static const GUID CLSID_Chakra = // {1b7cd997-e5ff-4932-a7a6-2a9e636da385}
{ 0x1b7cd997, 0xe5ff, 0x4932, { 0xa7, 0xa6, 0x2a, 0x9e, 0x63, 0x6d, 0xa3, 0x85 } };
#if defined(__MINGW32__)
extern "C" const GUID __declspec(selectany) IID_IActiveScriptParse32 = // {BB1A2AE2-A4F9-11cf-8F20-00805F2CD064}
{ 0xbb1a2ae2, 0xa4f9, 0x11cf, { 0x8f, 0x20, 0x00, 0x80, 0x5f, 0x2c, 0xd0, 0x64 }};
extern "C" const GUID __declspec(selectany) IID_IActiveScriptParse64 = // {C7EF7658-E1EE-480E-97EA-D52CB4D76D17}
{ 0xc7ef7658, 0xe1ee, 0x480e, { 0x97, 0xea, 0xd5, 0x2c, 0xb4, 0xd7, 0x6d, 0x17 }};
#endif

extern HMODULE hPropSysDLL;

// Forward declarations from Helpers.h to avoid include dependency issues with LLVM/clang
template<typename T>
inline T DLLFunction(HMODULE hModule, LPCSTR lpProcName) noexcept {
	FARPROC function = ::GetProcAddress(hModule, lpProcName);
	return __builtin_bit_cast(T, function);
}

template <class T>
inline void** AsPPVArgs(T** pp) noexcept {
	static_assert(__is_base_of(IUnknown, T));
	return reinterpret_cast<void **>(pp);
}

#if _WIN32_WINNT < _WIN32_WINNT_WIN8
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32	0x00000800
#endif
extern DWORD kSystemLibraryLoadFlags;
#else
#define kSystemLibraryLoadFlags		LOAD_LIBRARY_SEARCH_SYSTEM32
#endif

namespace {

struct EvalJSContext final : IActiveScriptSite {
	std::string errorMsg;

	STDMETHODIMP QueryInterface(REFIID riid, PVOID *ppv) noexcept override {
		if (riid == IID_IUnknown || riid == IID_IActiveScriptSite) {
			*ppv = this;
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() noexcept override { return 1; }
	STDMETHODIMP_(ULONG) Release() noexcept override { return 1; }
	STDMETHODIMP GetLCID(LCID *lcid) noexcept override { *lcid = LOCALE_USER_DEFAULT; return S_OK; }
	STDMETHODIMP GetDocVersionString(BSTR *ver) noexcept override { *ver = nullptr; return S_OK; }
	STDMETHODIMP OnScriptTerminate(const VARIANT *, const EXCEPINFO *) noexcept override { return S_OK; }
	STDMETHODIMP OnStateChange(SCRIPTSTATE) noexcept override { return S_OK; }
	STDMETHODIMP OnEnterScript() noexcept override { return S_OK; }
	STDMETHODIMP OnLeaveScript() noexcept override { return S_OK; }
	STDMETHODIMP GetItemInfo(const WCHAR *, DWORD, IUnknown **, ITypeInfo **) noexcept override { return S_OK; }
	STDMETHODIMP OnScriptError(IActiveScriptError *scriptError) override {
		EXCEPINFO excepInfo{};
		if (SUCCEEDED(scriptError->GetExceptionInfo(&excepInfo)) && excepInfo.bstrDescription) {
			const int len = WideCharToMultiByte(CP_UTF8, 0, excepInfo.bstrDescription, -1, nullptr, 0, nullptr, nullptr);
			if (len > 0) {
				errorMsg.resize(static_cast<size_t>(len));
				WideCharToMultiByte(CP_UTF8, 0, excepInfo.bstrDescription, -1, errorMsg.data(), len, nullptr, nullptr);
			}
		}
		return S_OK;
	}
};

struct JSEngine {
	IActiveScript* activeScript = nullptr;
	IActiveScriptParse* scriptParse = nullptr;
	EvalJSContext context;
	bool initialized = false;

	bool init() {
		HRESULT hr = CoCreateInstance(CLSID_Chakra, nullptr, CLSCTX_INPROC_SERVER, IID_IActiveScript, AsPPVArgs(&activeScript));
		if (!SUCCEEDED(hr)) {
			CLSID clsidScript;
			hr = CLSIDFromProgID(L"JavaScript", &clsidScript);
			if (SUCCEEDED(hr)) {
				hr = CoCreateInstance(clsidScript, nullptr, CLSCTX_INPROC_SERVER, IID_IActiveScript, AsPPVArgs(&activeScript));
			}
			if (!SUCCEEDED(hr))
				return false;
		}
		hr = activeScript->SetScriptSite(&context);
		if (!SUCCEEDED(hr)) {
			activeScript->Release();
			activeScript = nullptr;
			return false;
		}
		hr = activeScript->QueryInterface(IID_IActiveScriptParse, AsPPVArgs(&scriptParse));
		if (!SUCCEEDED(hr)) {
			activeScript->Release();
			activeScript = nullptr;
			return false;
		}
		hr = scriptParse->InitNew();
		if (!SUCCEEDED(hr)) {
			scriptParse->Release();
			scriptParse = nullptr;
			activeScript->Release();
			activeScript = nullptr;
			return false;
		}
		initialized = true;
		return true;
	}

	bool eval(const char* expr, int codepage, std::string& result) {
		const int exprLen = static_cast<int>(strlen(expr));
		const int wideLen = MultiByteToWideChar(codepage, 0, expr, exprLen + 1, nullptr, 0);
		std::wstring exprW;
		exprW.resize(static_cast<size_t>(wideLen) - 1);
		MultiByteToWideChar(codepage, 0, expr, exprLen, &exprW[0], static_cast<int>(exprW.size()));
		std::wstring wrapped = L"with(Math){\n" + exprW + L"\n}";

		VARIANT resultVar;
		VariantInit(&resultVar);
		HRESULT hr = scriptParse->ParseScriptText(wrapped.c_str(), nullptr, nullptr, nullptr, 0, 0, SCRIPTTEXT_ISEXPRESSION, &resultVar, nullptr);
		if (SUCCEEDED(hr)) {
			if (resultVar.vt == VT_DISPATCH) {
				IDispatch* dispatch = resultVar.pdispVal;
				LPWSTR toString = const_cast<LPWSTR>(L"toString");
				DISPID dispId;
				hr = dispatch->GetIDsOfNames(IID_NULL, &toString, 1, LOCALE_USER_DEFAULT, &dispId);
				if (SUCCEEDED(hr)) {
					DISPPARAMS params{};
					hr = dispatch->Invoke(dispId, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &params, &resultVar, nullptr, nullptr);
				}
			}
			using VariantToStringSig = HRESULT(WINAPI*)(REFVARIANT varIn, PWSTR pszBuf, UINT cchBuf) noexcept;
			static VariantToStringSig pfnVariantToString = nullptr;
			static bool propsysLoaded = false;
			if (!propsysLoaded) {
				propsysLoaded = true;
				HMODULE hDLL = LoadLibraryExW(L"propsys.dll", nullptr, kSystemLibraryLoadFlags);
				pfnVariantToString = DLLFunction<VariantToStringSig>(hDLL, "VariantToString");
				if (pfnVariantToString) {
					hPropSysDLL = hDLL;
				} else {
					FreeLibrary(hDLL);
					VariantClear(&resultVar);
					return false;
				}
			}
			WCHAR resultW[4096];
			resultW[0] = L'\0';
			hr = pfnVariantToString(resultVar, resultW, 4096);
			if (SUCCEEDED(hr) && resultW[0]) {
				const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, resultW, -1, nullptr, 0, nullptr, nullptr);
				if (utf8Len > 0) {
					result.resize(static_cast<size_t>(utf8Len) - 1);
					WideCharToMultiByte(CP_UTF8, 0, resultW, -1, result.data(), utf8Len, nullptr, nullptr);
				}
			}
			VariantClear(&resultVar);
		}
		return !result.empty();
	}

	void close() {
		if (scriptParse) {
			scriptParse->Release();
			scriptParse = nullptr;
		}
		if (activeScript) {
			activeScript->Close();
			activeScript->Release();
			activeScript = nullptr;
		}
		initialized = false;
	}

	~JSEngine() { close(); }
};

}

bool EvaluateJSExpression(const char* expr, int codepage, std::string& result) {
	using VariantToStringSig = HRESULT (WINAPI *)(REFVARIANT varIn, PWSTR pszBuf, UINT cchBuf) noexcept;
	static VariantToStringSig pfnVariantToString = nullptr;
	static bool propsysLoaded = false;
	if (!propsysLoaded) {
		propsysLoaded = true;
		HMODULE hDLL = LoadLibraryExW(L"propsys.dll", nullptr, kSystemLibraryLoadFlags);
		pfnVariantToString = DLLFunction<VariantToStringSig>(hDLL, "VariantToString");
		if (pfnVariantToString == nullptr) {
			FreeLibrary(hDLL);
			return false;
		}
		hPropSysDLL = hDLL;
	}

	IActiveScript *activeScript = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_Chakra, nullptr, CLSCTX_INPROC_SERVER, IID_IActiveScript, AsPPVArgs(&activeScript));
	if (!SUCCEEDED(hr)) {
		CLSID clsidScript;
		hr = CLSIDFromProgID(L"JavaScript", &clsidScript);
		if (SUCCEEDED(hr)) {
			hr = CoCreateInstance(clsidScript, nullptr, CLSCTX_INPROC_SERVER, IID_IActiveScript, AsPPVArgs(&activeScript));
		}
		if (!SUCCEEDED(hr)) {
			return false;
		}
	}

	EvalJSContext context;
	hr = activeScript->SetScriptSite(&context);
	if (SUCCEEDED(hr)) {
		IActiveScriptParse* scriptParse = nullptr;
		hr = activeScript->QueryInterface(IID_IActiveScriptParse, AsPPVArgs(&scriptParse));
		if (SUCCEEDED(hr)) {
			hr = scriptParse->InitNew();
			if (SUCCEEDED(hr)) {
				const int exprLen = static_cast<int>(strlen(expr));
				const int wideLen = MultiByteToWideChar(codepage, 0, expr, exprLen + 1, nullptr, 0);
				std::wstring exprW;
				exprW.resize(static_cast<size_t>(wideLen) - 1);
				MultiByteToWideChar(codepage, 0, expr, exprLen, &exprW[0], static_cast<int>(exprW.size()));
				std::wstring wrapped = L"with(Math){\n" + exprW + L"\n}";

				VARIANT resultVar;
				VariantInit(&resultVar);
				hr = scriptParse->ParseScriptText(wrapped.c_str(), nullptr, nullptr, nullptr, 0, 0, SCRIPTTEXT_ISEXPRESSION, &resultVar, nullptr);
				if (SUCCEEDED(hr)) {
					if (resultVar.vt == VT_DISPATCH) {
						IDispatch * const dispatch = resultVar.pdispVal;
						LPWSTR toString = const_cast<LPWSTR>(L"toString");
						DISPID dispId;
						hr = dispatch->GetIDsOfNames(IID_NULL, &toString, 1, LOCALE_USER_DEFAULT, &dispId);
						if (SUCCEEDED(hr)) {
							DISPPARAMS params{};
							hr = dispatch->Invoke(dispId, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &params, &resultVar, nullptr, nullptr);
						}
					}

					WCHAR resultW[4096];
					resultW[0] = L'\0';
					hr = pfnVariantToString(resultVar, resultW, 4096);
					if (SUCCEEDED(hr) && resultW[0]) {
						const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, resultW, -1, nullptr, 0, nullptr, nullptr);
						if (utf8Len > 0) {
							result.resize(static_cast<size_t>(utf8Len) - 1);
							WideCharToMultiByte(CP_UTF8, 0, resultW, -1, result.data(), utf8Len, nullptr, nullptr);
						}
					}
					VariantClear(&resultVar);
				}
			}
			scriptParse->Release();
		}
	}

	activeScript->Close();
	activeScript->Release();
	return !result.empty();
}

bool EvaluateJSExpressionCached(const char* expr, int codepage, std::string& result) {
	thread_local JSEngine engine;
	if (!engine.initialized && !engine.init())
		return false;
	return engine.eval(expr, codepage, result);
}

//=============================================================================
//
// 即时求值（JIT-Eval）实现
//
//=============================================================================

// 全局求值结果缓冲区
wchar_t g_wchEvalResult[64] = L"";

// 从光标位置到行首提取表达式并求值（键盘输入触发）
// 使用 teCpp（含 find_left_bound + reduce_left_bound 重试逻辑）
static bool EvaluateExprAtCursor() noexcept {
	const Sci_Position iCurPos = SciCall_GetCurrentPos();
	const Sci_Position iLineStart = SciCall_PositionFromLine(
		SciCall_LineFromPosition(iCurPos));

	if (iCurPos <= iLineStart) {
		g_wchEvalResult[0] = L'\0';
		return false;
	}

	// 提取从行首到光标位置的文本
	const Sci_Position bufLen = iCurPos - iLineStart + 1;
	const size_t allocSize = AlignUp(static_cast<size_t>(bufLen), 16);
	HeapBuffer buf(static_cast<char*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, allocSize)));
	if (!buf) {
		g_wchEvalResult[0] = L'\0';
		return false;
	}

	struct Sci_TextRangeFull tr = {{iLineStart, iCurPos}, buf.get()};
	SciCall_GetTextRangeFull(&tr);

	// 去除左侧无效字符（返回需跳过的字节偏移量，避免 std::string 复制）
	const size_t len = static_cast<size_t>(iCurPos - iLineStart);
	const size_t offset = RemoveUnnecessaryLeadingCharacters(buf.get(), len);
	if (offset >= len) {
		g_wchEvalResult[0] = L'\0';
		return false;
	}
	const size_t remaining = len - offset;

	// 使用 teCpp 求值（含左边界缩减重试）
	std::vector<char> result;
	result.reserve(64);
	if (!teCpp(std::string_view(buf.get() + offset, remaining), result) || result.empty()) {
		g_wchEvalResult[0] = L'\0';
		return false;
	}

	// 结果追加 '\0' 以转成字符串
	result.push_back('\0');

	// UTF-8 转宽字符
	const int wideLen = MultiByteToWideChar(CP_UTF8, 0, result.data(), -1, nullptr, 0);
	if (wideLen <= 0 || static_cast<size_t>(wideLen) > COUNTOF(g_wchEvalResult)) {
		g_wchEvalResult[0] = L'\0';
		return false;
	}
	MultiByteToWideChar(CP_UTF8, 0, result.data(), -1, g_wchEvalResult, wideLen);
	return true;
}

// 对选中文本直接求值（选中区域触发，不缩减左边界）
// 直接调用 tecpp_expr，仅尝试一次
static bool EvaluateSelectionExpr() noexcept {
	const Sci_Position iSelStart = SciCall_GetSelectionStart();
	const Sci_Position iSelEnd = SciCall_GetSelectionEnd();

	if (iSelStart == iSelEnd) {
		g_wchEvalResult[0] = L'\0';
		return false;
	}

	// 矩形选择跳过
	if (SciCall_IsRectangularSelection()) {
		g_wchEvalResult[0] = L'\0';
		return false;
	}

	// 提取选中文本（复用 StartSelCharCountAsync 的内存分配方式，避免 release 下异常）
	const Sci_Position iSelBytes = SciCall_GetSelTextLength();
	const size_t allocSize = AlignUp(static_cast<size_t>(iSelBytes) + 1, 16);
	HeapBuffer buf(static_cast<char*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, allocSize)));
	if (!buf) {
		g_wchEvalResult[0] = L'\0';
		return false;
	}
	SciCall_GetSelText(buf.get());

	// 剔除首尾 Unicode 空白（使用 Cntchr.cpp 的公共函数，支持 Unicode 空白全集）
	size_t length = static_cast<size_t>(iSelBytes);
	const auto [skip, remain] = TrimWhitespace(buf.get(), length);
	if (remain == 0) {
		g_wchEvalResult[0] = L'\0';
		return false;
	}

	// 直接调 tecpp_expr（仅一次，不缩减左边界）
	const std::string_view sv(buf.get() + skip, remain);
	const double rv = tecpp_expr(sv);
	if (std::isnan(rv)) {
		g_wchEvalResult[0] = L'\0';
		return false;
	}

	// 格式化数值
	std::string s = formatDoubleResult(rv);

	// 防御：过滤掉 "nan"/"-nan"/"inf"/"-inf" 等非数值（某些环境可能漏检 isnan）
	if (isNanOrInfString(s)) {
		g_wchEvalResult[0] = L'\0';
		return false;
	}

	// UTF-8 转宽字符
	const int wideLen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	if (wideLen <= 0 || static_cast<size_t>(wideLen) > COUNTOF(g_wchEvalResult)) {
		g_wchEvalResult[0] = L'\0';
		return false;
	}
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, g_wchEvalResult, wideLen);
	return true;
}

//=============================================================================
//
// 从 Notepad4.ini 加载 [Extend Expression] 节配置
// 若该节或其中键值不存在，则写入默认值
//
//=============================================================================
void LoadExtendExprSettings() noexcept {
	// 检查 [Extend Expression] 节是否存在
	WCHAR szCheck[2] = {0};
	GetPrivateProfileStringW(L"Extend Expression", L"Enable", L"", szCheck, 2, szIniFile);
	if (szCheck[0] == L'\0') {
		// 节或键不存在 → 写入默认值
		WritePrivateProfileStringW(L"Extend Expression", L"Enable", L"1", szIniFile);
		WritePrivateProfileStringW(L"Extend Expression", L"JITEval", L"1", szIniFile);
		WritePrivateProfileStringW(L"Extend Expression", L"SelEval", L"1", szIniFile);
		WritePrivateProfileStringW(L"Extend Expression", L"SignificantDigits", L"8", szIniFile);
	}

	bEnableExtendExpr = (GetPrivateProfileIntW(L"Extend Expression", L"Enable", 1, szIniFile) != 0);
	bJITEval         = (GetPrivateProfileIntW(L"Extend Expression", L"JITEval", 1, szIniFile) != 0);
	bSelEval         = (GetPrivateProfileIntW(L"Extend Expression", L"SelEval", 1, szIniFile) != 0);
	nSignificantDigits   = GetPrivateProfileIntW(L"Extend Expression", L"SignificantDigits", 8, szIniFile);
	if (nSignificantDigits < 1)  nSignificantDigits = 1;
	if (nSignificantDigits > 15) nSignificantDigits = 15;  // double 最多 15 位有效数字
}

// 统一入口，供 Notepad4.cpp 的 SCN_UPDATEUI 一行调用
void UpdateEvalFromUI(unsigned int updated) noexcept {
	// 选中区域求值（优先级高于键盘求值）
	if (updated & SC_UPDATE_SELECTION) {
		const Sci_Position iSelStart = SciCall_GetSelectionStart();
		const Sci_Position iSelEnd = SciCall_GetSelectionEnd();
		if (iSelStart != iSelEnd) {
			if (bEnableExtendExpr && bSelEval && EvaluateSelectionExpr())
				return;
			// 选中区域求值失败或未启用 → 清除上次结果
			g_wchEvalResult[0] = L'\0';
			return;
		}
	}

	// 键盘输入求值（仅当 bEnableExtendExpr 和 bJITEval 均启用时）
	if ((updated & (SC_UPDATE_CONTENT | SC_UPDATE_SELECTION)) && bEnableExtendExpr && bJITEval) {
		const Sci_Position iPos = SciCall_GetCurrentPos();
		if (iPos > 0) {
			const int ch = SciCall_GetCharAt(iPos - 1);
			if ((ch >= '0' && ch <= '9') || ch == ')') {
				EvaluateExprAtCursor();
				// 不成功时 EvaluateExprAtCursor 已清空 g_wchEvalResult
			}
		}
	}
}
