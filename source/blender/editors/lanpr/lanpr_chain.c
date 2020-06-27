/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2019 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup editors
 */

#include "BLI_linklist.h"
#include "BLI_listbase.h"
#include "BLI_math.h"

#include "BKE_customdata.h"
#include "BKE_object.h"

#include "DEG_depsgraph_query.h"

#include "DNA_camera_types.h"
#include "DNA_lanpr_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_scene_types.h"

#include "ED_lanpr.h"

#include "bmesh.h"

#include "lanpr_intern.h"

#include <math.h>

#define LANPR_OTHER_RV(rl, rv) ((rv) == (rl)->l ? (rl)->r : (rl)->l)

static eLineArtRenderLine *lineart_get_connected_render_line(eLineArtBoundingArea *ba,
                                                             eLineArtRenderVert *rv,
                                                             eLineArtRenderVert **new_rv,
                                                             int match_flag)
{
  LinkData *lip;
  eLineArtRenderLine *nrl;

  for (lip = ba->linked_lines.first; lip; lip = lip->next) {
    nrl = lip->data;

    if ((!(nrl->flags & LRT_EDGE_FLAG_ALL_TYPE)) || (nrl->flags & LRT_EDGE_FLAG_CHAIN_PICKED)) {
      continue;
    }

    if (match_flag && ((nrl->flags & LRT_EDGE_FLAG_ALL_TYPE) & match_flag) == 0) {
      continue;
    }

    /*  always chain connected lines for now. */
    /*  simplification will take care of the sharp points. */
    /*  if(cosine whatever) continue; */

    if (rv != nrl->l && rv != nrl->r) {
      if (nrl->flags & LRT_EDGE_FLAG_INTERSECTION) {
        if (rv->fbcoord[0] == nrl->l->fbcoord[0] && rv->fbcoord[1] == nrl->l->fbcoord[1]) {
          *new_rv = LANPR_OTHER_RV(nrl, nrl->l);
          return nrl;
        }
        else {
          if (rv->fbcoord[0] == nrl->r->fbcoord[0] && rv->fbcoord[1] == nrl->r->fbcoord[1]) {
            *new_rv = LANPR_OTHER_RV(nrl, nrl->r);
            return nrl;
          }
        }
      }
      continue;
    }

    *new_rv = LANPR_OTHER_RV(nrl, rv);
    return nrl;
  }

  return 0;
}

static eLineArtRenderLineChain *lineart_create_render_line_chain(eLineArtRenderBuffer *rb)
{
  eLineArtRenderLineChain *rlc;
  rlc = mem_static_aquire(&rb->render_data_pool, sizeof(eLineArtRenderLineChain));

  BLI_addtail(&rb->chains, rlc);

  return rlc;
}

static bool lineart_check_point_overlapping(eLineArtRenderLineChainItem *rlci,
                                            float x,
                                            float y,
                                            float threshold)
{
  if (!rlci) {
    return false;
  }
  if (rlci->pos[0] + threshold > x && rlci->pos[0] - threshold < x &&
      rlci->pos[1] + threshold > y && rlci->pos[1] - threshold < y) {
    return true;
  }
  return false;
}

static eLineArtRenderLineChainItem *lineart_append_render_line_chain_point(
    eLineArtRenderBuffer *rb,
    eLineArtRenderLineChain *rlc,
    float x,
    float y,
    float gx,
    float gy,
    float gz,
    float *normal,
    char type,
    int level)
{
  eLineArtRenderLineChainItem *rlci;

  if (lineart_check_point_overlapping(rlc->chain.last, x, y, 1e-10)) {
    /* Because segment type is determined by the leading chain point, so we need to ensure the type
     * and occlusion is correct after omitting overlapping point*/
    eLineArtRenderLineChainItem *old_rlci = rlc->chain.last;
    old_rlci->line_type = type;
    old_rlci->occlusion = level;
    return old_rlci;
  }

  rlci = mem_static_aquire(&rb->render_data_pool, sizeof(eLineArtRenderLineChainItem));

  rlci->pos[0] = x;
  rlci->pos[1] = y;
  rlci->gpos[0] = gx;
  rlci->gpos[1] = gy;
  rlci->gpos[2] = gz;
  copy_v3_v3(rlci->normal, normal);
  rlci->line_type = type & LRT_EDGE_FLAG_ALL_TYPE;
  rlci->occlusion = level;
  BLI_addtail(&rlc->chain, rlci);

  /*  printf("a %f,%f %d\n", x, y, level); */

  return rlci;
}

