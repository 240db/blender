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
 * The Original Code is Copyright (C) 2020 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup editor/io
 */

#include <errno.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "DNA_gpencil_types.h"
#include "DNA_space_types.h"

#include "BKE_context.h"
#include "BKE_gpencil.h"
#include "BKE_main.h"
#include "BKE_report.h"
#include "BKE_scene.h"
#include "BKE_screen.h"

#include "BLI_listbase.h"
#include "BLI_path_util.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "WM_api.h"
#include "WM_types.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "io_gpencil.h"

#include "gpencil_io_exporter.h"

static bool wm_gpencil_export_svg_common_check(bContext *UNUSED(C), wmOperator *op)
{

  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);

  if (!BLI_path_extension_check(filepath, ".svg")) {
    BLI_path_extension_ensure(filepath, FILE_MAX, ".svg");
    RNA_string_set(op->ptr, "filepath", filepath);
    return true;
  }

  return false;
}

static bool wm_gpencil_export_pdf_common_check(bContext *UNUSED(C), wmOperator *op)
{

  char filepath[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filepath);

  if (!BLI_path_extension_check(filepath, ".pdf")) {
    BLI_path_extension_ensure(filepath, FILE_MAX, ".pdf");
    RNA_string_set(op->ptr, "filepath", filepath);
    return true;
  }

  return false;
}

static void ui_gpencil_export_common_settings(uiLayout *layout,
                                              PointerRNA *imfptr,
                                              const bool story)
{
  uiLayout *box, *row, *col, *sub;

  box = uiLayoutBox(layout);
  row = uiLayoutRow(box, false);
  uiItemL(row, IFACE_("Export Options"), ICON_SCENE_DATA);

  col = uiLayoutColumn(box, false);

  sub = uiLayoutColumn(col, true);
  uiItemR(sub, imfptr, "stroke_sample", 0, NULL, ICON_NONE);
  uiItemR(sub, imfptr, "use_fill", 0, NULL, ICON_NONE);
  uiItemR(sub, imfptr, "use_normalized_thickness", 0, NULL, ICON_NONE);

  if (!story) {
    uiItemR(sub, imfptr, "use_clip_camera", 0, NULL, ICON_NONE);
  }
  else {
    uiItemR(sub, imfptr, "use_gray_scale", 0, NULL, ICON_NONE);
  }
}

static void gpencil_export_common_props_svg(wmOperatorType *ot)
{
  static const EnumPropertyItem gpencil_export_select_items[] = {
      {GP_EXPORT_ACTIVE, "ACTIVE", 0, "Active", "Include only active object"},
      {GP_EXPORT_SELECTED, "SELECTED", 0, "Selected", "Include selected objects"},
      {GP_EXPORT_VISIBLE, "VISIBLE", 0, "Visible", "Include visible objects"},
      {0, NULL, 0, NULL, NULL},
  };
  RNA_def_boolean(ot->srna, "use_fill", true, "Fill", "Export filled areas");
  RNA_def_boolean(ot->srna,
                  "use_normalized_thickness",
                  false,
                  "Normalize",
                  "Export strokes with constant thickness along the stroke");
  ot->prop = RNA_def_enum(ot->srna,
                          "selected_object_type",
                          gpencil_export_select_items,
                          GP_EXPORT_SELECTED,
                          "Object",
                          "Objects included in the export");

  RNA_def_boolean(ot->srna,
                  "use_clip_camera",
                  false,
                  "Clip Camera",
                  "Clip drawings to camera size when export in camera view");
  RNA_def_boolean(ot->srna,
                  "use_gray_scale",
                  false,
                  "Gray Scale",
                  "Export in gray scale instead of full color");
  RNA_def_float(
      ot->srna,
      "stroke_sample",
      0.0f,
      0.0f,
      100.0f,
      "Sampling",
      "Precision of sampling stroke, low values gets more precise result, zero to disable",
      0.0f,
      100.0f);
}

