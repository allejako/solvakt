#include "check.h"
#include "server/compute.h"

#include <stdlib.h>
#include <string.h>

#define BASE 946684800L

static const ComputeParams PARAMS = {
    .panel_area_m2 = 20.0,
    .panel_efficiency = 0.18,
    .base_load_kw = 1.0,
    .battery_capacity_kwh = 10.0,
    .battery_max_kw = 3.0,
};

static void fill(Slot *slots, int count, const double *prices, const double *radiation)
{
    memset(slots, 0, (size_t)count * sizeof(Slot));
    for (int i = 0; i < count; i++)
    {
        slots[i].ts = BASE + i * SLOT_SECONDS;
        slots[i].price = prices[i];
        slots[i].radiation = radiation[i];
        slots[i].has_weather = 1;
        slots[i].has_price = 1;
    }
}

static void test_production(void)
{
    /* 1000 W/m² * 20 m² * 0,18 = 3,6 kW; en kvart ger 0,9 kWh. */
    CHECK_NEAR(compute_production_kwh(1000.0, &PARAMS), 0.9, 1e-9);
    CHECK_NEAR(compute_production_kwh(0.0, &PARAMS), 0.0, 1e-9);
}

/* Ett litet dygn där alla fem åtgärderna ska förekomma. Låg gräns blir 0,1
 * och hög gräns 2,0. Förbrukningen är 0,25 kWh per kvart. */
static void test_all_actions(void)
{
    const double prices[12]    = {0.1, 0.1, 0.5, 0.5, 0.5, 0.5, 2.0, 2.0, 2.0, 2.0, 0.1, 0.1};
    const double radiation[12] = {0, 0, 0, 1000, 400, 200, 1000, 0, 0, 0, 0, 0};
    const Action expected[12] = {
        ACT_CHARGE,    /* billig el: köp och ladda */
        ACT_CHARGE,
        ACT_BUY,       /* normalpris utan sol */
        ACT_CHARGE,    /* solöverskott lagras */
        ACT_CHARGE,
        ACT_USE_SOLAR, /* solen täcker större delen av förbrukningen */
        ACT_SELL,      /* överskott vid högt pris säljs */
        ACT_DISCHARGE, /* dyr el: använd batteriet */
        ACT_DISCHARGE,
        ACT_DISCHARGE,
        ACT_CHARGE,
        ACT_CHARGE,
    };

    Slot slots[12];
    Plan *plan = calloc(1, sizeof(Plan));
    fill(slots, 12, prices, radiation);

    CHECK(compute_plan(slots, 12, &PARAMS, plan) == 0);
    CHECK(plan->count == 12);
    for (int i = 0; i < 12; i++)
        CHECK(plan->slots[i].action == expected[i]);

    /* Kvart 0: 0,25 kWh förbrukning + 0,75 kWh laddning köps från nätet. */
    CHECK_NEAR(plan->slots[0].grid_kwh, 1.0, 1e-9);
    CHECK_NEAR(plan->slots[0].battery_kwh, 0.75, 1e-9);
    /* Kvart 3: 0,9 producerat - 0,25 förbrukat = 0,65 till batteriet. */
    CHECK_NEAR(plan->slots[3].battery_kwh, 2.15, 1e-9);
    CHECK_NEAR(plan->slots[3].grid_kwh, 0.0, 1e-9);
    /* Kvart 6: hela överskottet säljs. */
    CHECK_NEAR(plan->slots[6].grid_kwh, -0.65, 1e-9);
    /* Kvart 7: batteriet täcker förbrukningen, inget köps. */
    CHECK_NEAR(plan->slots[7].grid_kwh, 0.0, 1e-9);
    CHECK_NEAR(plan->slots[7].battery_kwh, 2.01, 1e-9);

    free(plan);
}

