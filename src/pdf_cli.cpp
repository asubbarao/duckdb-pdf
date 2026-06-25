// pdf_cli.cpp
//
// Implementation of the self-contained PDF command-line bridge declared in
// pdf_cli.hpp. Pure std C++17 + POSIX -- no DuckDB headers. See the header for
// the high-level design notes (no-shell spawning, injection safety, etc).

#include "include/pdf_cli.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>

// Platform-specific process API
#if defined(_WIN32)
#include <windows.h>
#include <thread>
#include <utility>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace pdfcli {

// ===========================================================================
// 1. Process runner
// ===========================================================================

// ----------------------------------------------------------------------------
// Portable UTF-8 <-> wide helpers (Win only; not needed on POSIX path)
// ----------------------------------------------------------------------------

#if defined(_WIN32)
namespace {
std::wstring utf8_to_wide(const std::string &s) {
	if (s.empty())
		return {};
	int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	if (len <= 0)
		return {};
	std::wstring out(static_cast<size_t>(len), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], len);
	return out;
}

std::string wide_to_utf8(const std::wstring &ws) {
	if (ws.empty())
		return {};
	int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
	if (len <= 0)
		return {};
	std::string out(static_cast<size_t>(len), '\0');
	WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), &out[0], len, nullptr, nullptr);
	return out;
}
} // namespace
#endif

bool ToolExists(const std::string &tool) {
	if (tool.empty())
		return false;

#if defined(_WIN32)
	std::wstring wtool = utf8_to_wide(tool);
	// If contains separator, treat as path (check existence, allow .exe or not)
	if (wtool.find(L'\\') != std::wstring::npos || wtool.find(L'/') != std::wstring::npos) {
		// Try as-is and with .exe appended if no extension-like
		DWORD attr = GetFileAttributesW(wtool.c_str());
		if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
			return true;

		// If no dot in last component, try + ".exe"
		size_t last_sl = wtool.find_last_of(L"\\/");
		std::wstring base = (last_sl == std::wstring::npos) ? wtool : wtool.substr(last_sl + 1);
		if (base.find(L'.') == std::wstring::npos) {
			std::wstring with_exe = wtool + L".exe";
			attr = GetFileAttributesW(with_exe.c_str());
			if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
				return true;
		}
		return false;
	}

	// Bare name: use SearchPathW (honors PATHEXT, finds foo or foo.exe etc.)
	wchar_t found[MAX_PATH];
	// First try with explicit .exe (common for our "pdftotext")
	if (SearchPathW(nullptr, wtool.c_str(), L".exe", MAX_PATH, found, nullptr) > 0)
		return true;
	// Fall back to default PATHEXT search
	if (SearchPathW(nullptr, wtool.c_str(), nullptr, MAX_PATH, found, nullptr) > 0)
		return true;
	return false;

#else // POSIX
	// If contains slash, treat as concrete path.
	if (tool.find('/') != std::string::npos) {
		return ::access(tool.c_str(), X_OK) == 0;
	}
	// Walk $PATH.
	const char *path_env = ::getenv("PATH");
	if (!path_env) {
		path_env = "/usr/local/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin";
	}
	std::string path = path_env;
	size_t start = 0;
	while (start <= path.size()) {
		size_t colon = path.find(':', start);
		std::string dir = (colon == std::string::npos) ? path.substr(start) : path.substr(start, colon - start);
		if (!dir.empty()) {
			std::string candidate = dir + "/" + tool;
			if (::access(candidate.c_str(), X_OK) == 0) {
				return true;
			}
		}
		if (colon == std::string::npos)
			break;
		start = colon + 1;
	}
	return false;
#endif
}

// ----------------------------------------------------------------------------
// ArgvToCommandLineW (Windows only) - CRITICAL for correctness.
//    Matches CommandLineToArgvW / MS C runtime parsing rules.
// ----------------------------------------------------------------------------

#if defined(_WIN32)
namespace {
std::wstring ArgvToCommandLineW(const std::vector<std::string> &argv) {
	// Translate UTF-8 argv into a single command line string using the exact
	// quoting rules used by CommandLineToArgvW.
	//
	// Rules implemented (from MSDN + Raymond Chen):
	// - Args containing space/tab/empty or " must be quoted.
	// - " inside arg becomes \"
	// - Backslashes before " or at end of quoted arg are doubled appropriately.
	//   2n \ before "  -> n \ + delimiter
	//   2n+1 \ before " -> n \ + literal "
	// - Backslashes not before " are literal.

	std::wstring result;
	for (size_t i = 0; i < argv.size(); ++i) {
		if (i > 0)
			result += L' ';

		std::wstring arg = utf8_to_wide(argv[i]);

		// Determine if quoting needed
		bool need_quote = arg.empty();
		if (!need_quote) {
			for (wchar_t c : arg) {
				if (c == L' ' || c == L'\t' || c == L'"') {
					need_quote = true;
					break;
				}
			}
		}

		if (!need_quote) {
			result += arg;
			continue;
		}

		// Emit opening "
		result += L'"';

		std::wstring bs_buf;
		for (wchar_t c : arg) {
			if (c == L'\\') {
				bs_buf += L'\\';
			} else if (c == L'"') {
				// Double the accumulated backslashes, then emit \"
				result.append(bs_buf.size() * 2, L'\\');
				bs_buf.clear();
				result += L"\\\"";
			} else {
				if (!bs_buf.empty()) {
					result += bs_buf;
					bs_buf.clear();
				}
				result += c;
			}
		}

		// Trailing backslashes (if any)
		if (!bs_buf.empty()) {
			// For quoted arg, trailing \ must be doubled so the closing " is not escaped.
			result.append(bs_buf.size() * 2, L'\\'); // double them
		}

		result += L'"';
	}
	return result;
}
} // namespace
#endif

