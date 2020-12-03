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
 * \ingroup edasset
 */

#include <string.h>

#include "BKE_asset.h"
#include "BKE_context.h"
#include "BKE_report.h"

#include "BLI_listbase.h"
#include "BLI_string_utils.h"

#include "DNA_asset_types.h"

#include "ED_asset.h"

#include "MEM_guardedalloc.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "WM_api.h"
#include "WM_types.h"

/* -------------------------------------------------------------------- */

/**
 * Return the IDs to operate on as list of #CollectionPointerLink links. Needs freeing.
 */
static ListBase asset_make_get_ids_from_context(const bContext *C)
{
  ListBase list = {0};

  PointerRNA idptr = CTX_data_pointer_get_type(C, "id", &RNA_ID);

  if (idptr.data) {
    CollectionPointerLink *ctx_link = MEM_callocN(sizeof(*ctx_link), __func__);
    ctx_link->ptr = idptr;
    BLI_addtail(&list, ctx_link);
  }
  else {
    CTX_data_selected_ids(C, &list);
  }

  return list;
}

struct AssetMakeResultStats {
  int tot_created;
  int tot_selected_ids;
  int tot_already_asset;
  ID *last_id;
};

static void asset_make_for_idptr_list(const bContext *C,
                                      const ListBase *ids,
                                      struct AssetMakeResultStats *r_stats)
{
  memset(r_stats, 0, sizeof(*r_stats));

  LISTBASE_FOREACH (CollectionPointerLink *, ctx_id, ids) {
    BLI_assert(RNA_struct_is_ID(ctx_id->ptr.type));
    r_stats->tot_selected_ids++;

    ID *id = ctx_id->ptr.data;
    if (id->asset_data) {
      r_stats->tot_already_asset++;
      continue;
    }

    ED_asset_make_for_id(C, id);

    r_stats->last_id = id;
    r_stats->tot_created++;
  }
}

static int asset_make_exec(bContext *C, wmOperator *op)
{
  ListBase ids = asset_make_get_ids_from_context(C);

  struct AssetMakeResultStats stats;
  asset_make_for_idptr_list(C, &ids, &stats);
  BLI_freelistN(&ids);

  /* User feedback on failure. */
  if ((stats.tot_created < 1) && (stats.tot_already_asset > 0)) {
    BKE_report(op->reports, RPT_ERROR, "Selected data-blocks are already assets");
    return OPERATOR_CANCELLED;
  }
  if (stats.tot_created < 1) {
    BKE_report(op->reports, RPT_ERROR, "No data-blocks to create assets for found");
    return OPERATOR_CANCELLED;
  }

  /* User feedback on success. */
  if (stats.tot_created == 1) {
    /* If only one data-block: Give more useful message by printing asset name. */
    BKE_reportf(op->reports, RPT_INFO, "Data-block '%s' is now an asset", stats.last_id->name + 2);
  }
  else {
    BKE_reportf(op->reports, RPT_INFO, "%i data-blocks are now assets", stats.tot_created);
  }

  WM_main_add_notifier(NC_ID | NA_EDITED, NULL);
  WM_main_add_notifier(NC_ASSET | NA_ADDED, NULL);

  return OPERATOR_FINISHED;
}

static void ASSET_OT_make(wmOperatorType *ot)
{
  ot->name = "Make Asset";
  ot->description = "Enable asset management for a data-block";
  ot->idname = "ASSET_OT_make";

  ot->exec = asset_make_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* -------------------------------------------------------------------- */

void ED_operatortypes_asset(void)
{
  WM_operatortype_append(ASSET_OT_make);
}
