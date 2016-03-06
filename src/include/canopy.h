#ifndef CANOPY_H
#define CANOPY_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>


#include "gday.h"
#include "structures.h"
#include "constants.h"
#include "water_balance.h"
#include "utilities.h"
#include "radiation.h"
#include "photosynthesis.h"

/* C stuff */
void    zero_carbon_day_fluxes(fluxes *);
void    zero_hourly_fluxes(canopy_wk *);
void    update_daily_carbon_fluxes(fluxes *, params *, double, double);
void    canopy(control *, fluxes *, met *, params *, state *, double *,
               double *);
void    solve_leaf_energy_balance(control *, canopy_wk *, fluxes *, met *,
                                  params *, state *, double, double *,
                                  double *, double *);
void    sum_hourly_carbon_fluxes(canopy_wk *, fluxes *, params *);
void    scale_to_canopy(canopy_wk *);
double  calc_leaf_net_rad(params *, state *, double, double, double);
void    calculate_top_of_canopy_leafn(canopy_wk *, params *, state *);



#endif /* CANOPY_H */
