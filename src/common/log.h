#ifndef SOLVAKT_LOG_H
#define SOLVAKT_LOG_H

/* Trådsäker loggning med nivå och tidsstämpel. */

typedef enum
{
    LOG_LVL_DEBUG,
    LOG_LVL_INFO,
    LOG_LVL_WARN,
    LOG_LVL_ERROR
} LogLevel;

/* Öppnar loggfilen (skapas vid behov). Tom eller NULL path loggar bara till
 * stderr. Returnerar 0, eller -1 om filen inte gick att öppna. */
int log_init(const char *path, LogLevel level);
void log_close(void);

/* Okänd sträng ger LOG_LVL_INFO. */
LogLevel log_level_from_string(const char *s);

void log_write(LogLevel level, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

#define LOG_DEBUG(...) log_write(LOG_LVL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  log_write(LOG_LVL_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  log_write(LOG_LVL_WARN, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) log_write(LOG_LVL_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#endif