// RunCapture
//
// Portable, deadlock-safe.
//   POSIX:   create up to three pipes, spawn via posix_spawnp, then write the
//            stdin payload and drain stdout+stderr in a single non-blocking
//            select() loop. Deadlock-free regardless of payload sizes.
//   Windows: CreateProcessW + anonymous pipes + dedicated reader threads for
//            stdout/stderr (deadlock-safe), with CommandLineToArgvW-correct
//            argument quoting.
int RunCapture(const std::vector<std::string> &argv, const std::string &stdin_data, std::string &out,
               std::string &err) {
	out.clear();
	err.clear();
	if (argv.empty())
		return -1;

#if defined(_WIN32)
	// ------------------------- Windows branch -------------------------
	// Uses CreateProcessW + anonymous pipes + SetHandleInformation.
	// Deadlock avoidance: dedicated reader threads for stdout and stderr.
	// Stdin written from main thread (sufficient for small payloads; typical use here).
	//
	// CreateProcessW command line built with correct ArgvToCommandLineW quoting.

	std::wstring cmdline = ArgvToCommandLineW(argv);
	if (cmdline.empty())
		return -1;

	// Need mutable buffer for lpCommandLine
	std::vector<wchar_t> cmdbuf(cmdline.begin(), cmdline.end());
	cmdbuf.push_back(L'\0');

	SECURITY_ATTRIBUTES sa {};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = nullptr;

	HANDLE hStdInRd = nullptr, hStdInWr = nullptr;
	HANDLE hStdOutRd = nullptr, hStdOutWr = nullptr;
	HANDLE hStdErrRd = nullptr, hStdErrWr = nullptr;

	auto close = [](HANDLE &h) {
		if (h) {
			CloseHandle(h);
			h = nullptr;
		}
	};

	auto cleanup = [&]() {
		close(hStdInRd);
		close(hStdInWr);
		close(hStdOutRd);
		close(hStdOutWr);
		close(hStdErrRd);
		close(hStdErrWr);
	};

	bool feed_stdin = !stdin_data.empty();

	// Create pipes
	if (!CreatePipe(&hStdInRd, &hStdInWr, &sa, 0)) {
		cleanup();
		return -1;
	}
	if (!CreatePipe(&hStdOutRd, &hStdOutWr, &sa, 0)) {
		cleanup();
		return -1;
	}
	if (!CreatePipe(&hStdErrRd, &hStdErrWr, &sa, 0)) {
		cleanup();
		return -1;
	}

	// Parent ends must NOT be inherited
	if (!SetHandleInformation(hStdInWr, HANDLE_FLAG_INHERIT, 0)) {
		cleanup();
		return -1;
	}
	if (!SetHandleInformation(hStdOutRd, HANDLE_FLAG_INHERIT, 0)) {
		cleanup();
		return -1;
	}
	if (!SetHandleInformation(hStdErrRd, HANDLE_FLAG_INHERIT, 0)) {
		cleanup();
		return -1;
	}

	STARTUPINFOW si {};
	si.cb = sizeof(si);
	si.dwFlags |= STARTF_USESTDHANDLES;
	si.hStdInput = feed_stdin ? hStdInRd : GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = hStdOutWr;
	si.hStdError = hStdErrWr;

	if (!feed_stdin) {
		// Redirect child stdin to NUL so it gets immediate EOF instead of console.
		HANDLE hNul =
		    CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
		if (hNul != INVALID_HANDLE_VALUE) {
			si.hStdInput = hNul;
		} else {
			si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		}
	}

	PROCESS_INFORMATION pi {};
	BOOL ok = CreateProcessW(nullptr,       // lpApplicationName (search via cmdline)
	                         cmdbuf.data(), // lpCommandLine (mutable!)
	                         nullptr,       // process security
	                         nullptr,       // thread security
	                         TRUE,          // bInheritHandles
	                         0,             // dwCreationFlags
	                         nullptr,       // env
	                         nullptr,       // cwd
	                         &si, &pi);

	// Close the NUL handle we may have opened (child already duplicated it if used)
	if (si.hStdInput != hStdInRd && si.hStdInput != GetStdHandle(STD_INPUT_HANDLE)) {
		if (si.hStdInput && si.hStdInput != INVALID_HANDLE_VALUE)
			CloseHandle(si.hStdInput);
	}

	// Close child-side ends in parent now
	close(hStdInRd);
	close(hStdOutWr);
	close(hStdErrWr);

	if (!ok) {
		close(hStdInWr);
		close(hStdOutRd);
		close(hStdErrRd);
		return -1;
	}

	// Close unused process/thread handles later; keep hProcess for wait.
	CloseHandle(pi.hThread);

	// Now feed stdin (if any) then close write end so child sees EOF.
	if (feed_stdin && hStdInWr) {
		const char *data = stdin_data.data();
		size_t left = stdin_data.size();
		while (left > 0) {
			DWORD written = 0;
			BOOL w = WriteFile(hStdInWr, data, (DWORD)std::min<size_t>(left, 65536), &written, nullptr);
			if (!w)
				break;
			data += written;
			left -= written;
		}
	}
	close(hStdInWr); // crucial: signals EOF to child

	// Reader threads to drain concurrently (deadlock prevention).
	std::string captured_out, captured_err;

	auto reader = [](HANDLE h, std::string &dest) {
		char buf[65536];
		for (;;) {
			DWORD n = 0;
			BOOL r = ReadFile(h, buf, sizeof(buf), &n, nullptr);
			if (!r || n == 0)
				break;
			dest.append(buf, n);
		}
	};

	std::thread t_out(reader, hStdOutRd, std::ref(captured_out));
	std::thread t_err(reader, hStdErrRd, std::ref(captured_err));

	// Wait for child
	WaitForSingleObject(pi.hProcess, INFINITE);

	DWORD exit_code = 1; // default non-zero on weirdness
	GetExitCodeProcess(pi.hProcess, &exit_code);

	// Join readers (they will see EOF after we closed child's write ends + child exited)
	if (t_out.joinable())
		t_out.join();
	if (t_err.joinable())
		t_err.join();

	close(hStdOutRd);
	close(hStdErrRd);
	CloseHandle(pi.hProcess);

	out = std::move(captured_out);
	err = std::move(captured_err);
	return static_cast<int>(exit_code);

#else
	// ------------------------- POSIX branch -------------------------
	// Non-blocking select loop + EPIPE handling + SIGPIPE ignore.

	const bool feed_stdin = !stdin_data.empty();

	int in_pipe[2] = {-1, -1};
	int out_pipe[2] = {-1, -1};
	int err_pipe[2] = {-1, -1};

	auto close_fd = [](int &fd) {
		if (fd >= 0) {
			::close(fd);
			fd = -1;
		}
	};
	auto cleanup_all = [&]() {
		close_fd(in_pipe[0]);
		close_fd(in_pipe[1]);
		close_fd(out_pipe[0]);
		close_fd(out_pipe[1]);
		close_fd(err_pipe[0]);
		close_fd(err_pipe[1]);
	};

	if (feed_stdin && ::pipe(in_pipe) != 0) {
		return -1;
	}
	if (::pipe(out_pipe) != 0) {
		cleanup_all();
		return -1;
	}
	if (::pipe(err_pipe) != 0) {
		cleanup_all();
		return -1;
	}

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);

	if (feed_stdin) {
		posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
		posix_spawn_file_actions_addclose(&actions, in_pipe[0]);
		posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
	} else {
		posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	}

	posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
	posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
	posix_spawn_file_actions_addclose(&actions, out_pipe[1]);

	posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&actions, err_pipe[0]);
	posix_spawn_file_actions_addclose(&actions, err_pipe[1]);

	std::vector<char *> cargv;
	cargv.reserve(argv.size() + 1);
	for (const auto &a : argv) {
		cargv.push_back(const_cast<char *>(a.c_str()));
	}
	cargv.push_back(nullptr);

	pid_t pid = -1;
	int spawn_rc = posix_spawnp(&pid, cargv[0], &actions, nullptr, cargv.data(), environ);
	posix_spawn_file_actions_destroy(&actions);

	if (spawn_rc != 0) {
		cleanup_all();
		return -1;
	}

	// Parent closes child ends
	close_fd(out_pipe[1]);
	close_fd(err_pipe[1]);
	if (feed_stdin)
		close_fd(in_pipe[0]);

	int wfd = feed_stdin ? in_pipe[1] : -1;
	int ofd = out_pipe[0];
	int efd = err_pipe[0];

	auto set_nonblock = [](int fd) {
		if (fd < 0)
			return;
		int flags = ::fcntl(fd, F_GETFL, 0);
		if (flags >= 0) {
			::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		}
	};
	set_nonblock(wfd);
	set_nonblock(ofd);
	set_nonblock(efd);

	// Ignore SIGPIPE during write
	struct sigaction old_pipe_action {};
	struct sigaction ignore_action {};
	std::memset(&ignore_action, 0, sizeof(ignore_action));
	ignore_action.sa_handler = SIG_IGN;
	::sigaction(SIGPIPE, &ignore_action, &old_pipe_action);

	size_t stdin_off = 0;
	char buf[65536];

	while (ofd >= 0 || efd >= 0 || wfd >= 0) {
		fd_set rset, wset;
		FD_ZERO(&rset);
		FD_ZERO(&wset);
		int maxfd = -1;
		if (ofd >= 0) {
			FD_SET(ofd, &rset);
			maxfd = std::max(maxfd, ofd);
		}
		if (efd >= 0) {
			FD_SET(efd, &rset);
			maxfd = std::max(maxfd, efd);
		}
		if (wfd >= 0) {
			FD_SET(wfd, &wset);
			maxfd = std::max(maxfd, wfd);
		}
		if (maxfd < 0)
			break;

		int sel = ::select(maxfd + 1, &rset, &wset, nullptr, nullptr);
		if (sel < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (wfd >= 0 && FD_ISSET(wfd, &wset)) {
			size_t remaining = stdin_data.size() - stdin_off;
			ssize_t n = ::write(wfd, stdin_data.data() + stdin_off, remaining);
			if (n > 0) {
				stdin_off += static_cast<size_t>(n);
				if (stdin_off >= stdin_data.size()) {
					close_fd(wfd);
				}
			} else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
				close_fd(wfd); // EPIPE etc.
			}
		}

		if (ofd >= 0 && FD_ISSET(ofd, &rset)) {
			ssize_t n = ::read(ofd, buf, sizeof(buf));
			if (n > 0) {
				out.append(buf, static_cast<size_t>(n));
			} else if (n == 0) {
				close_fd(ofd);
			} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
				close_fd(ofd);
			}
		}

		if (efd >= 0 && FD_ISSET(efd, &rset)) {
			ssize_t n = ::read(efd, buf, sizeof(buf));
			if (n > 0) {
				err.append(buf, static_cast<size_t>(n));
			} else if (n == 0) {
				close_fd(efd);
			} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
				close_fd(efd);
			}
		}
	}

	close_fd(wfd);
	close_fd(ofd);
	close_fd(efd);

	::sigaction(SIGPIPE, &old_pipe_action, nullptr);

	int status = 0;
	pid_t w;
	do {
		w = ::waitpid(pid, &status, 0);
	} while (w < 0 && errno == EINTR);

	if (w < 0)
		return -1;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return -1;
