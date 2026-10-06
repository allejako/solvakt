#include "server/server.h"

#include "common/log.h"
#include "common/queue.h"
#include "common/types.h"
#include "server/cache.h"
#include "server/compute.h"
#include "server/fetcher_proc.h"
#include "server/parse.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define QUEUE_CAPACITY       4
#define LISTEN_BACKLOG       16
#define POLL_SLICE_MS        200
#define CLIENT_IDLE_MS       30000
#define MAX_COMMAND          128
#define MIN_PLAN_SLOTS       4
#define RETRY_AFTER_ERROR_S  60

/* Rå svar från API:erna, på väg från fetch- till parse-steget. */
typedef struct
{
    char *weather;
    char *prices_today;
    char *prices_tomorrow; /* NULL tills morgondagens priser är publicerade */
} RawData;

/* Matchade kvartsintervall, på väg från parse- till compute-steget. */
typedef struct
{
    Slot slots[MAX_SLOTS];
    int  count;
} SlotBatch;

struct Server
{
    Config      cfg;
    char        fetcher_path[PATH_MAX];

    Fetcher     fetcher;
    Queue       raw_queue;
    Queue       slot_queue;
    Cache       cache;
    atomic_bool running;

    int         listen_fd;
    pthread_t   fetch_thread;
    pthread_t   parse_thread;
    pthread_t   compute_thread;

    /* Antal klienttrådar som fortfarande kör, för ordnad nedstängning. */
    pthread_mutex_t clients_mutex;
    pthread_cond_t  clients_done;
    int             clients;
};

static void raw_free(RawData *raw)
{
    if (raw == NULL)
        return;
    free(raw->weather);
    free(raw->prices_today);
    free(raw->prices_tomorrow);
    free(raw);
}

/* ── Fetch-steget ─────────────────────────────────────────────────── */

/* Väntar till nästa hämtning. Under tiden övervakas fetcher-processen så
 * att en kraschad fetcher städas upp och startas om direkt. */
static void sleep_while_running(Server *s, int seconds)
{
    const struct timespec slice = {0, POLL_SLICE_MS * 1000000L};
    for (long waited = 0; waited < seconds * 1000L && atomic_load(&s->running); waited += POLL_SLICE_MS)
    {
        nanosleep(&slice, NULL);
        if (fetcher_reap_if_dead(&s->fetcher))
            fetcher_start(&s->fetcher, s->fetcher_path, s->cfg.http_timeout_s);
    }
}

static void price_url(const Server *s, const struct tm *day, char *url, size_t url_len)
{
    snprintf(url, url_len, "%s/%04d/%02d-%02d_%s.json", s->cfg.price_api_url,
             day->tm_year + 1900, day->tm_mon + 1, day->tm_mday, s->cfg.price_region);
}

/* Hämtar väder och priser och lägger resultatet på rå-kön.
 * Returnerar 1 om en komplett datamängd skickades vidare. */
static int fetch_once(Server *s)
{
    if (s->fetcher.pid <= 0 &&
        fetcher_start(&s->fetcher, s->fetcher_path, s->cfg.http_timeout_s) != 0)
        return 0;

    /* Spotpriserna publiceras per svenskt kalenderdygn. Processens tidszon
     * är satt till Europe/Stockholm i main(). */
    time_t now = time(NULL);
    struct tm today, tomorrow;
    localtime_r(&now, &today);
    tomorrow = today;
    tomorrow.tm_mday += 1;
    tomorrow.tm_hour = 12;
    mktime(&tomorrow); /* normaliserar månads- och årsskiften */

    char weather_url[512], today_url[512], tomorrow_url[512];
    /* Gårdagen tas med eftersom svenskt dygn börjar före 00:00 UTC. */
    snprintf(weather_url, sizeof(weather_url),
             "%s?latitude=%.4f&longitude=%.4f"
             "&minutely_15=shortwave_radiation,cloud_cover,temperature_2m"
             "&timeformat=unixtime&timezone=UTC&past_days=1&forecast_days=2",
             s->cfg.weather_api_url, s->cfg.latitude, s->cfg.longitude);
    price_url(s, &today, today_url, sizeof(today_url));
    price_url(s, &tomorrow, tomorrow_url, sizeof(tomorrow_url));

    RawData *raw = calloc(1, sizeof(*raw));
    if (raw == NULL)
        return 0;

    size_t len = 0;
    int rc = fetcher_request(&s->fetcher, weather_url, &raw->weather, &len, &s->running);
    if (rc == FETCH_OK)
        rc = fetcher_request(&s->fetcher, today_url, &raw->prices_today, &len, &s->running);
    if (rc == FETCH_OK)
    {
        int rc_tomorrow = fetcher_request(&s->fetcher, tomorrow_url, &raw->prices_tomorrow, &len, &s->running);
        if (rc_tomorrow == FETCH_DEAD)
            rc = FETCH_DEAD;
        else if (rc_tomorrow == FETCH_FAILED)
            LOG_INFO("Fetch: morgondagens priser finns inte ännu, planen täcker innevarande dygn");
    }

    if (rc == FETCH_DEAD)
        fetcher_stop(&s->fetcher); /* startas om vid nästa försök */
    if (rc != FETCH_OK)
    {
        raw_free(raw);
        return 0;
    }

    LOG_INFO("Fetch: väder och priser hämtade");
    if (queue_push(&s->raw_queue, raw) != 0)
    {
        raw_free(raw);
        return 0;
    }
    return 1;
}

