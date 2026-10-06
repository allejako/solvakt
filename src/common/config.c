#include "common/config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_defaults(Config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->socket_path, "/tmp/solvakt.sock");
    strcpy(cfg->log_file, "logs/solvakt.log");
    strcpy(cfg->log_level, "info");
    cfg->latitude = 59.3293;
    cfg->longitude = 18.0686;
    strcpy(cfg->price_region, "SE3");
    cfg->panel_area_m2 = 20.0;
    cfg->panel_efficiency = 0.18;
    cfg->base_load_kw = 1.0;
    cfg->battery_capacity_kwh = 10.0;
    cfg->battery_max_kw = 3.0;
    cfg->fetch_interval_s = 900;
    cfg->cache_ttl_s = 1800;
    cfg->http_timeout_s = 10;
    strcpy(cfg->weather_api_url, "https://api.open-meteo.com/v1/forecast");
    strcpy(cfg->price_api_url, "https://www.elprisetjustnu.se/api/v1/prices");
}

/* Tar bort blanktecken i början och slutet, på plats. */
static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';
    return s;
}

static int set_string(char *dst, size_t dst_len, const char *value)
{
    if (strlen(value) >= dst_len)
        return -1;
    strcpy(dst, value);
    return 0;
}

static int set_double(double *dst, const char *value)
{
    char *end = NULL;
    errno = 0;
    double v = strtod(value, &end);
    if (errno != 0 || end == value || *end != '\0')
        return -1;
    *dst = v;
    return 0;
}

static int set_int(int *dst, const char *value)
{
    char *end = NULL;
    errno = 0;
    long v = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || v < -1000000 || v > 1000000)
        return -1;
    *dst = (int)v;
    return 0;
}

/* Returnerar 0 om värdet sattes, -1 vid ogiltigt värde, 1 vid okänd nyckel. */
static int apply(Config *cfg, const char *key, const char *value)
{
#define STR(name) if (strcmp(key, #name) == 0) return set_string(cfg->name, sizeof(cfg->name), value)
#define DBL(name) if (strcmp(key, #name) == 0) return set_double(&cfg->name, value)
#define INT(name) if (strcmp(key, #name) == 0) return set_int(&cfg->name, value)
    STR(socket_path);
    STR(log_file);
    STR(log_level);
    DBL(latitude);
    DBL(longitude);
    STR(price_region);
    DBL(panel_area_m2);
    DBL(panel_efficiency);
    DBL(base_load_kw);
    DBL(battery_capacity_kwh);
    DBL(battery_max_kw);
    INT(fetch_interval_s);
    INT(cache_ttl_s);
    INT(http_timeout_s);
    STR(weather_api_url);
    STR(price_api_url);
#undef STR
#undef DBL
#undef INT
    return 1;
}

int config_validate(const Config *cfg, char *err, size_t err_len)
{
#define REQUIRE(cond, text) \
    do { if (!(cond)) { snprintf(err, err_len, "%s", text); return -1; } } while (0)

    REQUIRE(cfg->socket_path[0] != '\0', "socket_path får inte vara tom");
    REQUIRE(cfg->latitude >= -90.0 && cfg->latitude <= 90.0, "latitude måste ligga mellan -90 och 90");
    REQUIRE(cfg->longitude >= -180.0 && cfg->longitude <= 180.0, "longitude måste ligga mellan -180 och 180");
    REQUIRE(strlen(cfg->price_region) == 3 && strncmp(cfg->price_region, "SE", 2) == 0 &&
            cfg->price_region[2] >= '1' && cfg->price_region[2] <= '4',
            "price_region måste vara SE1, SE2, SE3 eller SE4");
    REQUIRE(cfg->panel_area_m2 >= 0.0 && cfg->panel_area_m2 <= 10000.0, "panel_area_m2 måste ligga mellan 0 och 10000");
    REQUIRE(cfg->panel_efficiency > 0.0 && cfg->panel_efficiency <= 1.0, "panel_efficiency måste ligga mellan 0 och 1");
    REQUIRE(cfg->base_load_kw >= 0.0 && cfg->base_load_kw <= 1000.0, "base_load_kw måste ligga mellan 0 och 1000");
    REQUIRE(cfg->battery_capacity_kwh >= 0.0 && cfg->battery_capacity_kwh <= 10000.0, "battery_capacity_kwh måste ligga mellan 0 och 10000");
    REQUIRE(cfg->battery_max_kw >= 0.0 && cfg->battery_max_kw <= 10000.0, "battery_max_kw måste ligga mellan 0 och 10000");
    REQUIRE(cfg->fetch_interval_s >= 10 && cfg->fetch_interval_s <= 86400, "fetch_interval_s måste ligga mellan 10 och 86400");
    REQUIRE(cfg->cache_ttl_s >= 1 && cfg->cache_ttl_s <= 604800, "cache_ttl_s måste ligga mellan 1 och 604800");
    REQUIRE(cfg->http_timeout_s >= 1 && cfg->http_timeout_s <= 300, "http_timeout_s måste ligga mellan 1 och 300");
    REQUIRE(cfg->weather_api_url[0] != '\0', "weather_api_url får inte vara tom");
    REQUIRE(cfg->price_api_url[0] != '\0', "price_api_url får inte vara tom");
#undef REQUIRE
    return 0;
}

int config_load(const char *path, Config *cfg, char *err, size_t err_len)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
    {
        snprintf(err, err_len, "kan inte öppna %s: %s", path, strerror(errno));
        return -1;
    }

    char line[512];
    int line_no = 0;
    int result = 0;
    while (result == 0 && fgets(line, sizeof(line), f) != NULL)
    {
        line_no++;
        char *text = trim(line);
        if (text[0] == '\0' || text[0] == '#')
            continue;

        char *eq = strchr(text, '=');
        if (eq == NULL)
        {
            snprintf(err, err_len, "%s rad %d: saknar '='", path, line_no);
            result = -1;
            break;
        }
        *eq = '\0';
        const char *key = trim(text);
        const char *value = trim(eq + 1);

        int rc = apply(cfg, key, value);
        if (rc != 0)
        {
            snprintf(err, err_len, "%s rad %d: %s '%s'", path, line_no,
                     rc > 0 ? "okänd nyckel" : "ogiltigt värde för", key);
            result = -1;
        }
    }
    fclose(f);

    if (result == 0)
        result = config_validate(cfg, err, err_len);
    return result;
}