#endif
}

// ===========================================================================
// 2. Quote-aware tokenizer
// ===========================================================================

std::vector<std::string> TokenizeArgs(const std::string &raw) {
	std::vector<std::string> tokens;
	std::string cur;
	bool in_token = false;
	enum class Q { NONE, SINGLE, DOUBLE } quote = Q::NONE;

	for (size_t i = 0; i < raw.size(); ++i) {
		char c = raw[i];

		if (quote == Q::SINGLE) {
			// Inside single quotes everything is literal until the next '.
			if (c == '\'') {
				quote = Q::NONE;
			} else {
				cur.push_back(c);
			}
			continue;
		}
		if (quote == Q::DOUBLE) {
			if (c == '\\' && i + 1 < raw.size()) {
				// Within double quotes a backslash escapes the next char
				// (we keep it simple: just emit the following char literally).
				cur.push_back(raw[i + 1]);
				++i;
			} else if (c == '"') {
				quote = Q::NONE;
			} else {
				cur.push_back(c);
			}
			continue;
		}

		// Unquoted context.
		if (c == '\'') {
			quote = Q::SINGLE;
			in_token = true;
		} else if (c == '"') {
			quote = Q::DOUBLE;
			in_token = true;
		} else if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
			if (in_token) {
				tokens.push_back(cur);
				cur.clear();
				in_token = false;
			}
		} else {
			cur.push_back(c);
			in_token = true;
		}
	}
	if (in_token) {
		tokens.push_back(cur);
	}
	return tokens;
}

