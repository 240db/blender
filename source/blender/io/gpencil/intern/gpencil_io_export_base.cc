

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
 * The Original Code is Copyright (C) 2020 Blender Foundation
 * All rights reserved.
 */

/** \file
 * \ingroup bgpencil
 */
#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>

#include "BKE_context.h"
#include "BKE_gpencil.h"
#include "BKE_gpencil_geom.h"
#include "BKE_layer.h"
#include "BKE_main.h"
#include "BKE_material.h"

#include "BLI_blenlib.h"
#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "DNA_gpencil_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "UI_view2d.h"

#include "ED_view3d.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "gpencil_io_export_base.h"
#include "gpencil_io_exporter.h"

#include "pugixml.hpp"

namespace blender::io::gpencil {

/* Constructor. */
GpencilExporter::GpencilExporter(const struct GpencilExportParams *iparams)
{
  params_.frame_start = iparams->frame_start;
  params_.frame_end = iparams->frame_end;
  params_.obact = iparams->obact;
  params_.region = iparams->region;
  params_.v3d = iparams->v3d;
  params_.C = iparams->C;
  params_.mode = iparams->mode;
  params_.flag = iparams->flag;
  params_.select = iparams->select;
  params_.stroke_sample = iparams->stroke_sample;
  params_.framenum = iparams->framenum;
  frame_ratio_[0] = frame_ratio_[1] = 1.0f;
  zero_v2(frame_offset_);

  copy_v2_v2_int((int *)params_.page_layout, (int *)iparams->page_layout);
  params_.page_type = iparams->page_type;
  copy_v2_v2(params_.paper_size, iparams->paper_size);
  params_.text_type = iparams->text_type;

  /* Easy access data. */
  bmain = CTX_data_main(params_.C);
  depsgraph = CTX_data_depsgraph_pointer(params_.C);
  scene = CTX_data_scene(params_.C);
  rv3d = (RegionView3D *)params_.region->regiondata;
  gpd = (bGPdata *)params_.obact->data;
  const bool is_storyboard = ((params_.flag & GP_EXPORT_STORYBOARD_MODE) != 0);

  /* Load list of selected objects. */
  create_object_list();

  winx_ = params_.region->winx;
  winy_ = params_.region->winy;

  invert_axis_[0] = false;
  invert_axis_[1] = true;

  /* Camera rectangle. */
  if (rv3d->persp == RV3D_CAMOB) {
    render_x_ = (scene->r.xsch * scene->r.size) / 100;
    render_y_ = (scene->r.ysch * scene->r.size) / 100;

    ED_view3d_calc_camera_border(CTX_data_scene(params_.C),
                                 depsgraph,
                                 params_.region,
                                 params_.v3d,
                                 rv3d,
                                 &camera_rect_,
                                 true);
    _is_camera = true;
    camera_ratio_ = render_x_ / (camera_rect_.xmax - camera_rect_.xmin);
    offset_[0] = camera_rect_.xmin;
    offset_[1] = camera_rect_.ymin;
  }
  else {
    _is_camera = false;
    if (!is_storyboard && (ob_list_.size() == 1)) {
      /* Calc selected object boundbox. Need set initial value to some variables. */
      camera_ratio_ = 1.0f;
      offset_[0] = 0.0f;
      offset_[1] = 0.0f;

      selected_objects_boundbox_set();
      rctf boundbox;
      selected_objects_boundbox_get(&boundbox);

      render_x_ = boundbox.xmax - boundbox.xmin;
      render_y_ = boundbox.ymax - boundbox.ymin;
      offset_[0] = boundbox.xmin;
      offset_[1] = boundbox.ymin;
    }
  }
}

/** Create a list of selected objects sorted from back to front */
void GpencilExporter::create_object_list(void)
{
  ViewLayer *view_layer = CTX_data_view_layer(params_.C);

  float camera_z_axis[3];
  copy_v3_v3(camera_z_axis, rv3d->viewinv[2]);
  ob_list_.clear();

  LISTBASE_FOREACH (Base *, base, &view_layer->object_bases) {
    Object *object = base->object;

    if (object->type != OB_GPENCIL) {
      continue;
    }
    if ((params_.select == GP_EXPORT_ACTIVE) && (params_.obact != object)) {
      continue;
    }

    if ((params_.select == GP_EXPORT_SELECTED) && ((base->flag & BASE_SELECTED) == 0)) {
      continue;
    }

    /* Save z-depth from view to sort from back to front. */
    if (_is_camera) {
      float camera_z = dot_v3v3(camera_z_axis, object->obmat[3]);
      ObjectZ obz = {camera_z, object};
      ob_list_.push_back(obz);
    }
    else {
      float zdepth = 0;
      if (rv3d) {
        if (rv3d->is_persp) {
          zdepth = ED_view3d_calc_zfac(rv3d, object->obmat[3], NULL);
        }
        else {
          zdepth = -dot_v3v3(rv3d->viewinv[2], object->obmat[3]);
        }
        ObjectZ obz = {zdepth * -1.0f, object};
        ob_list_.push_back(obz);
      }
    }

    /* Sort list of objects from point of view. */
    ob_list_.sort(
        [](const ObjectZ &obz1, const ObjectZ &obz2) { return obz1.zdepth < obz2.zdepth; });
  }
}
/**
 * Set output file input_text full path.
 * \param C: Context.
 * \param filename: Path of the file provided by save dialog.
 */
void GpencilExporter::set_out_filename(const char *filename)
{
  BLI_strncpy(out_filename_, filename, FILE_MAX);
  BLI_path_abs(out_filename_, BKE_main_blendfile_path(bmain));
}

/**
 * Convert to screenspace
 * \param co: 3D position
 * \param r_co: 2D position
 * \return False if error
 */
bool GpencilExporter::gpencil_3d_point_to_screen_space(const float co[3], float r_co[2])
{
  float parent_co[3];
  mul_v3_m4v3(parent_co, diff_mat_, co);
  float screen_co[2];
  eV3DProjTest test = (eV3DProjTest)(V3D_PROJ_RET_OK);
  if (ED_view3d_project_float_global(params_.region, parent_co, screen_co, test) ==
      V3D_PROJ_RET_OK) {
    if (!ELEM(V2D_IS_CLIPPED, screen_co[0], screen_co[1])) {
      copy_v2_v2(r_co, screen_co);
      /* Invert X axis. */
      if (invert_axis_[0]) {
        r_co[0] = winx_ - r_co[0];
      }
      /* Invert Y axis. */
      if (invert_axis_[1]) {
        r_co[1] = winy_ - r_co[1];
      }
      /* Apply offset and scale. */
      sub_v2_v2(r_co, offset_);
      mul_v2_fl(r_co, camera_ratio_);

      /* Apply frame offset and scale. */
      mul_v2_v2(r_co, frame_ratio_);
      add_v2_v2(r_co, frame_offset_);

      return true;
    }
  }
  r_co[0] = V2D_IS_CLIPPED;
  r_co[1] = V2D_IS_CLIPPED;

  /* Invert X axis. */
  if (invert_axis_[0]) {
    r_co[0] = winx_ - r_co[0];
  }
  /* Invert Y axis. */
  if (invert_axis_[1]) {
    r_co[1] = winy_ - r_co[1];
  }

  return false;
}

/**
 * Get average pressure
 * \param gps: Pointer to stroke
 * \retun value
 */
float GpencilExporter::stroke_average_pressure_get(struct bGPDstroke *gps)
{
  bGPDspoint *pt = NULL;

  if (gps->totpoints == 1) {
    pt = &gps->points[0];
    return pt->pressure;
  }

  float tot = 0.0f;
  for (uint32_t i = 0; i < gps->totpoints; i++) {
    pt = &gps->points[i];
    tot += pt->pressure;
  }

  return tot / (float)gps->totpoints;
}

/**
 * Check if the thickness of the stroke is constant
 * \param gps: Pointer to stroke
 * \retun true if all points thickness are equal.
 */
bool GpencilExporter::is_stroke_thickness_constant(struct bGPDstroke *gps)
{
  if (gps->totpoints == 1) {
    return true;
  }

  bGPDspoint *pt = &gps->points[0];
  float prv_pressure = pt->pressure;

  for (uint32_t i = 0; i < gps->totpoints; i++) {
    pt = &gps->points[i];
    if (pt->pressure != prv_pressure) {
      return false;
    }
  }

  return true;
}

/**
 * Get radius of point
 * \param gps: Stroke
 * \return Radius in pixels
 */
float GpencilExporter::stroke_point_radius_get(struct bGPDstroke *gps)
{
  const bGPDlayer *gpl = gpl_current_get();
  bGPDspoint *pt = NULL;
  float v1[2], screen_co[2], screen_ex[2];

  pt = &gps->points[0];
  gpencil_3d_point_to_screen_space(&pt->x, screen_co);

  /* Radius. */
  bGPDstroke *gps_perimeter = BKE_gpencil_stroke_perimeter_from_view(
      rv3d, gpd, gpl, gps, 3, diff_mat_);

  pt = &gps_perimeter->points[0];
  gpencil_3d_point_to_screen_space(&pt->x, screen_ex);

  sub_v2_v2v2(v1, screen_co, screen_ex);
  float radius = len_v2(v1);
  BKE_gpencil_free_stroke(gps_perimeter);

  return radius;
}

/**
 * Convert a color to Hex value (#FFFFFF)
 * \param color: Original RGB color
 * \return String with the conversion
 */
std::string GpencilExporter::rgb_to_hexstr(float color[3])
{
  uint8_t r = color[0] * 255.0f;
  uint8_t g = color[1] * 255.0f;
  uint8_t b = color[2] * 255.0f;
  char hex_string[20];
  sprintf(hex_string, "#%02X%02X%02X", r, g, b);

  std::string hexstr = hex_string;

  return hexstr;
}

/**
 * Convert a color to gray scale.
 * \param color: Color to convert
 */
void GpencilExporter::rgb_to_grayscale(float color[3])
{
  float grayscale = ((0.3f * color[0]) + (0.59f * color[1]) + (0.11f * color[2]));
  color[0] = grayscale;
  color[1] = grayscale;
  color[2] = grayscale;
}

/**
 * Convert a full string to lowercase
 * \param input_text: Input input_text
 * \return Lower case string
 */
std::string GpencilExporter::to_lower_string(char *input_text)
{
  ::std::string text = input_text;
  /* First remove any point of the string. */
  size_t found = text.find_first_of(".");
  while (found != std::string::npos) {
    text[found] = '_';
    found = text.find_first_of(".", found + 1);
  }

  std::transform(
      text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });

