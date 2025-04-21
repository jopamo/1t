#pragma once
#include <QLoggingCategory>
#include <QDebug>

extern bool g_debugMode;

Q_DECLARE_LOGGING_CATEGORY(oneTermDbg)

#ifdef DEBUG_LOGGING
#define DBG()         \
    if (!g_debugMode) \
        ;             \
    else              \
        qCDebug(oneTermDbg)
#else
#define DBG() ;
#endif
