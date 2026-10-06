#include "check.h"
#include "common/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Skriver text till en temporär fil och returnerar sökvägen i path. */
static void write_temp(char *path, size_t path_len, const char *text)
{
    snprintf(path, path_len, "/tmp/solvakt_test_config_XXXXXX");
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    close(fd);
}

static void test_defaults_are_valid(void)
{
    Config cfg;
    char err[256] = "";
    config_defaults(&cfg);
    CHECK(config_validate(&cfg, err, sizeof(err)) == 0);
    CHECK(strcmp(cfg.price_region, "SE3") == 0);
    CHECK(cfg.fetch_interval_s == 900);
}

static void test_load_overrides_defaults(void)
{
    char path[64];
    write_temp(path, sizeof(path),
               "# kommentar\n"
               "\n"
               "  latitude = 57.7089  \n"
               "longitude=11.9746\n"
               "price_region = SE4\n"
               "battery_capacity_kwh = 0\n"
               "cache_ttl_s = 60\n");

    Config cfg;
    char err[256] = "";
    config_defaults(&cfg);
    CHECK(config_load(path, &cfg, err, sizeof(err)) == 0);
    CHECK_NEAR(cfg.latitude, 57.7089, 1e-9);
    CHECK_NEAR(cfg.longitude, 11.9746, 1e-9);
    CHECK(strcmp(cfg.price_region, "SE4") == 0);
    CHECK_NEAR(cfg.battery_capacity_kwh, 0.0, 1e-9);
    CHECK(cfg.cache_ttl_s == 60);
    /* Nycklar som inte står i filen behåller standardvärdet. */
    CHECK_NEAR(cfg.panel_area_m2, 20.0, 1e-9);
    unlink(path);
}

static void expect_load_error(const char *text)
{
    char path[64];
    write_temp(path, sizeof(path), text);

    Config cfg;
    char err[256] = "";
    config_defaults(&cfg);
    CHECK(config_load(path, &cfg, err, sizeof(err)) == -1);
    CHECK(err[0] != '\0');
    unlink(path);
}

static void test_invalid_files(void)
{
    expect_load_error("latitude = 123\n");          /* utanför intervallet */
    expect_load_error("latitude = norr\n");         /* inte ett tal */
    expect_load_error("price_region = NO1\n");      /* okänt elområde */
    expect_load_error("no_such_key = 1\n");         /* okänd nyckel */
    expect_load_error("latitude 59\n");             /* saknar = */
    expect_load_error("http_timeout_s = 0\n");      /* för låg timeout */
    expect_load_error("panel_efficiency = 1.5\n");  /* över 100 % */
}

static void test_missing_file(void)
{
    Config cfg;
    char err[256] = "";
    config_defaults(&cfg);
    CHECK(config_load("/tmp/solvakt_finns_inte.conf", &cfg, err, sizeof(err)) == -1);
    CHECK(err[0] != '\0');
}

int main(void)
{
    test_defaults_are_valid();
    test_load_overrides_defaults();
    test_invalid_files();
    test_missing_file();
    return TEST_RESULT();
}
