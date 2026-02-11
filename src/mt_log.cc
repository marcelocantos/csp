#include <csp/internal/mt_log.h>

#include <csp/internal/on_scope_exit.h>

#include <cxxabi.h>
#include <errno.h>
#include <execinfo.h>
#include <sys/syslimits.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace csp;

extern "C" {

    // Return the current status message for the current microthread.
    char const * csp_getdescr(void* thr);

}


namespace {

    char g_vendor[256];
    char g_appname[256];
    bool g_registered = true;  // TODO: Fix/use regapp.

}


namespace csp {

    struct Logger::Rep {
        Rep(bool & enabled, const char * component);
        ~Rep() { free((void *)component_); }

        bool& enabled_;
        char const * component_;
        Rep* next_;

        static Rep * prereg_;

        bool reg(std::string const & component);
    };

    Logger::Rep * Logger::Rep::prereg_;

    Logger::Rep::Rep(bool & enabled, char const * component) : enabled_(enabled) {
        if (g_registered) {
            enabled_ = reg(component);
        } else {
            component_ = strdup(component);
            next_ = prereg_;
            prereg_ = this;
        }
    }

    bool Logger::Rep::reg(std::string const & component) {
        static auto components_re = []{
            std::regex result;
            if (auto env = getenv("BB_LOG")) {
                std::cerr << "BB_LOG=" << env << "\n";
                result.assign(env);
            }
            return result;
        }();
        return std::regex_match(component, components_re);
    }

    Logger::Logger(char const * component) : rep_(new Rep(enabled_, component)) { }

    Logger::~Logger() { }

    void Logger::log(int flags, char const * srcroot, char const * file, int line, char const * fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        vlog(flags, srcroot, file, line, fmt, ap);
        va_end(ap);
    }

    void Logger::vlog(int flags, char const * srcroot, char const * file, int line, char const * fmt, va_list ap) {
        enum { bufsize = 100 << 10 };
        auto buf = std::make_unique<char[]>(bufsize);

        std::string prefix = srcroot;
        std::string path = file;
        if (prefix == path.substr(0, prefix.length())) {
            file += prefix.length();
            prefix = "$(SRCROOT)";
        } else {
            auto i = path.rfind("/csp/");
            if (i != path.npos) {
                file += i;
                prefix = "…";
            } else {
                prefix = "";
            }
        }

        char* cp = buf.get();
        int len = bufsize - 1; // '- 1' leaves room for newline at the end.

        auto t = std::chrono::high_resolution_clock::now();
        static auto t0 = t;
        double dt = std::chrono::duration<double>(t - t0).count();

        auto advance = [&](int n) {
            cp += n;
            len -= n;
            return cp - n;
        };

        if (flags & 1) {
            auto descr = csp_getdescr(nullptr);
            advance(snprintf(cp, len, "%s%s:%d: %.3f [%s] ", prefix.c_str(), file, line, dt, descr));
        }

        if (flags & 8) {
            advance(snprintf(cp, len, "{errno = %u}", errno));
        }

        advance(vsnprintf(cp, len, fmt, ap));

        /*{
          LARGE_INTEGER ctr, freq;
          QueryPerformanceCounter(&ctr);
          QueryPerformanceFrequency(&freq);
          double t = (double)ctr.QuadPart / (double)freq.QuadPart;
          std::wostringstream oss;
          oss.precision(12);
          oss << '[' << t << "] " << std::flush;
          str = oss.str() + str;
          }*/

        if ((flags & 2) && (!len || buf[len - 1] != '\n')) {
            *cp = '\n';
            advance(1);
        }
        std::cerr << buf.get();

        if (!len) std::cerr << "...";

        static Logger stack_traces("log/backtrace");

        if ((flags & 2) && stack_traces) {
            dump_stack_(true);
        }

        if (flags & 4) exit(1);
    }

    void Logger::regapp(char const * vendor, char const * appname) {
        strcpy(g_vendor, vendor);
        strcpy(g_appname, appname);
        g_registered = true;

        for (Rep* r = Rep::prereg_; r; r = r->next_) {
            r->reg(r->component_);
            free((void*)r->component_);
            r->component_ = strdup("");
        }
    }