static eLineArtRenderLineChainItem *lineart_push_render_line_chain_point(
    eLineArtRenderBuffer *rb,
    eLineArtRenderLineChain *rlc,
    float x,
    float y,
    float gx,
    float gy,
    float gz,
    float *normal,
    char type,
    int level)
{
  eLineArtRenderLineChainItem *rlci;

  if (lineart_check_point_overlapping(rlc->chain.first, x, y, 1e-10)) {
    return rlc->chain.first;
  }

  rlci = mem_static_aquire(&rb->render_data_pool, sizeof(eLineArtRenderLineChainItem));

  rlci->pos[0] = x;
  rlci->pos[1] = y;
  rlci->gpos[0] = gx;
  rlci->gpos[1] = gy;
  rlci->gpos[2] = gz;
  copy_v3_v3(rlci->normal, normal);
  rlci->line_type = type & LRT_EDGE_FLAG_ALL_TYPE;
  rlci->occlusion = level;
  BLI_addhead(&rlc->chain, rlci);

  /*  printf("data %f,%f %d\n", x, y, level); */

  return rlci;
}

void ED_lineart_NO_THREAD_chain_feature_lines(eLineArtRenderBuffer *rb)
{
  eLineArtRenderLineChain *rlc;
  eLineArtRenderLineChainItem *rlci;
  eLineArtRenderLine *rl;
  eLineArtBoundingArea *ba;
  eLineArtRenderLineSegment *rls;
  int last_occlusion;

  for (rl = rb->all_render_lines.first; rl; rl = rl->next) {

    if ((!(rl->flags & LRT_EDGE_FLAG_ALL_TYPE)) || (rl->flags & LRT_EDGE_FLAG_CHAIN_PICKED)) {
      continue;
    }

    rl->flags |= LRT_EDGE_FLAG_CHAIN_PICKED;

    rlc = lineart_create_render_line_chain(rb);

    rlc->object_ref = rl->object_ref; /*  can only be the same object in a chain. */
    rlc->type = (rl->flags & LRT_EDGE_FLAG_ALL_TYPE);

    eLineArtRenderLine *new_rl = rl;
    eLineArtRenderVert *new_rv;
    float N[3] = {0};

    if (rl->tl) {
      N[0] += rl->tl->gn[0];
      N[1] += rl->tl->gn[1];
      N[2] += rl->tl->gn[2];
    }
    if (rl->tr) {
      N[0] += rl->tr->gn[0];
      N[1] += rl->tr->gn[1];
      N[2] += rl->tr->gn[2];
    }
    if (rl->tl || rl->tr) {
      normalize_v3(N);
    }

    /*  step 1: grow left */
    ba = ED_lineart_get_point_bounding_area_deep(rb, rl->l->fbcoord[0], rl->l->fbcoord[1]);
    new_rv = rl->l;
    rls = rl->segments.first;
    lineart_push_render_line_chain_point(rb,
                                         rlc,
                                         new_rv->fbcoord[0],
                                         new_rv->fbcoord[1],
                                         new_rv->gloc[0],
                                         new_rv->gloc[1],
                                         new_rv->gloc[2],
                                         N,
                                         rl->flags,
                                         rls->occlusion);
    while (ba && (new_rl = lineart_get_connected_render_line(ba, new_rv, &new_rv, rl->flags))) {
      new_rl->flags |= LRT_EDGE_FLAG_CHAIN_PICKED;

      if (new_rl->tl || new_rl->tr) {
        zero_v3(N);
        if (new_rl->tl) {
          N[0] += new_rl->tl->gn[0];
          N[1] += new_rl->tl->gn[1];
          N[2] += new_rl->tl->gn[2];
        }
        if (new_rl->tr) {
          N[0] += new_rl->tr->gn[0];
          N[1] += new_rl->tr->gn[1];
          N[2] += new_rl->tr->gn[2];
        }
        normalize_v3(N);
      }

      if (new_rv == new_rl->l) {
        for (rls = new_rl->segments.last; rls; rls = rls->prev) {
          double gpos[3], lpos[3];
          interp_v3_v3v3_db(lpos, new_rl->l->fbcoord, new_rl->r->fbcoord, rls->at);
          interp_v3_v3v3_db(gpos, new_rl->l->gloc, new_rl->r->gloc, rls->at);
          lineart_push_render_line_chain_point(rb,
                                               rlc,
                                               lpos[0],
                                               lpos[1],
                                               gpos[0],
                                               gpos[1],
                                               gpos[2],
                                               N,
                                               new_rl->flags,
                                               rls->occlusion);
          last_occlusion = rls->occlusion;
        }
      }
      else if (new_rv == new_rl->r) {
        rls = new_rl->segments.first;
        last_occlusion = rls->occlusion;
        rls = rls->next;
        for (; rls; rls = rls->next) {
          double gpos[3], lpos[3];
          interp_v3_v3v3_db(lpos, new_rl->l->fbcoord, new_rl->r->fbcoord, rls->at);
          interp_v3_v3v3_db(gpos, new_rl->l->gloc, new_rl->r->gloc, rls->at);
          lineart_push_render_line_chain_point(rb,
                                               rlc,
                                               lpos[0],
                                               lpos[1],
                                               gpos[0],
                                               gpos[1],
                                               gpos[2],
                                               N,
                                               new_rl->flags,
                                               last_occlusion);
          last_occlusion = rls->occlusion;
        }
        lineart_push_render_line_chain_point(rb,
                                             rlc,
                                             new_rl->r->fbcoord[0],
                                             new_rl->r->fbcoord[1],
                                             new_rl->r->gloc[0],
                                             new_rl->r->gloc[1],
                                             new_rl->r->gloc[2],
                                             N,
                                             new_rl->flags,
                                             last_occlusion);
      }
      ba = ED_lineart_get_point_bounding_area_deep(rb, new_rv->fbcoord[0], new_rv->fbcoord[1]);
    }

    /* Restore normal value */
    if (rl->tl || rl->tr) {
      zero_v3(N);
      if (rl->tl) {
        N[0] += rl->tl->gn[0];
        N[1] += rl->tl->gn[1];
        N[2] += rl->tl->gn[2];
      }
      if (rl->tr) {
        N[0] += rl->tr->gn[0];
        N[1] += rl->tr->gn[1];
        N[2] += rl->tr->gn[2];
      }
      normalize_v3(N);
    }
    /*  step 2: this line */
    rls = rl->segments.first;
    last_occlusion = ((eLineArtRenderLineSegment *)rls)->occlusion;
    for (rls = rls->next; rls; rls = rls->next) {
      double gpos[3], lpos[3];
      interp_v3_v3v3_db(lpos, rl->l->fbcoord, rl->r->fbcoord, rls->at);
      interp_v3_v3v3_db(gpos, rl->l->gloc, rl->r->gloc, rls->at);
      lineart_append_render_line_chain_point(
          rb, rlc, lpos[0], lpos[1], gpos[0], gpos[1], gpos[2], N, rl->flags, rls->occlusion);
      last_occlusion = rls->occlusion;
    }
    lineart_append_render_line_chain_point(rb,
                                           rlc,
                                           rl->r->fbcoord[0],
                                           rl->r->fbcoord[1],
                                           rl->r->gloc[0],
                                           rl->r->gloc[1],
                                           rl->r->gloc[2],
                                           N,
                                           rl->flags,
                                           last_occlusion);

    /*  step 3: grow right */
    ba = ED_lineart_get_point_bounding_area_deep(rb, rl->r->fbcoord[0], rl->r->fbcoord[1]);
    new_rv = rl->r;
    /*  below already done in step 2 */
    /*  lineart_push_render_line_chain_point(rb,rlc,new_rv->fbcoord[0],new_rv->fbcoord[1],rl->flags,0);
     */
    while (ba && (new_rl = lineart_get_connected_render_line(ba, new_rv, &new_rv, rl->flags))) {
      new_rl->flags |= LRT_EDGE_FLAG_CHAIN_PICKED;

      if (new_rl->tl || new_rl->tr) {
        zero_v3(N);
        if (new_rl->tl) {
          N[0] += new_rl->tl->gn[0];
          N[1] += new_rl->tl->gn[1];
          N[2] += new_rl->tl->gn[2];
        }
        if (new_rl->tr) {
          N[0] += new_rl->tr->gn[0];
          N[1] += new_rl->tr->gn[1];
          N[2] += new_rl->tr->gn[2];
        }
        normalize_v3(N);
      }

      /*  fix leading vertex type */
      rlci = rlc->chain.last;
      rlci->line_type = new_rl->flags & LRT_EDGE_FLAG_ALL_TYPE;

      if (new_rv == new_rl->l) {
        rls = new_rl->segments.last;
        last_occlusion = rls->occlusion;
        rlci->occlusion = last_occlusion; /*  fix leading vertex occlusion */
        for (rls = new_rl->segments.last; rls; rls = rls->prev) {
          double gpos[3], lpos[3];
          interp_v3_v3v3_db(lpos, new_rl->l->fbcoord, new_rl->r->fbcoord, rls->at);
          interp_v3_v3v3_db(gpos, new_rl->l->gloc, new_rl->r->gloc, rls->at);
          last_occlusion = rls->prev ? rls->prev->occlusion : last_occlusion;
          lineart_append_render_line_chain_point(rb,
                                                 rlc,
                                                 lpos[0],
                                                 lpos[1],
                                                 gpos[0],
                                                 gpos[1],
                                                 gpos[2],
                                                 N,
                                                 new_rl->flags,
                                                 last_occlusion);
        }
      }
      else if (new_rv == new_rl->r) {
        rls = new_rl->segments.first;
        last_occlusion = rls->occlusion;
        rlci->occlusion = last_occlusion;
        rls = rls->next;
        for (; rls; rls = rls->next) {
          double gpos[3], lpos[3];
          interp_v3_v3v3_db(lpos, new_rl->l->fbcoord, new_rl->r->fbcoord, rls->at);
          interp_v3_v3v3_db(gpos, new_rl->l->gloc, new_rl->r->gloc, rls->at);
          lineart_append_render_line_chain_point(rb,
                                                 rlc,
                                                 lpos[0],
                                                 lpos[1],
                                                 gpos[0],
                                                 gpos[1],
                                                 gpos[2],
                                                 N,
                                                 new_rl->flags,
                                                 rls->occlusion);
          last_occlusion = rls->occlusion;
        }
        lineart_append_render_line_chain_point(rb,
                                               rlc,
                                               new_rl->r->fbcoord[0],
                                               new_rl->r->fbcoord[1],
                                               new_rl->r->gloc[0],
                                               new_rl->r->gloc[1],
                                               new_rl->r->gloc[2],
                                               N,
                                               new_rl->flags,
                                               last_occlusion);
      }
      ba = ED_lineart_get_point_bounding_area_deep(rb, new_rv->fbcoord[0], new_rv->fbcoord[1]);
    }
  }
}