static void *fetch_main(void *arg)
{
    Server *s = arg;
    while (atomic_load(&s->running))
    {
        int ok = fetch_once(s);
        int retry = s->cfg.fetch_interval_s < RETRY_AFTER_ERROR_S ? s->cfg.fetch_interval_s : RETRY_AFTER_ERROR_S;
        if (!ok && atomic_load(&s->running))
            LOG_WARN("Fetch: hämtningen misslyckades, nytt försök om %d s", retry);
        sleep_while_running(s, ok ? s->cfg.fetch_interval_s : retry);
    }
    return NULL;
}

/* ── Parse-steget ─────────────────────────────────────────────────── */

/* Returnerar en färdig batch, eller NULL om datat inte gick att använda. */
static SlotBatch *parse_raw(const RawData *raw)
{
    SlotBatch *batch = calloc(1, sizeof(*batch));
    if (batch == NULL)
        return NULL;

    int count = parse_weather(raw->weather, batch->slots, MAX_SLOTS);
    if (count < 0)
    {
        LOG_WARN("Parse: ogiltig väderdata, förkastas");
        free(batch);
        return NULL;
    }
    if (parse_prices(raw->prices_today, batch->slots, count) <= 0)
    {
        LOG_WARN("Parse: ogiltig prisdata, förkastas");
        free(batch);
        return NULL;
    }
    if (raw->prices_tomorrow != NULL && parse_prices(raw->prices_tomorrow, batch->slots, count) < 0)
        LOG_WARN("Parse: morgondagens prisdata är ogiltig och används inte");

    batch->count = parse_finalize(batch->slots, count);
    if (batch->count < MIN_PLAN_SLOTS)
    {
        LOG_WARN("Parse: bara %d kvartar har både väder och pris, förkastas", batch->count);
        free(batch);
        return NULL;
    }
    return batch;
}

static void *parse_main(void *arg)
{
    Server *s = arg;
    void *item = NULL;
    while (queue_pop(&s->raw_queue, &item) == 0)
    {
        RawData *raw = item;
        SlotBatch *batch = parse_raw(raw);
        raw_free(raw);
        if (batch == NULL)
            continue;

        LOG_DEBUG("Parse: %d kvartar matchade", batch->count);
        if (queue_push(&s->slot_queue, batch) != 0)
            free(batch);
    }
    return NULL;
}

/* ── Compute- och cache-steget ────────────────────────────────────── */

static void *compute_main(void *arg)
{
    Server *s = arg;
    const ComputeParams params = {
        .panel_area_m2 = s->cfg.panel_area_m2,
        .panel_efficiency = s->cfg.panel_efficiency,
        .base_load_kw = s->cfg.base_load_kw,
        .battery_capacity_kwh = s->cfg.battery_capacity_kwh,
        .battery_max_kw = s->cfg.battery_max_kw,
    };

    void *item = NULL;
    while (queue_pop(&s->slot_queue, &item) == 0)
    {
        SlotBatch *batch = item;
        Plan *plan = malloc(sizeof(*plan));
        if (plan != NULL && compute_plan(batch->slots, batch->count, &params, plan) == 0)
        {
            cache_put(&s->cache, plan, time(NULL));
            LOG_INFO("Compute: ny plan med %d kvartar (%.0f timmar) lagrad i cachen",
                     plan->count, plan->count * SLOT_HOURS);
        }
        else
        {
            LOG_ERROR("Compute: kunde inte beräkna plan");
        }
        free(plan);
        free(batch);
    }
    return NULL;
}

/* ── Klientprotokoll ──────────────────────────────────────────────── */

/* Växande textbuffert för ett svar. */
typedef struct
{
    char  *data;
    size_t len;
    size_t cap;
    int    failed;
} Reply;