// ===========================================================================
// 3. Conversion utilities
// ===========================================================================

namespace {

// Append the tokenized raw_args into argv.
void AppendRawArgs(std::vector<std::string> &argv, const std::string &raw_args) {
	if (raw_args.empty()) {
		return;
	}
	for (auto &t : TokenizeArgs(raw_args)) {
		argv.push_back(std::move(t));
	}
}

// Run a converter argv and return stdout, or throw with stderr/details.
std::string RunConverterOrThrow(const std::vector<std::string> &argv) {
	if (!ToolExists(argv[0])) {
		throw std::runtime_error("pdfcli: required tool not found on PATH: " + argv[0]);
	}
	std::string out, err;
	int rc = RunCapture(argv, /*stdin_data=*/"", out, err);
	if (rc == -1) {
		throw std::runtime_error("pdfcli: failed to spawn '" + argv[0] + "'");
	}
	if (rc != 0) {
		std::string msg = "pdfcli: '" + argv[0] + "' exited with code " + std::to_string(rc);
		if (!err.empty()) {
			msg += ": " + err;
		}
		throw std::runtime_error(msg);
	}
	return out;
}

} // namespace

std::string PdfToText(const std::string &path, const std::string &layout, int first_page, int last_page,
                      const std::string &raw_args) {
	std::vector<std::string> argv = {"pdftotext", "-enc", "UTF-8"};

	if (layout == "physical") {
		argv.push_back("-layout");
	} else if (layout == "raw") {
		argv.push_back("-raw");
	}
	// layout == "reading" (or anything else): no flag (poppler default).

	if (first_page > 0) {
		argv.push_back("-f");
		argv.push_back(std::to_string(first_page));
	}
	if (last_page > 0) {
		argv.push_back("-l");
		argv.push_back(std::to_string(last_page));
	}

	AppendRawArgs(argv, raw_args);

	argv.push_back(path); // input
	argv.push_back("-");  // output to stdout

	return RunConverterOrThrow(argv);
}

