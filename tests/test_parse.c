#include "check.h"
#include "server/parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 2000-01-01T00:00:00Z */
#define BASE 946684800L

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc((size_t)size + 1);
    if (data != NULL && fread(data, 1, (size_t)size, f) != (size_t)size)
    {
        free(data);
        data = NULL;
    }
    if (data != NULL)
        data[size] = '\0';
    fclose(f);
    return data;
}

static const char *WEATHER =
    "{\"minutely_15\":{"
    "\"time\":[946684800,946685700,946686600,946687500,946688400,946689300,946690200,946691100],"
    "\"shortwave_radiation\":[0,100,200,null,400,500,600,2000],"
    "\"cloud_cover\":[10,20,30,40,50,60,70,80],"
    "\"temperature_2m\":[1.5,2,2.5,3,3.5,4,4.5,5]}}";

static const char *PRICES =
    "["
    /* timrad: ska fylla fyra kvartar */
    "{\"SEK_per_kWh\":0.5,\"time_start\":\"2000-01-01T00:00:00Z\",\"time_end\":\"2000-01-01T01:00:00Z\"},"
    /* kvartsrader, skrivna i svensk tid (+01:00) */
    "{\"SEK_per_kWh\":1.0,\"time_start\":\"2000-01-01T02:00:00+01:00\",\"time_end\":\"2000-01-01T02:15:00+01:00\"},"
    "{\"SEK_per_kWh\":-0.2,\"time_start\":\"2000-01-01T02:15:00+01:00\",\"time_end\":\"2000-01-01T02:30:00+01:00\"},"
    /* trasig rad: priset är en sträng */
    "{\"SEK_per_kWh\":\"dyrt\",\"time_start\":\"2000-01-01T01:30:00Z\",\"time_end\":\"2000-01-01T01:45:00Z\"},"
    /* utanför väderdatat */
    "{\"SEK_per_kWh\":9.0,\"time_start\":\"2000-01-02T00:00:00Z\",\"time_end\":\"2000-01-02T00:15:00Z\"}"
    "]";

static void test_iso8601(void)
{
    time_t t = 0;
    CHECK(parse_iso8601("2000-01-01T00:00:00Z", &t) == 0 && t == BASE);
    CHECK(parse_iso8601("2000-01-01T01:00:00+01:00", &t) == 0 && t == BASE);
    CHECK(parse_iso8601("1999-12-31T19:00:00-05:00", &t) == 0 && t == BASE);
    CHECK(parse_iso8601("2000-01-01T02:00:00+02:00", &t) == 0 && t == BASE);

    CHECK(parse_iso8601(NULL, &t) == -1);
    CHECK(parse_iso8601("", &t) == -1);
    CHECK(parse_iso8601("2000-01-01 00:00:00Z", &t) == -1);
    CHECK(parse_iso8601("2000-13-01T00:00:00Z", &t) == -1);
    CHECK(parse_iso8601("2000-01-01T00:00:00", &t) == -1);
    CHECK(parse_iso8601("2000-01-01T00:00:00X", &t) == -1);
}

static void test_weather(void)
{
    Slot slots[MAX_SLOTS];
    int n = parse_weather(WEATHER, slots, MAX_SLOTS);
    CHECK(n == 8);
    CHECK(slots[0].ts == BASE);
    CHECK(slots[7].ts == BASE + 7 * SLOT_SECONDS);
    CHECK(slots[1].has_weather);
    CHECK_NEAR(slots[1].radiation, 100.0, 1e-9);
    CHECK_NEAR(slots[1].cloud_cover, 20.0, 1e-9);
    CHECK_NEAR(slots[1].temperature, 2.0, 1e-9);
    CHECK(!slots[3].has_weather); /* null */
    CHECK(!slots[7].has_weather); /* orimlig instrålning */
    CHECK(!slots[0].has_price);
}