static eLineArtBoundingArea *lineart_get_rlci_bounding_area_recursive(
    eLineArtRenderBuffer *rb, eLineArtBoundingArea *root, eLineArtRenderLineChainItem *rlci)
{
  if (root->child == NULL) {
    return root;
  }
  else {
    eLineArtBoundingArea *ch = root->child;
#define IN_BOUND(ba, rlci) \
  ba.l <= rlci->pos[0] && ba.r >= rlci->pos[0] && ba.b <= rlci->pos[1] && ba.u >= rlci->pos[1]

    if (IN_BOUND(ch[0], rlci)) {
      return lineart_get_rlci_bounding_area_recursive(rb, &ch[0], rlci);
    }
    else if (IN_BOUND(ch[1], rlci)) {
      return lineart_get_rlci_bounding_area_recursive(rb, &ch[1], rlci);
    }
    else if (IN_BOUND(ch[2], rlci)) {
      return lineart_get_rlci_bounding_area_recursive(rb, &ch[2], rlci);
    }
    else if (IN_BOUND(ch[3], rlci)) {
      return lineart_get_rlci_bounding_area_recursive(rb, &ch[3], rlci);
    }
#undef IN_BOUND
  }
  return NULL;
}
static eLineArtBoundingArea *lineart_get_end_point_bounding_area(eLineArtRenderBuffer *rb,
                                                                 eLineArtRenderLineChainItem *rlci)
{
  if (!rlci) {
    return NULL;
  }
  eLineArtBoundingArea *root = ED_lineart_get_point_bounding_area(rb, rlci->pos[0], rlci->pos[1]);
  if (root == NULL) {
    return NULL;
  }
  return lineart_get_rlci_bounding_area_recursive(rb, root, rlci);
}

