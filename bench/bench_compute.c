/* Benchmark för beräkningssteget: kör compute_plan() många gånger på en
 * syntetisk 48-timmarsprognos och mäter tiden per plan.
 *
 *   bench_compute [antal iterationer]
 */

#include "server/compute.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SLOTS 192 /* 48 timmar */

static void fill_forecast(Slot *slots)
{
    unsigned seed = 2026;
    for (int i = 0; i < SLOTS; i++)
    {
        int quarter_of_day = i % 96;
        double hour = quarter_of_day / 4.0;

        /* Sol mellan 06 och 18 som en sinusbåge med topp 700 W/m². */
        double sun = hour > 6.0 && hour < 18.0 ? sin((hour - 6.0) / 12.0 * M_PI) * 700.0 : 0.0;

        /* Pris: basnivå, morgon- och kvällstopp samt lite brus. */
        seed = seed * 1103515245u + 12345u;
        double noise = (double)((seed >> 16) % 100) / 500.0;
        double price = 0.6 + noise;
        if (hour >= 7.0 && hour < 9.0)
            price += 0.8;
        if (hour >= 17.0 && hour < 20.0)
            price += 1.2;
        if (hour < 5.0)
            price -= 0.4;

        slots[i].ts = (time_t)i * SLOT_SECONDS;
        slots[i].radiation = sun;
        slots[i].cloud_cover = 30.0;
        slots[i].temperature = 10.0;
        slots[i].price = price;
        slots[i].has_weather = 1;
        slots[i].has_price = 1;
    }
}

static double elapsed_ms(const struct timespec *start, const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) * 1000.0 +
           (double)(end->tv_nsec - start->tv_nsec) / 1e6;
}

int main(int argc, char **argv)
{
    int iterations = argc > 1 ? atoi(argv[1]) : 200;
    if (iterations < 1)
        iterations = 1;

    const ComputeParams params = {
        .panel_area_m2 = 20.0,
        .panel_efficiency = 0.18,
        .base_load_kw = 1.0,
        .battery_capacity_kwh = 10.0,
        .battery_max_kw = 3.0,
    };

    Slot *slots = calloc(SLOTS, sizeof(Slot));
    Plan *plan = calloc(1, sizeof(Plan));
    if (slots == NULL || plan == NULL)
        return 1;
    fill_forecast(slots);

    /* Summan används så att kompilatorn inte kan optimera bort anropen,
     * och visar att resultatet är detsamma före och efter en optimering. */
    double checksum = 0.0;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < iterations; i++)
    {
        if (compute_plan(slots, SLOTS, &params, plan) != 0)
            return 1;
        checksum += plan->slots[i % SLOTS].battery_kwh + plan->slots[SLOTS - 1].grid_kwh;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double total = elapsed_ms(&start, &end);
    printf("iterationer : %d\n", iterations);
    printf("total tid   : %.2f ms\n", total);
    printf("per plan    : %.2f us\n", total * 1000.0 / iterations);
    printf("planer/s    : %.0f\n", iterations / (total / 1000.0));
    printf("kontrollsum : %.6f\n", checksum);

    free(slots);
    free(plan);
    return 0;
}