  return text;
}

struct bGPDlayer *GpencilExporter::gpl_current_get(void)
{
  return _gpl_cur;
}

void GpencilExporter::gpl_current_set(struct bGPDlayer *gpl)
{
  _gpl_cur = gpl;
  BKE_gpencil_parent_matrix_get(depsgraph, params_.obact, gpl, diff_mat_);
}

struct bGPDframe *GpencilExporter::gpf_current_get(void)
{
  return _gpf_cur;
}

void GpencilExporter::gpf_current_set(struct bGPDframe *gpf)
{
  _gpf_cur = gpf;
}
struct bGPDstroke *GpencilExporter::gps_current_get(void)
{
  return _gps_cur;
}

void GpencilExporter::gps_current_set(struct Object *ob,
                                      struct bGPDstroke *gps,
                                      const bool set_colors)
{
  _gps_cur = gps;
  if (set_colors) {
    _gp_style = BKE_gpencil_material_settings(ob, gps->mat_nr + 1);

    _is_stroke = ((_gp_style->flag & GP_MATERIAL_STROKE_SHOW) &&
                  (_gp_style->stroke_rgba[3] > GPENCIL_ALPHA_OPACITY_THRESH));
    _is_fill = ((_gp_style->flag & GP_MATERIAL_FILL_SHOW) &&
                (_gp_style->fill_rgba[3] > GPENCIL_ALPHA_OPACITY_THRESH));

    /* Stroke color. */
    copy_v4_v4(stroke_color_, _gp_style->stroke_rgba);
    _avg_opacity = 0;
    /* Get average vertex color and apply. */
    float avg_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (uint32_t i = 0; i < gps->totpoints; i++) {
      bGPDspoint *pt = &gps->points[i];
      add_v4_v4(avg_color, pt->vert_color);
      _avg_opacity += pt->strength;
    }

    mul_v4_v4fl(avg_color, avg_color, 1.0f / (float)gps->totpoints);
    interp_v3_v3v3(stroke_color_, stroke_color_, avg_color, avg_color[3]);
    _avg_opacity /= (float)gps->totpoints;

    /* Fill color. */
    copy_v4_v4(fill_color_, _gp_style->fill_rgba);
    /* Apply vertex color for fill. */
    interp_v3_v3v3(fill_color_, fill_color_, gps->vert_color_fill, gps->vert_color_fill[3]);
  }
}

