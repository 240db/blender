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
 * The Original Code is Copyright (C) 2021, Blender Foundation
 */

/** \file
 * \ingroup edarmature
 */

#include <math.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_path_util.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "BLO_readfile.h"

#include "BLT_translation.h"

#include "DNA_ID.h"
#include "DNA_action_types.h"
#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_action.h"
#include "BKE_anim_data.h"
#include "BKE_animsys.h"
#include "BKE_armature.h"
#include "BKE_context.h"
#include "BKE_idprop.h"
#include "BKE_lib_id.h"
#include "BKE_object.h"
#include "BKE_report.h"

#include "DEG_depsgraph.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "WM_api.h"
#include "WM_types.h"

#include "UI_interface.h"

#include "ED_asset.h"
#include "ED_keyframing.h"
#include "ED_object.h"
#include "ED_screen.h"

#include "armature_intern.h"

struct ScrArea;

typedef enum ePoseBlendState {
  POSE_BLEND_INIT,
  POSE_BLEND_BLENDING,
  POSE_BLEND_ORIGINAL,
  POSE_BLEND_CONFIRM,
  POSE_BLEND_CANCEL,
} ePoseBlendState;

typedef struct PoseBlendData {
  ePoseBlendState state;
  bool needs_redraw;
  bool is_bone_selection_relevant;

  struct {
    bool use_release_confirm;
    int drag_start_xy[2];
    int init_event_type;

    bool cursor_wrap_enabled;
  } release_confirm_info;

  /* For temp-loading the Action from the pose library. */
  AssetTempIDConsumer *temp_id_consumer;

  /* Blend factor, interval [0, 1] for interpolating between current and given pose. */
  float blend_factor;

  /** PoseChannelBackup structs for restoring poses. */
  ListBase backups;

  Object *ob;   /* Object to work on. */
  bAction *act; /* Pose to blend into the current pose. */
  bool free_action;

  Scene *scene;         /* For auto-keying. */
  struct ScrArea *area; /* For drawing status text. */

  /** Info-text to print in header. */
  char headerstr[UI_MAX_DRAW_STR];
} PoseBlendData;

/* simple struct for storing backup info for one pose channel */
typedef struct PoseChannelBackup {
  struct PoseChannelBackup *next, *prev;

  bPoseChannel *pchan; /* Pose channel this backup is for. */

  bPoseChannel olddata; /* Backup of pose channel. */
  IDProperty *oldprops; /* Backup copy (needs freeing) of pose channel's ID properties. */
} PoseChannelBackup;

/* Makes a copy of the current pose for restoration purposes - doesn't do constraints currently */
static void poselib_backup_posecopy(PoseBlendData *pbd)
{
  /* TODO(Sybren): reuse same approach as in `armature_pose.cc` in this function. */

  /* See if bone selection is relevant. */
  bool all_bones_selected = true;
  bool no_bones_selected = true;
  const bArmature *armature = pbd->ob->data;
  LISTBASE_FOREACH (bPoseChannel *, pchan, &pbd->ob->pose->chanbase) {
    const bool is_selected = PBONE_SELECTED(armature, pchan->bone);
    all_bones_selected &= is_selected;
    no_bones_selected &= !is_selected;
  }

  /* If no bones are selected, act as if all are. */
  pbd->is_bone_selection_relevant = !all_bones_selected && !no_bones_selected;

  LISTBASE_FOREACH (bActionGroup *, agrp, &pbd->act->groups) {
    bPoseChannel *pchan = BKE_pose_channel_find_name(pbd->ob->pose, agrp->name);
    if (pchan == NULL) {
      continue;
    }

    if (pbd->is_bone_selection_relevant && !PBONE_SELECTED(armature, pchan->bone)) {
      continue;
    }

    PoseChannelBackup *chan_bak;
    chan_bak = MEM_callocN(sizeof(*chan_bak), "PoseChannelBackup");
    chan_bak->pchan = pchan;
    memcpy(&chan_bak->olddata, chan_bak->pchan, sizeof(chan_bak->olddata));

    if (pchan->prop) {
      chan_bak->oldprops = IDP_CopyProperty(pchan->prop);
    }

    BLI_addtail(&pbd->backups, chan_bak);
  }

  if (pbd->state == POSE_BLEND_INIT) {
    /* Ready for blending now. */
    pbd->state = POSE_BLEND_BLENDING;
  }
}