static void reply_add(Reply *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void reply_add(Reply *r, const char *fmt, ...)
{
    if (r->failed)
        return;

    char line[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof(line))
    {
        r->failed = 1;
        return;
    }

    if (r->len + (size_t)n + 1 > r->cap)
    {
        size_t cap = r->cap == 0 ? 1024 : r->cap * 2;
        while (cap < r->len + (size_t)n + 1)
            cap *= 2;
        char *grown = realloc(r->data, cap);
        if (grown == NULL)
        {
            r->failed = 1;
            return;
        }
        r->data = grown;
        r->cap = cap;
    }
    memcpy(r->data + r->len, line, (size_t)n + 1);
    r->len += (size_t)n;
}

/* Hämtar planen ur cachen eller skriver ett ERR-svar. Returnerar 1 vid träff. */
static int load_plan(Server *s, Plan *plan, Reply *reply)
{
    switch (cache_get(&s->cache, plan, time(NULL), NULL))
    {
        case CACHE_OK:
            return 1;
        case CACHE_EMPTY:
            reply_add(reply, "ERR ingen data ännu, försök igen om en stund\n");
            return 0;
        default:
            reply_add(reply, "ERR datat är för gammalt, hämtningen misslyckas (se loggen)\n");
            return 0;
    }
}

static void reply_status(Server *s, Reply *reply)
{
    Plan *plan = malloc(sizeof(*plan));
    if (plan == NULL)
    {
        reply_add(reply, "ERR slut på minne\n");
        return;
    }

    int age = 0;
    int rc = cache_get(&s->cache, plan, time(NULL), &age);
    const char *state = rc == CACHE_OK ? "ok" : rc == CACHE_EMPTY ? "no_data" : "expired";

    reply_add(reply, "OK %d\n", rc == CACHE_OK ? 6 : 3);
    reply_add(reply, "state\t%s\n", state);
    reply_add(reply, "age_s\t%d\n", age);
    reply_add(reply, "ttl_s\t%d\n", s->cfg.cache_ttl_s);
    if (rc == CACHE_OK)
    {
        reply_add(reply, "slots\t%d\n", plan->count);
        reply_add(reply, "from\t%lld\n", (long long)plan->slots[0].ts);
        reply_add(reply, "to\t%lld\n", (long long)plan->slots[plan->count - 1].ts + SLOT_SECONDS);
    }
    free(plan);
}

/* Tolkar en kommandorad och bygger svaret. */
static void handle_command(Server *s, char *line, Reply *reply)
{
    char *save = NULL;
    char *command = strtok_r(line, " \t\r", &save);
    const char *argument = strtok_r(NULL, " \t\r", &save);
    const char *extra = strtok_r(NULL, " \t\r", &save);
    int fields = (command != NULL) + (argument != NULL) + (extra != NULL);

    if (command == NULL)
    {
        reply_add(reply, "ERR tomt kommando\n");
        return;
    }
    for (char *p = command; *p != '\0'; p++)
        *p = (char)toupper((unsigned char)*p);

    long hours = 0;
    if (argument != NULL)
    {
        char *end = NULL;
        hours = strtol(argument, &end, 10);
        if (end == argument || *end != '\0')
            hours = -1; /* inte ett heltal; avvisas nedan */
    }

    if (strcmp(command, "STATUS") == 0 && fields == 1)
    {
        reply_status(s, reply);
        return;
    }

    int is_plan = strcmp(command, "PLAN") == 0;
    int is_forecast = strcmp(command, "FORECAST") == 0;
    int is_prices = strcmp(command, "PRICES") == 0;
    if (!is_plan && !is_forecast && !is_prices)
    {
        reply_add(reply, "ERR okänt kommando, använd STATUS, FORECAST, PRICES eller PLAN [timmar]\n");
        return;
    }
    /* Bara PLAN tar ett argument, och bara ett. */
    if (fields > 2 || (fields == 2 && !is_plan))
    {
        reply_add(reply, "ERR för många argument\n");
        return;
    }
    if (fields == 2 && (hours < 1 || hours > MAX_SLOTS / 4))
    {
        reply_add(reply, "ERR antal timmar måste vara 1-%d\n", MAX_SLOTS / 4);
        return;
    }

    Plan *plan = malloc(sizeof(*plan));
    if (plan == NULL)
    {
        reply_add(reply, "ERR slut på minne\n");
        return;
    }
    if (load_plan(s, plan, reply))
    {
        int count = plan->count;
        if (fields == 2 && hours * 4 < count)
            count = (int)hours * 4;

        reply_add(reply, "OK %d\n", count);
        for (int i = 0; i < count; i++)
        {
            const PlanSlot *p = &plan->slots[i];
            if (is_forecast)
                reply_add(reply, "%lld\t%.1f\t%.0f\t%.1f\t%.4f\n", (long long)p->ts,
                          p->radiation, p->cloud_cover, p->temperature, p->production_kwh);
            else if (is_prices)
                reply_add(reply, "%lld\t%.5f\n", (long long)p->ts, p->price);
            else
                reply_add(reply, "%lld\t%s\t%.5f\t%.4f\t%.4f\t%.4f\t%.4f\n", (long long)p->ts,
                          action_name(p->action), p->price, p->production_kwh,
                          p->consumption_kwh, p->battery_kwh, p->grid_kwh);
        }
    }
    free(plan);
}

/* Läser en rad från klienten. Returnerar 0, eller -1 vid frånkoppling,
 * inaktivitet, för lång rad eller nedstängning. */
static int read_line(const Server *s, int fd, char *line, size_t size)
{
    size_t len = 0;
    int idle_ms = 0;
    while (atomic_load(&s->running) && idle_ms < CLIENT_IDLE_MS)
    {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int ready = poll(&pfd, 1, POLL_SLICE_MS);
        if (ready < 0 && errno != EINTR)
            return -1;
        if (ready <= 0)
        {
            idle_ms += POLL_SLICE_MS;
            continue;
        }

        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n == 0)
            return -1; /* klienten stängde anslutningen */
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (c == '\n')
        {
            line[len] = '\0';
            return 0;
        }
        if (len + 1 >= size)
            return -1;
        line[len++] = c;
    }
    return -1;
}

