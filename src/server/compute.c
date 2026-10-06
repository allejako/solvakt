#include "server/compute.h"

#include <stdlib.h>
#include <string.h>

/* Priser under 25:e percentilen räknas som låga, över 75:e som höga. */
#define LOW_PERCENTILE  25.0
#define HIGH_PERCENTILE 75.0

/* Solen ska täcka minst så här stor del av förbrukningen för att kvarten
 * ska räknas som "använd egen sol" och inte som ett rent köp. */
#define SOLAR_SHARE_THRESHOLD 0.5

#define EPSILON 1e-9

double compute_production_kwh(double radiation, const ComputeParams *params)
{
    /* W/m² * m² * verkningsgrad = W; gånger 0,25 h och delat med 1000 = kWh. */
    return radiation * params->panel_area_m2 * params->panel_efficiency * SLOT_HOURS / 1000.0;
}

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;
    return (x > y) - (x < y);
}

static int percentile_index(int count, double percentile)
{
    return (int)(percentile / 100.0 * (count - 1) + 0.5);
}

/* Gränserna för lågt och högt pris: 25:e och 75:e percentilen av alla
 * priser i prognosen. Priserna sorteras en gång per plan. Den första
 * versionen sorterade om dem för varje kvart, vilket profileringen visade
 * tog i stort sett hela körtiden (se docs/PROFILING.md). */
static void price_thresholds(const Slot *slots, int count, double *low, double *high)
{
    double sorted[MAX_SLOTS];
    for (int i = 0; i < count; i++)
        sorted[i] = slots[i].price;
    qsort(sorted, (size_t)count, sizeof(double), compare_double);

    *low = sorted[percentile_index(count, LOW_PERCENTILE)];
    *high = sorted[percentile_index(count, HIGH_PERCENTILE)];
}

static double min3(double a, double b, double c)
{
    double m = a < b ? a : b;
    return m < c ? m : c;
}

int compute_plan(const Slot *slots, int count, const ComputeParams *params, Plan *plan)
{
    if (slots == NULL || params == NULL || plan == NULL || count <= 0 || count > MAX_SLOTS)
        return -1;

    memset(plan, 0, sizeof(*plan));
    plan->count = count;
    plan->generated_at = time(NULL);

    double battery = 0.0;
    const double capacity = params->battery_capacity_kwh;
    const double max_step = params->battery_max_kw * SLOT_HOURS;
    const double consumption = params->base_load_kw * SLOT_HOURS;

    double low = 0.0, high = 0.0;
    price_thresholds(slots, count, &low, &high);
    /* Är alla priser lika finns inget att vinna på att flytta energi. */
    const int has_spread = high - low > EPSILON;

    for (int i = 0; i < count; i++)
    {
        const Slot *in = &slots[i];
        PlanSlot *out = &plan->slots[i];

        int price_is_low = has_spread && in->price <= low;
        int price_is_high = has_spread && in->price >= high;

        double production = compute_production_kwh(in->radiation, params);
        double net = production - consumption;
        double room = capacity - battery;
        double grid = 0.0;
        Action action;

        if (net > EPSILON)
        {
            /* Överskott: lagra det om det finns plats och priset inte är
             * högt just nu, annars sälj. */
            if (!price_is_high && room > EPSILON && max_step > EPSILON)
            {
                double charge = min3(net, max_step, room);
                battery += charge;
                grid = -(net - charge);
                action = ACT_CHARGE;
            }
            else
            {
                grid = -net;
                action = ACT_SELL;
            }
        }
        else
        {
            double deficit = -net;
            if (price_is_high && battery > EPSILON && max_step > EPSILON)
            {
                /* Dyr el: täck underskottet från batteriet. */
                double discharge = min3(deficit, max_step, battery);
                battery -= discharge;
                grid = deficit - discharge;
                action = ACT_DISCHARGE;
            }
            else if (price_is_low && room > EPSILON && max_step > EPSILON)
            {
                /* Billig el: köp extra och fyll batteriet. */
                double charge = max_step < room ? max_step : room;
                battery += charge;
                grid = deficit + charge;
                action = ACT_CHARGE;
            }
            else
            {
                grid = deficit;
                action = production >= SOLAR_SHARE_THRESHOLD * consumption && production > EPSILON
                             ? ACT_USE_SOLAR
                             : ACT_BUY;
            }
        }

        out->ts = in->ts;
        out->radiation = in->radiation;
        out->cloud_cover = in->cloud_cover;
        out->temperature = in->temperature;
        out->price = in->price;
        out->production_kwh = production;
        out->consumption_kwh = consumption;
        out->battery_kwh = battery;
        out->grid_kwh = grid;
        out->action = action;
    }
    return 0;
}