/* Restores backed-up pose. */
static void poselib_backup_restore(PoseBlendData *pbd)
{
  LISTBASE_FOREACH (PoseChannelBackup *, chan_bak, &pbd->backups) {
    memcpy(chan_bak->pchan, &chan_bak->olddata, sizeof(chan_bak->olddata));

    if (chan_bak->oldprops) {
      IDP_SyncGroupValues(chan_bak->pchan->prop, chan_bak->oldprops);
    }

    /* TODO: constraints settings aren't restored yet,
     * even though these could change (though not that likely) */
  }
}

/* Free list of backups, including any side data it may use. */
static void poselib_backup_free_data(PoseBlendData *pbd)
{
  for (PoseChannelBackup *chan_bak = pbd->backups.first; chan_bak;) {
    PoseChannelBackup *next = chan_bak->next;

    if (chan_bak->oldprops) {
      IDP_FreeProperty(chan_bak->oldprops);
    }
    BLI_freelinkN(&pbd->backups, chan_bak);

    chan_bak = next;
  }
}

/* ---------------------------- */

/* Auto-key/tag bones affected by the pose Action. */
static void poselib_keytag_pose(bContext *C, Scene *scene, PoseBlendData *pbd)
{
  if (!autokeyframe_cfra_can_key(scene, &pbd->ob->id)) {
    return;
  }

  AnimData *adt = BKE_animdata_from_id(&pbd->ob->id);
  if (adt != NULL && adt->action != NULL && ID_IS_LINKED(&adt->action->id)) {
    /* Changes to linked-in Actions are not allowed. */
    return;
  }

  bPose *pose = pbd->ob->pose;
  bAction *act = pbd->act;
  bActionGroup *agrp;

  KeyingSet *ks = ANIM_get_keyingset_for_autokeying(scene, ANIM_KS_WHOLE_CHARACTER_ID);
  ListBase dsources = {NULL, NULL};

  /* start tagging/keying */
  const bArmature *armature = pbd->ob->data;
  for (agrp = act->groups.first; agrp; agrp = agrp->next) {
    /* only for selected bones unless there aren't any selected, in which case all are included  */
    bPoseChannel *pchan = BKE_pose_channel_find_name(pose, agrp->name);
    if (pchan == NULL) {
      continue;
    }

    if (pbd->is_bone_selection_relevant && !PBONE_SELECTED(armature, pchan->bone)) {
      continue;
    }

    /* Add data-source override for the PoseChannel, to be used later. */
    ANIM_relative_keyingset_add_source(&dsources, &pbd->ob->id, &RNA_PoseBone, pchan);
  }

  /* Perform actual auto-keying. */
  ANIM_apply_keyingset(C, &dsources, NULL, ks, MODIFYKEY_MODE_INSERT, (float)CFRA);
  BLI_freelistN(&dsources);

  /* send notifiers for this */
  WM_event_add_notifier(C, NC_ANIMATION | ND_KEYFRAME | NA_EDITED, NULL);
}

/* Apply the relevant changes to the pose */
static void poselib_blend_apply(bContext *C, wmOperator *op)
{
  PoseBlendData *pbd = (PoseBlendData *)op->customdata;

  if (pbd->state == POSE_BLEND_BLENDING) {
    BLI_snprintf(pbd->headerstr,
                 sizeof(pbd->headerstr),
                 TIP_("PoseLib blending: \"%s\" at %3.0f%%"),
                 pbd->act->id.name + 2,
                 pbd->blend_factor * 100);
    ED_area_status_text(pbd->area, pbd->headerstr);

    ED_workspace_status_text(
        C, TIP_("Tab: show original pose; Horizontal mouse movement: change blend percentage"));
  }
  else {
    ED_area_status_text(pbd->area, TIP_("PoseLib showing original pose"));
    ED_workspace_status_text(C, TIP_("Tab: show blended pose"));
  }

  if (!pbd->needs_redraw) {
    return;
  }
  pbd->needs_redraw = false;

  poselib_backup_restore(pbd);

  /* The pose needs updating, whether it's for restoring the original pose or for showing the
   * result of the blend. */
  DEG_id_tag_update(&pbd->ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_POSE, pbd->ob);

  if (pbd->state != POSE_BLEND_BLENDING) {
    return;
  }

  /* Perform the actual blending. */
  struct Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  AnimationEvalContext anim_eval_context = BKE_animsys_eval_context_construct(depsgraph, 0.0f);
  BKE_pose_apply_action_blend(pbd->ob, pbd->act, &anim_eval_context, pbd->blend_factor);
}

