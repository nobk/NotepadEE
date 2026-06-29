// ExtendExpr_impl.hpp
#pragma once
// Inside anonymous namespace {
class Spk {
public:
	Spk() {
		HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY);
		m_comInit = SUCCEEDED(hr);
	};
	~Spk() {
		if (m_pVoice) {
			m_pVoice->Release();
			m_pVoice = nullptr;
		}
		if (m_hMod) {
			FreeLibrary(m_hMod);
		}
		if (m_comInit) {
			CoUninitialize();
		}
	};
private:
	bool m_comInit = false;
	HMODULE m_hMod = nullptr;
	ISpVoice* m_pVoice = nullptr;
public:
	static Spk& Instance()
	{
		thread_local static Spk s_tme;
		return s_tme;
	}

	bool tts(std::string_view sv)
	{
		if (sv.size() < 2)
			return false;
		int wlen = ::MultiByteToWideChar(CP_UTF8, 0, sv.data(), -1, nullptr, 0);
		if (wlen < 2)
			return false;
		std::wstring ws;
		ws.resize((size_t)wlen - 1); // not include tail \0
		::MultiByteToWideChar(CP_UTF8, 0, sv.data(), -1, ws.data(), wlen);
		if (m_pVoice == nullptr)
			if (FAILED(::CoCreateInstance(CLSID_SpVoice, NULL, CLSCTX_ALL, IID_ISpVoice, (void**)&m_pVoice)))
				return false;
		SPVOICESTATUS pSt{};
		if (FAILED(m_pVoice->GetStatus(&pSt, nullptr)))
			return false;
		if (pSt.dwRunningState == SPRS_IS_SPEAKING) {
			m_pVoice->Speak(nullptr, SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
		}
		else {
			m_pVoice->Speak(ws.c_str(), SPF_ASYNC, nullptr);
		}
		return true;
	}
};

double tecpp_expr(std::string_view str) {
	thread_local static te_parser s_parser;
	return s_parser.evaluate(str);
}

inline bool isCompoundOperatorPrefix(char c) noexcept {
	switch(c) {
		case '=': case '>': case '<': case '+': case '-':
		case '*': case '/': case '%': case '&': case '|':
		case '^': case '!': return true;
		default: return false;
	}
}

std::string_view find_left_bound(std::string_view s) {
	const auto len = static_cast<ptrdiff_t>(s.length());
	for (ptrdiff_t i = len - 1; i >= 0; --i) {
		const auto idx = static_cast<std::string_view::size_type>(i);
		if (s[idx] == '=') {
			if (i <= 0 || !isCompoundOperatorPrefix(s[idx - 1])) {
				return s.substr(idx + 1);
			} else {
				--i;  // 跳过复合操作符前缀字符
			}
		}
	}
	return s;
}

std::string_view reduce_left_bound(std::string_view sv) {
	constexpr std::array s_teSymbols = {
		'+','-','*','/',':','^','%','!','<','>','(',')',
		',',' ','\t','m','h','s',
		'0','1','2','3','4','5','6','7','8','9'
	};
	constexpr std::array s_teLookfwdSymbols = {'|','&'};
	if (sv.size() <= 1) {
		if (sv.size() == 1)
			sv = sv.substr(1);
		return sv;
	}
	sv = sv.substr(1);
	for (size_t i = 0; i < sv.size() - 1; ++i) {
		if (std::ranges::find(s_teSymbols, sv.at(i)) != s_teSymbols.end() ||
			(std::ranges::find(s_teLookfwdSymbols, sv.at(i)) != s_teLookfwdSymbols.end() &&
				std::ranges::find(s_teLookfwdSymbols, sv.at(i + 1)) != s_teLookfwdSymbols.end()))
			return sv.substr(i);
	}
	return sv;
}

