/* ============================================================================
* Calculates all within canopy C & water fluxes (live in water balance).
*
*
* NOTES:
*   - Should restructure the code so that MATE is called from within the canopy
*     space, rather than via plant growth
*
*   Future improvements:
*    - Add a two-stream approximation.
*    - Add a clumping term to the extinction coefficients for apar calcs
*
*
* AUTHOR:
*   Martin De Kauwe
*
* DATE:
*   09.02.2016
*
* =========================================================================== */
#include "canopy.h"

void canopy(canopy_wk *cw, control *c, fluxes *f, met_arrays *ma, met *m,
            params *p, state *s) {
    /*
        Canopy module consists of two parts:
        (1) a radiation sub-model to calculate apar of sunlit/shaded leaves
            - this is all handled in radiation.c
        (2) a coupled model of stomatal conductance, photosynthesis and
            the leaf energy balance to solve the leaf temperature and partition
            absorbed net radiation between sensible and latent heat.
        - The canopy is represented by a single layer with two big leaves
          (sunlit & shaded).

        - The logic broadly follows MAESTRA code, with some restructuring.

        References
        ----------
        * Wang & Leuning (1998) Agricultural & Forest Meterorology, 91, 89-111.
        * Dai et al. (2004) Journal of Climate, 17, 2281-2299.
        * De Pury & Farquhar (1997) PCE, 20, 537-557.
    */
    int    hod, iter = 0, itermax = 100, dummy, sunlight_hrs;
    double doy;


    /* loop through the day */
    zero_carbon_day_fluxes(f);
    zero_water_day_fluxes(f);
    sunlight_hrs = 0;
    doy = ma->doy[c->hour_idx];
    for (hod = 0; hod < c->num_hlf_hrs; hod++) {
        unpack_met_data(c, ma, m, hod);

        /* calculates diffuse frac from half-hourly incident radiation */
        calculate_solar_geometry(cw, p, doy, hod);
        get_diffuse_frac(cw, doy, m->sw_rad);

        /* Is the sun up? */
        if (cw->elevation > 0.0 && m->par > 20.0) {
            calculate_absorbed_radiation(cw, p, s, m->par);
            calculate_top_of_canopy_leafn(cw, p, s);

            /* sunlit / shaded loop */
            for (cw->leaf_idx = 0; cw->leaf_idx < NUM_LEAVES; cw->leaf_idx++) {

                /* initialise values of Tleaf, Cs, dleaf at the leaf surface */
                initialise_leaf_surface(cw, m);

                /* Leaf temperature loop */
                while (TRUE) {

                    if (c->ps_pathway == C3) {
                        photosynthesis_C3(c, cw, m, p, s);
                    } else {
                        /* Nothing implemented */
                        fprintf(stderr, "C4 photosynthesis not implemented\n");
                        exit(EXIT_FAILURE);
                    }

                    if (cw->an_leaf[cw->leaf_idx] > 1E-04) {
                        /* Calculate new Cs, dleaf, Tleaf */
                        solve_leaf_energy_balance(c, cw, f, m, p, s);


                    } else {
                        break;
                    }

                    if (iter >= itermax) {
                        fprintf(stderr, "No convergence in canopy loop:\n");
                        exit(EXIT_FAILURE);
                    } else if (fabs(cw->tleaf - cw->tleaf_new) < 0.02) {
                        break;
                    }

                    /* Update temperature & do another iteration */
                    cw->tleaf = cw->tleaf_new;
                    iter++;
                } /* end of leaf temperature loop */
            } /* end of sunlit/shaded leaf loop */
        } else {
            zero_hourly_fluxes(cw);

            /*
             * pre-dawn soil water potential, clearly one should link this
             * the actual sun-rise :). Here 10 = 5 am, 10 is num_half_hr
             */
            if (hod == 10) {
                calc_soil_water_potential(c, p, s);
                /**printf("%lf %.10lf\n", s->wtfac_root, s->psi_s_root );*/
            }



        }
        scale_to_canopy(cw);
        sum_hourly_carbon_fluxes(cw, f, p);
        calculate_water_balance(c, f, m, p, s, dummy, cw->trans_canopy,
                                cw->omega_canopy, cw->rnet_canopy);

        c->hour_idx++;
        sunlight_hrs++;
    } /* end of hour loop */

    /* work out average omega for the day over sunlight hours */
    f->omega /= sunlight_hrs;

    /* work out average omega for the day, including the night */
    m->tsoil /= c->num_hlf_hrs;

    if (c->water_stress) {
        /* Calculate the soil moisture availability factors [0,1] in the
           topsoil and the entire root zone */
        calculate_soil_water_fac(c, p, s);
    } else {
        /* really this should only be a debugging option! */
        s->wtfac_topsoil = 1.0;
        s->wtfac_root = 1.0;
    }



    return;
}