static int send_all(int fd, const char *data, size_t len)
{
    while (len > 0)
    {
        /* MSG_NOSIGNAL: en klient som kopplat ned ska ge EPIPE, inte SIGPIPE. */
        ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

typedef struct
{
    Server *server;
    int     fd;
} ClientArg;

static void *client_main(void *arg)
{
    ClientArg *client = arg;
    Server *s = client->server;
    int fd = client->fd;
    free(client);

    char line[MAX_COMMAND];
    while (read_line(s, fd, line, sizeof(line)) == 0)
    {
        Reply reply = {NULL, 0, 0, 0};
        LOG_DEBUG("Klient %d: %s", fd, line);
        handle_command(s, line, &reply);

        int sent = !reply.failed && reply.data != NULL && send_all(fd, reply.data, reply.len) == 0;
        free(reply.data);
        if (!sent)
            break;
    }
    close(fd);

    pthread_mutex_lock(&s->clients_mutex);
    s->clients--;
    pthread_cond_signal(&s->clients_done);
    pthread_mutex_unlock(&s->clients_mutex);
    return NULL;
}

/* Startar en fristående tråd för en ansluten klient. */
static void spawn_client(Server *s, int fd)
{
    ClientArg *client = malloc(sizeof(*client));
    if (client == NULL)
    {
        close(fd);
        return;
    }
    client->server = s;
    client->fd = fd;

    pthread_mutex_lock(&s->clients_mutex);
    s->clients++;
    pthread_mutex_unlock(&s->clients_mutex);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    int rc = pthread_create(&thread, &attr, client_main, client);
    pthread_attr_destroy(&attr);

    if (rc != 0)
    {
        LOG_ERROR("Server: kunde inte skapa klienttråd: %s", strerror(rc));
        close(fd);
        free(client);
        pthread_mutex_lock(&s->clients_mutex);
        s->clients--;
        pthread_mutex_unlock(&s->clients_mutex);
    }
}

/* ── Livscykel ────────────────────────────────────────────────────── */

/* Öppnar den lyssnande Unix-socketen. Returnerar fd eller -1. */
static int open_listener(const char *path)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        LOG_ERROR("Server: socket() misslyckades: %s", strerror(errno));
        return -1;
    }

    /* Finns filen redan: svarar någon på den kör en annan server, annars
     * är den kvar efter en krasch och kan tas bort. */
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
    {
        LOG_ERROR("Server: en annan server lyssnar redan på %s", path);
        close(fd);
        return -1;
    }
    close(fd);
    unlink(path);

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, LISTEN_BACKLOG) != 0)
    {
        LOG_ERROR("Server: kan inte lyssna på %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

Server *server_create(const Config *cfg, const char *fetcher_path)
{
    Server *s = calloc(1, sizeof(*s));
    if (s == NULL)
        return NULL;

    s->cfg = *cfg;
    snprintf(s->fetcher_path, sizeof(s->fetcher_path), "%s", fetcher_path);
    s->fetcher.pid = -1;
    s->fetcher.to_fd = -1;
    s->fetcher.from_fd = -1;
    atomic_init(&s->running, true);
    pthread_mutex_init(&s->clients_mutex, NULL);
    pthread_cond_init(&s->clients_done, NULL);
    cache_init(&s->cache, cfg->cache_ttl_s);

    if (queue_init(&s->raw_queue, QUEUE_CAPACITY) != 0)
        goto fail_raw_queue;
    if (queue_init(&s->slot_queue, QUEUE_CAPACITY) != 0)
        goto fail_slot_queue;

    s->listen_fd = open_listener(cfg->socket_path);
    if (s->listen_fd < 0)
        goto fail_listener;

    if (fetcher_start(&s->fetcher, s->fetcher_path, cfg->http_timeout_s) != 0)
        goto fail_fetcher;

    return s;

fail_fetcher:
    close(s->listen_fd);
    unlink(cfg->socket_path);
fail_listener:
    queue_destroy(&s->slot_queue);
fail_slot_queue:
    queue_destroy(&s->raw_queue);
fail_raw_queue:
    cache_destroy(&s->cache);
    pthread_cond_destroy(&s->clients_done);
    pthread_mutex_destroy(&s->clients_mutex);
    free(s);
    return NULL;
}

static void accept_loop(Server *s, const volatile sig_atomic_t *stop)
{
    while (!*stop)
    {
        struct pollfd pfd = {.fd = s->listen_fd, .events = POLLIN};
        int ready = poll(&pfd, 1, POLL_SLICE_MS);
        if (ready < 0 && errno != EINTR)
        {
            LOG_ERROR("Server: poll() misslyckades: %s", strerror(errno));
            break;
        }
        if (ready <= 0)
            continue;

        int fd = accept4(s->listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0)
        {
            if (errno != EINTR && errno != ECONNABORTED)
                LOG_WARN("Server: accept() misslyckades: %s", strerror(errno));
            continue;
        }
        spawn_client(s, fd);
    }
}

int server_run(Server *s, const volatile sig_atomic_t *stop)
{
    int started = 0;
    if (pthread_create(&s->fetch_thread, NULL, fetch_main, s) == 0)
        started++;
    if (started == 1 && pthread_create(&s->parse_thread, NULL, parse_main, s) == 0)
        started++;
    if (started == 2 && pthread_create(&s->compute_thread, NULL, compute_main, s) == 0)
        started++;

    if (started == 3)
    {
        LOG_INFO("Server: lyssnar på %s", s->cfg.socket_path);
        accept_loop(s, stop);
        LOG_INFO("Server: stoppsignal mottagen, stänger ned");
    }
    else
    {
        LOG_ERROR("Server: kunde inte starta pipeline-trådarna");
    }

    /* Ordnad nedstängning: flaggan stoppar fetch-tråden, stängda köer
     * väcker parse- och compute-trådarna ur queue_pop. */
    atomic_store(&s->running, false);
    queue_close(&s->raw_queue);
    queue_close(&s->slot_queue);
    if (started >= 1)
        pthread_join(s->fetch_thread, NULL);
    if (started >= 2)
        pthread_join(s->parse_thread, NULL);
    if (started >= 3)
        pthread_join(s->compute_thread, NULL);

    fetcher_stop(&s->fetcher);

    /* Klienttrådarna ser running-flaggan inom en poll-period. */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    pthread_mutex_lock(&s->clients_mutex);
    while (s->clients > 0)
    {
        if (pthread_cond_timedwait(&s->clients_done, &s->clients_mutex, &deadline) == ETIMEDOUT)
            break;
    }
    int remaining = s->clients;
    pthread_mutex_unlock(&s->clients_mutex);
    if (remaining > 0)
        LOG_WARN("Server: %d klienttrådar avslutades inte i tid", remaining);

    return started == 3 ? 0 : -1;
}

void server_destroy(Server *s)
{
    if (s == NULL)
        return;

    fetcher_stop(&s->fetcher);
    close(s->listen_fd);
    unlink(s->cfg.socket_path);

    /* Det som eventuellt ligger kvar i köerna ägs nu av oss. */
    void *item = NULL;
    while (queue_pop(&s->raw_queue, &item) == 0)
        raw_free(item);
    while (queue_pop(&s->slot_queue, &item) == 0)
        free(item);

    queue_destroy(&s->raw_queue);
    queue_destroy(&s->slot_queue);
    cache_destroy(&s->cache);
    pthread_cond_destroy(&s->clients_done);
    pthread_mutex_destroy(&s->clients_mutex);
    free(s);
}