/*  if reduction threshold is even larger than a small bounding area, */
/*  then 1) geometry is simply too dense. */
/*       2) probably need to add it to root bounding area which has larger surface area then it
 * will */
/*       cover typical threshold values. */
static void lineart_link_point_with_bounding_area_recursive(eLineArtRenderBuffer *rb,
                                                            eLineArtBoundingArea *root,
                                                            eLineArtRenderLineChain *rlc,
                                                            eLineArtRenderLineChainItem *rlci)
{
  if (root->child == NULL) {
    eLineArtChainRegisterEntry *cre = list_append_pointer_static_sized(
        &root->linked_chains, &rb->render_data_pool, rlc, sizeof(eLineArtChainRegisterEntry));

    cre->rlci = rlci;

    if (rlci == rlc->chain.first) {
      cre->is_left = 1;
    }
  }
  else {
    eLineArtBoundingArea *ch = root->child;

#define IN_BOUND(ba, rlci) \
  ba.l <= rlci->pos[0] && ba.r >= rlci->pos[0] && ba.b <= rlci->pos[1] && ba.u >= rlci->pos[1]

    if (IN_BOUND(ch[0], rlci)) {
      lineart_link_point_with_bounding_area_recursive(rb, &ch[0], rlc, rlci);
    }
    else if (IN_BOUND(ch[1], rlci)) {
      lineart_link_point_with_bounding_area_recursive(rb, &ch[1], rlc, rlci);
    }
    else if (IN_BOUND(ch[2], rlci)) {
      lineart_link_point_with_bounding_area_recursive(rb, &ch[2], rlc, rlci);
    }
    else if (IN_BOUND(ch[3], rlci)) {
      lineart_link_point_with_bounding_area_recursive(rb, &ch[3], rlc, rlci);
    }

#undef IN_BOUND
  }
}

