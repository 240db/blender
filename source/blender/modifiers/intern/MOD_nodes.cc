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
 * along with this program; if not, write to the Free Software  Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2005 by the Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup modifiers
 */

#include <cstring>
#include <iostream>
#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_float3.hh"
#include "BLI_listbase.h"
#include "BLI_string.h"
#include "BLI_utildefines.h"

#include "DNA_defaults.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_pointcloud_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BKE_customdata.h"
#include "BKE_lib_query.h"
#include "BKE_mesh.h"
#include "BKE_modifier.h"
#include "BKE_pointcloud.h"
#include "BKE_screen.h"
#include "BKE_simulation.h"

#include "BLO_read_write.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "RNA_access.h"

#include "DEG_depsgraph_build.h"
#include "DEG_depsgraph_query.h"

#include "MOD_modifiertypes.h"
#include "MOD_ui_common.h"

#include "NOD_derived_node_tree.hh"
#include "NOD_geometry_exec.hh"

using blender::float3;

static void initData(ModifierData *md)
{
  NodesModifierData *nmd = (NodesModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(nmd, modifier));

  MEMCPY_STRUCT_AFTER(nmd, DNA_struct_default_get(NodesModifierData), modifier);
}

static void updateDepsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  NodesModifierData *nmd = reinterpret_cast<NodesModifierData *>(md);
  if (nmd->node_tree != nullptr) {
    DEG_add_node_tree_relation(ctx->node, nmd->node_tree, "Nodes Modifier");
  }
}

static void foreachIDLink(ModifierData *md, Object *ob, IDWalkFunc walk, void *userData)
{
  NodesModifierData *nmd = reinterpret_cast<NodesModifierData *>(md);
  walk(userData, ob, (ID **)&nmd->node_tree, IDWALK_CB_USER);
}

static bool isDisabled(const struct Scene *UNUSED(scene),
                       ModifierData *md,
                       bool UNUSED(useRenderParams))
{
  NodesModifierData *nmd = reinterpret_cast<NodesModifierData *>(md);
  UNUSED_VARS(nmd);
  return false;
}

static PointCloud *modifyPointCloud(ModifierData *md,
                                    const ModifierEvalContext *UNUSED(ctx),
                                    PointCloud *pointcloud)
{
  NodesModifierData *nmd = reinterpret_cast<NodesModifierData *>(md);
  UNUSED_VARS(nmd);
  std::cout << __func__ << "\n";
  return pointcloud;
}

using namespace blender;
using namespace blender::nodes;
using namespace blender::fn;
using namespace blender::bke;

static Geometry *compute_geometry(const DOutputSocket *group_input,
                                  Geometry *group_input_geometry,
                                  const DInputSocket &socket_to_compute)
{
  Span<const DOutputSocket *> from_sockets = socket_to_compute.linked_sockets();
  Span<const DGroupInput *> from_group_inputs = socket_to_compute.linked_group_inputs();
  const int total_inputs = from_sockets.size() + from_group_inputs.size();
  if (total_inputs != 1) {
    return nullptr;
  }
  if (!from_group_inputs.is_empty()) {
    return nullptr;
  }

  const DOutputSocket &from_socket = *from_sockets[0];
  if (&from_socket == group_input) {
    return group_input_geometry;
  }
  if (from_socket.idname() != "NodeSocketGeometry") {
    return nullptr;
  }

  const DNode &from_node = from_socket.node();
  if (from_node.inputs().size() != 1) {
    return nullptr;
  }

  Geometry *input_geometry = compute_geometry(
      group_input, group_input_geometry, from_node.input(0));

  bNode *bnode = from_node.node_ref().bnode();
  GeometryNodeExecFunction execute = bnode->typeinfo->geometry_node_execute;

  LinearAllocator<> allocator;
  GeoNodeInputBuilder input_builder;
  GeometryP geometry_p{input_geometry};
  input_builder.add("Geometry", CPPType::get<GeometryP>(), &geometry_p);
  GeoNodeOutputCollector output_collector{allocator};
  execute(bnode, input_builder, output_collector);

  Geometry *output_geometry =
      output_collector.get<GeometryP>(from_socket.socket_ref().bsocket()->identifier).p;
  return output_geometry;
}

