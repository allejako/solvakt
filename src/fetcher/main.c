/* solvakt-fetcher - datahämtare som körs som egen process.
 *
 * Startas av servern med stdin och stdout kopplade till två pipes.
 *
 *   Förfrågan (stdin):  GET <url>\n
 *   Svar (stdout):      OK <antal byte>\n<data>
 *                       ERR <felmeddelande>\n
 *
 * Processen avslutas när servern stänger sin ände av pipen (EOF på stdin).
 */

#include <curl/curl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BODY (4 * 1024 * 1024)

typedef struct
{
    char  *data;
    size_t len;
} Body;

static size_t on_data(char *chunk, size_t size, size_t nmemb, void *userdata)
{
    Body *body = userdata;
    size_t n = size * nmemb;
    if (body->len + n > MAX_BODY)
        return 0; /* avbryter överföringen */

    char *grown = realloc(body->data, body->len + n);
    if (grown == NULL)
        return 0;
    memcpy(grown + body->len, chunk, n);
    body->data = grown;
    body->len += n;
    return n;
}

static void reply_error(const char *message)
{
    printf("ERR %s\n", message);
    fflush(stdout);
}

static void handle_get(CURL *curl, const char *url, long timeout_s)
{
    Body body = {NULL, 0};

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L); /* HTTP >= 400 räknas som fel */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "solvakt/1.0");

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_HTTP_RETURNED_ERROR)
    {
        long status = 0;
        char message[64];
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        snprintf(message, sizeof(message), "HTTP %ld", status);
        reply_error(message);
    }
    else if (rc != CURLE_OK)
    {
        reply_error(curl_easy_strerror(rc));
    }
    else
    {
        printf("OK %zu\n", body.len);
        fwrite(body.data, 1, body.len, stdout);
        fflush(stdout);
    }
    free(body.data);
}

int main(int argc, char **argv)
{
    long timeout_s = argc > 1 ? strtol(argv[1], NULL, 10) : 10;
    if (timeout_s < 1)
        timeout_s = 10;

    /* Servern styr livscykeln: Ctrl-C i terminalen ska inte döda fetchern
     * innan servern hunnit stänga ned ordnat. */
    signal(SIGINT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
        return 1;
    CURL *curl = curl_easy_init();
    if (curl == NULL)
        return 1;

    char line[2048];
    while (fgets(line, sizeof(line), stdin) != NULL)
    {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "GET ", 4) == 0 && line[4] != '\0')
            handle_get(curl, line + 4, timeout_s);
        else
            reply_error("okänd förfrågan");

        if (ferror(stdout))
            break; /* servern har stängt läsänden */
    }

    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return 0;
}