/* ---------------------------- */

static void poselib_blend_set_factor(PoseBlendData *pbd, const float new_factor)
{
  pbd->blend_factor = CLAMPIS(new_factor, 0.0f, 1.0f);
  pbd->needs_redraw = true;
}

#define BLEND_FACTOR_BY_WAVING_MOUSE_AROUND

#ifdef BLEND_FACTOR_BY_WAVING_MOUSE_AROUND
static void poselib_slide_mouse_update_blendfactor(PoseBlendData *pbd, const wmEvent *event)
{
  if (pbd->release_confirm_info.use_release_confirm) {
    /* Release confirm calculates factor based on where the dragging was started from. */
    const float range = 300 * U.pixelsize;
    const float new_factor = (event->x - pbd->release_confirm_info.drag_start_xy[0]) / range;
    poselib_blend_set_factor(pbd, new_factor);
  }
  else {
    const float new_factor = (event->x - pbd->area->v1->vec.x) / ((float)pbd->area->winx);
    poselib_blend_set_factor(pbd, new_factor);
  }
}
#else
static void poselib_blend_step_factor(PoseBlendData *pbd,
                                      const float blend_factor_step,
                                      const bool reduce_step)
{
  const float step_size = reduce_step ? blend_factor_step / 10.0f : blend_factor_step;
  const float new_factor = pbd->blend_factor + step_size;
  poselib_blend_set_factor(pbd, new_factor);
}
#endif

/* Return operator return value. */
static int poselib_blend_handle_event(bContext *UNUSED(C), wmOperator *op, const wmEvent *event)
{
  PoseBlendData *pbd = op->customdata;

#ifdef BLEND_FACTOR_BY_WAVING_MOUSE_AROUND
  if (event->type == MOUSEMOVE) {
    poselib_slide_mouse_update_blendfactor(pbd, event);
    return OPERATOR_RUNNING_MODAL;
  }
#endif

  /* Handle the release confirm event directly, it has priority over others. */
  if (pbd->release_confirm_info.use_release_confirm &&
      (event->type == pbd->release_confirm_info.init_event_type) && (event->val == KM_RELEASE)) {
    pbd->state = POSE_BLEND_CONFIRM;
    return OPERATOR_RUNNING_MODAL;
  }

  /* only accept 'press' event, and ignore 'release', so that we don't get double actions */
  if (ELEM(event->val, KM_PRESS, KM_NOTHING) == 0) {
    return OPERATOR_RUNNING_MODAL;
  }

#ifndef BLEND_FACTOR_BY_WAVING_MOUSE_AROUND
  if (ELEM(event->type,
           EVT_HOMEKEY,
           EVT_PAD0,
           EVT_PAD1,
           EVT_PAD2,
           EVT_PAD3,
           EVT_PAD4,
           EVT_PAD5,
           EVT_PAD6,
           EVT_PAD7,
           EVT_PAD8,
           EVT_PAD9,
           EVT_PADMINUS,
           EVT_PADPLUSKEY,
           MIDDLEMOUSE,
           MOUSEMOVE)) {
    /* Pass-through of view manipulation events. */
    return OPERATOR_PASS_THROUGH;
  }
#endif

  /* NORMAL EVENT HANDLING... */
  /* searching takes priority over normal activity */
  switch (event->type) {
    /* Exit - cancel. */
    case EVT_ESCKEY:
    case RIGHTMOUSE:
      pbd->state = POSE_BLEND_CANCEL;
      break;

    /* Exit - confirm. */
    case LEFTMOUSE:
    case EVT_RETKEY:
    case EVT_PADENTER:
    case EVT_SPACEKEY:
      pbd->state = POSE_BLEND_CONFIRM;
      break;

    /* TODO(Sybren): toggle between original pose and poselib pose. */
    case EVT_TABKEY:
      pbd->state = pbd->state == POSE_BLEND_BLENDING ? POSE_BLEND_ORIGINAL : POSE_BLEND_BLENDING;
      pbd->needs_redraw = true;
      break;

      /* TODO(Sybren): use better UI for slider. */
#ifndef BLEND_FACTOR_BY_WAVING_MOUSE_AROUND
    case WHEELUPMOUSE:
      poselib_blend_step_factor(pbd, 0.1f, event->shift);
      break;
    case WHEELDOWNMOUSE:
      poselib_blend_step_factor(pbd, -0.1f, event->shift);
      break;
#endif
  }

  return OPERATOR_RUNNING_MODAL;
}

