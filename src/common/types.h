#ifndef SOLVAKT_TYPES_H
#define SOLVAKT_TYPES_H

#include <time.h>

/* All data i systemet har 15 minuters upplösning. */
#define SLOT_SECONDS 900
#define SLOT_HOURS   0.25
#define MAX_SLOTS    288 /* 72 timmar */

/* Ett kvartsintervall efter parse-steget: väder och spotpris för samma tid. */
typedef struct
{
    time_t ts;          /* intervallets start, UTC */
    double radiation;   /* solinstrålning, W/m² */
    double cloud_cover; /* molnighet, % */
    double temperature; /* °C */
    double price;       /* spotpris, SEK/kWh */
    int    has_weather;
    int    has_price;
} Slot;

typedef enum
{
    ACT_BUY,       /* köp el från nätet */
    ACT_USE_SOLAR, /* använd egen solproduktion direkt */
    ACT_CHARGE,    /* ladda batteriet */
    ACT_DISCHARGE, /* ladda ur batteriet */
    ACT_SELL       /* sälj överskott till nätet */
} Action;

/* Ett kvartsintervall i den färdiga energiplanen. */
typedef struct
{
    time_t ts;
    double radiation;
    double cloud_cover;
    double temperature;
    double price;
    double production_kwh;  /* förväntad solproduktion under kvarten */
    double consumption_kwh; /* förväntad förbrukning under kvarten */
    double battery_kwh;     /* batteriets laddning efter kvarten */
    double grid_kwh;        /* positivt = köpt från nätet, negativt = sålt */
    Action action;
} PlanSlot;

typedef struct
{
    PlanSlot slots[MAX_SLOTS];
    int      count;
    time_t   generated_at;
} Plan;

static inline const char *action_name(Action a)
{
    switch (a)
    {
        case ACT_BUY:       return "BUY";
        case ACT_USE_SOLAR: return "USE_SOLAR";
        case ACT_CHARGE:    return "CHARGE";
        case ACT_DISCHARGE: return "DISCHARGE";
        case ACT_SELL:      return "SELL";
    }
    return "BUY";
}

#endif