std::string PdfToHtml(const std::string &path, bool single_doc, bool ignore_images, const std::string &raw_args) {
	std::vector<std::string> argv = {"pdftohtml", "-stdout", "-noframes"};
	if (single_doc) {
		argv.push_back("-s");
	}
	if (ignore_images) {
		argv.push_back("-i");
	}

	AppendRawArgs(argv, raw_args);

	argv.push_back(path);
	// pdftohtml emits to stdout because of -stdout.
	return RunConverterOrThrow(argv);
}

std::string PdfToXml(const std::string &path, const std::string &raw_args) {
	// -bbox-layout yields XML with <page>/<flow>/<block>/<line>/<word> nodes,
	// each <word> carrying xMin/yMin/xMax/yMax attributes -- exactly what the
	// table reconstructor needs.
	std::vector<std::string> argv = {"pdftotext", "-enc", "UTF-8", "-bbox-layout"};

	AppendRawArgs(argv, raw_args);

	argv.push_back(path);
	argv.push_back("-");
	return RunConverterOrThrow(argv);
}

std::string PdfToSvg(const std::string &path, int page, const std::string &raw_args) {
	if (page <= 0) {
		page = 1;
	}
	std::vector<std::string> argv = {"pdftocairo", "-svg", "-f", std::to_string(page), "-l", std::to_string(page)};

	AppendRawArgs(argv, raw_args);

	argv.push_back(path);
	argv.push_back("-");
	return RunConverterOrThrow(argv);
}

// ===========================================================================
// 4. Structured table extraction
// ===========================================================================

namespace {

// --- minimal XML helpers ---------------------------------------------------

// Decode the handful of XML entities pdftotext emits.
std::string DecodeEntities(const std::string &s) {
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size();) {
		if (s[i] == '&') {
			if (s.compare(i, 5, "&amp;") == 0) {
				out.push_back('&');
				i += 5;
				continue;
			}
			if (s.compare(i, 4, "&lt;") == 0) {
				out.push_back('<');
				i += 4;
				continue;
			}
			if (s.compare(i, 4, "&gt;") == 0) {
				out.push_back('>');
				i += 4;
				continue;
			}
			if (s.compare(i, 6, "&quot;") == 0) {
				out.push_back('"');
				i += 6;
				continue;
			}
			if (s.compare(i, 6, "&apos;") == 0) {
				out.push_back('\'');
				i += 6;
				continue;
			}
			// numeric &#NN; / &#xNN;
			if (s.compare(i, 2, "&#") == 0) {
				size_t semi = s.find(';', i);
				if (semi != std::string::npos) {
					std::string num = s.substr(i + 2, semi - (i + 2));
					long code = 0;
					try {
						if (!num.empty() && (num[0] == 'x' || num[0] == 'X')) {
							code = std::stol(num.substr(1), nullptr, 16);
						} else {
							code = std::stol(num, nullptr, 10);
						}
					} catch (...) {
						code = 0;
					}
					if (code > 0 && code < 128) {
						out.push_back(static_cast<char>(code));
						i = semi + 1;
						continue;
					}
					// Non-ASCII: just drop the entity rather than emit garbage.
					i = semi + 1;
					continue;
				}
			}
		}
		out.push_back(s[i]);
		++i;
	}
	return out;
}