static void lineart_link_chain_with_bounding_areas(eLineArtRenderBuffer *rb,
                                                   eLineArtRenderLineChain *rlc)
{
  eLineArtRenderLineChainItem *pl = rlc->chain.first;
  eLineArtRenderLineChainItem *pr = rlc->chain.last;
  eLineArtBoundingArea *ba1 = ED_lineart_get_point_bounding_area(rb, pl->pos[0], pl->pos[1]);
  eLineArtBoundingArea *ba2 = ED_lineart_get_point_bounding_area(rb, pr->pos[0], pr->pos[1]);

  if (ba1) {
    lineart_link_point_with_bounding_area_recursive(rb, ba1, rlc, pl);
  }
  if (ba2) {
    lineart_link_point_with_bounding_area_recursive(rb, ba2, rlc, pr);
  }
}

void ED_lineart_split_chains_for_fixed_occlusion(eLineArtRenderBuffer *rb)
{
  eLineArtRenderLineChain *rlc, *new_rlc;
  eLineArtRenderLineChainItem *rlci, *next_rlci, *prev_rlci;
  ListBase swap = {0};

  swap.first = rb->chains.first;
  swap.last = rb->chains.last;

  rb->chains.last = rb->chains.first = NULL;

  while ((rlc = BLI_pophead(&swap)) != NULL) {
    rlc->next = rlc->prev = NULL;
    BLI_addtail(&rb->chains, rlc);
    eLineArtRenderLineChainItem *first_rlci = (eLineArtRenderLineChainItem *)rlc->chain.first;
    int fixed_occ = first_rlci->occlusion;
    rlc->level = fixed_occ;
    for (rlci = first_rlci->next; rlci; rlci = next_rlci) {
      next_rlci = rlci->next;
      prev_rlci = rlci->prev;
      if (rlci->occlusion != fixed_occ) {
        if (lineart_check_point_overlapping(prev_rlci, rlci->pos[0], rlci->pos[1], 1e-10)) {
          fixed_occ = rlci->occlusion;
          rlc->level = fixed_occ;
          continue;
        }
        new_rlc = lineart_create_render_line_chain(rb);
        new_rlc->chain.first = rlci;
        new_rlc->chain.last = rlc->chain.last;
        rlc->chain.last = rlci->prev;
        ((eLineArtRenderLineChainItem *)rlc->chain.last)->next = 0;
        rlci->prev = 0;

        /*  end the previous one */
        lineart_append_render_line_chain_point(rb,
                                               rlc,
                                               rlci->pos[0],
                                               rlci->pos[1],
                                               rlci->gpos[0],
                                               rlci->gpos[1],
                                               rlci->gpos[2],
                                               rlci->normal,
                                               rlci->line_type,
                                               fixed_occ);
        new_rlc->object_ref = rlc->object_ref;
        new_rlc->type = rlc->type;
        rlc = new_rlc;
        fixed_occ = rlci->occlusion;
        rlc->level = fixed_occ;
      }
    }
  }
  for (rlc = rb->chains.first; rlc; rlc = rlc->next) {
    lineart_link_chain_with_bounding_areas(rb, rlc);
  }
}

