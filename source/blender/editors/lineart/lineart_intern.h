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

#ifndef __LRT_INTERN_H__
#define __LRT_INTERN_H__

#include "BLI_linklist.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_threads.h"

#include "DNA_lineart_types.h"

#include <math.h>
#include <string.h>

struct LineartStaticMemPoolNode;
struct LineartRenderLine;
struct LineartRenderBuffer;

void *lineart_list_append_pointer_pool(ListBase *h, struct LineartStaticMemPool *smp, void *p);
void *lineart_list_append_pointer_pool_sized(ListBase *h,
                                             struct LineartStaticMemPool *smp,
                                             void *p,
                                             int size);
void *list_push_pointer_static(ListBase *h, struct LineartStaticMemPool *smp, void *p);
void *list_push_pointer_static_sized(ListBase *h,
                                     struct LineartStaticMemPool *smp,
                                     void *p,
                                     int size);

void *lineart_list_pop_pointer_no_free(ListBase *h);
void lineart_list_remove_pointer_item_no_free(ListBase *h, LinkData *lip);

LineartStaticMemPoolNode *lineart_mem_new_static_pool(struct LineartStaticMemPool *smp,
                                                      size_t size);
void *lineart_mem_aquire(struct LineartStaticMemPool *smp, size_t size);
void *lineart_mem_aquire_thread(struct LineartStaticMemPool *smp, size_t size);
void lineart_mem_destroy(LineartStaticMemPool *smp);

void lineart_prepend_line_direct(LineartRenderLine **first, void *node);
void lineart_prepend_pool(LinkNode **first, LineartStaticMemPool *smp, void *link);

void lineart_matrix_ortho_44d(double (*mProjection)[4],
                              double xMin,
                              double xMax,
                              double yMin,
                              double yMax,
                              double zMin,
                              double zMax);
void lineart_matrix_perspective_44d(
    double (*mProjection)[4], double fFov_rad, double fAspect, double zMin, double zMax);

int lineart_count_intersection_segment_count(struct LineartRenderBuffer *rb);

void lineart_count_and_print_render_buffer_memory(LineartRenderBuffer *rb);

#define LRT_ITER_ALL_LINES_BEGIN \
  LineartRenderLine *rl, *next_rl, **current_list; \
  rl = rb->contours; \
  for (current_list = &rb->contours; rl; rl = next_rl) { \
    next_rl = rl->next;

#define LRT_ITER_ALL_LINES_END \
  while (!next_rl) { \
    if (current_list == &rb->contours) { \
      current_list = &rb->crease_lines; \
    } \
    else if (current_list == &rb->crease_lines) { \
      current_list = &rb->material_lines; \
    } \
    else if (current_list == &rb->material_lines) { \
      current_list = &rb->edge_marks; \
    } \
    else if (current_list == &rb->edge_marks) { \
      current_list = &rb->intersection_lines; \
    } \
    else { \
      break; \
    } \
    next_rl = *current_list; \
  } \
  }

#endif