/* <-------- SVG single frame export. --------> */
static int wm_gpencil_export_svg_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  UNUSED_VARS(event);

  if (!RNA_struct_property_is_set(op->ptr, "filepath")) {
    Main *bmain = CTX_data_main(C);
    char filepath[FILE_MAX];

    if (BKE_main_blendfile_path(bmain)[0] == '\0') {
      BLI_strncpy(filepath, "untitled", sizeof(filepath));
    }
    else {
      BLI_strncpy(filepath, BKE_main_blendfile_path(bmain), sizeof(filepath));
    }

    BLI_path_extension_replace(filepath, sizeof(filepath), ".svg");
    RNA_string_set(op->ptr, "filepath", filepath);
  }

  WM_event_add_fileselect(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static int wm_gpencil_export_svg_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  Object *ob = CTX_data_active_object(C);

  if (!RNA_struct_property_is_set(op->ptr, "filepath")) {
    BKE_report(op->reports, RPT_ERROR, "No filename given");
    return OPERATOR_CANCELLED;
  }

  /*For some reason the region cannot be retrieved from the context.
   * If a better solution is found in the future, remove this function. */
  ARegion *region = get_invoke_region(C);
  if (region == NULL) {
    BKE_report(op->reports, RPT_ERROR, "Unable to find valid 3D View area");
    return OPERATOR_CANCELLED;
  }
  View3D *v3d = get_invoke_view3d(C);

  char filename[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filename);

  const bool use_fill = RNA_boolean_get(op->ptr, "use_fill");
  const bool use_norm_thickness = RNA_boolean_get(op->ptr, "use_normalized_thickness");
  const short select = RNA_enum_get(op->ptr, "selected_object_type");

  const bool use_clip_camera = RNA_boolean_get(op->ptr, "use_clip_camera");
  const bool use_gray_scale = RNA_boolean_get(op->ptr, "use_gray_scale");

  /* Set flags. */
  int flag = 0;
  SET_FLAG_FROM_TEST(flag, use_fill, GP_EXPORT_FILL);
  SET_FLAG_FROM_TEST(flag, use_norm_thickness, GP_EXPORT_NORM_THICKNESS);
  SET_FLAG_FROM_TEST(flag, use_clip_camera, GP_EXPORT_CLIP_CAMERA);
  SET_FLAG_FROM_TEST(flag, use_gray_scale, GP_EXPORT_GRAY_SCALE);

  struct GpencilExportParams params = {
      .C = C,
      .region = region,
      .v3d = v3d,
      .obact = ob,
      .mode = GP_EXPORT_TO_SVG,
      .frame_start = CFRA,
      .frame_end = CFRA,
      .flag = flag,
      .select = select,
      .frame_type = GP_EXPORT_FRAME_ACTIVE,
      .stroke_sample = RNA_float_get(op->ptr, "stroke_sample"),
      .page_layout = {0.0f, 0.0f},
      .page_type = 0,
      .paper_size = {0.0f, 0.0f},
      .text_type = 0,

  };

  /* Do export. */
  WM_cursor_wait(1);
  bool done = gpencil_io_export(filename, &params);
  WM_cursor_wait(0);

  if (done) {
    BKE_report(op->reports, RPT_INFO, "SVG export file created");
  }
  else {
    BKE_report(op->reports, RPT_WARNING, "Unable to export SVG");
  }

  return OPERATOR_FINISHED;
}

static void ui_gpencil_export_svg_settings(uiLayout *layout, PointerRNA *imfptr)
{
  uiLayout *box, *row;

  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);

  box = uiLayoutBox(layout);

  row = uiLayoutRow(box, false);
  uiItemL(row, IFACE_("Scene Options"), ICON_SCENE_DATA);

  row = uiLayoutRow(box, false);
  uiItemR(row, imfptr, "selected_object_type", 0, NULL, ICON_NONE);

  ui_gpencil_export_common_settings(layout, imfptr, false);
}