static void poselib_blend_cursor_update(bContext *C, wmOperator *op)
{
  PoseBlendData *pbd = op->customdata;

  /* Ensure cursor-grab (continuous grabbing) is enabled when using release-confirm. */
  if (pbd->release_confirm_info.use_release_confirm &&
      !pbd->release_confirm_info.cursor_wrap_enabled) {
    WM_cursor_grab_enable(CTX_wm_window(C), WM_CURSOR_WRAP_XY, true, NULL);
    pbd->release_confirm_info.cursor_wrap_enabled = true;
  }
}

/* ---------------------------- */

/* Get object that Pose Lib should be found on */
/* XXX C can be zero */
static Object *get_poselib_object(bContext *C)
{
  ScrArea *area;

  /* sanity check */
  if (C == NULL) {
    return NULL;
  }

  area = CTX_wm_area(C);

  if (area && (area->spacetype == SPACE_PROPERTIES)) {
    return ED_object_context(C);
  }
  return BKE_object_pose_armature_get(CTX_data_active_object(C));
}

static void poselib_tempload_exit(PoseBlendData *pbd)
{
  ED_asset_temp_id_consumer_free(&pbd->temp_id_consumer);
}

static bAction *poselib_blend_init_get_action(bContext *C, wmOperator *op)
{
  bool asset_handle_valid;
  const AssetLibraryReference *asset_library = CTX_wm_asset_library(C);
  const AssetHandle asset_handle = CTX_wm_asset_handle(C, &asset_handle_valid);
  /* Poll callback should check. */
  BLI_assert((asset_library != NULL) && asset_handle_valid);

  PoseBlendData *pbd = op->customdata;

  pbd->temp_id_consumer = ED_asset_temp_id_consumer_create(&asset_handle);
  return (bAction *)ED_asset_temp_id_consumer_ensure_local_id(
      pbd->temp_id_consumer, C, asset_library, ID_AC, CTX_data_main(C), op->reports);
}

/* Return true on success, false if the context isn't suitable. */
static bool poselib_blend_init_data(bContext *C, wmOperator *op, const wmEvent *event)
{
  op->customdata = NULL;

  /* check if valid poselib */
  Object *ob = get_poselib_object(C);
  if (ELEM(NULL, ob, ob->pose, ob->data)) {
    BKE_report(op->reports, RPT_ERROR, TIP_("Pose lib is only for armatures in pose mode"));
    return false;
  }

  /* Set up blend state info. */
  PoseBlendData *pbd;
  op->customdata = pbd = MEM_callocN(sizeof(PoseBlendData), "PoseLib Preview Data");

  bAction *action = poselib_blend_init_get_action(C, op);
  if (action == NULL) {
    return false;
  }

  /* Maybe flip the Action. */
  const bool apply_flipped = RNA_boolean_get(op->ptr, "flipped");
  if (apply_flipped) {
    action = (bAction *)BKE_id_copy_ex(NULL, &action->id, NULL, LIB_ID_COPY_LOCALIZE);
    BKE_action_flip_with_pose(action, ob);
    pbd->free_action = true;
  }
  pbd->act = action;

  /* Get the basic data. */
  pbd->ob = ob;
  pbd->ob->pose = ob->pose;

  pbd->scene = CTX_data_scene(C);
  pbd->area = CTX_wm_area(C);

  pbd->state = POSE_BLEND_INIT;
  pbd->needs_redraw = true;
  pbd->blend_factor = RNA_float_get(op->ptr, "blend_factor");
  /* Just to avoid a clang-analyzer warning (false positive), it's set properly below. */
  pbd->release_confirm_info.use_release_confirm = false;

  /* Release confirm data. Only available if there's an event to work with. */
  if (event != NULL) {
    PropertyRNA *release_confirm_prop = RNA_struct_find_property(op->ptr, "release_confirm");
    pbd->release_confirm_info.use_release_confirm = (release_confirm_prop != NULL) &&
                                                    RNA_property_boolean_get(op->ptr,
                                                                             release_confirm_prop);
  }

  if (pbd->release_confirm_info.use_release_confirm) {
    BLI_assert(event != NULL);
    pbd->release_confirm_info.drag_start_xy[0] = event->x;
    pbd->release_confirm_info.drag_start_xy[1] = event->y;
    pbd->release_confirm_info.init_event_type = WM_userdef_event_type_from_keymap_type(
        event->type);
  }

  /* Make backups for blending and restoring the pose. */
  poselib_backup_posecopy(pbd);

  /* Set pose flags to ensure the depsgraph evaluation doesn't overwrite it. */
  pbd->ob->pose->flag &= ~POSE_DO_UNLOCK;
  pbd->ob->pose->flag |= POSE_LOCKED;

  return true;
}

