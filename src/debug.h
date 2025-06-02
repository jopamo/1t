#pragma once
#include <QLoggingCategory>
#include <QDebug>

extern bool g_debugMode;

Q_DECLARE_LOGGING_CATEGORY(oneTermDbg)

#if ENABLE_DEBUG
#  define DBG()                                                             \
    if (!g_debugMode)                                                      \
        ;                                                                  \
    else                                                                   \
        qCDebug(oneTermDbg)
#else
class NullDebugStream {
public:
    template <typename T> NullDebugStream &operator<<(const T &) { return *this; }
};
static inline NullDebugStream qNullDebugStream() { return {}; }
#  define DBG() qNullDebugStream()
#endif