static Mesh *modifyMesh(ModifierData *md, const ModifierEvalContext *UNUSED(ctx), Mesh *mesh)
{
  NodesModifierData *nmd = reinterpret_cast<NodesModifierData *>(md);
  if (nmd->node_tree == nullptr) {
    return mesh;
  }

  NodeTreeRefMap tree_refs;
  DerivedNodeTree tree{nmd->node_tree, tree_refs};

  Span<const DNode *> input_nodes = tree.nodes_by_type("NodeGroupInput");
  Span<const DNode *> output_nodes = tree.nodes_by_type("NodeGroupOutput");

  if (input_nodes.size() > 1) {
    return mesh;
  }
  if (output_nodes.size() != 1) {
    return mesh;
  }

  Span<const DOutputSocket *> group_inputs = (input_nodes.size() == 1) ?
                                                 input_nodes[0]->outputs().drop_back(1) :
                                                 Span<const DOutputSocket *>{};
  Span<const DInputSocket *> group_outputs = output_nodes[0]->inputs().drop_back(1);

  if (group_outputs.size() != 1) {
    return mesh;
  }

  const DInputSocket *group_output = group_outputs[0];
  if (group_output->idname() != "NodeSocketGeometry") {
    return mesh;
  }

  Geometry *input_geometry = Geometry::from_mesh(mesh);
  Geometry *new_geometry = compute_geometry(group_inputs[0], input_geometry, *group_outputs[0]);
  if (new_geometry == nullptr) {
    return mesh;
  }
  Mesh *new_mesh = new_geometry->extract_mesh();
  return new_mesh;
}

static void panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);

  uiItemR(layout, ptr, "node_tree", 0, NULL, ICON_MESH_DATA);

  modifier_panel_end(layout, ptr);
}

static void panelRegister(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Nodes, panel_draw);
}

static void blendWrite(BlendWriter *writer, const ModifierData *md)
{
  const NodesModifierData *nmd = reinterpret_cast<const NodesModifierData *>(md);
  UNUSED_VARS(nmd, writer);
}

static void blendRead(BlendDataReader *reader, ModifierData *md)
{
  NodesModifierData *nmd = reinterpret_cast<NodesModifierData *>(md);
  UNUSED_VARS(nmd, reader);
}

static void copyData(const ModifierData *md, ModifierData *target, const int flag)
{
  const NodesModifierData *nmd = reinterpret_cast<const NodesModifierData *>(md);
  NodesModifierData *tnmd = reinterpret_cast<NodesModifierData *>(target);
  UNUSED_VARS(nmd, tnmd);

  BKE_modifier_copydata_generic(md, target, flag);
}

static void freeData(ModifierData *md)
{
  NodesModifierData *nmd = reinterpret_cast<NodesModifierData *>(md);
  UNUSED_VARS(nmd);
}

ModifierTypeInfo modifierType_Nodes = {
    /* name */ "Nodes",
    /* structName */ "NodesModifierData",
    /* structSize */ sizeof(NodesModifierData),
#ifdef WITH_GEOMETRY_NODES
    /* srna */ &RNA_NodesModifier,
#else
    /* srna */ &RNA_Modifier,
#endif
    /* type */ eModifierTypeType_Constructive,
    /* flags */ eModifierTypeFlag_AcceptsMesh,
    /* icon */ ICON_MESH_DATA, /* TODO: Use correct icon. */

    /* copyData */ copyData,

    /* deformVerts */ NULL,
    /* deformMatrices */ NULL,
    /* deformVertsEM */ NULL,
    /* deformMatricesEM */ NULL,
    /* modifyMesh */ modifyMesh,
    /* modifyHair */ NULL,
    /* modifyPointCloud */ modifyPointCloud,
    /* modifyVolume */ NULL,

    /* initData */ initData,
    /* requiredDataMask */ NULL,
    /* freeData */ freeData,
    /* isDisabled */ isDisabled,
    /* updateDepsgraph */ updateDepsgraph,
    /* dependsOnTime */ NULL,
    /* dependsOnNormals */ NULL,
    /* foreachIDLink */ foreachIDLink,
    /* foreachTexLink */ NULL,
    /* freeRuntimeData */ NULL,
    /* panelRegister */ panelRegister,
    /* blendWrite */ blendWrite,
    /* blendRead */ blendRead,
};