/*  note: segment type (crease/material/contour...) is ambiguous after this. */
static void lineart_connect_two_chains(eLineArtRenderBuffer *UNUSED(rb),
                                       eLineArtRenderLineChain *onto,
                                       eLineArtRenderLineChain *sub,
                                       int reverse_1,
                                       int reverse_2)
{
  eLineArtRenderLineChainItem *rlci;
  if (!reverse_1) {  /*  L--R L-R */
    if (reverse_2) { /*  L--R R-L */
      BLI_listbase_reverse(&sub->chain);
    }
    rlci = sub->chain.first;
    if (lineart_check_point_overlapping(onto->chain.last, rlci->pos[0], rlci->pos[1], 1e-10)) {
      BLI_pophead(&sub->chain);
      if (sub->chain.first == NULL) {
        return;
      }
    }
    ((eLineArtRenderLineChainItem *)onto->chain.last)->next = sub->chain.first;
    ((eLineArtRenderLineChainItem *)sub->chain.first)->prev = onto->chain.last;
    onto->chain.last = sub->chain.last;
  }
  else {              /*  L-R L--R */
    if (!reverse_2) { /*  R-L L--R */
      BLI_listbase_reverse(&sub->chain);
    }
    rlci = onto->chain.first;
    if (lineart_check_point_overlapping(sub->chain.last, rlci->pos[0], rlci->pos[1], 1e-10)) {
      BLI_pophead(&onto->chain);
      if (onto->chain.first == NULL) {
        return;
      }
    }
    ((eLineArtRenderLineChainItem *)sub->chain.last)->next = onto->chain.first;
    ((eLineArtRenderLineChainItem *)onto->chain.first)->prev = sub->chain.last;
    onto->chain.first = sub->chain.first;
  }
}