bool teCpp(std::string_view tmf, std::vector<char>& res) {
	std::string_view sv = find_left_bound(tmf);
	double rv = 0.0;
	bool find = false;
	while (sv.size() >= 1) {
		rv = tecpp_expr(sv);
		if (!std::isnan(rv)) {
			find = true;
			break;
		}
		sv = reduce_left_bound(sv);
	}
	if (!find)
		return false;
	double       idbl = 0.0;
	double const fracpart = fabs(modf(rv, &idbl));
	double const intpart = fabs(idbl);
	std::string s;
	if ((fracpart < 1.0E-8) && (intpart < 1.0E+21)) {
		if(idbl < 0.0)
			s = std::format("-{:.21G}", intpart);
		else
			s = std::format("{:.21G}", intpart);
	}
	else {
		s = std::format("{:.8G}", rv);
	}
	res.assign(s.begin(), s.end());
	return true;
}

inline std::string msTrimTailZero(int64_t ms)
{
	// 只去掉末尾的0，保持正确的小数位数
	// 例如 70ms 应该显示为 .07，而不是 .7
	std::string ms_str;
	if (ms > 0) {
		ms_str = std::format("{:03d}", ms);
		// 从后向前去掉尾0
		while (ms_str.length() > 1 && ms_str.back() == '0') {
			ms_str.pop_back();
		}
	}
	else {
		ms_str = "";
	}
	return ms_str;
}

// 将正则匹配的毫秒数字符串（如 ".444"、".4"、""）转换为毫秒整数（0-999），
// 保留第4位做四舍五入而非直接截断。
int64_t fractionToMs(std::string_view frac)
{
	if (frac.length() <= 1)
		return 0;
	std::string digits(frac.substr(1)); // 去掉开头的 '.'
	// 保留4位小数：前3位作为毫秒值，第4位用于四舍五入
	while (digits.length() < 4)
		digits += '0';
	int64_t ms = std::stoll(digits.substr(0, 3));
	if (digits[3] >= '5')
		++ms;
	return ms;
}

struct TimeComponents {
	int64_t hours = 0;
	int64_t minutes = 0;
	int64_t seconds = 0;
	int64_t milliseconds = 0;
	bool isColonFormat = false;
};

// 获取匹配组的字符串内容
template <size_t base, int off>
inline std::string grpStr(const auto& mtf) {
	constexpr auto idx = static_cast<size_t>(static_cast<ptrdiff_t>(base) + off);
	return mtf.template get<idx>().str();
}

// 从 regex 的匹配组解析一个时间分量（h/m/s/ms）
// 正则子模式：((\d+)[h:])?((\d+)[m:])?((\d+(\.\d+)?)s?)?
// base 是 h_part 的组号（tm1=2, tm2=11）
// 组偏移: base-1=完整时间串, base+0=h_part, base+1=h_val, base+2=m_part,
//         base+3=m_val, base+4=s_part, base+5=s_val_frac, base+6=frac
template <size_t base>
TimeComponents parseTimeComponent(const auto& mtf)
{
	TimeComponents tc;
	tc.isColonFormat = (grpStr<base, -1>(mtf).find(':') != std::string::npos);
	if (tc.isColonFormat) {
		// 01:02:03.4 格式
		tc.seconds = std::stoi(grpStr<base, 5>(mtf));
		tc.milliseconds = fractionToMs(grpStr<base, 6>(mtf));
		if (grpStr<base, 3>(mtf).length()) {
			tc.hours = std::stoi(grpStr<base, 1>(mtf));
			tc.minutes = std::stoi(grpStr<base, 3>(mtf));
		} else if (grpStr<base, 1>(mtf).length()) {
			tc.minutes = std::stoi(grpStr<base, 1>(mtf));
		}
	} else {
		// 01h02m03.4s 格式
		if (grpStr<base, 1>(mtf).length())
			tc.hours = std::stoi(grpStr<base, 1>(mtf));
		if (grpStr<base, 3>(mtf).length())
			tc.minutes = std::stoi(grpStr<base, 3>(mtf));
		if (grpStr<base, 5>(mtf).length())
			tc.seconds = std::stoi(grpStr<base, 5>(mtf));
		tc.milliseconds = fractionToMs(grpStr<base, 6>(mtf));
	}
	return tc;
}