static void wm_gpencil_export_svg_draw(bContext *UNUSED(C), wmOperator *op)
{

  PointerRNA ptr;

  RNA_pointer_create(NULL, op->type->srna, op->properties, &ptr);

  ui_gpencil_export_svg_settings(op->layout, &ptr);
}

static bool wm_gpencil_export_svg_poll(bContext *C)
{
  if (CTX_wm_window(C) == NULL) {
    return false;
  }
  Object *ob = CTX_data_active_object(C);
  if ((ob == NULL) || (ob->type != OB_GPENCIL)) {
    return false;
  }

  bGPdata *gpd = (bGPdata *)ob->data;
  bGPDlayer *gpl = BKE_gpencil_layer_active_get(gpd);

  if (gpl == NULL) {
    return false;
  }

  return true;
}

void WM_OT_gpencil_export_svg(wmOperatorType *ot)
{
  ot->name = "Export to SVG";
  ot->description = "Export grease pencil to SVG";
  ot->idname = "WM_OT_gpencil_export_svg";

  ot->invoke = wm_gpencil_export_svg_invoke;
  ot->exec = wm_gpencil_export_svg_exec;
  ot->poll = wm_gpencil_export_svg_poll;
  ot->ui = wm_gpencil_export_svg_draw;
  ot->check = wm_gpencil_export_svg_common_check;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_OBJECT_IO,
                                 FILE_BLENDER,
                                 FILE_SAVE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_SHOW_PROPS,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_ALPHA);

  gpencil_export_common_props_svg(ot);
}

/* <-------- SVG Storyboard export. --------> */
static int wm_gpencil_export_stb_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  UNUSED_VARS(event);

  RNA_boolean_set(op->ptr, "init_scene_frame_range", true);

  if (!RNA_struct_property_is_set(op->ptr, "filepath")) {
    Main *bmain = CTX_data_main(C);
    char filepath[FILE_MAX];

    if (BKE_main_blendfile_path(bmain)[0] == '\0') {
      BLI_strncpy(filepath, "untitled", sizeof(filepath));
    }
    else {
      BLI_strncpy(filepath, BKE_main_blendfile_path(bmain), sizeof(filepath));
    }

    BLI_path_extension_replace(filepath, sizeof(filepath), ".svg");
    RNA_string_set(op->ptr, "filepath", filepath);
  }

  WM_event_add_fileselect(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static int wm_gpencil_export_stb_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);

  if (!RNA_struct_property_is_set(op->ptr, "filepath")) {
    BKE_report(op->reports, RPT_ERROR, "No filename given");
    return OPERATOR_CANCELLED;
  }

  /*For some reason the region cannot be retrieved from the context.
   * If a better solution is found in the future, remove this function. */
  ARegion *region = get_invoke_region(C);
  if (region == NULL) {
    BKE_report(op->reports, RPT_ERROR, "Unable to find valid 3D View area");
    return OPERATOR_CANCELLED;
  }
  View3D *v3d = get_invoke_view3d(C);

  char filename[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filename);

  const bool use_fill = RNA_boolean_get(op->ptr, "use_fill");
  const bool use_norm_thickness = RNA_boolean_get(op->ptr, "use_normalized_thickness");
  const short select = RNA_enum_get(op->ptr, "selected_object_type");
  const bool use_gray_scale = RNA_boolean_get(op->ptr, "use_gray_scale");
  const bool use_markers = RNA_boolean_get(op->ptr, "use_markers");

  /* Set flags. */
  int flag = GP_EXPORT_STORYBOARD_MODE | GP_EXPORT_CLIP_CAMERA;
  SET_FLAG_FROM_TEST(flag, use_fill, GP_EXPORT_FILL);
  SET_FLAG_FROM_TEST(flag, use_norm_thickness, GP_EXPORT_NORM_THICKNESS);
  SET_FLAG_FROM_TEST(flag, use_gray_scale, GP_EXPORT_GRAY_SCALE);
  SET_FLAG_FROM_TEST(flag, use_markers, GP_EXPORT_MARKERS);

  float paper_size[2];
  if (RNA_enum_get(op->ptr, "page_type") == GP_EXPORT_PAPER_LANDSCAPE) {
    paper_size[0] = gpencil_export_paper_sizes[0];
    paper_size[1] = gpencil_export_paper_sizes[1];
  }
  else {
    paper_size[0] = gpencil_export_paper_sizes[1];
    paper_size[1] = gpencil_export_paper_sizes[0];
  }

  const int page_layout[2] = {RNA_int_get(op->ptr, "size_col"), RNA_int_get(op->ptr, "size_row")};

  struct GpencilExportParams params = {
      .C = C,
      .region = region,
      .v3d = v3d,
      .obact = ob,
      .mode = GP_EXPORT_TO_SVG,
      .frame_start = RNA_int_get(op->ptr, "start"),
      .frame_end = RNA_int_get(op->ptr, "end"),
      .flag = flag,
      .select = select,
      .frame_type = GP_EXPORT_FRAME_ACTIVE,
      .stroke_sample = RNA_float_get(op->ptr, "stroke_sample"),
      .page_layout = {page_layout[0], page_layout[1]},
      .page_type = (int)RNA_enum_get(op->ptr, "page_type"),
      .paper_size = {paper_size[0], paper_size[1]},
      .text_type = (int)RNA_enum_get(op->ptr, "text_type"),

  };
  /* Take some defaults from the scene, if not specified explicitly. */
  Scene *scene = CTX_data_scene(C);
  if (params.frame_start == INT_MIN) {
    params.frame_start = SFRA;
  }
  if (params.frame_end == INT_MIN) {
    params.frame_end = EFRA;
  }

  /* Do export. */
  WM_cursor_wait(1);
  bool done = gpencil_io_export(filename, &params);
  WM_cursor_wait(0);

  if (done) {
    BKE_report(op->reports, RPT_INFO, "SVG export file created");
  }
  else {
    BKE_report(op->reports, RPT_WARNING, "Unable to export SVG");
  }

  return OPERATOR_FINISHED;
}

