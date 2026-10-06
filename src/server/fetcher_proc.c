#include "server/fetcher_proc.h"

#include "common/log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_BODY (4 * 1024 * 1024)
#define POLL_SLICE_MS 200

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int fetcher_start(Fetcher *f, const char *exe_path, int http_timeout_s)
{
    f->pid = -1;
    f->to_fd = -1;
    f->from_fd = -1;
    f->timeout_s = http_timeout_s;

    if (access(exe_path, X_OK) != 0)
    {
        LOG_ERROR("Fetcher: %s går inte att köra: %s", exe_path, strerror(errno));
        return -1;
    }

    int request_pipe[2];  /* server -> fetcher */
    int response_pipe[2]; /* fetcher -> server */
    if (pipe(request_pipe) != 0)
    {
        LOG_ERROR("Fetcher: pipe() misslyckades: %s", strerror(errno));
        return -1;
    }
    if (pipe(response_pipe) != 0)
    {
        LOG_ERROR("Fetcher: pipe() misslyckades: %s", strerror(errno));
        close(request_pipe[0]);
        close(request_pipe[1]);
        return -1;
    }

    /* Formateras före fork(): i en flertrådad process får barnet bara
     * använda async-signal-säkra funktioner fram till exec. */
    char timeout_arg[16];
    snprintf(timeout_arg, sizeof(timeout_arg), "%d", http_timeout_s);

    pid_t pid = fork();
    if (pid < 0)
    {
        LOG_ERROR("Fetcher: fork() misslyckades: %s", strerror(errno));
        close(request_pipe[0]);
        close(request_pipe[1]);
        close(response_pipe[0]);
        close(response_pipe[1]);
        return -1;
    }

    if (pid == 0)
    {
        /* Barnprocess: koppla pipes till stdin/stdout och stäng originalen. */
        dup2(request_pipe[0], STDIN_FILENO);
        dup2(response_pipe[1], STDOUT_FILENO);
        close(request_pipe[0]);
        close(request_pipe[1]);
        close(response_pipe[0]);
        close(response_pipe[1]);
        execl(exe_path, "solvakt-fetcher", timeout_arg, (char *)NULL);
        _exit(127);
    }

    /* Förälder: stäng de ändar som bara barnet använder, annars kommer
     * aldrig EOF fram när någon sida stänger. */
    close(request_pipe[0]);
    close(response_pipe[1]);
    f->pid = pid;
    f->to_fd = request_pipe[1];
    f->from_fd = response_pipe[0];

    /* En omstartad fetcher ska inte ärva den gamla fetcherns pipes. */
    fcntl(f->to_fd, F_SETFD, FD_CLOEXEC);
    fcntl(f->from_fd, F_SETFD, FD_CLOEXEC);

    LOG_INFO("Fetcher: startad med PID %d", (int)pid);
    return 0;
}