// 将 duration 格式化为时间字符串
// useColon=true:  输出 "01:02:03.4" 格式（无后缀）
// useColon=false: 输出 "01h02m03.4s" 格式
std::string formatDuration(std::chrono::milliseconds dur, bool useColon, bool neg = false)
{
	const auto hms = std::chrono::hh_mm_ss(dur);
	const auto hours = hms.hours().count();
	const auto minutes = hms.minutes().count();
	const auto seconds = hms.seconds().count();
	const auto ms = hms.subseconds().count();

	std::string out;
	if (neg)
		out += '-';
	if (hours)
		out += std::format("{:02}{}", hours, (useColon ? ":" : "h"));
	out += std::format("{:02}{}{:02}", minutes, (useColon ? ":" : "m"), seconds);
	if (ms)
		out += "." + msTrimTailZero(ms);
	if (!useColon)
		out += "s";
	return out;
}

bool calcTime(std::string_view tmf, std::vector<char>& res) {
	// fast path: must contain at least one of h/m/s/: character
	if (tmf.find_first_of("hms:") == std::string::npos)
		return false;
auto mtf = ctre::match<R"(^(((\d+)[h:])?((\d+)[m:])?((\d+(\.\d+)?)s?)?)([+\-])(((\d+)[h:])?((\d+)[m:])?((\d+(\.\d+)?)s?)?)$)", ctre::case_insensitive>(tmf);
	if (!mtf)
		return false;
	const bool tmadd = bool(mtf.get<9>().to_view() == "+");
	const auto tm1 = mtf.get<1>();
	const auto tm2 = mtf.get<10>();
#if (defined(DEBUG) || defined(DEBUG)) && !defined(NDEBUG)
	for (int i = 0; i < 18; ++i) {
		dbgPrint("[{}] {}\n", i, mtf.str(i));
	}
#endif

	// [hms] and colon cannot be mixed in the same time string, "10:32m11" or "2h33:14" invalid.
	// fast string scan to replace costly regex negative lookahead
	const auto has_mixed_format = [](std::string_view sv) noexcept -> bool {
		const bool hms = (sv.find_first_of("hms") != std::string_view::npos);
		const bool col = (sv.find(':') != std::string_view::npos);
		return hms && col;
	};
	if (has_mixed_format(tm1) || has_mixed_format(tm2))
		return false;

	auto tc1 = parseTimeComponent<2>(mtf);
	auto tc2 = parseTimeComponent<11>(mtf);

	if (tc1.minutes > 59 || tc2.minutes > 59 ||
		tc1.seconds > 59 || tc2.seconds > 59)
		return false;

	using namespace std::chrono_literals;
	const auto dur1 = std::chrono::hours(tc1.hours) + std::chrono::minutes(tc1.minutes) +
						std::chrono::seconds(tc1.seconds) + std::chrono::milliseconds(tc1.milliseconds);
	const auto dur2 = std::chrono::hours(tc2.hours) + std::chrono::minutes(tc2.minutes) +
						std::chrono::seconds(tc2.seconds) + std::chrono::milliseconds(tc2.milliseconds);

	std::chrono::milliseconds result{ 0 };
	bool neg = false;
	if (tmadd) {
		result = dur1 + dur2;
	} else {
		if (dur1 > dur2) {
			result = dur1 - dur2;
		} else {
			result = dur2 - dur1;
			neg = true;
		}
	}

	auto out = formatDuration(result, tc2.isColonFormat, neg);
	res.assign(out.begin(), out.end());
	return true;
}

