#include "server/parse.h"

#include "libs/cJSON.h"

#include <stdio.h>
#include <string.h>

/* Gränser för vad som räknas som rimlig indata. */
#define MAX_RADIATION     1500.0 /* W/m², över solkonstanten vid marken */
#define MIN_TEMPERATURE    -70.0
#define MAX_TEMPERATURE     70.0
#define MIN_PRICE          -50.0 /* SEK/kWh; negativa priser förekommer */
#define MAX_PRICE          500.0
#define MAX_PRICE_SPAN    7200   /* en prisrad täcker högst två timmar */

static int number_in_range(const cJSON *item, double min, double max, double *out)
{
    if (!cJSON_IsNumber(item))
        return 0;
    if (item->valuedouble < min || item->valuedouble > max)
        return 0;
    *out = item->valuedouble;
    return 1;
}

int parse_weather(const char *json, Slot *slots, int max_slots)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL)
        return -1;

    int count = -1;
    const cJSON *block = cJSON_GetObjectItemCaseSensitive(root, "minutely_15");
    const cJSON *times = cJSON_GetObjectItemCaseSensitive(block, "time");
    const cJSON *radiation = cJSON_GetObjectItemCaseSensitive(block, "shortwave_radiation");
    const cJSON *clouds = cJSON_GetObjectItemCaseSensitive(block, "cloud_cover");
    const cJSON *temperature = cJSON_GetObjectItemCaseSensitive(block, "temperature_2m");

    if (cJSON_IsArray(times) && cJSON_IsArray(radiation) &&
        cJSON_IsArray(clouds) && cJSON_IsArray(temperature))
    {
        int n = cJSON_GetArraySize(times);
        if (n > 0 && n <= max_slots &&
            cJSON_GetArraySize(radiation) == n &&
            cJSON_GetArraySize(clouds) == n &&
            cJSON_GetArraySize(temperature) == n)
        {
            /* Arrayerna är länkade listor; de stegas parallellt. */
            const cJSON *t = times->child;
            const cJSON *r = radiation->child;
            const cJSON *c = clouds->child;
            const cJSON *k = temperature->child;
            int regular = 1;

            for (int i = 0; i < n && regular; i++)
            {
                Slot *slot = &slots[i];
                memset(slot, 0, sizeof(*slot));

                if (!cJSON_IsNumber(t))
                {
                    regular = 0;
                    break;
                }
                slot->ts = (time_t)t->valuedouble;
                /* Resten av systemet räknar index från tiden, så tidsserien
                 * måste vara obruten. */
                if (slot->ts != slots[0].ts + (time_t)i * SLOT_SECONDS)
                    regular = 0;

                slot->has_weather =
                    number_in_range(r, 0.0, MAX_RADIATION, &slot->radiation) &&
                    number_in_range(c, 0.0, 100.0, &slot->cloud_cover) &&
                    number_in_range(k, MIN_TEMPERATURE, MAX_TEMPERATURE, &slot->temperature);

                t = t->next;
                r = r->next;
                c = c->next;
                k = k->next;
            }
            if (regular)
                count = n;
        }
    }

    cJSON_Delete(root);
    return count;
}

int parse_iso8601(const char *text, time_t *out)
{
    if (text == NULL || strlen(text) < 20)
        return -1;

    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    int consumed = 0;
    if (sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d%n", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &consumed) != 6 || consumed != 19)
        return -1;
    if (tm.tm_mon < 1 || tm.tm_mon > 12 || tm.tm_mday < 1 || tm.tm_mday > 31 ||
        tm.tm_hour > 23 || tm.tm_min > 59 || tm.tm_sec > 60)
        return -1;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;

    long offset = 0;
    const char *zone = text + consumed;
    if (zone[0] == '+' || zone[0] == '-')
    {
        int hours = 0, minutes = 0;
        if (sscanf(zone + 1, "%2d:%2d", &hours, &minutes) != 2)
            return -1;
        offset = (long)hours * 3600 + (long)minutes * 60;
        if (zone[0] == '-')
            offset = -offset;
    }
    else if (zone[0] != 'Z')
    {
        return -1;
    }

    /* timegm tolkar tm som UTC; lokal tid minus offset ger UTC. */
    *out = timegm(&tm) - offset;
    return 0;
}

int parse_prices(const char *json, Slot *slots, int count)
{
    if (count <= 0)
        return -1;

    cJSON *root = cJSON_Parse(json);
    if (root == NULL)
        return -1;
    if (!cJSON_IsArray(root))
    {
        cJSON_Delete(root);
        return -1;
    }

    int matched = 0;
    const cJSON *row = NULL;
    cJSON_ArrayForEach(row, root)
    {
        const cJSON *price = cJSON_GetObjectItemCaseSensitive(row, "SEK_per_kWh");
        const cJSON *start = cJSON_GetObjectItemCaseSensitive(row, "time_start");
        const cJSON *end = cJSON_GetObjectItemCaseSensitive(row, "time_end");

        double value = 0.0;
        time_t from = 0, to = 0;
        if (!number_in_range(price, MIN_PRICE, MAX_PRICE, &value) ||
            !cJSON_IsString(start) || !cJSON_IsString(end) ||
            parse_iso8601(start->valuestring, &from) != 0 ||
            parse_iso8601(end->valuestring, &to) != 0 ||
            to <= from || to - from > MAX_PRICE_SPAN)
            continue; /* hoppa över trasiga rader, behåll resten */

        /* En rad kan täcka en kvart eller en hel timme; fyll alla kvartar. */
        for (time_t ts = from; ts < to; ts += SLOT_SECONDS)
        {
            long delta = (long)(ts - slots[0].ts);
            if (delta < 0 || delta % SLOT_SECONDS != 0)
                continue;
            long index = delta / SLOT_SECONDS;
            if (index >= count)
                continue;
            if (!slots[index].has_price)
                matched++;
            slots[index].price = value;
            slots[index].has_price = 1;
        }
    }

    cJSON_Delete(root);
    return matched;
}

int parse_finalize(Slot *slots, int count)
{
    int first = 0;
    while (first < count && !(slots[first].has_weather && slots[first].has_price))
        first++;

    int length = 0;
    while (first + length < count &&
           slots[first + length].has_weather && slots[first + length].has_price)
        length++;

    if (first > 0 && length > 0)
        memmove(slots, slots + first, (size_t)length * sizeof(Slot));
    return length;
}