void solve_leaf_energy_balance(control *c, canopy_wk *cw, fluxes *f, met *m,
                              params *p, state *s) {
    /*
        Wrapper to solve conductances, transpiration and calculate a new
        leaf temperautre, vpd and Cs at the leaf surface.

        - The logic broadly follows MAESTRA code, with some restructuring.

        References
        ----------
        * Wang & Leuning (1998) Agricultural & Forest Meterorology, 91, 89-111.

    */
    int    idx;
    double omega, transpiration, LE, Tdiff, gv, gbc, gh, Tk, sw_rad;

    idx = cw->leaf_idx;
    sw_rad = cw->apar_leaf[idx] * PAR_2_SW; /* W m-2 */

    cw->rnet_leaf[idx] = calc_leaf_net_rad(p, s, m->tair, m->vpd, sw_rad);
    penman_leaf_wrapper(m, p, s, cw->tleaf, cw->rnet_leaf[idx],
                        cw->gsc_leaf[idx], &transpiration, &LE, &gbc, &gh, &gv,
                        &omega);

    /* store in structure */
    cw->trans_leaf[idx] = transpiration;
    cw->omega_leaf[idx] = omega;

    /*
     * calculate new Cs, dleaf & tleaf
     */
    Tdiff = (cw->rnet_leaf[idx] - LE) / (CP * MASS_AIR * gh);
    cw->tleaf_new = m->tair + Tdiff / 4.0;
    cw->Cs = m->Ca - cw->an_leaf[idx] / gbc;
    cw->dleaf = cw->trans_leaf[idx] * m->press / gv;

    return;
}

double calc_leaf_net_rad(params *p, state *s, double tair, double vpd,
                         double sw_rad) {

    double rnet, Tk, ea, emissivity_atm, net_lw_rad;
    /*
        extinction coefficient for diffuse radiation and black leaves
        (m2 ground m2 leaf)
    */
    double kd = 0.8;

    /* isothermal net LW radiaiton at top of canopy, assuming emissivity of
       the canopy is 1 */
    Tk = tair + DEG_TO_KELVIN;

    /* Isothermal net radiation (Leuning et al. 1995, Appendix) */
    ea = calc_sat_water_vapour_press(tair) - vpd;

    /* apparent emissivity for a hemisphere radiating at air temp eqn D4 */
    emissivity_atm = 0.642 * pow((ea / Tk), (1.0 / 7.0));

    net_lw_rad = (1.0 - emissivity_atm) * SIGMA * pow(Tk, 4.0);
    rnet = p->leaf_abs * sw_rad - net_lw_rad * kd * exp(-kd * s->lai);

    return (rnet);
}

void zero_carbon_day_fluxes(fluxes *f) {

    f->gpp_gCm2 = 0.0;
    f->npp_gCm2 = 0.0;
    f->gpp = 0.0;
    f->npp = 0.0;
    f->auto_resp = 0.0;
    f->apar = 0.0;

    return;
}




void calculate_top_of_canopy_leafn(canopy_wk *cw, params *p, state *s) {

    /*
    Calculate the N at the top of the canopy (g N m-2), N0.

    References:
    -----------
    * Chen et al 93, Oecologia, 93,63-69.

    */
    double Ntot, N0;
    double kn = 0.3; /* extinction coefficent for Nitrogen less steep */

    /* leaf mass per area (g C m-2 leaf) */
    double LMA = 1.0 / p->sla * p->cfracts * KG_AS_G;

    if (s->lai > 0.0) {
        /* the total amount of nitrogen in the canopy */
        Ntot = s->shootnc * LMA * s->lai;

        /* top of canopy leaf N (gN m-2) */
        cw->N0 = Ntot * kn / (1.0 - exp(-kn * s->lai));
    } else {
        cw->N0 = 0.0;
    }

    return;
}

void zero_hourly_fluxes(canopy_wk *cw) {

    int i;

    /* sunlit / shaded loop */
    for (i = 0; i < NUM_LEAVES; i++) {
        cw->an_leaf[i] = 0.0;
        cw->gsc_leaf[i] = 0.0;
        cw->trans_leaf[i] = 0.0;
        cw->rnet_leaf[i] = 0.0;
        cw->apar_leaf[i] = 0.0;
        cw->omega_leaf[i] = 0.0;
    }

    return;
}

void scale_to_canopy(canopy_wk *cw) {

    cw->an_canopy = cw->an_leaf[SUNLIT] + cw->an_leaf[SHADED];
    cw->gsc_canopy = cw->gsc_leaf[SUNLIT] + cw->gsc_leaf[SHADED];
    cw->apar_canopy = cw->apar_leaf[SUNLIT] + cw->apar_leaf[SHADED];
    cw->trans_canopy = cw->trans_leaf[SUNLIT] + cw->trans_leaf[SHADED];
    cw->omega_canopy = (cw->omega_leaf[SUNLIT] + cw->omega_leaf[SHADED]) / 2.0;
    cw->rnet_canopy = cw->rnet_leaf[SUNLIT] + cw->rnet_leaf[SHADED];

    return;
}

void sum_hourly_carbon_fluxes(canopy_wk *cw, fluxes *f, params *p) {

    /* umol m-2 s-1 -> gC m-2 30 min-1 */
    f->gpp_gCm2 += cw->an_canopy * UMOL_TO_MOL * MOL_C_TO_GRAMS_C * SEC_2_HLFHR;
    f->npp_gCm2 = f->gpp_gCm2 * p->cue;
    f->gpp = f->gpp_gCm2 * GRAM_C_2_TONNES_HA;
    f->npp = f->npp_gCm2 * GRAM_C_2_TONNES_HA;
    f->auto_resp = f->gpp - f->npp;
    f->apar += cw->apar_canopy;
    f->gs_mol_m2_sec += cw->gsc_canopy;

    return;
}

void initialise_leaf_surface(canopy_wk *cw, met *m) {
    /* initialise values of Tleaf, Cs, dleaf at the leaf surface */
    cw->tleaf = m->tair;
    cw->dleaf = m->vpd;
    cw->Cs = m->Ca;
}