bool toSeconds(std::string_view tmf, std::vector<char>& res) {
	// Extract inner expression between toSec( and the closing )
	auto const open_pos = tmf.find('(');
	auto const close_pos = tmf.rfind(')');
	if (open_pos == std::string::npos || close_pos == std::string::npos ||
		close_pos <= open_pos)
		return false;

	auto const inner_len = close_pos - open_pos - 1;
	std::string_view inner(tmf.substr(open_pos + 1, inner_len));

	// Try to evaluate the inner expression as a time arithmetic via calcTime.
	// If it succeeds, use calcTime's output as the resolved time string;
	// otherwise use the original inner expression as-is.
	std::string timeStr{ inner };
	if (calcTime(inner, res)) {
		timeStr.assign(res.begin(), res.end());
	}

	// Parse the time string into components and convert to total seconds.
auto mtf = ctre::match<R"(^\s*(((\d+)[h:])?((\d+)[m:])?((\d+(\.\d+)?)s?)?)\s*$)", ctre::case_insensitive>(timeStr);
	if (!mtf)
		return false;
	auto tc = parseTimeComponent<2>(mtf);
	int64_t total_seconds = tc.hours * 3600 + tc.minutes * 60 + tc.seconds;
	int64_t ms = tc.milliseconds;
	if (ms >= 1000) {
		total_seconds += ms / 1000;
		ms = ms % 1000;
	}
	auto msStr = msTrimTailZero(ms);
	if (!msStr.empty())
		msStr = "." + msStr;
	timeStr = std::format("{}{}", total_seconds, msStr);
	res.assign(timeStr.begin(), timeStr.end());
	return true;
}

bool toTime(std::string_view tmf, std::vector<char>& res) {
	// Extract inner expression between toTime( and the closing )
	auto const open_pos = tmf.find('(');
	auto const close_pos = tmf.rfind(')');
	if (open_pos == std::string::npos || close_pos == std::string::npos ||
		close_pos <= open_pos)
		return false;

	auto const inner_len = close_pos - open_pos - 1;
	std::string_view expr{ tmf.substr(open_pos + 1, inner_len) };

	double seconds = 0.0;
	bool parsed = false;

	// Check for explicit 's'/'S' seconds suffix with a valid numeric prefix,
	// e.g. "5415.5s", "30S", "-5.0s"
auto mtf = ctre::match<R"(^(-?\d+(\.\d+)?)s$)" > (expr);
	if (mtf) {
		seconds = std::stod(mtf.get<1>().str());
		parsed = true;
	}
	// Otherwise, evaluate via teCpp which supports math expressions.
	if (!parsed) {
		if (!teCpp(expr, res))
			return false;
		seconds = std::strtod(res.data(), nullptr);
	}

	// Convert seconds to milliseconds (rounding to nearest millisecond)
	auto total_ms = std::chrono::milliseconds(
		static_cast<int64_t>(std::llround(seconds * 1000.0)));

	auto result = formatDuration(total_ms, false);
	res.assign(result.begin(), result.end());
	return true;
}

inline int is_valid_num_function(std::string_view svNum)
{
	std::stack<char> brackets;
	bool             firstBr = true;
	for (int i = static_cast<int>(svNum.size()) - 1; i >= 3; --i) {
		char c = svNum[i];
		if (c == ')' || c == ']' || c == '}') {
			if (firstBr) {
				if (c != ')')
					return -1;
				firstBr = false;
			}
			char p = (c == ')') ? '(' : (c == ']') ? '[' : '{';
			brackets.push(p);
		}
		else if (c == '(' || c == '[' || c == '{') {
			if (brackets.empty() || brackets.top() != c) {
				return -1; // Mismatched brackets
			}
			brackets.pop();
		}
		if (brackets.empty()) {
			std::string checknum(svNum.substr(i - 3, 3));
			std::ranges::transform(checknum.begin(), checknum.end(), checknum.begin(),
				[](char n) { return static_cast<char>(std::tolower(n)); });
			if (checknum == "num")
				return i - 3;
			break;
		}
	}
	return -1;
}