// Pull a double-valued attribute out of a start-tag's attribute span.
bool ExtractAttr(const std::string &tag, const std::string &name, double &value) {
	std::string key = name + "=\"";
	size_t p = tag.find(key);
	if (p == std::string::npos) {
		return false;
	}
	p += key.size();
	size_t q = tag.find('"', p);
	if (q == std::string::npos) {
		return false;
	}
	try {
		value = std::stod(tag.substr(p, q - p));
	} catch (...) {
		return false;
	}
	return true;
}

} // namespace

// ParseBBoxLayoutWords
//
// A tiny forward scan over the -bbox-layout XML. We do not build a DOM; we
// just look for <page ...>, </page>, and <word ...>text</word> spans. Words
// are appended in document order; page boundaries are tracked via out_pages.
std::vector<Word> ParseBBoxLayoutWords(const std::string &xml, int &out_pages) {
	std::vector<Word> words;
	out_pages = 0;

	size_t i = 0;
	const size_t n = xml.size();
	while (i < n) {
		size_t lt = xml.find('<', i);
		if (lt == std::string::npos) {
			break;
		}

		// <page ...>
		if (xml.compare(lt, 5, "<page") == 0 && (lt + 5 >= n || xml[lt + 5] == ' ' || xml[lt + 5] == '>')) {
			++out_pages;
			size_t gt = xml.find('>', lt);
			i = (gt == std::string::npos) ? n : gt + 1;
			continue;
		}

		// <word ...>text</word>
		if (xml.compare(lt, 5, "<word") == 0 && (lt + 5 < n && (xml[lt + 5] == ' ' || xml[lt + 5] == '>'))) {
			size_t gt = xml.find('>', lt);
			if (gt == std::string::npos) {
				break;
			}
			std::string start_tag = xml.substr(lt, gt - lt + 1);

			Word w;
			ExtractAttr(start_tag, "xMin", w.xMin);
			ExtractAttr(start_tag, "yMin", w.yMin);
			ExtractAttr(start_tag, "xMax", w.xMax);
			ExtractAttr(start_tag, "yMax", w.yMax);

			size_t close = xml.find("</word>", gt + 1);
			if (close == std::string::npos) {
				break;
			}
			std::string raw_text = xml.substr(gt + 1, close - (gt + 1));
			w.text = DecodeEntities(raw_text);

			if (!w.text.empty()) {
				words.push_back(std::move(w));
			}
			i = close + 7; // strlen("</word>")
			continue;
		}

		// Any other tag: skip to its '>'.
		size_t gt = xml.find('>', lt);
		i = (gt == std::string::npos) ? n : gt + 1;
	}

	return words;
}

