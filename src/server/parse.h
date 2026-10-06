#ifndef SOLVAKT_PARSE_H
#define SOLVAKT_PARSE_H

#include "common/types.h"

/* Parse-steget: rå JSON från API:erna blir en rad av kvartsintervall där
 * väder och spotpris är matchade på tid. */

/* Tolkar Open-Meteo-svar (minutely_15 med timeformat=unixtime). Fyller
 * slots[0..n-1] i tidsordning med exakt 15 minuters mellanrum. Värden som
 * saknas eller är orimliga ger has_weather = 0 för det intervallet.
 * Returnerar antal intervall, eller -1 om svaret inte går att använda. */
int parse_weather(const char *json, Slot *slots, int max_slots);

/* Tolkar svar från elprisetjustnu.se och lägger in priset på de intervall i
 * slots som täcks av varje prisrad. Returnerar antal intervall som fick ett
 * pris, eller -1 om svaret inte går att använda. */
int parse_prices(const char *json, Slot *slots, int count);

/* Behåller den första sammanhängande följden av intervall som har både väder
 * och pris och flyttar den till början av slots. Returnerar dess längd. */
int parse_finalize(Slot *slots, int count);

/* Tolkar "YYYY-MM-DDTHH:MM:SS" följt av "Z" eller "+HH:MM"/"-HH:MM".
 * Returnerar 0 och sätter *out, eller -1 vid ogiltigt format. */
int parse_iso8601(const char *text, time_t *out);

#endif