static void poselib_blend_cleanup(bContext *C, wmOperator *op)
{
  PoseBlendData *pbd = op->customdata;
  wmWindow *win = CTX_wm_window(C);

  /* Redraw the header so that it doesn't show any of our stuff anymore. */
  ED_area_status_text(pbd->area, NULL);
  ED_workspace_status_text(C, NULL);

  /* This signals the depsgraph to unlock and reevaluate the pose on the next evaluation. */
  bPose *pose = pbd->ob->pose;
  pose->flag |= POSE_DO_UNLOCK;

  switch (pbd->state) {
    case POSE_BLEND_CONFIRM: {
      Scene *scene = pbd->scene;
      poselib_keytag_pose(C, scene, pbd);

      /* Ensure the redo panel has the actually-used value, instead of the initial value. */
      RNA_float_set(op->ptr, "blend_factor", pbd->blend_factor);
      break;
    }

    case POSE_BLEND_INIT:
    case POSE_BLEND_BLENDING:
    case POSE_BLEND_ORIGINAL:
      /* Cleanup should not be called directly from these states. */
      BLI_assert(!"poselib_blend_cleanup: unexpected pose blend state");
      BKE_report(op->reports, RPT_ERROR, "Internal pose library error, cancelling operator");
      ATTR_FALLTHROUGH;
    case POSE_BLEND_CANCEL:
      poselib_backup_restore(pbd);
      break;
  }

  if (pbd->release_confirm_info.cursor_wrap_enabled) {
    WM_cursor_grab_disable(win, pbd->release_confirm_info.drag_start_xy);
    pbd->release_confirm_info.cursor_wrap_enabled = false;
  }

  DEG_id_tag_update(&pbd->ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_OBJECT | ND_POSE, pbd->ob);
  /* Update mouse-hover highlights. */
  WM_event_add_mousemove(win);
}

static void poselib_blend_free(wmOperator *op)
{
  PoseBlendData *pbd = op->customdata;
  if (pbd == NULL) {
    return;
  }

  poselib_tempload_exit(pbd);
  if (pbd->free_action) {
    BKE_id_free(NULL, pbd->act);
  }
  /* Must have been dealt with before! */
  BLI_assert(pbd->release_confirm_info.cursor_wrap_enabled == false);

  /* Free temp data for operator */
  poselib_backup_free_data(pbd);
  MEM_SAFE_FREE(op->customdata);
}

static int poselib_blend_exit(bContext *C, wmOperator *op)
{
  PoseBlendData *pbd = op->customdata;
  const ePoseBlendState exit_state = pbd->state;

  poselib_blend_cleanup(C, op);
  poselib_blend_free(op);

  if (exit_state == POSE_BLEND_CANCEL) {
    return OPERATOR_CANCELLED;
  }
  return OPERATOR_FINISHED;
}

/* Cancel previewing operation (called when exiting Blender) */
static void poselib_blend_cancel(bContext *C, wmOperator *op)
{
  PoseBlendData *pbd = op->customdata;
  pbd->state = POSE_BLEND_CANCEL;
  poselib_blend_exit(C, op);
}