/*  this only does head-tail connection. */
/*  overlapping / tiny isolated segment / loop reduction not implemented here yet. */
void ED_lineart_connect_chains(eLineArtRenderBuffer *rb, const int do_geometry_space)
{
  eLineArtRenderLineChain *rlc;
  eLineArtRenderLineChainItem *rlci;
  eLineArtBoundingArea *ba;
  eLineArtChainRegisterEntry *cre, *next_cre, *closest_cre;
  float dist;
  int occlusion;
  ListBase swap = {0};

  if ((!do_geometry_space && rb->chaining_image_threshold < 0.0001) ||
      (do_geometry_space && rb->chaining_geometry_threshold < 0.0001)) {
    return;
  }

  swap.first = rb->chains.first;
  swap.last = rb->chains.last;

  rb->chains.last = rb->chains.first = NULL;

  while ((rlc = BLI_pophead(&swap)) != NULL) {
    rlc->next = rlc->prev = NULL;
    BLI_addtail(&rb->chains, rlc);
    if (rlc->picked) {
      continue;
    }

    rlc->picked = 1;

    occlusion = ((eLineArtRenderLineChainItem *)rlc->chain.first)->occlusion;

    rlci = rlc->chain.last;
    while (rlci && ((ba = lineart_get_end_point_bounding_area(rb, rlci)) != NULL)) {
      dist = do_geometry_space ? rb->chaining_geometry_threshold : rb->chaining_image_threshold;
      closest_cre = NULL;
      if (ba->linked_chains.first == NULL) {
        break;
      }
      for (cre = ba->linked_chains.first; cre; cre = next_cre) {
        next_cre = cre->next;
        if (cre->rlc->object_ref != rlc->object_ref) {
          continue;
        }
        if (cre->rlc->picked) {
          BLI_remlink(&ba->linked_chains, cre);
          continue;
        }
        if (cre->rlc == rlc || (!cre->rlc->chain.first) ||
            ((eLineArtRenderLineChainItem *)cre->rlc->chain.first)->occlusion != occlusion ||
            (cre->rlc->type != rlc->type)) {
          continue;
        }
        float new_len = do_geometry_space ? len_v3v3(cre->rlci->gpos, rlci->gpos) :
                                            len_v2v2(cre->rlci->pos, rlci->pos);
        if (new_len < dist) {
          closest_cre = cre;
          dist = new_len;
        }
      }
      if (closest_cre) {
        closest_cre->picked = 1;
        closest_cre->rlc->picked = 1;
        BLI_remlink(&ba->linked_chains, cre);
        if (closest_cre->is_left) {
          lineart_connect_two_chains(rb, rlc, closest_cre->rlc, 0, 0);
        }
        else {
          lineart_connect_two_chains(rb, rlc, closest_cre->rlc, 0, 1);
        }
        BLI_remlink(&swap, closest_cre->rlc);
      }
      else {
        break;
      }
      rlci = rlc->chain.last;
    }

    rlci = rlc->chain.first;
    while (rlci && ((ba = lineart_get_end_point_bounding_area(rb, rlci)) != NULL)) {
      dist = do_geometry_space ? rb->chaining_geometry_threshold : rb->chaining_image_threshold;
      closest_cre = NULL;
      if (ba->linked_chains.first == NULL) {
        break;
      }
      for (cre = ba->linked_chains.first; cre; cre = next_cre) {
        next_cre = cre->next;
        if (cre->rlc->object_ref != rlc->object_ref) {
          continue;
        }
        if (cre->rlc->picked) {
          BLI_remlink(&ba->linked_chains, cre);
          continue;
        }
        if (cre->rlc == rlc || (!cre->rlc->chain.first) ||
            ((eLineArtRenderLineChainItem *)cre->rlc->chain.first)->occlusion != occlusion ||
            (cre->rlc->type != rlc->type)) {
          continue;
        }
        float new_len = do_geometry_space ? len_v3v3(cre->rlci->gpos, rlci->gpos) :
                                            len_v2v2(cre->rlci->pos, rlci->pos);
        if (new_len < dist) {
          closest_cre = cre;
          dist = new_len;
        }
      }
      if (closest_cre) {
        closest_cre->picked = 1;
        closest_cre->rlc->picked = 1;
        BLI_remlink(&ba->linked_chains, cre);
        if (closest_cre->is_left) {
          lineart_connect_two_chains(rb, rlc, closest_cre->rlc, 1, 0);
        }
        else {
          lineart_connect_two_chains(rb, rlc, closest_cre->rlc, 1, 1);
        }
        BLI_remlink(&swap, closest_cre->rlc);
      }
      else {
        break;
      }
      rlci = rlc->chain.first;
    }
  }
}

/* length is in image space */
float ED_lineart_compute_chain_length(eLineArtRenderLineChain *rlc)
{
  eLineArtRenderLineChainItem *rlci;
  float offset_accum = 0;
  float dist;
  float last_point[2];

  rlci = rlc->chain.first;
  copy_v2_v2(last_point, rlci->pos);
  for (rlci = rlc->chain.first; rlci; rlci = rlci->next) {
    dist = len_v2v2(rlci->pos, last_point);
    offset_accum += dist;
    copy_v2_v2(last_point, rlci->pos);
  }
  return offset_accum;
}

void ED_lineart_discard_short_chains(eLineArtRenderBuffer *rb, const float threshold)
{
  eLineArtRenderLineChain *rlc, *next_rlc;
  for (rlc = rb->chains.first; rlc; rlc = next_rlc) {
    next_rlc = rlc->next;
    if (ED_lineart_compute_chain_length(rlc) < threshold) {
      BLI_remlink(&rb->chains, rlc);
    }
  }
}

int ED_lineart_count_chain(const eLineArtRenderLineChain *rlc)
{
  eLineArtRenderLineChainItem *rlci;
  int count = 0;
  for (rlci = rlc->chain.first; rlci; rlci = rlci->next) {
    count++;
  }
  return count;
}

void ED_lineart_chain_clear_picked_flag(eLineArtRenderBuffer *rb)
{
  eLineArtRenderLineChain *rlc;
  if (rb == NULL) {
    return;
  }
  for (rlc = rb->chains.first; rlc; rlc = rlc->next) {
    rlc->picked = 0;
  }
}
