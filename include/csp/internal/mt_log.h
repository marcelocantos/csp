#ifndef INCLUDED__csp__mt_log_h
#define INCLUDED__csp__mt_log_h

#include <cstdarg>
#include <string>

#define CSP_LOG(       logger, fmt, ...)   CSP_LOG_  (    2+1 , (logger), (fmt), ##__VA_ARGS__)
#define CSP_LOG_START( logger, fmt, ...)   CSP_LOG_  (      1 , (logger), (fmt), ##__VA_ARGS__)
#define CSP_LOG_CONT(  logger, fmt, ...)   CSP_LOG_  (       0, (logger), (fmt), ##__VA_ARGS__)
#define CSP_LOG_FINISH(logger, fmt, ...)   CSP_LOG_  (    2   , (logger), (fmt), ##__VA_ARGS__)

#define CSP_GRIPE(     logger, fmt, ...)   CSP_GRIPE_(    2+1 , (logger), (fmt), ##__VA_ARGS__)
#define BRAC_FATAL(     logger, fmt, ...)   CSP_GRIPE_(  4+2+1 , (logger), (fmt), ##__VA_ARGS__)

#define BRAC_SCOPE(logger, func, fmt, ...)        BRAC_SCOPE_((logger), __FILE__, __LINE__, (func), (fmt), ##__VA_ARGS__)

#define CSP__DETAIL__SOURCE_ROOT__(root) #root
#define CSP__DETAIL__SOURCE_ROOT_(root) CSP__DETAIL__SOURCE_ROOT__(root)
#define CSP__DETAIL__SOURCE_ROOT CSP__DETAIL__SOURCE_ROOT_(BRAC_SOURCE_ROOT)

namespace csp {

    class Logger {
    public:
        static constexpr char const * suffix(char const * file) {
            char const srcroot[] = CSP__DETAIL__SOURCE_ROOT;
            return file + sizeof(srcroot);
        }

        Logger(char const * component);
        ~Logger();

        explicit operator bool() const { return enabled_; }

        void log(int flags, char const * srcroot, char const * file, int line, char const * fmt, ...);
        void vlog(int flags, char const * srcroot, char const * file, int line, char const * fmt, va_list ap);

        static void regapp(char const * vendor, char const * appname);

        static void dump_stack();

    private:
        struct Rep;

        union {
            bool enabled_;
            struct { char a, b; };
        };
        Rep* rep_;

        static void dump_stack_(bool truncate);
    };

    class LogScope {
    public:
        LogScope(Logger & logger, char const * prefix, char const * file, int line, char const * func, char const * fmt, ...);
        ~LogScope();

    private:
        Logger & logger_;
        bool log_;
        std::string prefix_, file_, func_;
        int line_;
    };

}

#define CSP_LOG_(  flags, logger, fmt, ...) CSP_LOG___(flags, logger, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define CSP_LOG__( flags, logger, file, line, fmt, ...) CSP_LOG___(flags, logger, file, line, fmt, ##__VA_ARGS__)
#define CSP_LOG___(flags, logger, file, line, fmt, ...) \
    ((void)(logger && (logger.log(flags, CSP__DETAIL__SOURCE_ROOT, file, line, fmt, ##__VA_ARGS__), true)))

#define CSP_GRIPE_(  flags, logger, fmt, ...) CSP_GRIPE___(flags, logger, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define CSP_GRIPE__( flags, logger, file, line, fmt, ...) CSP_GRIPE___(flags, logger, file, line, fmt, ##__VA_ARGS__)
#define CSP_GRIPE___(flags, logger, file, line, fmt, ...) \
    (logger.log(flags, CSP__DETAIL__SOURCE_ROOT, file, line, fmt, ##__VA_ARGS__))

#define BRAC_SCOPE_( logger, file, line, func, fmt, ...) BRAC_SCOPE__(logger, file, line, func, fmt, ##__VA_ARGS__)
#define BRAC_SCOPE__(logger, file, line, func, fmt, ...) \
    LogScope csp__logScope__##line(logger, CSP__DETAIL__SOURCE_ROOT, file, line, func, fmt, func, ##__VA_ARGS__);

#endif // INCLUDED__csp__mt_log_h