/* num(b|x|o|d,tinyexpr_or_val)*/
bool strNum(std::string_view tmf, std::vector<char>& res) {
	const auto numLoc = is_valid_num_function(tmf);
	if (numLoc < 0 || static_cast<std::size_t>(numLoc) > tmf.size())
		return false;
	std::string substr{ tmf.substr(static_cast<std::size_t>(numLoc)) };
auto match = ctre::match<R"(num\s*\(\s*(\S+)\s*,\s*(.*)\)$)" > (substr);
	if (!match)
		return false;
	if (!teCpp(match.get<2>().to_view(), res))
		return false;
	char t = match.get<1>().to_view()[0];
	double dExprEval = std::strtod(res.data(), nullptr);
	res.resize(0);

	bool findNumType = false;
	std::string f;
	switch (std::tolower(t)) {
	case 'b':
		f = "0b" + std::format("{:b}", static_cast<int64_t>(dExprEval));
		findNumType = true;
		break;
	case 'o':
		f = "0o" + std::format("{:o}", static_cast<int64_t>(dExprEval));
		findNumType = true;
		break;
	case 'x':
		f = "0x" + std::format("{:x}", static_cast<int64_t>(dExprEval));
		findNumType = true;
		break;
	case 'd':
	default:
		f = std::format("{}", dExprEval);
		findNumType = true;
		break;
	}
	if (!findNumType)
		return false;

	res.assign(f.begin(), f.end());
	res.push_back('\0');
	return true;
}

// 最终输出为UTF-8结果，确保添加空终止字符
void CopyToUtf8Result(const std::string& source, std::vector<char>& tar) {
	if (source.empty()) {
		tar.clear();
		return;
	}

	// 检查是否已经是 UTF‑8（使用 Scintilla 内置函数）
	const bool isValidUtf8 = Scintilla::Internal::UTF8IsValid(source);

	if (isValidUtf8) {
		// 已经是 UTF‑8，直接移动内存
		tar.assign(source.begin(), source.end());
		tar.push_back('\0');
		return;
	}

	// 从系统本地编码（ACP）转换到 UTF‑8
	const int wlen = ::MultiByteToWideChar(GetACP(), 0, source.c_str(), -1, nullptr, 0);
	if (wlen <= 0) {
		// 转码失败，回退为直接返回原数据（尽可能保留）
		tar.assign(source.begin(), source.end());
		tar.push_back('\0');
		return;
	}

	std::wstring wide(wlen, L'\0');
	::MultiByteToWideChar(GetACP(), 0, source.c_str(), -1, &wide[0], wlen);

	// 计算 UTF‑8 所需字节数（不含空终止符）
	const int u8len = ::WideCharToMultiByte(CP_UTF8, WC_COMPOSITECHECK | WC_DISCARDNS,
										wide.data(), static_cast<int>(wide.size()),
										nullptr, 0, nullptr, nullptr);
	if (u8len <= 0) {
		tar.assign(source.begin(), source.end());
		tar.push_back('\0');
		return;
	}

	tar.assign(static_cast<size_t>(u8len) + 1, '\0');
	::WideCharToMultiByte(CP_UTF8, WC_COMPOSITECHECK | WC_DISCARDNS,
							wide.data(), wlen - 1,
							tar.data(), u8len, nullptr, nullptr);
}

// 假设这些是你的基础结构定义
struct SpeedExpr {
    std::string_view name;
    std::string_view teExpr;    // tinyexpr++ 表达式
    std::string_view jsExpr;    // JS 表达式
};