static void ui_gpencil_export_stb_settings(uiLayout *layout, PointerRNA *imfptr)
{
  uiLayout *box, *row, *col, *sub, *col1;

  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);

  box = uiLayoutBox(layout);

  row = uiLayoutRow(box, false);
  uiItemL(row, IFACE_("Scene Options"), ICON_SCENE_DATA);

  row = uiLayoutRow(box, false);
  uiItemR(row, imfptr, "selected_object_type", 0, NULL, ICON_NONE);

  box = uiLayoutBox(layout);
  col = uiLayoutColumn(box, false);

  sub = uiLayoutColumn(col, true);
  uiItemR(sub, imfptr, "start", 0, IFACE_("Frame Start"), ICON_NONE);
  uiItemR(sub, imfptr, "end", 0, IFACE_("End"), ICON_NONE);

  uiLayoutSetPropSep(box, true);

  /* Add the Row, Col. */
  col1 = uiLayoutColumnWithHeading(col, true, IFACE_("Layout"));
  uiItemR(col1, imfptr, "size_col", 0, NULL, ICON_NONE);
  uiItemR(col1, imfptr, "size_row", 0, NULL, ICON_NONE);

  uiLayoutSetPropSep(box, true);

  row = uiLayoutRow(col, false);
  uiItemR(row, imfptr, "page_type", 0, NULL, ICON_NONE);

  row = uiLayoutRow(col, false);
  uiItemR(row, imfptr, "text_type", 0, NULL, ICON_NONE);

  row = uiLayoutRow(col, false);
  uiItemR(row, imfptr, "use_markers", 0, NULL, ICON_NONE);

  ui_gpencil_export_common_settings(layout, imfptr, true);
}

static void wm_gpencil_export_stb_draw(bContext *C, wmOperator *op)
{

  PointerRNA ptr;

  RNA_pointer_create(NULL, op->type->srna, op->properties, &ptr);

  /* Conveniently set start and end frame to match the scene's frame range. */
  Scene *scene = CTX_data_scene(C);

  if (scene != NULL && RNA_boolean_get(&ptr, "init_scene_frame_range")) {
    RNA_int_set(&ptr, "start", SFRA);
    RNA_int_set(&ptr, "end", EFRA);

    RNA_boolean_set(&ptr, "init_scene_frame_range", false);
  }

  ui_gpencil_export_stb_settings(op->layout, &ptr);
}