static void test_weather_invalid(void)
{
    Slot slots[MAX_SLOTS];
    CHECK(parse_weather("", slots, MAX_SLOTS) == -1);
    CHECK(parse_weather("inte json", slots, MAX_SLOTS) == -1);
    CHECK(parse_weather("{}", slots, MAX_SLOTS) == -1);
    CHECK(parse_weather("{\"error\":true,\"reason\":\"bad latitude\"}", slots, MAX_SLOTS) == -1);
    /* olika långa arrayer */
    CHECK(parse_weather("{\"minutely_15\":{\"time\":[946684800,946685700],"
                        "\"shortwave_radiation\":[0],\"cloud_cover\":[0,0],"
                        "\"temperature_2m\":[0,0]}}", slots, MAX_SLOTS) == -1);
    /* lucka i tidsserien */
    CHECK(parse_weather("{\"minutely_15\":{\"time\":[946684800,946688400],"
                        "\"shortwave_radiation\":[0,0],\"cloud_cover\":[0,0],"
                        "\"temperature_2m\":[0,0]}}", slots, MAX_SLOTS) == -1);
    /* fler intervall än bufferten rymmer */
    CHECK(parse_weather(WEATHER, slots, 4) == -1);
}

static void test_prices_and_finalize(void)
{
    Slot slots[MAX_SLOTS];
    int n = parse_weather(WEATHER, slots, MAX_SLOTS);
    CHECK(n == 8);

    CHECK(parse_prices(PRICES, slots, n) == 6);
    CHECK_NEAR(slots[0].price, 0.5, 1e-9);
    CHECK_NEAR(slots[3].price, 0.5, 1e-9);
    CHECK_NEAR(slots[4].price, 1.0, 1e-9);
    CHECK_NEAR(slots[5].price, -0.2, 1e-9);
    CHECK(!slots[6].has_price); /* den trasiga raden */
    CHECK(!slots[7].has_price);

    /* Intervall 3 saknar väder, så den sammanhängande följden är 0-2. */
    CHECK(parse_finalize(slots, n) == 3);
    CHECK(slots[0].ts == BASE);
    CHECK(slots[2].ts == BASE + 2 * SLOT_SECONDS);
}

static void test_finalize_shifts_to_start(void)
{
    Slot slots[4];
    memset(slots, 0, sizeof(slots));
    for (int i = 0; i < 4; i++)
    {
        slots[i].ts = BASE + i * SLOT_SECONDS;
        slots[i].has_weather = 1;
        slots[i].has_price = i >= 2;
    }
    CHECK(parse_finalize(slots, 4) == 2);
    CHECK(slots[0].ts == BASE + 2 * SLOT_SECONDS);
    CHECK(slots[1].ts == BASE + 3 * SLOT_SECONDS);

    memset(slots, 0, sizeof(slots));
    CHECK(parse_finalize(slots, 4) == 0);
}

static void test_prices_invalid(void)
{
    Slot slots[MAX_SLOTS];
    int n = parse_weather(WEATHER, slots, MAX_SLOTS);
    CHECK(parse_prices("", slots, n) == -1);
    CHECK(parse_prices("{\"message\":\"not found\"}", slots, n) == -1);
    CHECK(parse_prices("[]", slots, n) == 0);
    CHECK(parse_prices(PRICES, slots, 0) == -1);
}

/* Riktiga svar från API:erna, sparade 2026-10-06. */
static void test_real_responses(void)
{
    char *weather = read_file("tests/fixtures/weather.json");
    char *prices = read_file("tests/fixtures/prices.json");
    CHECK(weather != NULL);
    CHECK(prices != NULL);
    if (weather == NULL || prices == NULL)
    {
        printf("  (kör testerna från projektroten)\n");
        free(weather);
        free(prices);
        return;
    }

    Slot *slots = calloc(MAX_SLOTS, sizeof(Slot));
    int n = parse_weather(weather, slots, MAX_SLOTS);
    CHECK(n == MAX_SLOTS);
    CHECK(parse_prices(prices, slots, n) == 96); /* ett dygn i kvartar */

    int kept = parse_finalize(slots, n);
    CHECK(kept == 96);
    for (int i = 1; i < kept; i++)
        CHECK(slots[i].ts == slots[i - 1].ts + SLOT_SECONDS);

    free(slots);
    free(weather);
    free(prices);
}

int main(void)
{
    test_iso8601();
    test_weather();
    test_weather_invalid();
    test_prices_and_finalize();
    test_finalize_shifts_to_start();
    test_prices_invalid();
    test_real_responses();
    return TEST_RESULT();
}