// --- 1. 专门用于验证计算结果是否一致的函数 ---
std::string VerifyMathResults(std::span<const SpeedExpr> tests) {
    auto resultStr = std::format("=== Math Verification: TExp vs JS (String Comparison) ===\r\n");
    resultStr += std::format("{:<24} | {:<20} | {:<20} | {:<10}\r\n",
        "Expression", "TExp Output", "JS Output", "Match?");
    resultStr += std::string(75, '-') + "\r\n";

    for (const auto& test : tests) {
        // 1. 获取 tinyexpr++ 的原始字符串结果
        std::vector<char> teVec;
        teVec.reserve(256);
        teCpp(test.teExpr, teVec);
        std::string teVal(teVec.begin(), teVec.end());

        // 2. 获取 JS 引擎的原始字符串结果
        std::string jsVal;
        EvaluateJSExpression(test.jsExpr.data(), CP_UTF8, jsVal);

        // 3. 直接进行字符串比对
        bool isMatch = (teVal == jsVal);

        resultStr += std::format("{:<24} | {:<20} | {:<20} | {:<10}\r\n",
            test.name, teVal, jsVal, isMatch ? "YES" : "NO");
    }

    return resultStr;
}


std::string RunSpeedTest() {
	// 基准测试：比较 JS 引擎 (Chakra/JScript) 与 tinyexpr++ 的数学求值性能
	constexpr SpeedExpr speedTests[] = {
		{"5*5/5+5",            "5*5/5+5",            "5*5/5+5"},
		{"(1+2)*3/4",          "(1+2)*3/4",          "(1+2)*3/4"},
		{"100/3",              "100/3",              "100/3"},
		{"sqrt(pi)",           "sqrt(pi)",           "sqrt(PI)"},
		{"sin(pi/2)",          "sin(pi/2)",          "sin(PI/2)"},
		{"cos(pi/6)",          "cos(pi/6)",          "cos(PI/6)"},
		{"pow(2,10)",          "pow(2,10)",          "pow(2,10)"},
		{"sqrt(1.234)",        "sqrt(1.234)",        "sqrt(1.234)"},
		{"abs(-3.5)",          "abs(-3.5)",          "abs(-3.5)"},
		{"ln(exp(1))",        "log(exp(1))",        "log(exp(1))"},
		{"sqrt(1+2^10-pi)",    "sqrt(1+2^10-pi)",    "sqrt(1+pow(2,10)-PI)"},
		{"sin(pi/4)^2+cos(pi/4)^2", "sin(pi/4)^2+cos(pi/4)^2", "pow(sin(PI/4),2)+pow(cos(PI/4),2)"},
	};
	constexpr int NUM_SPEED = sizeof(speedTests) / sizeof(speedTests[0]);
	constexpr int ITERATIONS = 4000;

	std::string resultStr;
	resultStr = VerifyMathResults(speedTests);
	resultStr += std::format("=== Speed Test: JS vs tinyexpr++ (3-way) ===\r\n");
	resultStr += std::format("Expressions: {}, Iterations per expr: {}\r\n\r\n", NUM_SPEED, ITERATIONS);
	resultStr += std::format("{:<24} {:>12} {:>12} {:>12} {:>12} {:>12} {:>12}\r\n",
		"Expression", "TExp(us)", "JSEng(us)", "JSCache(us)", "TE/iter", "JS/iter", "JSCache/iter");
	resultStr += std::string(102, '-') + "\r\n";

	// tinyexpr++ 预热一次（thread_local static 会在此创建）
	tecpp_expr("1+1");
	// JS 缓存引擎预热一次（thread_local static engine 在此创建）
	{ std::string _; EvaluateJSExpressionCached("1+1", CP_UTF8, _); }

	double sumTeRatio = 0, sumCacheRatio = 0;
	long long totalTe = 0, totalJs = 0, totalJsCached = 0;
	for (const auto& test : speedTests) {
		// --- tinyexpr++ 测试 (缓存解析器) ---
		{
		std::vector<char> teResult;
		teResult.reserve(256);
		auto start = std::chrono::high_resolution_clock::now();
		for (int i = 0; i < ITERATIONS; ++i) {
			teResult.clear();
			teCpp(test.teExpr, teResult);
		}
		auto end = std::chrono::high_resolution_clock::now();
		const long long teTotal = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
		const auto tePerIter = static_cast<double>(teTotal) / ITERATIONS;
		totalTe += teTotal;

		// --- JS 引擎测试 (每次创建/销毁) ---
		std::string jsResult;
		start = std::chrono::high_resolution_clock::now();
		for (int i = 0; i < ITERATIONS; ++i) {
			jsResult.clear();
			EvaluateJSExpression(test.jsExpr.data(), CP_UTF8, jsResult);
		}
		end = std::chrono::high_resolution_clock::now();
		const long long jsTotal = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
		const auto jsPerIter = static_cast<double>(jsTotal) / ITERATIONS;
		totalJs += jsTotal;

		// --- JS 引擎测试 (缓存引擎，不销毁) ---
		start = std::chrono::high_resolution_clock::now();
		for (int i = 0; i < ITERATIONS; ++i) {
			jsResult.clear();
			EvaluateJSExpressionCached(test.jsExpr.data(), CP_UTF8, jsResult);
		}
		end = std::chrono::high_resolution_clock::now();
		const long long jsCachedTotal = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
		const auto jsCachedPerIter = static_cast<double>(jsCachedTotal) / ITERATIONS;
		totalJsCached += jsCachedTotal;

		sumTeRatio += static_cast<double>(jsTotal) / std::max(teTotal, 1LL);
		sumCacheRatio += static_cast<double>(jsTotal) / std::max(jsCachedTotal, 1LL);

		resultStr += std::format("{:<24s}  {:>8d} us  {:>8d} us  {:>8d} us  {:8.2f}  {:8.2f}  {:8.2f}\r\n",
			test.name, teTotal, jsTotal, jsCachedTotal, tePerIter, jsPerIter, jsCachedPerIter);
		}
	}

	// 汇总统计
	resultStr += std::string(102, '-') + "\r\n";
	resultStr += std::format("  合计:               {:>8d} us  {:>8d} us  {:>8d} us\r\n\r\n",
		totalTe, totalJs, totalJsCached);
	resultStr += std::format("  注:\r\n");
	resultStr += std::format("    TExp    = tinyexpr++ (thread_local static 缓存解析器)\r\n");
	resultStr += std::format("    JSEng   = JS 引擎 (每次调用创建/销毁 COM 引擎)\r\n");
	resultStr += std::format("    JSCache = JS 引擎 (缓存 COM 引擎，类似 te 的缓存方式)\r\n");
	resultStr += std::format("    tinyexpr++ 平均比 JS(每次创建) 快约 {:.0f} 倍\r\n", sumTeRatio / NUM_SPEED);
	resultStr += std::format("    JS(缓存引擎) 平均比 JS(每次创建) 快约 {:.0f} 倍\r\n", sumCacheRatio / NUM_SPEED);

	return resultStr;
}

