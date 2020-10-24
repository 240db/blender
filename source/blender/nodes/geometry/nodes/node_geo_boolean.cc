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

#include "MEM_guardedalloc.h"

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"

#include "BKE_mesh.h"

#include "BLI_alloca.h"
#include "BLI_math_matrix.h"

#include "bmesh.h"
#include "tools/bmesh_boolean.h"

#include "node_geometry_util.hh"

static bNodeSocketTemplate geo_node_boolean_in[] = {
    {SOCK_GEOMETRY, N_("Geometry A")},
    {SOCK_GEOMETRY, N_("Geometry B")},
    {-1, ""},
};

static bNodeSocketTemplate geo_node_boolean_out[] = {
    {SOCK_GEOMETRY, N_("Geometry")},
    {-1, ""},
};

static int bm_face_isect_pair(BMFace *f, void *UNUSED(user_data))
{
  return BM_elem_flag_test(f, BM_ELEM_DRAW) ? 1 : 0;
}

static Mesh *BM_mesh_boolean_calc(Mesh *mesh_a, Mesh *mesh_b, int boolean_mode)
{
  const BMAllocTemplate allocsize = BMALLOC_TEMPLATE_FROM_ME(mesh_a, mesh_b);

  BMesh *bm;
  {
    struct BMeshCreateParams bmesh_create_params = {.use_toolflags = false};
    bm = BM_mesh_create(&allocsize, &bmesh_create_params);
  }

  {
    struct BMeshFromMeshParams bmesh_from_mesh_params = {.calc_face_normal = true};
    BM_mesh_bm_from_me(bm, mesh_a, &bmesh_from_mesh_params);

    BM_mesh_bm_from_me(bm, mesh_b, &bmesh_from_mesh_params);
  }

  const int looptris_tot = poly_to_tri_count(bm->totface, bm->totloop);
  int tottri;
  BMLoop *(*looptris)[3];
  looptris = (BMLoop * (*)[3])(MEM_malloc_arrayN(looptris_tot, sizeof(*looptris), __func__));
  BM_mesh_calc_tessellation_beauty(bm, looptris, &tottri);

  BMIter iter;
  int i;
  const int i_faces_end = mesh_a->totpoly;

  /* We need face normals because of 'BM_face_split_edgenet'
   * we could calculate on the fly too (before calling split). */

  const short ob_src_totcol = mesh_a->totcol;
  short *material_remap = BLI_array_alloca(material_remap, ob_src_totcol ? ob_src_totcol : 1);

  BMFace *bm_face;
  i = 0;
  BM_ITER_MESH (bm_face, &iter, bm, BM_FACES_OF_MESH) {
    normalize_v3(bm_face->no);

    /* Temp tag to test which side split faces are from. */
    BM_elem_flag_enable(bm_face, BM_ELEM_DRAW);

    /* Remap material. */
    if (bm_face->mat_nr < ob_src_totcol) {
      bm_face->mat_nr = material_remap[bm_face->mat_nr];
    }

    if (++i == i_faces_end) {
      break;
    }
  }

  BM_mesh_boolean(bm, looptris, tottri, bm_face_isect_pair, NULL, 2, false, boolean_mode);

  Mesh *result = BKE_mesh_from_bmesh_for_eval_nomain(bm, NULL, mesh_a);
  BM_mesh_free(bm);
  result->runtime.cd_dirty_vert |= CD_MASK_NORMAL;

  return result;
}

// Mesh *mesh_boolean_calc(Mesh *mesh_a, Mesh *UNUSED(mesh_b), int UNUSED(boolean_mode))
// {
//   return mesh_a;
// }

namespace blender::nodes {
static void geo_boolean_exec(bNode *UNUSED(node), GValueByName &inputs, GValueByName &outputs)
{
  GeometryPtr geometry_in_a = inputs.extract<GeometryPtr>("Geometry A");
  GeometryPtr geometry_in_b = inputs.extract<GeometryPtr>("Geometry B");
  GeometryPtr geometry_out;

  if (!geometry_in_a.has_value() || !geometry_in_b.has_value()) {
    outputs.move_in("Geometry", std::move(geometry_out));
    return;
  }

  Mesh *mesh_in_a = geometry_in_a->mesh_get_for_read();
  Mesh *mesh_in_b = geometry_in_b->mesh_get_for_read();
  if (mesh_in_a == nullptr || mesh_in_b == nullptr) {
    outputs.move_in("Geometry", std::move(geometry_out));
    return;
  }

  geometry_out = GeometryPtr{new Geometry()};

  Mesh *mesh_out = BM_mesh_boolean_calc(mesh_in_a, mesh_in_b, eBooleanModifierOp_Difference);

  geometry_out->mesh_set_and_transfer_ownership(mesh_out);

  outputs.move_in("Geometry", std::move(geometry_out));
}
}  // namespace blender::nodes

void register_node_type_geo_boolean()
{
  static bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_BOOLEAN, "Boolean", 0, 0);
  node_type_socket_templates(&ntype, geo_node_boolean_in, geo_node_boolean_out);
  ntype.geometry_node_execute = blender::nodes::geo_boolean_exec;
  nodeRegisterType(&ntype);
}
