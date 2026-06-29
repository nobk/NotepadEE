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
	extern bool bEnableExtendExpr;
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

namespace {

struct SelCharCountInternal {
	std::atomic<bool>	cancel{false};

	// 以下字段仅在工作线程内写入，不对外暴露
	size_t				chinese{0};
	size_t				punctuation{0};
	size_t				nonChineseNonSpace{0};
	size_t				space{0};
};

static SelCharCountInternal s_selCharCountInt;

} // namespace

void StartSelCharCountAsync(void* hwndMain) noexcept {
	const Sci_Position iSelStart = SciCall_GetSelectionStart();
	const Sci_Position iSelEnd = SciCall_GetSelectionEnd();

	if (iSelStart == iSelEnd) {
		// 无选中：清除之前的结果
		s_selCharCountInt.cancel.store(true, std::memory_order_relaxed);
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
	char* pszSnapshot = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, iSelBytes + 1));
	if (!pszSnapshot) {
		return;
	}
	SciCall_GetSelText(pszSnapshot);

	// 2. 取消旧任务，递增序号
	s_selCharCountInt.cancel.store(true, std::memory_order_relaxed);
	const unsigned int seq = ++g_selCharCountResult.currentSeqNo;
	s_selCharCountInt.cancel.store(false, std::memory_order_relaxed);

	// 3. 启动新线程
	std::thread([seq, iSelStart, iSelEnd, iSelBytes, pszSnapshot, hwndMain]() {
		// 在后台线程中计算
		size_t chinese = 0, punctuation = 0, nonChineseNonSpace = 0, space = 0;
		CountCharacterTypes(pszSnapshot, iSelBytes,
			&chinese, &punctuation, &nonChineseNonSpace, &space,
			&s_selCharCountInt.cancel);

		// 检查是否被取消
		if (s_selCharCountInt.cancel.load(std::memory_order_relaxed)) {
			HeapFree(GetProcessHeap(), 0, pszSnapshot);
			return;
		}

		// 写入结果（工作线程独占写入）
		s_selCharCountInt.chinese = chinese;
		s_selCharCountInt.punctuation = punctuation;
		s_selCharCountInt.nonChineseNonSpace = nonChineseNonSpace;
		s_selCharCountInt.space = space;
		g_selCharCountResult.selStart = iSelStart;
		g_selCharCountResult.selEnd = iSelEnd;

		// 格式化显示文本
		wchar_t tchC[32], tchP[32], tchO[32], tchS[32];
		swprintf(tchC, 32, L"%zu", chinese);
		swprintf(tchP, 32, L"%zu", punctuation);
		swprintf(tchO, 32, L"%zu", nonChineseNonSpace);
		swprintf(tchS, 32, L"%zu", space);

		// 系统语言为中文时使用汉字标签（硬编码 Unicode 避免本地编码问题）
		if (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE) {
			swprintf(g_selCharCountResult.tchFormatted, SELCHARCOUNT_FORMATTED_SIZE,
				L"\x4E2D%s \x7B26%s \x5B57%s \x7A7A%s",
				tchC, tchP, tchO, tchS);
		} else {
			swprintf(g_selCharCountResult.tchFormatted, SELCHARCOUNT_FORMATTED_SIZE,
				L"C:%s P:%s O:%s S:%s", tchC, tchP, tchO, tchS);
		}

		// release-store: 确保所有写入在线程发出完成信号前对其他线程可见
		g_selCharCountResult.completedSeqNo.store(seq, std::memory_order_release);

		HeapFree(GetProcessHeap(), 0, pszSnapshot);

		// 通知主线程更新状态栏
		PostMessage(static_cast<HWND>(hwndMain), APPM_SELCHARCOUNT, static_cast<WPARAM>(seq), 0);
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