struct MaterialGPencilStyle *GpencilExporter::gp_style_current_get(void)
{
  return _gp_style;
}

bool GpencilExporter::material_is_stroke(void)
{
  return _is_stroke;
}

bool GpencilExporter::material_is_fill(void)
{
  return _is_fill;
}

float GpencilExporter::stroke_average_opacity_get(void)
{
  return _avg_opacity;
}

bool GpencilExporter::is_camera_mode(void)
{
  return _is_camera;
}

/* Calc selected strokes boundbox. */
void GpencilExporter::selected_objects_boundbox_set(void)
{
  const float gap = 10.0f;
  const bGPDspoint *pt;
  uint32_t i;

  float screen_co[2];
  float r_min[2], r_max[2];
  INIT_MINMAX2(r_min, r_max);

  for (ObjectZ &obz : ob_list_) {
    Object *ob = obz.ob;
    /* Use evaluated version to get strokes with modifiers. */
    Object *ob_eval = (Object *)DEG_get_evaluated_id(depsgraph, &ob->id);
    bGPdata *gpd_eval = (bGPdata *)ob_eval->data;

    LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd_eval->layers) {
      if (gpl->flag & GP_LAYER_HIDE) {
        continue;
      }
      BKE_gpencil_parent_matrix_get(depsgraph, ob_eval, gpl, diff_mat_);

      bGPDframe *gpf = gpl->actframe;
      if (gpf == NULL) {
        continue;
      }

      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        if (gps->totpoints == 0) {
          continue;
        }
        for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
          /* Convert to 2D. */
          gpencil_3d_point_to_screen_space(&pt->x, screen_co);
          minmax_v2v2_v2(r_min, r_max, screen_co);
        }
      }
    }
  }
  /* Add small gap. */
  add_v2_fl(r_min, gap * -1.0f);
  add_v2_fl(r_max, gap);

  _select_boundbox.xmin = r_min[0];
  _select_boundbox.ymin = r_min[1];
  _select_boundbox.xmax = r_max[0];
  _select_boundbox.ymax = r_max[1];
}

void GpencilExporter::selected_objects_boundbox_get(rctf *boundbox)
{
  boundbox->xmin = _select_boundbox.xmin;
  boundbox->xmax = _select_boundbox.xmax;
  boundbox->ymin = _select_boundbox.ymin;
  boundbox->ymax = _select_boundbox.ymax;
}

void GpencilExporter::set_frame_number(int value)
{
  cfra_ = value;
}

void GpencilExporter::set_frame_offset(float value[2])
{
  copy_v2_v2(frame_offset_, value);
}

void GpencilExporter::set_frame_ratio(float value[2])
{
  copy_v2_v2(frame_ratio_, value);
}

void GpencilExporter::set_frame_box(float value[2])
{
  copy_v2_v2(frame_box_, value);
}

void GpencilExporter::set_shot(int value)
{
  shot_ = value;
}
}  // namespace blender::io::gpencil
