// CmdProc.hpp
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

class ExtCmdProcess {
public:
	explicit ExtCmdProcess(int timeout) : timeout_(timeout) {}

	int Run(const std::string& exePath,
			const std::string& args,
			const std::string& input,
			std::string& output,
			std::string& error)
	{
		output.clear();
		error.clear();
		try {
			return runImpl(exePath, args, input, output, error);
		} catch (const std::exception& e) {
			error = e.what();
		} catch (...) {
			error = "unknown error";
		}
		return -1;
	}

private:
	int timeout_;

	struct HandleDeleter {
		void operator()(HANDLE h) const noexcept {
			if (h && h != INVALID_HANDLE_VALUE)
				::CloseHandle(h);
		}
	};
	using UniqueHandle = std::unique_ptr<void, HandleDeleter>;

	static std::wstring utf8ToWide(const std::string& s) {
		if (s.empty()) return {};
		int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
										static_cast<int>(s.size()), nullptr, 0);
		if (len <= 0) {
			throw std::runtime_error("utf8ToWide failed: invalid UTF-8 sequence.");
		}
		std::wstring w(len, L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, s.data(),
								static_cast<int>(s.size()), &w[0], len);
		return w;
	}

	// 仅对 exePath 进行简单的双引号包裹（如果包含空格）
	// 不再对用户传入的 args 做任何拆分和重新转义，保持原样传给子进程
	static std::wstring quoteExePath(const std::wstring& exePath) {
		if (exePath.empty()) return L"\"\"";
		if (exePath.find_first_of(L" \t") != std::wstring::npos) {
			std::wstring escaped = L"\"";
			for (wchar_t ch : exePath) {
				if (ch == L'"') escaped.push_back(L'"'); // 内嵌引号转为 ""
				escaped.push_back(ch);
			}
			escaped.push_back(L'"');
			return escaped;
		}
		return exePath;
	}

	int runImpl(const std::string& exePath,
				const std::string& args,
				const std::string& input,
				std::string& output,
				std::string& error)
	{
		// 构造命令行
		std::wstring wExe = utf8ToWide(exePath);
		std::wstring wArgs = utf8ToWide(args);
		std::wstring cmdLine = quoteExePath(wExe);
		if (!wArgs.empty()) {
			cmdLine.push_back(L' ');
			cmdLine += wArgs;
		}
		std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
		cmdBuf.push_back(L'\0');

		// 检查输入是否包含非空白字符
		const bool hasInput = std::ranges::any_of(input, [](char c) {
			return !std::isspace(static_cast<unsigned char>(c));
		});

		// 创建可继承的匿名管道（立即用 RAII 包裹防泄漏）
		SECURITY_ATTRIBUTES sa{
			.nLength = sizeof(SECURITY_ATTRIBUTES),
			.lpSecurityDescriptor = nullptr,
			.bInheritHandle = TRUE
		};

		auto createPipe = [&sa]() -> std::pair<UniqueHandle, UniqueHandle> {
			HANDLE hRead = nullptr, hWrite = nullptr;
			if (!::CreatePipe(&hRead, &hWrite, &sa, 0))
				throw std::runtime_error("CreatePipe failed: " + std::to_string(::GetLastError()));
			return {UniqueHandle(hRead), UniqueHandle(hWrite)};
		};

		auto [uInRead,   uInWrite]   = createPipe();
		auto [uOutRead,  uOutWrite]  = createPipe();
		auto [uErrRead,  uErrWrite]  = createPipe();

		// 父端句柄设为不可继承
		::SetHandleInformation(uInWrite.get(), HANDLE_FLAG_INHERIT, 0);
		::SetHandleInformation(uOutRead.get(), HANDLE_FLAG_INHERIT, 0);
		::SetHandleInformation(uErrRead.get(), HANDLE_FLAG_INHERIT, 0);

		// 启动子进程
		STARTUPINFOW si{};
		si.cb = sizeof(STARTUPINFOW);
		si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		si.hStdInput = uInRead.get();
		si.hStdOutput = uOutWrite.get();
		si.hStdError = uErrWrite.get();

		PROCESS_INFORMATION pi{};

		// 【修改点】第一个参数设为 nullptr，让系统从 cmdBuf 中解析并搜索 PATH
		if (!::CreateProcessW(nullptr,               // <-- 改为 nullptr
								cmdBuf.data(),         // 命令行，包含转义后的 exePath 和 args
								nullptr, nullptr,
								TRUE,
								CREATE_NO_WINDOW,
								nullptr, nullptr,
								&si, &pi)) {
			throw std::runtime_error("CreateProcessW failed: " + std::to_string(::GetLastError()));
		}

		UniqueHandle uProc(pi.hProcess);
		UniqueHandle uThread(pi.hThread);

		// 关闭父进程持有的子端，保证读线程能检测到 EOF
		uInRead.reset();
		uOutWrite.reset();
		uErrWrite.reset();

		// I/O 工作线程
		std::jthread stdinThread;
		if (hasInput) {
			stdinThread = std::jthread([&uInWrite, &input] {
				std::size_t total = input.size();
				std::size_t written = 0;
				while (written < total) {
					DWORD chunk = static_cast<DWORD>(
						std::min<std::size_t>(total - written, 4u * 1024 * 1024));
					DWORD wrote = 0;
					if (!::WriteFile(uInWrite.get(),
										input.data() + written, chunk,
										&wrote, nullptr) || wrote == 0)
						break; // 写入失败（子进程可能已退出）
					written += wrote;
				}
				uInWrite.reset(); // 发送 EOF
			});
		} else {
			uInWrite.reset();
		}

		auto reader = [](HANDLE h, std::string& out) {
			char buf[64 * 1024];
			DWORD bytesRead;
			// 循环读取直到 EOF (bytesRead == 0 且 GetLastError == ERROR_BROKEN_PIPE)
			while (::ReadFile(h, buf, static_cast<DWORD>(sizeof(buf)), &bytesRead, nullptr) && bytesRead > 0) {
				out.append(buf, bytesRead);
			}
		};
		std::jthread outThread(reader, uOutRead.get(), std::ref(output));
		std::jthread errThread(reader, uErrRead.get(), std::ref(error));

		// 等待进程结束（带超时）
		DWORD timeoutMs = (timeout_ > 0)
								? static_cast<DWORD>(timeout_) * 1000
								: INFINITE;
		DWORD waitResult = ::WaitForSingleObject(uProc.get(), timeoutMs);
		if (waitResult == WAIT_TIMEOUT) {
			::TerminateProcess(uProc.get(), 1);
			::WaitForSingleObject(uProc.get(), INFINITE);
		}

		// 回收所有 I/O 线程
		// 此时子进程已退出，管道写端已关闭，reader 线程会读到 EOF 并自然结束
		outThread.join();
		errThread.join();
		if (stdinThread.joinable())
			stdinThread.join();

		// 获取退出码
		DWORD exitCode = 0;
		if (::GetExitCodeProcess(uProc.get(), &exitCode))
			return static_cast<int>(exitCode);
		return -1;
	}
};
