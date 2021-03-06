#include <iostream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <string>
#include <algorithm>

#include <boost/range.hpp>
#include <boost/range/adaptors.hpp>
#include <boost/range/algorithm.hpp>
#include <boost/range/numeric.hpp>
#include <boost/spirit/include/qi.hpp>
#include <boost/lexical_cast.hpp>

#include <cstddef>
#include <cstdint>

#include <cerrno>
#include <csignal>
#include <cstring>

#include <unistd.h>
#include <fcntl.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/syscall.h>
#include <sys/reg.h>
#include <syslog.h>

#define read_reg(pid, name) ptrace(PTRACE_PEEKUSER, pid, offsetof(user_regs_struct, name), 0)
#define write_reg(pid, name, val) ptrace(PTRACE_POKEUSER, pid, offsetof(user_regs_struct, name), val)

namespace wandbox {
	template <typename T, bool ...>
	struct do_to_string_impl;
	template <typename T, bool ...B>
	struct do_to_string_impl<T, true, B...> {
		// use implicit conversion
		static std::string call(const T &x) { return x; }
	};
	template <typename T, bool ...B>
	struct do_to_string_impl<T, false, true, B...> {
		// use std::to_string
		static std::string call(const T &x) { return std::to_string(x); }
	};
	template <typename T, bool ...B>
	struct do_to_string_impl<T, false, false, true, B...> {
		// use boost::lexical_cast
		static std::string call(const T &x) { return boost::lexical_cast<std::string>(x); }
	};
	template <typename T>
	struct do_to_string {
		template <typename U, typename = decltype(std::to_string(std::declval<U>()))>
		static std::true_type check_std_to_string(U *);
		static std::false_type check_std_to_string(void *);
		typedef decltype(check_std_to_string((T*)0)) use_std_to_string;
		template <typename U, typename = decltype(boost::lexical_cast(std::declval<U>()))>
		static std::true_type check_lexical_cast(U *);
		static std::false_type check_lexical_cast(void *);
		typedef decltype(check_lexical_cast((T*)0)) use_lexical_cast;
		static std::string call(const T &x) {
			return do_to_string_impl<
				T,
				std::is_convertible<T, std::string>::type::value,
				decltype(check_std_to_string((T*)0))::type::value,
				decltype(check_lexical_cast((T*)0))::type::value
			>::call(x);
		}
	};
	template <typename ...Args>
	void trace(int pri, const char *const fmt, const Args &...args) {
		syslog(pri, fmt, do_to_string<Args>::call(args).c_str()...);
	}
	namespace rng {
		using namespace boost::range;
		using namespace boost::adaptors;
		using boost::accumulate;
		template <typename SinglePassRange>
		typename boost::range_value<SinglePassRange>::type accumulate1(const SinglePassRange &src) {
			if (boost::empty(src)) throw std::runtime_error("empty range");
			auto ite = boost::begin(src);
			const auto &first = *ite++;
			return std::accumulate(ite, boost::end(src), first);
		}
		template <typename SinglePassRange, typename BinaryOp>
		typename boost::range_value<SinglePassRange>::type accumulate1(const SinglePassRange &src, const BinaryOp &op) {
			if (boost::empty(src)) throw std::runtime_error("empty range");
			auto ite = boost::begin(src);
			const auto &first = *ite++;
			return std::accumulate(ite, boost::end(src), first, op);
		}
	}
	template <typename AddrT, typename DataT>
	inline long ptrace(__ptrace_request req, pid_t pid, AddrT addr, DataT data) {
		return ::ptrace(req, pid, (void *)(size_t)addr, (void *)(size_t)data);
	}
	std::string read_cstring_from_process(pid_t pid, uintptr_t addr) {
		std::vector<char> buf;
		while (std::find(buf.begin(), buf.end(), 0) == buf.end()) {
			const long d = ptrace(PTRACE_PEEKDATA, pid, addr, 0);
			for (unsigned n = 0; n < sizeof(d); ++n) buf.push_back(d>>(n*8));
			addr += sizeof(d);
		}
		return buf.data();
	}
	/// パスコンポーネントを正規化する(./ と ../ を削除する)
	/// @param src 入力パス(絶対パスのみ)
	/// @return 正規化パスまたは空文字列
	std::string canonicalize_path(const std::string &src) {
		// realpath(3) は symlink を解決してしまう
		// boost::filesystem::canonical はファイルが存在しないと失敗する
		// ので、/proc/self の場合どちらも役に立たない
		if (src.empty() || src[0] != '/') return "";
		const bool absolutep = src[0] == '/';

		std::vector<std::string> dirs;
		auto ite = src.begin();
		if (absolutep) ++ite;
		const auto end = src.end();
		namespace qi = boost::spirit::qi;
		qi::parse(ite, end, *(qi::char_ - '/') % '/', dirs);
		return (absolutep ? "/" : "") + rng::accumulate1(
			rng::accumulate(
				dirs | rng::filtered([](const std::string &s) { return !s.empty() && s != "."; }),
				std::vector<std::string>(),
				[absolutep](std::vector<std::string> a, std::string s) -> std::vector<std::string> {
					if (s == "..") {
						if (absolutep) {
							if (!a.empty()) a.pop_back();
						} else {
							if (a.empty() || a.back() == "..") a.push_back("..");
							else a.pop_back();
						}
					} else {
						a.emplace_back(move(s));
					}
					return move(a);
				}),
			[](const std::string &a, const std::string &b) { return a + "/" + b; });
	}
	inline bool starts_with(const std::string &tested, const std::string &prefix) {
		return tested.length() >= prefix.length() && tested.substr(0, prefix.length()) == prefix;
	}
	/// ファイルを開かせてもよいかどうか判断する
	/// @return 開かせてよい時 @true
	bool check_openable_file_p(const std::string &path, int flags, mode_t) {
		// TODO: この辺の許可リストは外から読むようにしたい
		static const auto allowed_paths = []() -> std::vector<std::string> {
			std::vector<std::string> l = {"/etc/ld.so.cache", "/dev/null", "/dev/zero", "/proc/stat"};
			std::sort(l.begin(), l.end());
			return l;
		}();
		static const std::vector<std::string> allowed_prefixes = { "/lib", "/usr/lib", "/proc/self" };
		const auto realpath = canonicalize_path(path);
		const int openmode = flags & O_ACCMODE;
		if (rng::binary_search(allowed_paths, path) && openmode == O_RDONLY) return true;
		if (rng::find_if(allowed_prefixes, std::bind(starts_with, realpath, std::placeholders::_1)) != allowed_prefixes.end() && openmode == O_RDONLY) return true;
		if (!starts_with(realpath, "..") && !starts_with(realpath, "/")) return true;
		trace(LOG_DEBUG, "opening path '%s' is not allowed", path);
		return false;
	}
	/// システムコールの呼出しを許すかどうか判断する
	/// @return 呼び出しを許す時 @true
	bool check_syscall_permission(pid_t pid, user_regs_struct &reg) {
		switch (reg.orig_rax) {
			// ここにずらーーーーーーっと許可syscallを並べる
			// Aは場合によってはblockしたい
			// Bは場合によっては許可したい
		case SYS_open: // B
			return check_openable_file_p(read_cstring_from_process(pid, reg.rdi), reg.rsi, reg.rdx);

		case SYS_read: // B
		case SYS_write: // B

		case SYS_close:
		case SYS_stat:
		case SYS_fstat:
		case SYS_lstat:
		case SYS_poll:

		case SYS_lseek:
		case SYS_mmap:
		case SYS_mprotect: // A
		case SYS_munmap:
		case SYS_brk:
		case SYS_rt_sigaction:
		case SYS_rt_sigprocmask:
		case SYS_rt_sigreturn:

		case SYS_pread64:
		case SYS_pwrite64:
		case SYS_readv:
		case SYS_writev:
		case SYS_access:
		case SYS_pipe:
		case SYS_select:

		case SYS_sched_yield:
		case SYS_mremap:
		case SYS_mincore:
		case SYS_madvise:

		case SYS_pause:
		case SYS_nanosleep:
		case SYS_getitimer:
		case SYS_alarm:
		case SYS_setitimer:
		case SYS_getpid:

		case SYS_exit:
		case SYS_uname:

		case SYS_gettimeofday:
		case SYS_getrlimit:
		case SYS_getrusage:
		case SYS_sysinfo:
		case SYS_times:

		case SYS_rt_sigpending:
		case SYS_rt_sigtimedwait:
		case SYS_rt_sigqueueinfo:
		case SYS_rt_sigsuspend:
		case SYS_sigaltstack:

		case SYS_arch_prctl:

		case SYS_clock_gettime:
		case SYS_clock_nanosleep:
		case SYS_exit_group:

		case SYS_set_robust_list:
		case SYS_set_tid_address:
		case SYS_timer_create:
		case SYS_timer_delete:
		case SYS_timer_settime:

		case SYS_fcntl:
		case SYS_futex:
		case SYS_getcwd:
		case SYS_sched_getaffinity:
		case SYS_tgkill:
		case SYS_clone: // B
			// TODO: clone(2) した先を ptrace(2) 出来るようにする
			return true;
		}
		return false;
	}
	int main(int argc, char **argv) {
		if (argc < 2) return -1;

		sigset_t sigs;
		sigfillset(&sigs);
		sigdelset(&sigs, SIGKILL);
		sigdelset(&sigs, SIGSTOP);
		sigprocmask(SIG_BLOCK, &sigs, 0);
		pid_t pid = ::fork();
		if (pid == 0) {
			sigprocmask(SIG_UNBLOCK, &sigs, 0);
			ptrace(PTRACE_TRACEME, 0, 0, 0);
			++argv;
			return ::execv(*argv, argv);
		} else if (pid > 0) {
			bool in_syscall = false;
			bool noperm = false;
			::openlog("ptracer", LOG_PID|LOG_CONS, LOG_AUTHPRIV);
			::waitpid(pid, 0, 0);
			ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD);
			ptrace(PTRACE_SYSCALL, pid, 0, 0);
			while (1) {
				{
					siginfo_t siginfo;
					const int sig = sigwaitinfo(&sigs, &siginfo);
					if (sig == -1) return -1;
					if (sig != SIGCHLD) {
						ptrace(PTRACE_SYSCALL, siginfo.si_pid, 0, siginfo.si_signo);
						continue;
					}
				}

				siginfo_t siginfo;
				siginfo.si_pid = 0;
				if (::waitid(P_ALL, 0, &siginfo, WEXITED|WSTOPPED)) return -1;
				if (siginfo.si_pid == 0) return siginfo.si_status;
				if (siginfo.si_code == CLD_EXITED) return siginfo.si_status;
				if (siginfo.si_code == CLD_KILLED) return -siginfo.si_status;
				if (siginfo.si_code == CLD_DUMPED) return -siginfo.si_status;

				if (siginfo.si_status != (SIGTRAP|0x80)) {
					trace(LOG_DEBUG, "child signaled: %s", (siginfo.si_status&~0x80));
					ptrace(PTRACE_SYSCALL, siginfo.si_pid, 0, siginfo.si_status & ~0x80);
					continue;
				}

				user_regs_struct reg;
				ptrace(PTRACE_GETREGS, siginfo.si_pid, 0, &reg);
				if (in_syscall) {
					trace(LOG_DEBUG, "leave syscall %s (%s)", reg.orig_rax, reg.rax);
					if (noperm) write_reg(siginfo.si_pid, rax, -EPERM);
					noperm = false;
				} else {
					noperm = !check_syscall_permission(siginfo.si_pid, reg);
					trace(LOG_DEBUG, "enter syscall %s %s(%s, %s, %s, %s, %s, %s)", noperm?"[blocked]":"", reg.orig_rax, reg.rdi, reg.rsi, reg.rdx, reg.r10, reg.r8, reg.r9);
					if (noperm) write_reg(siginfo.si_pid, orig_rax, -1);
				}
				in_syscall = !in_syscall;
				ptrace(PTRACE_SYSCALL, pid, 0, 0);
			}
			return 0;
		} else {
			return -1;
		}
	}
}

int main(int argc, char **argv) {
	return wandbox::main(argc, argv);
}