static bool wm_gpencil_export_stb_poll(bContext *C)
{
  if (CTX_wm_window(C) == NULL) {
    return false;
  }
  Object *ob = CTX_data_active_object(C);
  if ((ob == NULL) || (ob->type != OB_GPENCIL)) {
    return false;
  }

  bGPdata *gpd = (bGPdata *)ob->data;
  bGPDlayer *gpl = BKE_gpencil_layer_active_get(gpd);

  if (gpl == NULL) {
    return false;
  }

  /* Need camera view. */
  ARegion *region = get_invoke_region(C);
  if (region == NULL) {
    return false;
  }
  RegionView3D *rv3d = (RegionView3D *)region->regiondata;
  return (rv3d && (rv3d->persp == RV3D_CAMOB));
}

void WM_OT_gpencil_export_storyboard(wmOperatorType *ot)
{

  static const EnumPropertyItem gpencil_export_paper_items[] = {
      {GP_EXPORT_PAPER_LANDSCAPE, "LANDSCAPE", 0, "Landscape", ""},
      {GP_EXPORT_PAPER_PORTRAIT, "PORTRAIT", 0, "Portrait", ""},
      {0, NULL, 0, NULL, NULL},
  };

  static const EnumPropertyItem gpencil_export_text_items[] = {
      {GP_EXPORT_TXT_NONE, "NONE", 0, "None", ""},
      {GP_EXPORT_TXT_SHOT, "SHOT", 0, "Shot", "Include shot number"},
      {GP_EXPORT_TXT_FRAME, "FRAME", 0, "Frame", "Include Frame number"},
      {GP_EXPORT_TXT_SHOT_FRAME, "SHOTFRAME", 0, "Shot & Frame", "Include Shot and Frame number"},
      {0, NULL, 0, NULL, NULL},
  };

  ot->name = "Export to Storyboard";
  ot->description = "Export grease pencil in Storyboard format from camera view";
  ot->idname = "WM_OT_gpencil_export_storyboard";

  ot->invoke = wm_gpencil_export_stb_invoke;
  ot->exec = wm_gpencil_export_stb_exec;
  ot->poll = wm_gpencil_export_stb_poll;
  ot->ui = wm_gpencil_export_stb_draw;
  ot->check = wm_gpencil_export_svg_common_check;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_OBJECT_IO,
                                 FILE_BLENDER,
                                 FILE_SAVE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_SHOW_PROPS,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_ALPHA);

  gpencil_export_common_props_svg(ot);

  RNA_def_int(ot->srna,
              "start",
              INT_MIN,
              INT_MIN,
              INT_MAX,
              "Start Frame",
              "Start frame of the export, use the default value to "
              "take the start frame of the current scene",
              INT_MIN,
              INT_MAX);

  RNA_def_int(ot->srna,
              "end",
              INT_MIN,
              INT_MIN,
              INT_MAX,
              "End Frame",
              "End frame of the export, use the default value to "
              "take the end frame of the current scene",
              INT_MIN,
              INT_MAX);

  RNA_def_enum(ot->srna, "page_type", gpencil_export_paper_items, 0, "Page", "Page orientation");
  RNA_def_enum(ot->srna,
               "text_type",
               gpencil_export_text_items,
               GP_EXPORT_TXT_SHOT_FRAME,
               "Text",
               "Text included by frame");

  RNA_def_int(ot->srna, "size_col", 3, 1, 6, "Colums", "Number of columns per page", 1, 6);
  RNA_def_int(ot->srna, "size_row", 2, 1, 6, "Rows", "Number of rows per page", 1, 6);
  RNA_def_boolean(ot->srna, "use_markers", false, "Markers", "Use markers to select frames");

  /* This dummy prop is used to check whether we need to init the start and
   * end frame values to that of the scene's, otherwise they are reset at
   * every change, draw update. */
  RNA_def_boolean(ot->srna, "init_scene_frame_range", false, "", "");
}

