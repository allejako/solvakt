#ifndef SOLVAKT_COMPUTE_H
#define SOLVAKT_COMPUTE_H

#include "common/types.h"

/* Compute-steget: solproduktion per kvart och en energiplan som säger när
 * el ska köpas, användas direkt, lagras i batteriet eller säljas. */

typedef struct
{
    double panel_area_m2;
    double panel_efficiency;     /* 0-1 */
    double base_load_kw;         /* hushållets förbrukning */
    double battery_capacity_kwh; /* 0 = inget batteri */
    double battery_max_kw;       /* högsta ladd- och urladdningseffekt */
} ComputeParams;

/* Förväntad produktion i kWh under en kvart vid given instrålning (W/m²). */
double compute_production_kwh(double radiation, const ComputeParams *params);

/* Bygger planen för slots[0..count-1]. Batteriet antas vara tomt vid start.
 * Returnerar 0, eller -1 vid ogiltiga argument. */
int compute_plan(const Slot *slots, int count, const ComputeParams *params, Plan *plan);

#endif