static void test_no_battery(void)
{
    const double prices[12]    = {0.1, 0.1, 0.5, 0.5, 0.5, 0.5, 2.0, 2.0, 2.0, 2.0, 0.1, 0.1};
    const double radiation[12] = {0, 0, 0, 1000, 400, 200, 1000, 0, 0, 0, 0, 0};
    ComputeParams params = PARAMS;
    params.battery_capacity_kwh = 0.0;

    Slot slots[12];
    Plan *plan = calloc(1, sizeof(Plan));
    fill(slots, 12, prices, radiation);
    CHECK(compute_plan(slots, 12, &params, plan) == 0);

    for (int i = 0; i < 12; i++)
    {
        CHECK(plan->slots[i].action != ACT_CHARGE);
        CHECK(plan->slots[i].action != ACT_DISCHARGE);
        CHECK_NEAR(plan->slots[i].battery_kwh, 0.0, 1e-9);
    }
    CHECK(plan->slots[3].action == ACT_SELL);
    free(plan);
}

/* Utan prisskillnad ska batteriet aldrig laddas från nätet. */
static void test_flat_prices(void)
{
    const double prices[8]    = {1, 1, 1, 1, 1, 1, 1, 1};
    const double radiation[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    Slot slots[8];
    Plan *plan = calloc(1, sizeof(Plan));
    fill(slots, 8, prices, radiation);
    CHECK(compute_plan(slots, 8, &PARAMS, plan) == 0);

    for (int i = 0; i < 8; i++)
    {
        CHECK(plan->slots[i].action == ACT_BUY);
        CHECK_NEAR(plan->slots[i].grid_kwh, 0.25, 1e-9);
    }
    free(plan);
}

/* Energibalans och batterigränser på en full 72-timmarsprognos med
 * pseudoslumpade värden. */
static void test_invariants(void)
{
    Slot *slots = calloc(MAX_SLOTS, sizeof(Slot));
    Plan *plan = calloc(1, sizeof(Plan));
    unsigned seed = 12345;

    for (int i = 0; i < MAX_SLOTS; i++)
    {
        seed = seed * 1103515245u + 12345u;
        slots[i].ts = BASE + i * SLOT_SECONDS;
        slots[i].price = (double)((seed >> 16) % 300) / 100.0 - 0.2;
        seed = seed * 1103515245u + 12345u;
        slots[i].radiation = (double)((seed >> 16) % 900);
    }
    CHECK(compute_plan(slots, MAX_SLOTS, &PARAMS, plan) == 0);

    double previous = 0.0;
    int balanced = 1, within_limits = 1, rate_ok = 1;
    for (int i = 0; i < plan->count; i++)
    {
        const PlanSlot *s = &plan->slots[i];
        double delta = s->battery_kwh - previous;

        /* produktion + nät = förbrukning + ändring i batteriet */
        if (fabs(s->production_kwh + s->grid_kwh - s->consumption_kwh - delta) > 1e-9)
            balanced = 0;
        if (s->battery_kwh < -1e-9 || s->battery_kwh > PARAMS.battery_capacity_kwh + 1e-9)
            within_limits = 0;
        if (fabs(delta) > PARAMS.battery_max_kw * SLOT_HOURS + 1e-9)
            rate_ok = 0;
        previous = s->battery_kwh;
    }
    CHECK(balanced);
    CHECK(within_limits);
    CHECK(rate_ok);

    free(slots);
    free(plan);
}

static void test_invalid_arguments(void)
{
    Slot slot;
    Plan *plan = calloc(1, sizeof(Plan));
    memset(&slot, 0, sizeof(slot));
    CHECK(compute_plan(NULL, 1, &PARAMS, plan) == -1);
    CHECK(compute_plan(&slot, 0, &PARAMS, plan) == -1);
    CHECK(compute_plan(&slot, MAX_SLOTS + 1, &PARAMS, plan) == -1);
    CHECK(compute_plan(&slot, 1, NULL, plan) == -1);
    CHECK(compute_plan(&slot, 1, &PARAMS, NULL) == -1);
    CHECK(compute_plan(&slot, 1, &PARAMS, plan) == 0);
    free(plan);
}

int main(void)
{
    test_production();
    test_all_actions();
    test_no_battery();
    test_flat_prices();
    test_invariants();
    test_invalid_arguments();
    return TEST_RESULT();
}