static int write_all(int fd, const char *data, size_t len)
{
    while (len > 0)
    {
        ssize_t n = write(fd, data, len);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1; /* EPIPE: fetchern har avslutats */
        }
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Läser exakt len byte före deadline. Returnerar 0, eller -1 vid EOF, fel,
 * timeout eller avbruten körning. */
static int read_all(int fd, char *buf, size_t len, long long deadline_ms,
                    const atomic_bool *running)
{
    while (len > 0)
    {
        if (running != NULL && !atomic_load(running))
            return -1;
        if (now_ms() > deadline_ms)
            return -1;

        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int ready = poll(&pfd, 1, POLL_SLICE_MS);
        if (ready < 0 && errno != EINTR)
            return -1;
        if (ready <= 0)
            continue;

        ssize_t n = read(fd, buf, len);
        if (n == 0)
            return -1; /* EOF: skrivänden är stängd */
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

int fetcher_request(Fetcher *f, const char *url, char **body, size_t *len,
                    const atomic_bool *running)
{
    *body = NULL;
    *len = 0;
    if (f->pid <= 0)
        return FETCH_DEAD;

    char request[2048];
    int request_len = snprintf(request, sizeof(request), "GET %s\n", url);
    if (request_len < 0 || (size_t)request_len >= sizeof(request))
    {
        LOG_ERROR("Fetcher: för lång URL");
        return FETCH_FAILED;
    }
    if (write_all(f->to_fd, request, (size_t)request_len) != 0)
    {
        LOG_WARN("Fetcher: kunde inte skriva till pipen: %s", strerror(errno));
        return FETCH_DEAD;
    }

    /* Fetchern har själv en HTTP-timeout; marginalen täcker uppstart. */
    long long deadline = now_ms() + (long long)(f->timeout_s + 5) * 1000;

    /* Svarshuvudet är en rad: "OK <n>" eller "ERR <text>". */
    char header[256];
    size_t header_len = 0;
    for (;;)
    {
        char c;
        if (read_all(f->from_fd, &c, 1, deadline, running) != 0)
        {
            LOG_WARN("Fetcher: inget svar (stängd pipe eller timeout)");
            return FETCH_DEAD;
        }
        if (c == '\n')
            break;
        if (header_len + 1 >= sizeof(header))
        {
            LOG_WARN("Fetcher: för långt svarshuvud");
            return FETCH_DEAD;
        }
        header[header_len++] = c;
    }
    header[header_len] = '\0';

    if (strncmp(header, "ERR ", 4) == 0)
    {
        LOG_WARN("Fetcher: %s (%s)", header + 4, url);
        return FETCH_FAILED;
    }

    char *end = NULL;
    unsigned long body_len = 0;
    if (strncmp(header, "OK ", 3) == 0)
        body_len = strtoul(header + 3, &end, 10);
    if (end == NULL || end == header + 3 || *end != '\0' || body_len > MAX_BODY)
    {
        /* Strömmen går inte längre att tolka; enda säkra vägen är omstart. */
        LOG_WARN("Fetcher: ogiltigt svarshuvud '%s'", header);
        return FETCH_DEAD;
    }

    char *data = malloc(body_len + 1);
    if (data == NULL)
        return FETCH_DEAD;
    if (read_all(f->from_fd, data, body_len, deadline, running) != 0)
    {
        LOG_WARN("Fetcher: svaret avbröts efter huvudet");
        free(data);
        return FETCH_DEAD;
    }
    data[body_len] = '\0';

    *body = data;
    *len = body_len;
    return FETCH_OK;
}

int fetcher_reap_if_dead(Fetcher *f)
{
    if (f->pid <= 0)
        return 0;

    int status = 0;
    if (waitpid(f->pid, &status, WNOHANG) == 0)
        return 0; /* lever fortfarande */

    if (WIFSIGNALED(status))
        LOG_WARN("Fetcher: PID %d dog oväntat av signal %d", (int)f->pid, WTERMSIG(status));
    else
        LOG_WARN("Fetcher: PID %d avslutades oväntat med kod %d", (int)f->pid,
                 WIFEXITED(status) ? WEXITSTATUS(status) : -1);

    close(f->to_fd);
    close(f->from_fd);
    f->to_fd = -1;
    f->from_fd = -1;
    f->pid = -1;
    return 1;
}

void fetcher_stop(Fetcher *f)
{
    if (f->to_fd >= 0)
        close(f->to_fd); /* fetchern får EOF på stdin och avslutar sig själv */
    if (f->from_fd >= 0)
        close(f->from_fd);
    f->to_fd = -1;
    f->from_fd = -1;

    if (f->pid <= 0)
        return;

    /* Vänta upp till en sekund på frivilligt avslut, döda annars. Utan
     * waitpid skulle barnet bli kvar som zombie. */
    int status = 0;
    pid_t done = 0;
    for (int i = 0; i < 20 && done == 0; i++)
    {
        done = waitpid(f->pid, &status, WNOHANG);
        if (done == 0)
        {
            struct timespec pause = {0, 50 * 1000 * 1000};
            nanosleep(&pause, NULL);
        }
    }
    if (done == 0)
    {
        kill(f->pid, SIGKILL);
        waitpid(f->pid, &status, 0);
    }

    if (WIFEXITED(status))
        LOG_INFO("Fetcher: PID %d avslutad med kod %d", (int)f->pid, WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        LOG_INFO("Fetcher: PID %d avslutad av signal %d", (int)f->pid, WTERMSIG(status));
    f->pid = -1;
}