namespace {

// A word annotated with its page index, used internally by the reconstructor.
struct PagedWord {
	int page;
	Word w;
};

// Parse words while tracking, for each word, which page it belongs to. We
// reuse the scanning logic but need the page association, so this is a small
// variant of ParseBBoxLayoutWords.
std::vector<PagedWord> ParseWordsWithPages(const std::string &xml, int &out_pages) {
	std::vector<PagedWord> words;
	out_pages = 0;
	int cur_page = 0;

	size_t i = 0;
	const size_t n = xml.size();
	while (i < n) {
		size_t lt = xml.find('<', i);
		if (lt == std::string::npos) {
			break;
		}
		if (xml.compare(lt, 5, "<page") == 0 && (lt + 5 >= n || xml[lt + 5] == ' ' || xml[lt + 5] == '>')) {
			++out_pages;
			++cur_page;
			size_t gt = xml.find('>', lt);
			i = (gt == std::string::npos) ? n : gt + 1;
			continue;
		}
		if (xml.compare(lt, 5, "<word") == 0 && (lt + 5 < n && (xml[lt + 5] == ' ' || xml[lt + 5] == '>'))) {
			size_t gt = xml.find('>', lt);
			if (gt == std::string::npos) {
				break;
			}
			std::string start_tag = xml.substr(lt, gt - lt + 1);
			Word w;
			ExtractAttr(start_tag, "xMin", w.xMin);
			ExtractAttr(start_tag, "yMin", w.yMin);
			ExtractAttr(start_tag, "xMax", w.xMax);
			ExtractAttr(start_tag, "yMax", w.yMax);
			size_t close = xml.find("</word>", gt + 1);
			if (close == std::string::npos) {
				break;
			}
			w.text = DecodeEntities(xml.substr(gt + 1, close - (gt + 1)));
			if (!w.text.empty()) {
				words.push_back({cur_page, std::move(w)});
			}
			i = close + 7;
			continue;
		}
		size_t gt = xml.find('>', lt);
		i = (gt == std::string::npos) ? n : gt + 1;
	}
	return words;
}

double Median(std::vector<double> v) {
	if (v.empty()) {
		return 0.0;
	}
	std::sort(v.begin(), v.end());
	size_t m = v.size() / 2;
	if (v.size() % 2 == 1) {
		return v[m];
	}
	return 0.5 * (v[m - 1] + v[m]);
}

// Reconstruct one page's words into a grid of text cells.
//
// HEURISTICS (documented):
//  1. ROW CLUSTERING: sort words by yMin. The typical line height is the
//     median of (yMax - yMin) across words. Two words belong to the same row
//     if their yMin differs by less than half that median height. This groups
//     words that sit on the same text baseline even if their tops jitter.
//  2. COLUMN BOUNDARY DETECTION: collect each word's xMin across the whole
//     page, sort them, and cluster xMins that are within a tolerance
//     (~ median char-ish width, derived from median word width / word length)
//     into "column start" positions. Each cluster's representative is the min
//     xMin in the cluster. These become the left edges of columns.
//  3. CELL ASSIGNMENT: for each word, pick the column whose left edge is the
//     greatest left edge <= the word's xMin (i.e. the column the word starts
//     in). Words in the same (row, col) are joined with a single space in
//     left-to-right order.
//  4. TABULAR TEST: a page is considered a table only if it yields more than
//     one column AND more than one row; prose (single column) pages return an
//     empty grid and are skipped by the caller.
std::vector<std::vector<std::string>> ReconstructPageGrid(std::vector<Word> page_words) {
	std::vector<std::vector<std::string>> grid;
	if (page_words.size() < 2) {
		return grid;
	}

	// --- median line height ---
	std::vector<double> heights;
	heights.reserve(page_words.size());
	for (const auto &w : page_words) {
		double h = w.yMax - w.yMin;
		if (h > 0) {
			heights.push_back(h);
		}
	}
	double med_h = Median(heights);
	if (med_h <= 0) {
		med_h = 10.0; // fallback
	}
	double row_tol = med_h * 0.5;

	// --- cluster into rows by yMin ---
	std::sort(page_words.begin(), page_words.end(), [](const Word &a, const Word &b) { return a.yMin < b.yMin; });

	std::vector<std::vector<Word>> rows;
	{
		std::vector<Word> cur;
		double row_anchor = page_words.front().yMin;
		for (auto &w : page_words) {
			if (cur.empty()) {
				cur.push_back(w);
				row_anchor = w.yMin;
			} else if (std::fabs(w.yMin - row_anchor) <= row_tol) {
				cur.push_back(w);
			} else {
				rows.push_back(cur);
				cur.clear();
				cur.push_back(w);
				row_anchor = w.yMin;
			}
		}
		if (!cur.empty()) {
			rows.push_back(cur);
		}
	}

	// --- detect column left-edges from all xMins ---
	std::vector<double> xs;
	std::vector<double> widths;
	for (const auto &w : page_words) {
		xs.push_back(w.xMin);
		double ww = w.xMax - w.xMin;
		// approximate per-char width to derive a clustering tolerance
		size_t len = w.text.size();
		if (ww > 0 && len > 0) {
			widths.push_back(ww / static_cast<double>(len));
		}
	}
	double char_w = Median(widths);
	if (char_w <= 0) {
		char_w = med_h * 0.5;
	}
	// Tolerance: words whose left edges are within ~1.5 char widths belong to
	// the same column. Small enough to separate adjacent columns, large enough
	// to absorb sub-pixel jitter / kerning.
	double col_tol = char_w * 1.5;

	std::sort(xs.begin(), xs.end());
	std::vector<double> col_edges;
	for (double x : xs) {
		if (col_edges.empty() || (x - col_edges.back()) > col_tol) {
			col_edges.push_back(x);
		}
		// else: same column cluster -- keep the earliest (smallest) edge.
	}

	if (col_edges.size() < 2 || rows.size() < 2) {
		// Single column => prose, or single row => not a meaningful table.
		return grid;
	}

	auto col_for = [&](double xMin) -> size_t {
		// greatest edge <= xMin (+ small slack)
		size_t chosen = 0;
		for (size_t c = 0; c < col_edges.size(); ++c) {
			if (xMin + col_tol * 0.5 >= col_edges[c]) {
				chosen = c;
			} else {
				break;
			}
		}
		return chosen;
	};

	const size_t ncols = col_edges.size();
	for (auto &row : rows) {
		std::sort(row.begin(), row.end(), [](const Word &a, const Word &b) { return a.xMin < b.xMin; });
		std::vector<std::string> cells(ncols);
		for (auto &w : row) {
			size_t c = col_for(w.xMin);
			if (!cells[c].empty()) {
				cells[c].push_back(' ');
			}
			cells[c] += w.text;
		}
		grid.push_back(std::move(cells));
	}

	// --- TABULAR REGULARITY GATE (precision over recall) -------------------
	// >=2 columns and >=2 rows alone is far too weak: ordinary prose, where each
	// line's words land at different x positions, trivially produces a multi-row,
	// multi-"column" grid. A real table instead has a REGULAR number of populated
	// cells per row (a header and its data rows share a column structure) and
	// enough rows to be a table rather than two aligned sentences. We therefore
	// require: at least 3 rows, and a strong majority of rows sharing the same
	// non-empty cell count (the modal count), with that modal count >= 2. This
	// favors precision — clean ruled/aligned tables pass; prose and incidental
	// two-line alignment are rejected. Ragged tables (merged/empty cells) are a
	// documented limitation of this v0.1 heuristic.
	if (grid.size() < 3) {
		grid.clear();
		return grid;
	}
	std::vector<int> filled_per_row;
	filled_per_row.reserve(grid.size());
	for (const auto &row : grid) {
		int filled = 0;
		for (const auto &cell : row) {
			if (!cell.empty()) {
				filled++;
			}
		}
		filled_per_row.push_back(filled);
	}
	// modal non-empty count (grids are tiny; an O(n^2) scan is fine and avoids a map)
	int modal_filled = 0;
	int modal_count = 0;
	for (size_t i = 0; i < filled_per_row.size(); ++i) {
		int c = 0;
		for (size_t j = 0; j < filled_per_row.size(); ++j) {
			if (filled_per_row[j] == filled_per_row[i]) {
				c++;
			}
		}
		if (c > modal_count) {
			modal_count = c;
			modal_filled = filled_per_row[i];
		}
	}
	double modal_fraction = static_cast<double>(modal_count) / static_cast<double>(grid.size());
	if (modal_filled < 2 || modal_fraction < 0.6) {
		grid.clear();
	}

	return grid;
}

} // namespace