/* <-------- PDF single frame export. --------> */
static int wm_gpencil_export_pdf_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  UNUSED_VARS(event);

  if (!RNA_struct_property_is_set(op->ptr, "filepath")) {
    Main *bmain = CTX_data_main(C);
    char filepath[FILE_MAX];

    if (BKE_main_blendfile_path(bmain)[0] == '\0') {
      BLI_strncpy(filepath, "untitled", sizeof(filepath));
    }
    else {
      BLI_strncpy(filepath, BKE_main_blendfile_path(bmain), sizeof(filepath));
    }

    BLI_path_extension_replace(filepath, sizeof(filepath), ".pdf");
    RNA_string_set(op->ptr, "filepath", filepath);
  }

  WM_event_add_fileselect(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static int wm_gpencil_export_pdf_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  Object *ob = CTX_data_active_object(C);

  if (!RNA_struct_property_is_set(op->ptr, "filepath")) {
    BKE_report(op->reports, RPT_ERROR, "No filename given");
    return OPERATOR_CANCELLED;
  }

  /*For some reason the region cannot be retrieved from the context.
   * If a better solution is found in the future, remove this function. */
  ARegion *region = get_invoke_region(C);
  if (region == NULL) {
    BKE_report(op->reports, RPT_ERROR, "Unable to find valid 3D View area");
    return OPERATOR_CANCELLED;
  }
  View3D *v3d = get_invoke_view3d(C);

  char filename[FILE_MAX];
  RNA_string_get(op->ptr, "filepath", filename);

  const bool use_fill = RNA_boolean_get(op->ptr, "use_fill");
  const bool use_norm_thickness = RNA_boolean_get(op->ptr, "use_normalized_thickness");
  const short select = RNA_enum_get(op->ptr, "selected_object_type");
  const short frame_type = RNA_enum_get(op->ptr, "frame_type");

  /* Set flags. */
  int flag = 0;
  SET_FLAG_FROM_TEST(flag, use_fill, GP_EXPORT_FILL);
  SET_FLAG_FROM_TEST(flag, use_norm_thickness, GP_EXPORT_NORM_THICKNESS);

  float paper[2];
  paper[0] = scene->r.xsch * scene->r.size;
  paper[1] = scene->r.ysch * scene->r.size;

  struct GpencilExportParams params = {
      .C = C,
      .region = region,
      .v3d = v3d,
      .obact = ob,
      .mode = GP_EXPORT_TO_PDF,
      .frame_start = SFRA,
      .frame_end = EFRA,
      .flag = flag,
      .select = select,
      .frame_type = frame_type,
      .stroke_sample = RNA_float_get(op->ptr, "stroke_sample"),
      .page_layout = {0.0f, 0.0f},
      .page_type = 0,
      .paper_size = {paper[0], paper[1]},
      .text_type = 0,
  };

  /* Do export. */
  WM_cursor_wait(1);
  bool done = gpencil_io_export(filename, &params);
  WM_cursor_wait(0);

  if (done) {
    BKE_report(op->reports, RPT_INFO, "PDF export file created");
  }
  else {
    BKE_report(op->reports, RPT_WARNING, "Unable to export PDF");
  }

  return OPERATOR_FINISHED;
}

