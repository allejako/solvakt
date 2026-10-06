/* solvakt-server - hämtar väder och spotpriser, beräknar en energiplan och
 * svarar klienter över en Unix domain socket.
 *
 *   solvakt-server [-c konfigfil]
 *
 * Stoppas med SIGINT (Ctrl-C) eller SIGTERM.
 */

#include "common/config.h"
#include "common/log.h"
#include "server/server.h"

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_stop_signal(int sig)
{
    (void)sig;
    g_stop = 1; /* det enda en signalhanterare säkert kan göra */
}

static void install_signal_handlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    /* Skrivning till en stängd pipe eller socket ska ge EPIPE, inte döda
     * processen. */
    signal(SIGPIPE, SIG_IGN);
}

/* Fetchern ligger i samma katalog som serverns binär. */
static int find_fetcher(char *path, size_t path_len)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 0)
        return -1;
    exe[n] = '\0';
    int written = snprintf(path, path_len, "%s/solvakt-fetcher", dirname(exe));
    return written > 0 && (size_t)written < path_len ? 0 : -1;
}

/* Skapar katalogen som loggfilen ligger i om den saknas. */
static void ensure_log_dir(const char *log_file)
{
    char copy[sizeof(((Config *)0)->log_file)];
    snprintf(copy, sizeof(copy), "%s", log_file);
    const char *dir = dirname(copy);
    if (strcmp(dir, ".") != 0 && strcmp(dir, "/") != 0)
        mkdir(dir, 0755);
}

int main(int argc, char **argv)
{
    const char *config_path = "config/solvakt.conf";
    int opt;
    while ((opt = getopt(argc, argv, "c:h")) != -1)
    {
        if (opt == 'c')
        {
            config_path = optarg;
        }
        else
        {
            fprintf(stderr, "Användning: %s [-c konfigfil]\n", argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    /* Spotpriserna gäller svenska kalenderdygn, så all datumräkning och
     * loggning sker i svensk tid oavsett maskinens inställning. */
    setenv("TZ", "Europe/Stockholm", 1);
    tzset();

    Config cfg;
    char err[256];
    config_defaults(&cfg);
    if (config_load(config_path, &cfg, err, sizeof(err)) != 0)
    {
        fprintf(stderr, "Konfigurationsfel: %s\n", err);
        return 1;
    }

    if (cfg.log_file[0] != '\0')
        ensure_log_dir(cfg.log_file);
    if (log_init(cfg.log_file, log_level_from_string(cfg.log_level)) != 0)
        fprintf(stderr, "Varning: kan inte öppna loggfilen %s (%s), loggar till stderr\n",
                cfg.log_file, strerror(errno));

    char fetcher_path[PATH_MAX];
    if (find_fetcher(fetcher_path, sizeof(fetcher_path)) != 0)
    {
        LOG_ERROR("Kan inte hitta solvakt-fetcher");
        log_close();
        return 1;
    }

    install_signal_handlers();

    LOG_INFO("Solvakt startar (PID %d, konfiguration %s)", (int)getpid(), config_path);
    Server *server = server_create(&cfg, fetcher_path);
    if (server == NULL)
    {
        LOG_ERROR("Servern kunde inte startas");
        log_close();
        return 1;
    }

    int rc = server_run(server, &g_stop);
    server_destroy(server);

    LOG_INFO("Solvakt avslutad");
    log_close();
    return rc == 0 ? 0 : 1;
}
