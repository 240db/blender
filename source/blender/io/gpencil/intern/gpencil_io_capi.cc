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
 */

/** \file
 * \ingroup bgpencil
 */

#include <stdio.h>

#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_path_util.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "DNA_gpencil_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BKE_context.h"
#include "BKE_gpencil.h"
#include "BKE_main.h"
#include "BKE_scene.h"

#include "ED_markers.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "../gpencil_io_exporter.h"
#include "gpencil_io_export_svg.h"

using blender::io::gpencil::GpencilExporterSVG;

static bool is_keyframe_empty(bContext *C, bGPdata *gpd, int framenum, bool use_markers)
{
  if (!use_markers) {
    /* Check if exist a frame. */
    LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
      if (gpl->flag & GP_LAYER_HIDE) {
        continue;
      }
      LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
        if (gpf->framenum == framenum) {
          return false;
        }
      }
    }
  }
  else {
    ListBase *markers = ED_context_get_markers(C);
    /* Check if exist a marker. */
    LISTBASE_FOREACH (TimeMarker *, marker, markers) {
      if (marker->frame == framenum) {
        return false;
      }
    }
  }

  return true;
}

/* Export current frame. */
static bool gpencil_io_export_frame(GpencilExporterSVG *writter,
                                    const GpencilExportParams *iparams,
                                    float frame_offset[2],
                                    const bool newpage,
                                    const bool body,
                                    const bool savepage)
{

  bool result = false;
  switch (iparams->mode) {
    case GP_EXPORT_TO_SVG: {
      writter->set_frame_number(iparams->framenum);
      writter->set_frame_offset(frame_offset);
      std::string subfix = iparams->file_subfix;
      result = writter->write(subfix, newpage, body, savepage);
      break;
    }
    default:
      break;
  }

  return result;
}

/* Export full animation in Storyboard mode. */
static bool gpencil_export_storyboard(
    Depsgraph *depsgraph, Main *bmain, Scene *scene, GpencilExportParams *iparams, Object *ob)
{
  Object *ob_eval_ = (Object *)DEG_get_evaluated_id(depsgraph, &ob->id);
  bGPdata *gpd_eval = (bGPdata *)ob_eval_->data;
  bool done = false;

  GpencilExporterSVG *writter = new GpencilExporterSVG(iparams);

  /* Storyboard only works in camera view. */
  RegionView3D *rv3d = (RegionView3D *)iparams->region->regiondata;
  if ((rv3d == NULL) || (rv3d->persp != RV3D_CAMOB)) {
    printf("Storyboard only allowed in camera view.\n");
    delete writter;
    return false;
  }

  /* Calc paper sizes. */
  const int blocks[2] = {iparams->page_layout[0], iparams->page_layout[1]};
  float frame_box[2];
  float render_ratio[2];

  frame_box[0] = iparams->paper_size[0] / ((float)blocks[0] + 1.0f);
  frame_box[1] = ((float)scene->r.ysch / (float)scene->r.xsch) * frame_box[0];

  render_ratio[0] = frame_box[0] / ((scene->r.xsch * scene->r.size) / 100);
  render_ratio[1] = render_ratio[0];

  float ysize = iparams->paper_size[1] - (frame_box[1] * (float)blocks[1]);
  ysize /= (float)blocks[1] + 1.0f;

  const float gap[2] = {frame_box[0] / ((float)blocks[0] + 1.0f), ysize};
  float frame_offset[2] = {gap[0], gap[1]};

  int col = 1;
  int row = 1;
  int page = 1;
  bool header = true;
  bool pending_save = false;
  int shot = 0;

  const bool use_markers = ((iparams->flag & GP_EXPORT_MARKERS) != 0);

  for (int i = iparams->frame_start; i < iparams->frame_end + 1; i++) {
    if (is_keyframe_empty(iparams->C, gpd_eval, i, use_markers)) {
      continue;
    }
    shot++;
    writter->set_shot(shot);

    if (header) {
      writter->set_frame_box(frame_box);
      writter->set_frame_ratio(render_ratio);

      pending_save |= gpencil_io_export_frame(writter, iparams, frame_offset, true, false, false);
      header = false;
    }

    CFRA = i;
    BKE_scene_graph_update_for_newframe(depsgraph, bmain);
    sprintf(iparams->file_subfix, "%04d", page);
    iparams->framenum = i;

    pending_save |= gpencil_io_export_frame(writter, iparams, frame_offset, false, true, false);
    col++;

    if (col > blocks[0]) {
      col = 1;
      frame_offset[0] = gap[0];

      row++;
      frame_offset[1] += frame_box[1];
      frame_offset[1] += gap[1];
    }
    else {
      frame_offset[0] += frame_box[0];
      frame_offset[0] += gap[0];
    }

    if (row > blocks[1]) {
      done |= gpencil_io_export_frame(writter, iparams, frame_offset, false, false, true);
      page++;
      header = true;
      pending_save = false;
      row = col = 1;
      copy_v2_v2(frame_offset, gap);

      /* Create a new class object per page. */
      delete writter;
      writter = new GpencilExporterSVG(iparams);
    }
  }

  if (pending_save) {
    done |= gpencil_io_export_frame(writter, iparams, frame_offset, false, false, true);
  }

  delete writter;

  return done;
}

/* Main export entry point function. */
bool gpencil_io_export(const char *filename, GpencilExportParams *iparams)
{
  Main *bmain = CTX_data_main(iparams->C);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(iparams->C);
  Scene *scene = CTX_data_scene(iparams->C);
  Object *ob = CTX_data_active_object(iparams->C);

  const bool is_storyboard = ((iparams->flag & GP_EXPORT_STORYBOARD_MODE) != 0);

  bool done = false;

  /* Prepare document. */
  copy_v2_v2(iparams->paper_size, iparams->paper_size);

  if (!is_storyboard) {
    GpencilExporterSVG writter = GpencilExporterSVG(iparams);
    /* Prepare output filename with full path. */
    writter.set_out_filename(filename);

    float no_offset[2] = {0.0f, 0.0f};
    float ratio[2] = {1.0f, 1.0f};
    writter.set_frame_ratio(ratio);
    iparams->file_subfix[0] = '\0';
    done |= gpencil_io_export_frame(&writter, iparams, no_offset, true, true, true);
  }
  else {
    int oldframe = (int)DEG_get_ctime(depsgraph);

    done |= gpencil_export_storyboard(depsgraph, bmain, scene, iparams, ob);

    /* Return frame state and DB to original state. */
    CFRA = oldframe;
    BKE_scene_graph_update_for_newframe(depsgraph, bmain);
  }

  return done;
}
