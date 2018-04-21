/***************************************************************************
 *            nc_hipert_first_order.c
 *
 *  Mon October 09 16:58:16 2017
 *  Copyright  2017  Sandro Dias Pinto Vitenti
 *  <sandro@isoftware.com.br>
 ****************************************************************************/
/*
 * nc_hipert_first_order.c
 * Copyright (C) 2017 Sandro Dias Pinto Vitenti <sandro@isoftware.com.br>
 *
 * numcosmo is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * numcosmo is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * SECTION:nc_hipert_first_order
 * @title: NcHIPertFirstOrder
 * @short_description: Base object for implementing first order perturbation in a Friedmann background.
 *
 * FIXME
 *
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif /* HAVE_CONFIG_H */
#include "build_cfg.h"

#include "perturbations/nc_hipert_first_order.h"
#include "nc_recomb_seager.h"
#include "nc_enum_types.h"

#ifndef NUMCOSMO_GIR_SCAN
#include "math/rcm.h"

#include <cvodes/cvodes.h>
#include <cvodes/cvodes_band.h>

#include <arkode/arkode.h>
#include <arkode/arkode_band.h>

#include <nvector/nvector_serial.h>
#endif /* NUMCOSMO_GIR_SCAN */

typedef struct _NcHIPertFirstOrderVar
{
  gint src;
  gint index;
  GArray *deps;
} NcHIPertFirstOrderVar;

struct _NcHIPertFirstOrderPrivate
{
  NcHIPertGrav *grav;
  GPtrArray *comps;
  GPtrArray *active_comps;
  GArray *vars;
  NcHIPertBGVar *bg_var;
  NcHIPertGravGauge gauge;
  gpointer cvode;
  gboolean cvode_init;
#ifdef HAVE_SUNDIALS_ARKODE
  gpointer arkode;
  gboolean arkode_init;
#endif /* HAVE_SUNDIALS_ARKODE */
  guint cur_sys_size;
  N_Vector y;
  N_Vector abstol_v;
  gdouble reltol;
  gdouble abstol;
  gint mupper;
  gint mlower;
  NcHIPertFirstOrderInteg integ;
  NcHIPertGravTScalar *T_scalar_i;
  NcHIPertGravTScalar *T_scalar_tot;
  NcHIPertGravScalar *G_scalar;
};

enum
{
  PROP_0,
  PROP_GAUGE,
  PROP_GRAV,
  PROP_CARRAY,
  PROP_DIST,
  PROP_RECOMB,
  PROP_SCALEFACTOR,
  PROP_RELTOL,
  PROP_ABSTOL,
  PROP_INTEG,
  PROP_LEN,
};

G_DEFINE_TYPE (NcHIPertFirstOrder, nc_hipert_first_order, NC_TYPE_HIPERT_BOLTZMANN);

void 
_nc_hipert_first_order_clear_var (NcHIPertFirstOrderVar *var)
{
  g_clear_pointer (&var->deps, (GDestroyNotify) g_array_unref);
}

static void
nc_hipert_first_order_init (NcHIPertFirstOrder *fo)
{
  fo->priv               = G_TYPE_INSTANCE_GET_PRIVATE (fo, NC_TYPE_HIPERT_FIRST_ORDER, NcHIPertFirstOrderPrivate);
  fo->priv->grav         = NULL;
  fo->priv->comps        = g_ptr_array_new ();
  fo->priv->active_comps = g_ptr_array_new ();
  fo->priv->vars         = g_array_new (TRUE, TRUE, sizeof (NcHIPertFirstOrderVar));
  fo->priv->bg_var       = nc_hipert_bg_var_new ();
  fo->priv->gauge        = NC_HIPERT_GRAV_GAUGE_LEN;

  fo->priv->cvode        = NULL;
  fo->priv->cvode_init   = FALSE;
  fo->priv->arkode       = NULL;
  fo->priv->arkode_init  = FALSE;

  fo->priv->reltol       = 0.0;
  fo->priv->abstol       = 0.0;

  fo->priv->mupper       = 0;
  fo->priv->mlower       = 0;

  fo->priv->integ        = NC_HIPERT_FIRST_ORDER_INTEG_LEN;
  fo->priv->cur_sys_size = 0;
  fo->priv->y            = NULL;
  fo->priv->abstol_v     = NULL;

  fo->priv->T_scalar_i   = nc_hipert_grav_T_scalar_new ();
  fo->priv->T_scalar_tot = nc_hipert_grav_T_scalar_new ();
  fo->priv->G_scalar     = nc_hipert_grav_scalar_new ();

  g_array_set_clear_func (fo->priv->vars, (GDestroyNotify)_nc_hipert_first_order_clear_var);  
  g_ptr_array_set_free_func (fo->priv->active_comps, (GDestroyNotify)nc_hipert_comp_free);
}

