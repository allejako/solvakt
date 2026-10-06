#ifndef SOLVAKT_CONFIG_H
#define SOLVAKT_CONFIG_H

#include <stddef.h>

/* Systeminställningar som läses från config/solvakt.conf. */
typedef struct
{
    char   socket_path[100]; /* ryms i sockaddr_un.sun_path */
    char   log_file[256];
    char   log_level[16];

    double latitude;
    double longitude;
    char   price_region[8];

    double panel_area_m2;
    double panel_efficiency;
    double base_load_kw;
    double battery_capacity_kwh;
    double battery_max_kw;

    int    fetch_interval_s;
    int    cache_ttl_s;
    int    http_timeout_s;

    char   weather_api_url[256];
    char   price_api_url[256];
} Config;

void config_defaults(Config *cfg);

/* Läser filen ovanpå de värden som redan finns i cfg och validerar resultatet.
 * Returnerar 0, eller -1 med en förklaring i err. */
int config_load(const char *path, Config *cfg, char *err, size_t err_len);

/* Kontrollerar att alla värden ligger inom rimliga gränser. */
int config_validate(const Config *cfg, char *err, size_t err_len);

#endif