static void ui_gpencil_export_pdf_settings(uiLayout *layout, PointerRNA *imfptr)
{
  uiLayout *box, *row, *col, *sub;

  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);

  box = uiLayoutBox(layout);

  row = uiLayoutRow(box, false);
  uiItemL(row, IFACE_("Scene Options"), ICON_SCENE_DATA);

  row = uiLayoutRow(box, false);
  uiItemR(row, imfptr, "selected_object_type", 0, NULL, ICON_NONE);

  box = uiLayoutBox(layout);
  row = uiLayoutRow(box, false);
  uiItemL(row, IFACE_("Export Options"), ICON_SCENE_DATA);

  col = uiLayoutColumn(box, false);
  sub = uiLayoutColumn(col, true);
  uiItemR(sub, imfptr, "frame_type", 0, IFACE_("Frame"), ICON_NONE);

  uiLayoutSetPropSep(box, true);

  sub = uiLayoutColumn(col, true);
  uiItemR(sub, imfptr, "stroke_sample", 0, NULL, ICON_NONE);
  uiItemR(sub, imfptr, "use_fill", 0, NULL, ICON_NONE);
  uiItemR(sub, imfptr, "use_normalized_thickness", 0, NULL, ICON_NONE);
}

static void wm_gpencil_export_pdf_draw(bContext *C, wmOperator *op)
{
  PointerRNA ptr;

  RNA_pointer_create(NULL, op->type->srna, op->properties, &ptr);

  ui_gpencil_export_pdf_settings(op->layout, &ptr);
}

static bool wm_gpencil_export_pdf_poll(bContext *C)
{
  if (CTX_wm_window(C) == NULL) {
    return false;
  }
  Object *ob = CTX_data_active_object(C);
  if ((ob == NULL) || (ob->type != OB_GPENCIL)) {
    return false;
  }

  bGPdata *gpd = (bGPdata *)ob->data;
  bGPDlayer *gpl = BKE_gpencil_layer_active_get(gpd);

  if (gpl == NULL) {
    return false;
  }

  return true;
}

void WM_OT_gpencil_export_pdf(wmOperatorType *ot)
{
  ot->name = "Export to PDF";
  ot->description = "Export grease pencil to PDF";
  ot->idname = "WM_OT_gpencil_export_pdf";

  ot->invoke = wm_gpencil_export_pdf_invoke;
  ot->exec = wm_gpencil_export_pdf_exec;
  ot->poll = wm_gpencil_export_pdf_poll;
  ot->ui = wm_gpencil_export_pdf_draw;
  ot->check = wm_gpencil_export_pdf_common_check;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_OBJECT_IO,
                                 FILE_BLENDER,
                                 FILE_SAVE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_SHOW_PROPS,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_ALPHA);

  static const EnumPropertyItem gpencil_export_select_items[] = {
      {GP_EXPORT_ACTIVE, "ACTIVE", 0, "Active", "Include only active object"},
      {GP_EXPORT_SELECTED, "SELECTED", 0, "Selected", "Include selected objects"},
      {GP_EXPORT_VISIBLE, "VISIBLE", 0, "Visible", "Include visible objects"},
      {0, NULL, 0, NULL, NULL},
  };
  static const EnumPropertyItem gpencil_export_frame_items[] = {
      {GP_EXPORT_FRAME_ACTIVE, "ACTIVE", 0, "Active", "Include only active frame"},
      {GP_EXPORT_FRAME_SELECTED, "SELECTED", 0, "Selected", "Include selected frames"},
      {0, NULL, 0, NULL, NULL},
  };

  RNA_def_boolean(ot->srna, "use_fill", true, "Fill", "Export filled areas");
  RNA_def_boolean(ot->srna,
                  "use_normalized_thickness",
                  false,
                  "Normalize",
                  "Export strokes with constant thickness along the stroke");
  ot->prop = RNA_def_enum(ot->srna,
                          "selected_object_type",
                          gpencil_export_select_items,
                          GP_EXPORT_SELECTED,
                          "Object",
                          "Objects included in the export");

  RNA_def_float(
      ot->srna,
      "stroke_sample",
      0.0f,
      0.0f,
      100.0f,
      "Sampling",
      "Precision of sampling stroke, low values gets more precise result, zero to disable",
      0.0f,
      100.0f);
  ot->prop = RNA_def_enum(ot->srna,
                          "frame_type",
                          gpencil_export_frame_items,
                          GP_EXPORT_ACTIVE,
                          "Frames",
                          "Frames included in the export");
}
