#pragma once

// Protocol tests include per-protocol headers (`<csp/net.h>`, `<csp/http.h>`,
// …). The dist drop-in folds those into the single `dist/csp.h`, so the same
// includes do not exist when `CSP_INCLUDE=dist`. Map them here so one test
// source compiles against both layouts.

#if __has_include(<csp/net.h>)
#include <csp/net.h>
#include <csp/http.h>
#include <csp/http2.h>
#include <csp/http3.h>
#include <csp/ws.h>
#ifdef CSP_TLS
#include <csp/quic.h>
#endif
#else
#include "csp.h"
#endif