/* Main modal status check. */
static int poselib_blend_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  const int operator_result = poselib_blend_handle_event(C, op, event);

  poselib_blend_cursor_update(C, op);

  const PoseBlendData *pbd = op->customdata;
  if (ELEM(pbd->state, POSE_BLEND_CONFIRM, POSE_BLEND_CANCEL)) {
    return poselib_blend_exit(C, op);
  }

  if (pbd->needs_redraw) {
    poselib_blend_apply(C, op);
  }

  return operator_result;
}

/* Modal Operator init. */
static int poselib_blend_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (!poselib_blend_init_data(C, op, event)) {
    poselib_blend_free(op);
    return OPERATOR_CANCELLED;
  }

  /* Do initial apply to have something to look at. */
  poselib_blend_apply(C, op);

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

/* Single-shot apply. */
static int poselib_blend_exec(bContext *C, wmOperator *op)
{
  if (!poselib_blend_init_data(C, op, NULL)) {
    poselib_blend_free(op);
    return OPERATOR_CANCELLED;
  }

  poselib_blend_apply(C, op);

  PoseBlendData *pbd = op->customdata;
  pbd->state = POSE_BLEND_CONFIRM;
  return poselib_blend_exit(C, op);
}

static bool poselib_asset_in_context(bContext *C)
{
  bool asset_handle_valid;
  /* Check whether the context provides the asset data needed to add a pose. */
  const AssetLibraryReference *asset_library = CTX_wm_asset_library(C);
  AssetHandle asset_handle = CTX_wm_asset_handle(C, &asset_handle_valid);

  return (asset_library != NULL) && asset_handle_valid &&
         (asset_handle.file_data->blentype == ID_AC);
}

/* Poll callback for operators that require existing PoseLib data (with poses) to work. */
static bool poselib_blend_poll(bContext *C)
{
  Object *ob = get_poselib_object(C);
  if (ELEM(NULL, ob, ob->pose, ob->data)) {
    /* Pose lib is only for armatures in pose mode. */
    return false;
  }

  return poselib_asset_in_context(C);
}

void POSELIB_OT_apply_pose_asset(wmOperatorType *ot)
{
  /* Identifiers: */
  ot->name = "Apply Pose Library Pose";
  ot->idname = "POSELIB_OT_apply_pose_asset";
  ot->description = "Apply the given Pose Action to the rig";

  /* Callbacks: */
  ot->exec = poselib_blend_exec;
  ot->poll = poselib_blend_poll;

  /* Flags: */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Properties: */
  RNA_def_float_factor(ot->srna,
                       "blend_factor",
                       1.0f,
                       0.0f,
                       1.0f,
                       "Blend Factor",
                       "Amount that the pose is applied on top of the existing poses",
                       0.0f,
                       1.0f);
  RNA_def_boolean(ot->srna,
                  "flipped",
                  false,
                  "Apply Flipped",
                  "When enabled, applies the pose flipped over the X-axis");
}

void POSELIB_OT_blend_pose_asset(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* Identifiers: */
  ot->name = "Blend Pose Library Pose";
  ot->idname = "POSELIB_OT_blend_pose_asset";
  ot->description = "Blend the given Pose Action to the rig";

  /* Callbacks: */
  ot->invoke = poselib_blend_invoke;
  ot->modal = poselib_blend_modal;
  ot->cancel = poselib_blend_cancel;
  ot->exec = poselib_blend_exec;
  ot->poll = poselib_blend_poll;

  /* Flags: */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  /* Properties: */
  RNA_def_float_factor(ot->srna,
                       "blend_factor",
                       0.0f,
                       0.0f,
                       1.0f,
                       "Blend Factor",
                       "Amount that the pose is applied on top of the existing poses",
                       0.0f,
                       1.0f);
  RNA_def_boolean(ot->srna,
                  "flipped",
                  false,
                  "Apply Flipped",
                  "When enabled, applies the pose flipped over the X-axis");
  prop = RNA_def_boolean(ot->srna,
                         "release_confirm",
                         false,
                         "Confirm on Release",
                         "Always confirm operation when releasing button");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}