    void Logger::dump_stack() {
        dump_stack_(false);
    }

    void Logger::dump_stack_(bool truncate) {
        //constexpr int n_slots = 256;
        //auto bt = std::make_unique<void *[]>(n_slots);
        //int n_frames = backtrace(bt.get(), n_slots);

        struct FP {
            FP * next;
            void * ret;
        };
        std::vector<void *> bt;
        bt.reserve(32);
        auto here = static_cast<FP *>(__builtin_frame_address(0));
        for (int i = 0; here && here->ret; ++i) {
            bt.push_back(here->ret);
            here = here->next;
        }

        auto symbols = mallocedResource(backtrace_symbols(bt.data(), static_cast<int>(bt.size())));

        static std::vector<std::string> last_bt;
        std::vector<std::string> curr_bt;

        bool done = false;
#if 0
        static size_t demangle_buf_size = 1024;
        static char * demangle_buf = static_cast<char *>(malloc(demangle_buf_size));
#endif

        for (size_t i = 1; !done && i < bt.size(); ++i) {
            std::string s = (*symbols)[i] + 4;
            auto plus = s.rfind('+');
            if (plus != s.npos) {
                auto sp = s.rfind(' ', plus - 2);
                if (sp != s.npos) {
                    auto sym = s.substr(sp + 1, plus - sp - 2);
#if 0
                    int status;
                    if (char * d = abi::__cxa_demangle(sym.c_str(), demangle_buf, &demangle_buf_size, &status)) {
                        s = s.substr(0, sp + 1) + d + s.substr(plus - 1);
                        if (demangle_buf != d) {
                            demangle_buf = d;
                        }
                    } else {
                        switch (status) {
                            case -1: throw std::bad_alloc();
                            case -3: throw std::runtime_error("EINVAL");
                        }
                    }
#endif

                    if (sym == "main") {
                        done = true;
                    }
                }
            }

            {
                std::vector<std::string> ignore{
                    "csp::Logger::",
                    "_ZN3csp6detailL9switch_toERNS0_11MicrothreadEl",
                    "std::__1::",
                    "<redacted>",
                };
                if (std::any_of(begin(ignore), end(ignore), [&](auto i) { return s.find(i) != s.npos; })) {
                    continue;
                }
            }

            {
                static std::vector<std::pair<std::string, std::string>> replacements{
                    {"'lambda'", "λ"},
                    {"::operator()()", "::()"},
                    {"λ()::()", "λ"},
                    {"(anonymous namespace)", "(anon)"},
                };
                for (auto & r : replacements) {
                    for (size_t pos; (pos = s.find(r.first)) != s.npos;) {
                        s = s.replace(pos, r.first.length(), r.second);
                    }
                }
            }
            if (s.find("testing::internal::") == s.npos &&
                s.find(" testing::Test") == s.npos)
            {
                char buf[5];
                snprintf(buf, sizeof(buf), "%3ld ", bt.size() - i - 1);
                curr_bt.push_back(buf + s);
            }
        }

        if (!curr_bt.empty()) {
            auto mm = std::mismatch(rbegin(last_bt), truncate ? rend(last_bt) : rbegin(last_bt),
                                    rbegin(curr_bt), rend(curr_bt));
            auto finish = (&*mm.second) + 1;
            for (auto i = &curr_bt.front(); i != finish; ++i) {
                std::cerr << "  " << *i << "\n";
            }
            last_bt = std::move(curr_bt);
        }
    }

    LogScope::LogScope(Logger & logger, char const * prefix, char const * file, int line, char const * func, char const * fmt, ...)
        : logger_(logger)
    {
        if ((log_ = bool(logger_))) {
            prefix_ = prefix;
            file_ = file;
            line_ = line;
            func_ = func;
            va_list ap;
            va_start(ap, fmt);
            logger_.vlog(2+1, prefix, file, line, (std::string(">>>> ENTER %s(") + fmt + ")").c_str(), ap);
            va_end(ap);
        }
    }

    LogScope::~LogScope() {
        if (log_) {
            logger_.log(2+1, prefix_.c_str(), file_.c_str(), line_, std::string("<<<< EXIT %s").c_str(), func_.c_str());
        }
    }

}