bool extendExpr(std::string_view tmf, std::vector<char>& res) {
	std::string stmf{ tmf };
	RemoveUnnecessaryLeadingCharacters(stmf);
	if (stmf.empty()) return false;

	// calcTime: time arithmetic expression, e.g. "1h30m+15m", "01:30:00-15.079s"
	if (std::isdigit(static_cast<unsigned char>(stmf[0]))) {
		auto const op_pos = stmf.find_first_of("+-", 1);
		if (op_pos != std::string::npos &&
			stmf.substr(0, op_pos).find_first_of("hms:.") != std::string::npos &&
			calcTime(stmf, res))
			return true;
	}

	// toSeconds: "toSec(...)"  /  toTime: "toTime(...)"  /  strNum: "num(...)"
	if (stmf.rfind(')') != std::string::npos) {
		if (ctre::starts_with<"toSec", ctre::case_insensitive>(stmf) && toSeconds(stmf, res))
			return true;
		if (ctre::starts_with<"toTime", ctre::case_insensitive>(stmf) && toTime(stmf, res))
			return true;
		if (ctre::starts_with<"num", ctre::case_insensitive>(stmf) && strNum(stmf, res))
			return true;
	}

	return teCpp(tmf, res);
}

bool extendExprExtd(std::string_view tmf, std::vector<char>& res) {
	if (tmf.empty()) return false;

	// 分离命令行和标准输入内容
	const size_t pos = tmf.find('\n');
	std::string_view cmdLineStr;
	std::string_view inputStr;

	if (pos == std::string_view::npos) {
		cmdLineStr = tmf;
	} else {
		cmdLineStr = tmf.substr(0, pos);
		inputStr = tmf.substr(pos + 1);
	}

	// 去除命令行末尾可能存在的 \r (兼容 \r\n 换行)
	if (!cmdLineStr.empty() && cmdLineStr.back() == '\r') {
		cmdLineStr.remove_suffix(1);
	}

	if (cmdLineStr.empty()) {
		return false;
	}

	// 从命令行中解析 exePath 和 args
	std::string exePath;
	std::string args;

	if (cmdLineStr.front() == '"') {
		// 如果路径用双引号包裹
		const size_t endQuote = cmdLineStr.find('"', 1);
		if (endQuote != std::string_view::npos) {
			exePath = std::string(cmdLineStr.substr(1, endQuote - 1));
			const size_t argsStart = cmdLineStr.find_first_not_of(" \t", endQuote + 1);
			if (argsStart != std::string_view::npos) {
				args = std::string(cmdLineStr.substr(argsStart));
			}
		} else {
			// 引号未闭合，容错处理：将整体作为 exePath
			exePath = std::string(cmdLineStr);
		}
	} else {
		// 如果路径没有双引号包裹，按第一个空格分割
		const size_t spacePos = cmdLineStr.find_first_of(" \t");
		if (spacePos != std::string_view::npos) {
			exePath = std::string(cmdLineStr.substr(0, spacePos));
			const size_t argsStart = cmdLineStr.find_first_not_of(" \t", spacePos + 1);
			if (argsStart != std::string_view::npos) {
				args = std::string(cmdLineStr.substr(argsStart));
			}
		} else {
			// 没有参数，整体作为 exePath
			exePath = std::string(cmdLineStr);
		}
	}

	// 是否是内部自定义特殊命令（以冒号开头，避免与外部命令冲突）
	if (ctre::starts_with<":speedtest", ctre::case_insensitive>(exePath)) {
		// 基准测试：比较 JS 引擎 (Chakra/JScript) 与 tinyexpr++ 的数学求值性能
		auto resultStr = RunSpeedTest();
		if (resultStr.empty())
			return false;
		CopyToUtf8Result(resultStr, res);
		return true;
	}
	if (ctre::starts_with<":runtest", ctre::case_insensitive>(exePath)) {
		auto testResult = getBitsetInfo() + RunPerformanceTest() +
			RunTrimPerformanceTest() + RunTests() + RunValidationBenchmark();
		if (testResult.empty())
			return false;
		CopyToUtf8Result(testResult, res);
		return true;
	}
	if (ctre::starts_with<":tts", ctre::case_insensitive>(exePath))
		return Spk::Instance().tts(inputStr);

	// 执行子进程
	// 设定超时时间为 30 秒（可根据实际需求调整）
	ExtCmdProcess proc(30);
	std::string output;
	std::string error;
	std::string input(inputStr);

	int exitCode = proc.Run(exePath, args, input, output, error);

	// 将标准输出和错误输出依次追加到 outall 中
	std::string outall;
	if (!output.empty()) {
		outall = "\r\n" + output;
	}
	if (!error.empty()) {
		outall += std::format("\r\n[Error num: {}]{}", exitCode, error);
	}
	if (outall.empty())
		return false;
	CopyToUtf8Result(outall, res);
	return true;
}

// Inside anonymous namespace }