static void
_nc_hipert_first_order_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  NcHIPertFirstOrder *fo = NC_HIPERT_FIRST_ORDER (object);
  g_return_if_fail (NC_IS_HIPERT_FIRST_ORDER (object));

  switch (prop_id)
  {
    case PROP_GAUGE:
      nc_hipert_first_order_set_gauge (fo, g_value_get_enum (value));
      break;    
    case PROP_GRAV:
      nc_hipert_first_order_set_grav (fo, g_value_get_object (value));
      break;    
    case PROP_CARRAY:
    {
      NcmObjArray *oa = (NcmObjArray *) g_value_get_boxed (value);
      if (oa != NULL)
      {
        guint i;
        for (i = 0; i < oa->len; i++)
        {
          NcHIPertComp *comp = NC_HIPERT_COMP (ncm_obj_array_peek (oa, i));
          nc_hipert_first_order_add_comp (fo, comp);
        }
      }
      break;
    }
    case PROP_DIST:
      nc_hipert_bg_var_set_dist (fo->priv->bg_var, g_value_get_object (value));
      break;    
    case PROP_RECOMB:
      nc_hipert_bg_var_set_recomb (fo->priv->bg_var, g_value_get_object (value));
      break;    
    case PROP_SCALEFACTOR:
      nc_hipert_bg_var_set_scalefactor (fo->priv->bg_var, g_value_get_object (value));
      break;
    case PROP_RELTOL:
      nc_hipert_first_order_set_reltol (fo, g_value_get_double (value));
      break;
    case PROP_ABSTOL:
      nc_hipert_first_order_set_abstol (fo, g_value_get_double (value));
      break;
    case PROP_INTEG:
      nc_hipert_first_order_set_integ (fo, g_value_get_enum (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
_nc_hipert_first_order_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  NcHIPertFirstOrder *fo = NC_HIPERT_FIRST_ORDER (object);
  g_return_if_fail (NC_IS_HIPERT_FIRST_ORDER (object));

  switch (prop_id)
  {
    case PROP_GAUGE:
      g_value_set_enum (value, nc_hipert_first_order_get_gauge (fo));
      break;    
    case PROP_GRAV:
      g_value_take_object (value, nc_hipert_first_order_get_grav (fo));
      break;    
    case PROP_CARRAY:
    {
      NcmObjArray *oa = ncm_obj_array_new ();
      guint i;

      for (i = 0; i < fo->priv->comps->len; i++)
      {
        NcHIPertComp *comp = g_ptr_array_index (fo->priv->comps, i);
        if (comp != NULL)
          ncm_obj_array_add (oa, G_OBJECT (comp));
      }

      g_value_take_boxed (value, oa);
      break;
    }    
    case PROP_DIST:
      g_value_take_object (value, nc_hipert_bg_var_get_dist (fo->priv->bg_var));
      break;    
    case PROP_RECOMB:
      g_value_take_object (value, nc_hipert_bg_var_get_recomb (fo->priv->bg_var));
      break;    
    case PROP_SCALEFACTOR:
      g_value_take_object (value, nc_hipert_bg_var_get_scalefactor (fo->priv->bg_var));
      break;
    case PROP_RELTOL:
      g_value_set_double (value, nc_hipert_first_order_get_reltol (fo));
      break;
    case PROP_ABSTOL:
      g_value_set_double (value, nc_hipert_first_order_get_abstol (fo));
      break;
    case PROP_INTEG:
      g_value_set_enum (value, nc_hipert_first_order_get_integ (fo));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
_nc_hipert_first_order_dispose (GObject *object)
{
  NcHIPertFirstOrder *fo = NC_HIPERT_FIRST_ORDER (object);

  nc_hipert_grav_clear (&fo->priv->grav);
  
  if (fo->priv->comps != NULL)
  {
    guint i;
    for (i = 0; i < fo->priv->comps->len; i++)
    {
      NcHIPertComp *comp = NC_HIPERT_COMP (g_ptr_array_index (fo->priv->comps, i));

      nc_hipert_comp_clear (&comp);
      g_ptr_array_index (fo->priv->comps, i) = comp; 
    }
  }

  g_clear_pointer (&fo->priv->comps,        (GDestroyNotify) g_ptr_array_unref);
  g_clear_pointer (&fo->priv->active_comps, (GDestroyNotify) g_ptr_array_unref);

  nc_hipert_bg_var_clear (&fo->priv->bg_var);
  
  /* Chain up : end */
  G_OBJECT_CLASS (nc_hipert_first_order_parent_class)->dispose (object);
}

static void
_nc_hipert_first_order_finalize (GObject *object)
{
  NcHIPertFirstOrder *fo = NC_HIPERT_FIRST_ORDER (object);

  if (fo->priv->cvode != NULL)
  {
    CVodeFree (&fo->priv->cvode);
    fo->priv->cvode      = NULL;
    fo->priv->cvode_init = FALSE;
  }
#ifdef HAVE_SUNDIALS_ARKODE
  if (fo->priv->arkode != NULL)
  {
    ARKodeFree (&fo->priv->arkode);
    fo->priv->arkode      = NULL;
    fo->priv->arkode_init = FALSE;
  }
#endif /* HAVE_SUNDIALS_ARKODE */  

  g_clear_pointer (&fo->priv->y, N_VDestroy);
  g_clear_pointer (&fo->priv->abstol_v, N_VDestroy);

  fo->priv->cur_sys_size = 0;

  g_clear_pointer (&fo->priv->T_scalar_i,   nc_hipert_grav_T_scalar_free);
  g_clear_pointer (&fo->priv->T_scalar_tot, nc_hipert_grav_T_scalar_free);

  g_clear_pointer (&fo->priv->G_scalar,     nc_hipert_grav_scalar_free);
  
  /* Chain up : end */
  G_OBJECT_CLASS (nc_hipert_first_order_parent_class)->finalize (object);
}

static void
nc_hipert_first_order_class_init (NcHIPertFirstOrderClass *klass)
{
  GObjectClass* object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (NcHIPertFirstOrderPrivate));

  object_class->set_property = &_nc_hipert_first_order_set_property;
  object_class->get_property = &_nc_hipert_first_order_get_property;
  object_class->dispose      = &_nc_hipert_first_order_dispose;
  object_class->finalize     = &_nc_hipert_first_order_finalize;

  g_object_class_install_property (object_class,
                                   PROP_GAUGE,
                                   g_param_spec_enum ("gauge",
                                                      NULL,
                                                      "Gauge",
                                                      NC_TYPE_HIPERT_GRAV_GAUGE, NC_HIPERT_GRAV_GAUGE_SYNCHRONOUS, 
                                                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_NAME | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (object_class,
                                   PROP_GRAV,
                                   g_param_spec_object ("grav",
                                                        NULL,
                                                        "Gravitation object",
                                                        NC_TYPE_HIPERT_GRAV,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_NAME | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (object_class,
                                   PROP_CARRAY,
                                   g_param_spec_boxed ("comp-array",
                                                       NULL,
                                                       "Components array",
                                                       NCM_TYPE_OBJ_ARRAY,
                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_NAME | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (object_class,
                                   PROP_DIST,
                                   g_param_spec_object ("distance",
                                                        NULL,
                                                        "Distance object",
                                                        NC_TYPE_DISTANCE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_NAME | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (object_class,
                                   PROP_RECOMB,
                                   g_param_spec_object ("recomb",
                                                        NULL,
                                                        "Recombination object",
                                                        NC_TYPE_RECOMB,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_NAME | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (object_class,
                                   PROP_SCALEFACTOR,
                                   g_param_spec_object ("scalefactor",
                                                        NULL,
                                                        "Scale factor object",
                                                        NC_TYPE_SCALEFACTOR,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_NAME | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (object_class,
                                   PROP_RELTOL,
                                   g_param_spec_double ("reltol",
                                                        NULL,
                                                        "Relative tolerance",
                                                        0.0, 1.0, NCM_DEFAULT_PRECISION,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_NAME | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (object_class,
                                   PROP_ABSTOL,
                                   g_param_spec_double ("abstol",
                                                        NULL,
                                                        "Absolute tolerance tolerance",
                                                        0.0, G_MAXDOUBLE, 0.0,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_NAME | G_PARAM_STATIC_BLURB));
  g_object_class_install_property (object_class,
                                   PROP_INTEG,
                                   g_param_spec_enum ("integ",
                                                        NULL,
                                                        "ODE integrator",
                                                        NC_TYPE_HIPERT_FIRST_ORDER_INTEG,
#ifdef HAVE_SUNDIALS_ARKODE
                                                        NC_HIPERT_FIRST_ORDER_INTEG_ARKODE,
#else
                                                        NC_HIPERT_FIRST_ORDER_INTEG_CVODE,
#endif /* HAVE_SUNDIALS_ARKODE */                                                      
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_NAME | G_PARAM_STATIC_BLURB));

}

/**
 * nc_hipert_first_order_new:
 * 
 * Creates a new #NcHIPertFirstOrder.
 * 
 * Returns: (transfer full): the newly instantiated #NcHIPertFirstOrder.
 */
NcHIPertFirstOrder *
nc_hipert_first_order_new (void)
{
  NcDistance *dist = nc_distance_new (1.0);
  NcRecomb *recomb = NC_RECOMB (nc_recomb_seager_new ());
  NcScalefactor *a = nc_scalefactor_new (0, 1.0, dist);

  NcHIPertFirstOrder *fo = nc_hipert_first_order_new_full (dist, recomb, a);

  nc_distance_free (dist);
  nc_recomb_free (recomb);
  nc_scalefactor_free (a);

  return fo;
}

/**
 * nc_hipert_first_order_new_full:
 * @dist: a #NcDistance
 * @recomb: a #NcRecomb
 * @a: a #NcScalefactor
 * 
 * Creates a new #NcHIPertFirstOrder.
 * 
 * Returns: (transfer full): the newly instantiated #NcHIPertFirstOrder.
 */
NcHIPertFirstOrder *
nc_hipert_first_order_new_full (NcDistance *dist, NcRecomb *recomb, NcScalefactor *a)
{
  NcHIPertFirstOrder *fo = g_object_new (NC_TYPE_HIPERT_FIRST_ORDER,
                                         "distance",    dist,
                                         "recomb",      recomb,
                                         "scalefactor", a,
                                         NULL);
  return fo;
}


/**
 * nc_hipert_first_order_ref:
 * @fo: a #NcHIPertFirstOrder
 *
 * Increases the reference count of @fo.
 *
 * Returns: (transfer full): @fo.
 */
NcHIPertFirstOrder *
nc_hipert_first_order_ref (NcHIPertFirstOrder *fo)
{
  return g_object_ref (fo);
}

/**
 * nc_hipert_first_order_free:
 * @fo: a #NcHIPertFirstOrder
 *
 * Decreases the reference count of @fo.
 *
 */
void 
nc_hipert_first_order_free (NcHIPertFirstOrder *fo)
{
  g_object_unref (fo);
}

/**
 * nc_hipert_first_order_clear:
 * @fo: a #NcHIPertFirstOrder
 *
 * Decreases the reference count of *@fo and sets the pointer *@fo to NULL.
 *
 */
void 
nc_hipert_first_order_clear (NcHIPertFirstOrder **fo)
{
  g_clear_object (fo);
}

static void 
_nc_hipert_first_order_add_pad (GArray *a, gint pad)
{
  guint i;
  if (pad == 0)
    return;
  
  for (i = 0; i < a->len; i++)
  {
    if (g_array_index (a, gint, i) >= 0)
    {
      g_array_index (a, gint, i) += pad;
    }
  }
}

#define APPEND(a,b) (g_array_append_vals ((a), (b)->data, (b)->len))

gint __cmp_gint (gconstpointer a, gconstpointer b) { const gint *u = a; const gint *v = b; return (u[0] < v[0]) ? -1 : ((u[0] > v[0]) ? 1 : 0); }

static void
_nc_hipert_first_order_solve_deps (NcHIPertFirstOrder *fo, NcHIPertGravInfo *ginfo, NcHIPertGravTScalarInfo *Tsinfo, GArray *deps, guint r)
{
  gboolean subs = FALSE;
  guint i;

  if (r > 9)
    g_error ("_nc_hipert_first_order_solve_deps: too many recursion levels.");
  
  for (i = 0; i < deps->len; )
  {
    const gint v = g_array_index (deps, gint, i);
    /*printf ("%d %d %d\n", i, deps->len, v);*/
    
    if (v < 0)
    {
      g_array_remove_index (deps, i);
      subs = TRUE;
      switch (v)
      {
        case NC_HIPERT_GRAV_SELEM_PHI:
          /*g_message ("Appending phi     %d!\n", ginfo->phi_deps->len);*/
          APPEND (deps, ginfo->phi_deps);
          break;
        case NC_HIPERT_GRAV_SELEM_DSIGMA:
/*          g_message ("Appending dsigma  %d!\n", ginfo->dsigma_deps->len);*/
          APPEND (deps, ginfo->dsigma_deps);
          break;
        case NC_HIPERT_GRAV_SELEM_PSI:
          /*g_message ("Appending psi     %d!\n", ginfo->psi_deps->len);*/
          APPEND (deps, ginfo->psi_deps);
          break;
        case NC_HIPERT_GRAV_SELEM_DOTPSI:
          /*g_message ("Appending dotphi  %d!\n", ginfo->dotpsi_deps->len);*/
          APPEND (deps, ginfo->dotpsi_deps);
          break;
        case NC_HIPERT_GRAV_SELEM_DRHO:
          /*g_message ("Appending drho    %d!\n", Tsinfo->drho_deps->len);*/
          APPEND (deps, Tsinfo->drho_deps);
          break;
        case NC_HIPERT_GRAV_SELEM_RHOPPV:
          /*g_message ("Appending rhoppv  %d!\n", Tsinfo->rhoppv_deps->len);*/
          APPEND (deps, Tsinfo->rhoppv_deps);
          break;
        case NC_HIPERT_GRAV_SELEM_DP:
          /*g_message ("Appending dp      %d!\n", Tsinfo->dp_deps->len);*/
          APPEND (deps, Tsinfo->dp_deps);
          break;
        case NC_HIPERT_GRAV_SELEM_DPI:
          /*g_message ("Appending dPi     %d!\n", Tsinfo->dPi_deps->len);*/
          APPEND (deps, Tsinfo->dPi_deps);
          break;
        default:
          g_assert_not_reached ();
          break;
      }
    }
    else
      i++;
  }

  if (subs)
  {
    _nc_hipert_first_order_solve_deps (fo, ginfo, Tsinfo, deps, r + 1);
  }
  else
  {
    g_array_sort (deps, &__cmp_gint);
    if (deps->len > 1)
    {
      gint last = g_array_index (deps, gint, 0);
      for (i = 1; i < deps->len; )
      {
        gint v = g_array_index (deps, gint, i);
        if (v == last)
        {
          g_array_remove_index (deps, i);
        }
        else
        {
          last = v;
          i++;
        }
      }
    }
  }
}

static void
_nc_hipert_first_order_arrange_vars (NcHIPertFirstOrder *fo)
{
  gint *adj;
  gint node_num = fo->priv->vars->len;
  gint adj_max  = node_num * (node_num - 1); 
  gint adj_num  = 0;
  gint *adj_row;
  gint *perm;
  gint *perm_inv;
  gint i;
  gchar *Jrow = g_new0 (gchar, node_num + 1);

  adj_row  = g_new0 (gint, node_num + 1);
  adj      = g_new0 (gint, adj_max);
  perm     = g_new0 (gint, node_num);
  perm_inv = g_new0 (gint, node_num);

  if (FALSE)
  {
    gint lll = fo->priv->vars->len - 1;
    g_array_append_val (g_array_index (fo->priv->vars, NcHIPertFirstOrderVar, 0).deps, lll);
  }
  
  adj_set (node_num, adj_max, &adj_num, adj_row, adj, -1, -1 );

  ncm_message ("#\n# Original jacobian:\n#\n");
  for (i = 0; i < node_num; i++)
  {
    NcHIPertFirstOrderVar var = g_array_index (fo->priv->vars, NcHIPertFirstOrderVar, i);
    gint j;

    for (j = 0; j < node_num; j++)
    {
      if (i == j)
        Jrow[j] = 'D'; 
      else
        Jrow[j] = '.';
    }
    Jrow[j] = '\0';
    
    for (j = 0; j < var.deps->len; j++)
    {
      gint dep = g_array_index (var.deps, gint, j);
      adj_set (node_num, adj_max, &adj_num, adj_row, adj, var.index + 1, dep + 1);
      if (var.index != dep)
        Jrow[dep] = 'X';
      /*printf ("%d %d %d %d\n", var.index, dep, i, j);*/
    }
    ncm_message ("#  %s\n", Jrow);
  }
/*  
  g_message ("#\n");
  adj_print ( node_num, adj_num, adj_row, adj, "  Adjacency matrix:" );

  adj_show ( node_num, adj_num, adj_row, adj );

  bandwidth = adj_bandwidth ( node_num, adj_num, adj_row, adj );

  g_message ("#    ADJ bandwidth = %d\n#\n", bandwidth);
*/
  genrcm ( node_num, adj_num, adj_row, adj, perm );

  perm_inverse3 ( node_num, perm, perm_inv );
  /*g_message ("#\n#    The RCM permutation and inverse:\n#\n");*/

  for ( i = 0; i < node_num; i++ )
  {
    NcHIPertFirstOrderVar *var = &g_array_index (fo->priv->vars, NcHIPertFirstOrderVar, i);
    var->index = perm[i] - 1;
    /*g_message ("#    %8d  %8d  %8d | %8d  %8d\n", i + 1, perm[i], perm_inv[i], perm[perm_inv[i]-1], perm_inv[perm[i]-1]);*/
  }

/*
  g_message ("#\n#    Permuted adjacency matrix:\n#\n");

  adj_perm_show ( node_num, adj_num, adj_row, adj, perm, perm_inv );
*/
  
  fo->priv->mupper = 0;
  fo->priv->mlower = 0;
    
  ncm_message ("#\n# Reordered jacobian:\n#\n");
  for (i = 0; i < node_num; i++)
  {
    NcHIPertFirstOrderVar var = g_array_index (fo->priv->vars, NcHIPertFirstOrderVar, perm[i] - 1);
    gint j;

    for (j = 0; j < node_num; j++)
    {
      if (i == j)
        Jrow[j] = 'D'; 
      else
        Jrow[j] = '.';
    }
    Jrow[j] = '\0';

    for (j = 0; j < var.deps->len; j++)
    {
      gint dep = perm_inv[g_array_index (var.deps, gint, j)] - 1;

      fo->priv->mupper = MAX (fo->priv->mupper, dep - i);
      fo->priv->mlower = MAX (fo->priv->mlower, i - dep);

      if (i != dep)
        Jrow[dep] = 'X';
    }
    ncm_message ("#  %s\n", Jrow);
  }

  g_free (Jrow);
  g_message ("#\n#  ADJ (permuted) bandwidth = (%d, %d)\n", fo->priv->mupper, fo->priv->mlower);

  g_free (adj);
  g_free (adj_row);
  g_free (perm);
  g_free (perm_inv);
}

static void
_nc_hipert_first_order_prepare_internal (NcHIPertFirstOrder *fo)
{
  if (fo->priv->grav != NULL)
  {
    NcHIPertGravInfo *ginfo         = nc_hipert_grav_get_G_scalar_info (fo->priv->grav);
    NcHIPertGravTScalarInfo *Tsinfo = nc_hipert_grav_T_scalar_info_new ();
    const guint grav_ndyn           = nc_hipert_grav_ndyn_var (fo->priv->grav);

    guint i, pad = 0;

    g_array_set_size (fo->priv->vars, 0);
      
    /* Adding gravitation potentials to the variables list */
    for (i = 0; i < grav_ndyn; i++)
    {
      GArray *grav_dyn_var_i_deps = nc_hipert_grav_get_deps (fo->priv->grav, i);
      NcHIPertFirstOrderVar var = {-1, fo->priv->vars->len, grav_dyn_var_i_deps};

      _nc_hipert_first_order_add_pad (grav_dyn_var_i_deps, pad);

      g_array_append_val (fo->priv->vars, var);
    }
    pad = fo->priv->vars->len;

    for (i = 0; i < fo->priv->comps->len; i++)
    {
      NcHIPertComp *comp = NC_HIPERT_COMP (g_ptr_array_index (fo->priv->comps, i));
      if (comp != NULL)
      {
        NcHIPertGravTScalarInfo *Tsinfo_i = nc_hipert_comp_get_T_scalar_info (comp);
        guint ndyn = nc_hipert_comp_ndyn_var (comp);
        guint j;

        nc_hipert_grav_T_scalar_info_add_pad (Tsinfo_i, pad);
        nc_hipert_grav_T_scalar_info_append (Tsinfo, Tsinfo_i);
        nc_hipert_grav_T_scalar_info_free (Tsinfo_i);

        for (j = 0; j < ndyn; j++)
        {
          GArray *comp_j_deps       = nc_hipert_comp_get_deps (comp, j);
          NcHIPertFirstOrderVar var = {i, fo->priv->vars->len, comp_j_deps};

          _nc_hipert_first_order_add_pad (comp_j_deps, pad);
          
          g_array_append_val (fo->priv->vars, var);
        }
        pad = fo->priv->vars->len;
      }
    }

    if (FALSE)
    {
      guint i;

      ncm_cfg_msg_sepa ();
      g_message ("# phi deps:    ");
      for (i = 0; i < ginfo->phi_deps->len; i++)
      {
        g_message (" %2d", g_array_index (ginfo->phi_deps, gint, i));
      }
      g_message ("\n");
      g_message ("# dsigma deps: ");
      for (i = 0; i < ginfo->dsigma_deps->len; i++)
      {
        g_message (" %2d", g_array_index (ginfo->dsigma_deps, gint, i));
      }
      g_message ("\n");
      g_message ("# psi deps:    ");
      for (i = 0; i < ginfo->psi_deps->len; i++)
      {
        g_message (" %2d", g_array_index (ginfo->psi_deps, gint, i));
      }
      g_message ("\n");
      g_message ("# dotpsi deps: ");
      for (i = 0; i < ginfo->dotpsi_deps->len; i++)
      {
        g_message (" %2d", g_array_index (ginfo->dotpsi_deps, gint, i));
      }
      g_message ("\n");


      g_message ("# drho deps:   ");
      for (i = 0; i < Tsinfo->drho_deps->len; i++)
      {
        g_message (" %2d", g_array_index (Tsinfo->drho_deps, gint, i));
      }
      g_message ("\n");
      g_message ("# rhoppv deps: ");
      for (i = 0; i < Tsinfo->rhoppv_deps->len; i++)
      {
        g_message (" %2d", g_array_index (Tsinfo->rhoppv_deps, gint, i));
      }
      g_message ("\n");
      g_message ("# dp deps:     ");
      for (i = 0; i < Tsinfo->dp_deps->len; i++)
      {
        g_message (" %2d", g_array_index (Tsinfo->dp_deps, gint, i));
      }
      g_message ("\n");
      g_message ("# dPi deps:    ");
      for (i = 0; i < Tsinfo->dPi_deps->len; i++)
      {
        g_message (" %2d", g_array_index (Tsinfo->dPi_deps, gint, i));
      }
      g_message ("\n");
    }
    
    {
      guint i;
      for (i = 0; i < fo->priv->vars->len; i++)
      {
        NcHIPertFirstOrderVar var = g_array_index (fo->priv->vars, NcHIPertFirstOrderVar, i);
        _nc_hipert_first_order_solve_deps (fo, ginfo, Tsinfo, var.deps, 0);
      }      
    }

    _nc_hipert_first_order_arrange_vars (fo);
      
    nc_hipert_grav_T_scalar_info_free (Tsinfo);
    nc_hipert_grav_info_free (ginfo);
  }
}

/**
 * nc_hipert_first_order_set_gauge:
 * @fo: a #NcHIPertFirstOrder
 * @gauge: a #NcHIPertGravGauge
 *
 * Sets the gauge to be used in the first order system.
 *
 */
void 
nc_hipert_first_order_set_gauge (NcHIPertFirstOrder *fo, NcHIPertGravGauge gauge)
{
  if (gauge != fo->priv->gauge)
  {
    guint i;
    if (fo->priv->grav != NULL)
      nc_hipert_grav_set_gauge (fo->priv->grav, gauge);

    for (i = 0; i < fo->priv->comps->len; i++)
    {
      NcHIPertComp *comp = NC_HIPERT_COMP (g_ptr_array_index (fo->priv->comps, i));
      if (comp != NULL)
        nc_hipert_comp_set_gauge (comp, gauge);
    }

    fo->priv->gauge = gauge;
    _nc_hipert_first_order_prepare_internal (fo);
  }
}

/**
 * nc_hipert_first_order_get_gauge:
 * @fo: a #NcHIPertFirstOrder
 * 
 * Gets the gauge used by @fo.
 * 
 * Returns: the gauge used by @fo
 */
NcHIPertGravGauge 
nc_hipert_first_order_get_gauge (NcHIPertFirstOrder *fo)
{
  return fo->priv->gauge;
}

/**
 * nc_hipert_first_order_set_reltol:
 * @fo: a #NcHIPertFirstOrder
 * @reltol: relative tolerance used during the integration
 * 
 * Sets the relative tolerance to @reltol.
 * 
 */
void 
nc_hipert_first_order_set_reltol (NcHIPertFirstOrder *fo, const gdouble reltol)
{
  fo->priv->reltol = reltol;
}

/**
 * nc_hipert_first_order_set_abstol:
 * @fo: a #NcHIPertFirstOrder
 * @abstol: absolute tolerance used during the integration
 * 
 * Sets the absolute tolerance to @abstol.
 * 
 */
void 
nc_hipert_first_order_set_abstol (NcHIPertFirstOrder *fo, const gdouble abstol)
{
  fo->priv->abstol = abstol;
}

/**
 * nc_hipert_first_order_get_reltol:
 * @fo: a #NcHIPertFirstOrder
 *
 * Gets the relative tolerance.
 * 
 * Returns: the current relative tolerance.
 */
gdouble 
nc_hipert_first_order_get_reltol (NcHIPertFirstOrder *fo)
{
  return fo->priv->reltol;
}

/**
 * nc_hipert_first_order_get_abstol:
 * @fo: a #NcHIPertFirstOrder
 *
 * Gets the absolute tolerance.
 * 
 * Returns: the current absolute tolerance.
 */
gdouble 
nc_hipert_first_order_get_abstol (NcHIPertFirstOrder *fo)
{
  return fo->priv->abstol;
}

/**
 * nc_hipert_first_order_set_integ:
 * @fo: a #NcHIPertFirstOrder
 *
 * Sets the integrator to be used.
 * 
 */
void
nc_hipert_first_order_set_integ (NcHIPertFirstOrder *fo, NcHIPertFirstOrderInteg integ)
{
  if (fo->priv->integ != integ)
  {
    fo->priv->integ = integ;
  }
}

/**
 * nc_hipert_first_order_get_integ:
 *
 * Gets the integrator used.
 * 
 * Returns: the current integrator used by @fo.
 */
NcHIPertFirstOrderInteg 
nc_hipert_first_order_get_integ (NcHIPertFirstOrder *fo)
{
  return fo->priv->integ;
}

/**
 * nc_hipert_first_order_set_grav:
 * @fo: a #NcHIPertFirstOrder
 * @grav: a #NcHIPertGrav
 *
 * Sets the gravitation object.
 *
 */
void 
nc_hipert_first_order_set_grav (NcHIPertFirstOrder *fo, NcHIPertGrav *grav)
{
  nc_hipert_grav_clear (&fo->priv->grav);
  if (grav != NULL)
  {
    fo->priv->grav = nc_hipert_grav_ref (grav);
    nc_hipert_grav_set_gauge (fo->priv->grav, fo->priv->gauge);
    _nc_hipert_first_order_prepare_internal (fo);
  }
}

/**
 * nc_hipert_first_order_get_grav:
 * @fo: a #NcHIPertFirstOrder
 * 
 * Gets the gravitation #NcHIPertGrav object.
 * 
 * Returns: (transfer full) (nullable): the #NcHIPertGrav object used by @fo.
 */
NcHIPertGrav *
nc_hipert_first_order_get_grav (NcHIPertFirstOrder *fo)
{
  return (fo->priv->grav != NULL) ? nc_hipert_grav_ref (fo->priv->grav) : fo->priv->grav;
}

/**
 * nc_hipert_first_order_peek_grav:
 * @fo: a #NcHIPertFirstOrder
 * 
 * Peeks the #NcHIPertGrav object.
 * 
 * Returns: (transfer none) (nullable): the #NcHIPertGrav object used by @fo.
 */
NcHIPertGrav *
nc_hipert_first_order_peek_grav (NcHIPertFirstOrder *fo)
{
  return fo->priv->grav;
}

/**
 * nc_hipert_first_order_add_comp:
 * @fo: a #NcHIPertFirstOrder
 * @comp: a #NcHIPertComp
 *
 * Adds a new component @comp to the system.
 *
 */
void 
nc_hipert_first_order_add_comp (NcHIPertFirstOrder *fo, NcHIPertComp *comp)
{
  const guint len    = nc_hipert_bg_var_len (fo->priv->bg_var);
  NcHIPertBGVarID id = nc_hipert_comp_get_id (comp);

  if (NC_IS_HIPERT_GRAV (comp))
    g_error ("nc_hipert_first_order_add_comp: the gravitation component should be added using `nc_hipert_first_order_set_grav'.");

  g_assert_cmpint (id, >=, 0);
  g_assert_cmpint (id, <, len);
  g_ptr_array_set_size (fo->priv->comps, len);

  if (g_ptr_array_index (fo->priv->comps, id) != NULL)
    g_warning ("nc_hipert_first_order_add_comp: component with `%d' (%s) already included, ignoring...", id, G_OBJECT_TYPE_NAME (comp));
  else
  {
    g_ptr_array_index (fo->priv->comps, id) = nc_hipert_comp_ref (comp);

    g_ptr_array_add (fo->priv->active_comps, nc_hipert_comp_ref (comp));
      
    nc_hipert_comp_set_gauge (comp, fo->priv->gauge);
    _nc_hipert_first_order_prepare_internal (fo);
  }
}

typedef struct _NcHIPertFirstOrderWS
{
  NcHIPertFirstOrder *fo;
  NcHIPertBGVarYDY *ydy;
  NcHICosmo *cosmo;
} NcHIPertFirstOrderWS;

static gint
_nc_hipert_first_order_f (realtype t, N_Vector y, N_Vector ydot, gpointer f_data)
{
  NcHIPertFirstOrderWS *ws = (NcHIPertFirstOrderWS *) f_data;
  NcHIPertFirstOrder *fo = ws->fo;
  NcHICosmo *cosmo       = ws->cosmo;
  NcHIPertBGVarYDY *ydy  = ws->ydy;
  NcHIPertBGVar *bg_var  = fo->priv->bg_var;
  const guint ncomps     = fo->priv->active_comps->len;
  guint i;

  nc_hicosmo_get_bg_var (cosmo, t, bg_var);
  ydy->y  = y;
  ydy->dy = ydot;
  
  nc_hipert_grav_T_scalar_set_zero (fo->priv->T_scalar_tot);
  
  for (i = 0; i < ncomps; i++)
  {
    NcHIPertComp *comp = g_ptr_array_index (fo->priv->active_comps, i);

    nc_hipert_grav_T_scalar_set_zero (fo->priv->T_scalar_i);
    nc_hipert_comp_get_T_scalar (comp, bg_var, ydy, fo->priv->T_scalar_i);
    nc_hipert_grav_T_scalar_add (fo->priv->T_scalar_tot, fo->priv->T_scalar_tot, fo->priv->T_scalar_i);
  }

  nc_hipert_grav_scalar_set_zero (fo->priv->G_scalar);
  nc_hipert_grav_get_G_scalar (fo->priv->grav, bg_var, ydy, fo->priv->T_scalar_tot, fo->priv->G_scalar);

  nc_hipert_grav_get_dy_scalar (fo->priv->grav, bg_var, ydy, fo->priv->T_scalar_tot, fo->priv->G_scalar);
  
  for (i = 0; i < ncomps; i++)
  {
    NcHIPertComp *comp = g_ptr_array_index (fo->priv->active_comps, i);

    nc_hipert_comp_get_dy_scalar (comp, bg_var, ydy, fo->priv->T_scalar_tot, fo->priv->G_scalar);
  }
  
  return 0;
}

static gdouble
_nc_hipert_first_order_set_init_cond (NcHIPertFirstOrder *fo, const gdouble k)
{

  return 0.0;
}

static void
_nc_hipert_first_order_prepare_integrator (NcHIPertFirstOrder *fo, const gdouble t0)
{
  gint flag;

  if (fo->priv->cur_sys_size != fo->priv->vars->len)
  {
    if (fo->priv->cvode != NULL)
    {
      CVodeFree (&fo->priv->cvode);
      fo->priv->cvode      = NULL;
      fo->priv->cvode_init = FALSE;
    }
#ifdef HAVE_SUNDIALS_ARKODE
    if (fo->priv->arkode != NULL)
    {
      ARKodeFree (&fo->priv->arkode);
      fo->priv->arkode      = NULL;
      fo->priv->arkode_init = FALSE;
    }
#endif /* HAVE_SUNDIALS_ARKODE */  

    g_clear_pointer (&fo->priv->y, N_VDestroy);
    g_clear_pointer (&fo->priv->abstol_v, N_VDestroy);

    fo->priv->cur_sys_size = fo->priv->vars->len;

    fo->priv->y        = N_VNew_Serial (fo->priv->cur_sys_size);
    fo->priv->abstol_v = N_VNew_Serial (fo->priv->cur_sys_size);    
  }

  N_VConst (fo->priv->abstol, fo->priv->abstol_v);

  switch (fo->priv->integ)
  {
    case NC_HIPERT_FIRST_ORDER_INTEG_CVODE:
    {
      if (fo->priv->cvode_init)
      {
        fo->priv->cvode = CVodeCreate (CV_BDF, CV_NEWTON); /*CVodeCreate (CV_BDF, CV_NEWTON);*/
      
        flag = CVodeInit (fo->priv->cvode, &_nc_hipert_first_order_f, t0, fo->priv->y);
        NCM_CVODE_CHECK (&flag, "CVodeInit", 1, );

        flag = CVodeSVtolerances (fo->priv->cvode, fo->priv->reltol, fo->priv->abstol_v);
        NCM_CVODE_CHECK (&flag, "CVodeSVtolerances", 1, );

        flag = CVodeSetMaxNumSteps (fo->priv->cvode, 0);
        NCM_CVODE_CHECK (&flag, "CVodeSetMaxNumSteps", 1, );

        flag = CVBand (fo->priv->cvode, fo->priv->cur_sys_size, fo->priv->mupper, fo->priv->mlower);
        NCM_CVODE_CHECK (&flag, "CVDense", 1, );

        flag = CVDlsSetDenseJacFn (fo->priv->cvode, NULL /*J*/);
        NCM_CVODE_CHECK (&flag, "CVDlsSetDenseJacFn", 1, );

        flag = CVodeSetInitStep (fo->priv->cvode, fabs (t0) * fo->priv->reltol);
        NCM_CVODE_CHECK (&flag, "CVodeSetInitStep", 1, );

        fo->priv->cvode_init = TRUE;
      }
      else
      {    
        flag = CVodeReInit (fo->priv->cvode, t0, fo->priv->y);
        NCM_CVODE_CHECK (&flag, "CVodeInit", 1, );

        flag = CVodeSetInitStep (fo->priv->cvode, fabs (t0) * fo->priv->reltol);
        NCM_CVODE_CHECK (&flag, "CVodeSetInitStep", 1, );
      }  
      break;
    }
#ifdef HAVE_SUNDIALS_ARKODE      
    case NC_HIPERT_FIRST_ORDER_INTEG_ARKODE:
    {
#define INTTYPE _nc_hipert_first_order_f, NULL
      if (!fo->priv->arkode_init)
      {
        fo->priv->arkode = ARKodeCreate ();
        
        flag = ARKodeInit (fo->priv->arkode, INTTYPE, t0, fo->priv->y);
        NCM_CVODE_CHECK (&flag, "ARKodeInit", 1, );

        flag = ARKodeSVtolerances (fo->priv->arkode, fo->priv->reltol, fo->priv->abstol_v);
        NCM_CVODE_CHECK (&flag, "ARKodeSVtolerances", 1, );

        flag = ARKodeSetMaxNumSteps (fo->priv->arkode, 0);
        NCM_CVODE_CHECK (&flag, "ARKodeSetMaxNumSteps", 1, );

        flag = ARKBand (fo->priv->arkode, fo->priv->cur_sys_size, fo->priv->mupper, fo->priv->mlower);
        NCM_CVODE_CHECK (&flag, "ARKDense", 1, );

        flag = ARKDlsSetDenseJacFn (fo->priv->arkode, /*J*/ NULL);
        NCM_CVODE_CHECK (&flag, "ARKDlsSetDenseJacFn", 1, );

        flag = ARKodeSetLinear (fo->priv->arkode, 1);
        NCM_CVODE_CHECK (&flag, "ARKodeSetLinear", 1, );

        flag = ARKodeSetOrder (fo->priv->arkode, 7);
        NCM_CVODE_CHECK (&flag, "ARKodeSetOrder", 1, );

        //flag = ARKodeSetERKTableNum (fo->priv->arkode, FEHLBERG_13_7_8);
        //NCM_CVODE_CHECK (&flag, "ARKodeSetERKTableNum", 1, );

        flag = ARKodeSetInitStep (fo->priv->arkode, fabs (t0) * fo->priv->reltol);
        NCM_CVODE_CHECK (&flag, "ARKodeSetInitStep", 1, );

        fo->priv->arkode_init = TRUE;
      }
      else
      {
        flag = ARKodeReInit (fo->priv->arkode, INTTYPE, t0, fo->priv->y);
        NCM_CVODE_CHECK (&flag, "ARKodeInit", 1, );

        flag = ARKodeSetInitStep (fo->priv->arkode, fabs (t0) * fo->priv->reltol);
        NCM_CVODE_CHECK (&flag, "ARKodeSetInitStep", 1, );
      }
      break;
    }
#endif /* HAVE_SUNDIALS_ARKODE */
    default:
      g_error ("_nc_hipert_first_order_prepare_integrator: integrator %d not supported.", fo->priv->integ);
      break;
  }
}

/**
 * nc_hipert_first_order_prepare:
 * @fo: a #NcHIPertFirstOrder
 * @cosmo: a #NcHICosmo
 *
 * Adds a new component @comp to the system.
 *
 */
void 
nc_hipert_first_order_prepare (NcHIPertFirstOrder *fo, NcHICosmo *cosmo)
{
  gdouble t0;
  nc_hipert_bg_var_prepare_if_needed (fo->priv->bg_var, cosmo);


  t0 = _nc_hipert_first_order_set_init_cond (fo, 1.0);
  _nc_hipert_first_order_prepare_integrator (fo, t0);


  
}