std::vector<Table> ReconstructTables(const std::string &path, int first_page, int last_page,
                                     const std::string &raw_args) {
	// Build the page-range raw_args for pdftotext -bbox-layout. We inject -f/-l
	// alongside any user raw_args so the XML only contains the requested pages.
	std::string ranged = raw_args;
	if (first_page > 0) {
		ranged += " -f " + std::to_string(first_page);
	}
	if (last_page > 0) {
		ranged += " -l " + std::to_string(last_page);
	}

	std::string xml = PdfToXml(path, ranged);

	int pages = 0;
	std::vector<PagedWord> paged = ParseWordsWithPages(xml, pages);

	// Group words by page index.
	std::map<int, std::vector<Word>> by_page;
	for (auto &pw : paged) {
		by_page[pw.page].push_back(pw.w);
	}

	std::vector<Table> tables;
	for (auto &kv : by_page) {
		auto grid = ReconstructPageGrid(kv.second);
		if (grid.size() >= 2 && !grid.empty() && grid.front().size() >= 2) {
			Table t;
			// `first_page` shifts the logical page number back to the document
			// page when a range was requested; default to the local index.
			int base = (first_page > 0) ? (first_page - 1) : 0;
			t.page = base + kv.first;
			t.rows = std::move(grid);
			tables.push_back(std::move(t));
		}
	}

	return tables;
}

} // namespace pdfcli

// ===========================================================================
// Standalone test harness
// ===========================================================================
#ifdef PDFCLI_STANDALONE_TEST

#include <cstdio>
#include <iostream>

int main(int argc, char **argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <pdf-path>\n", argv[0]);
		return 2;
	}
	const std::string path = argv[1];

	std::cout << "=== tool availability ===\n";
	for (const char *t : {"pdftotext", "pdftohtml", "pdftocairo", "tesseract"}) {
		std::cout << "  " << t << ": " << (pdfcli::ToolExists(t) ? "FOUND" : "MISSING") << "\n";
	}

	std::cout << "\n=== PdfToText(physical), first 200 chars ===\n";
	try {
		std::string txt = pdfcli::PdfToText(path, "physical", 0, 0, "");
		std::cout << txt.substr(0, 200) << "\n";
		std::cout << "  [total chars: " << txt.size() << "]\n";
	} catch (const std::exception &e) {
		std::cout << "  ERROR: " << e.what() << "\n";
	}

	std::cout << "\n=== ReconstructTables ===\n";
	try {
		std::vector<pdfcli::Table> tables = pdfcli::ReconstructTables(path, 0, 0, "");
		std::cout << "  tables found: " << tables.size() << "\n";
		if (!tables.empty()) {
			const auto &t = tables.front();
			std::cout << "  first table on page " << t.page << " has " << t.rows.size() << " rows, "
			          << (t.rows.empty() ? 0 : t.rows.front().size()) << " cols\n";
			for (size_t r = 0; r < t.rows.size(); ++r) {
				std::cout << "    row " << r << ": ";
				for (size_t c = 0; c < t.rows[r].size(); ++c) {
					if (c) {
						std::cout << " | ";
					}
					std::cout << "[" << t.rows[r][c] << "]";
				}
				std::cout << "\n";
			}
		}
	} catch (const std::exception &e) {
		std::cout << "  ERROR: " << e.what() << "\n";
	}

	return 0;
}

#endif // PDFCLI_STANDALONE_TEST
