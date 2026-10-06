#include "common/log.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static FILE *g_file = NULL;
static LogLevel g_level = LOG_LVL_INFO;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *level_name(LogLevel level)
{
    switch (level)
    {
        case LOG_LVL_DEBUG: return "DEBUG";
        case LOG_LVL_INFO:  return "INFO ";
        case LOG_LVL_WARN:  return "WARN ";
        case LOG_LVL_ERROR: return "ERROR";
    }
    return "?";
}

LogLevel log_level_from_string(const char *s)
{
    if (s == NULL)                   return LOG_LVL_INFO;
    if (strcasecmp(s, "debug") == 0) return LOG_LVL_DEBUG;
    if (strcasecmp(s, "warn") == 0)  return LOG_LVL_WARN;
    if (strcasecmp(s, "error") == 0) return LOG_LVL_ERROR;
    return LOG_LVL_INFO;
}

int log_init(const char *path, LogLevel level)
{
    g_level = level;
    if (path == NULL || path[0] == '\0')
        return 0;

    /* "e" sätter O_CLOEXEC så att fetcher-processen inte ärver loggfilen. */
    g_file = fopen(path, "ae");
    return g_file != NULL ? 0 : -1;
}

void log_close(void)
{
    pthread_mutex_lock(&g_mutex);
    if (g_file != NULL)
    {
        fclose(g_file);
        g_file = NULL;
    }
    pthread_mutex_unlock(&g_mutex);
}

void log_write(LogLevel level, const char *file, int line, const char *fmt, ...)
{
    if (level < g_level)
        return;

    char stamp[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

    char message[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    const char *base = strrchr(file, '/');
    base = base != NULL ? base + 1 : file;

    pthread_mutex_lock(&g_mutex);
    if (g_file != NULL)
    {
        fprintf(g_file, "[%s] %s %s:%d: %s\n", stamp, level_name(level), base, line, message);
        fflush(g_file);
    }
    /* Utan loggfil går allt till stderr, annars bara varningar och fel. */
    if (g_file == NULL || level >= LOG_LVL_WARN)
        fprintf(stderr, "[%s] %s %s\n", stamp, level_name(level), message);
    pthread_mutex_unlock(&g_mutex);
}
