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
 * \ingroup obj
 */

#include "BKE_customdata.h"
#include "BKE_lib_id.h"
#include "BKE_material.h"
#include "BKE_mesh.h"
#include "BKE_mesh_mapping.h"
#include "BKE_object.h"

#include "BLI_listbase.h"
#include "BLI_math.h"

#include "DEG_depsgraph_query.h"

#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "obj_export_mesh.hh"

namespace blender::io::obj {
/**
 * Store evaluated object and mesh pointers depending on object type.
 * New meshes are created for supported curves converted to meshes, and triangulated
 * meshes.
 */
OBJMesh::OBJMesh(Depsgraph *depsgraph, const OBJExportParams &export_params, Object *export_object)
    : depsgraph_(depsgraph), export_object_eval_(export_object)
{
  export_object_eval_ = DEG_get_evaluated_object(depsgraph_, export_object);
  export_mesh_eval_ = BKE_object_get_evaluated_mesh(export_object_eval_);
  mesh_eval_needs_free_ = false;

  if (!export_mesh_eval_) {
    /* Curves and NURBS surfaces need a new mesh when they're
     * exported in the form of vertices and edges.
     */
    export_mesh_eval_ = BKE_mesh_new_from_object(depsgraph_, export_object_eval_, true);
    /* Since a new mesh been allocated, it needs to be freed in the destructor. */
    mesh_eval_needs_free_ = true;
  }
  if (export_params.export_triangulated_mesh &&
      ELEM(export_object_eval_->type, OB_MESH, OB_SURF)) {
    std::tie(export_mesh_eval_, mesh_eval_needs_free_) = triangulate_mesh_eval();
  }
  set_world_axes_transform(export_params.forward_axis, export_params.up_axis);
}

/**
 * Free new meshes allocated for triangulated meshes, and curves converted to meshes.
 */
OBJMesh::~OBJMesh()
{
  free_mesh_if_needed();
  if (poly_smooth_groups_) {
    MEM_freeN(poly_smooth_groups_);
  }
  BLI_assert(!export_mesh_eval_);
}

void OBJMesh::free_mesh_if_needed()
{
  if (mesh_eval_needs_free_ && export_mesh_eval_) {
    BKE_id_free(NULL, export_mesh_eval_);
  }
}

/**
 * Allocate a new Mesh with triangulate polygons.
 *
 * The returned mesh can be the same as the old one.
 * \return Owning pointer to the new Mesh, and whether a new Mesh was created.
 */
std::pair<Mesh *, bool> OBJMesh::triangulate_mesh_eval()
{
  if (export_mesh_eval_->totpoly <= 0) {
    return {export_mesh_eval_, false};
  }
  const struct BMeshCreateParams bm_create_params = {false};
  /* If `BMeshFromMeshParams.calc_face_normal` is false, it triggers
   * BLI_assert(BM_face_is_normal_valid(f)). */
  const struct BMeshFromMeshParams bm_convert_params = {true, 0, 0, 0};
  /* Lower threshold where triangulation of a face starts, i.e. a quadrilateral will be
   * triangulated here. */
  const int triangulate_min_verts = 4;

  unique_bmesh_ptr bmesh(
      BKE_mesh_to_bmesh_ex(export_mesh_eval_, &bm_create_params, &bm_convert_params));
  BM_mesh_triangulate(bmesh.get(),
                      MOD_TRIANGULATE_NGON_BEAUTY,
                      MOD_TRIANGULATE_QUAD_SHORTEDGE,
                      triangulate_min_verts,
                      false,
                      NULL,
                      NULL,
                      NULL);

  Mesh *triangulated = BKE_mesh_from_bmesh_for_eval_nomain(bmesh.get(), NULL, export_mesh_eval_);
  free_mesh_if_needed();
  return {triangulated, true};
}

/**
 * Store the product of export axes settings and an object's world transform matrix in
 * world_and_axes_transform[4][4].
 */
void OBJMesh::set_world_axes_transform(const eTransformAxisForward forward,
                                       const eTransformAxisUp up)
{
  float axes_transform[3][3];
  unit_m3(axes_transform);
  /* -Y-forward and +Z-up are the default Blender axis settings. */
  mat3_from_axis_conversion(
      OBJ_AXIS_NEGATIVE_Y_FORWARD, OBJ_AXIS_Z_UP, forward, up, axes_transform);
  mul_m4_m3m4(world_and_axes_transform_, axes_transform, export_object_eval_->obmat);
  /* mul_m4_m3m4 does not copy last row of obmat, i.e. location data. */
  copy_v4_v4(world_and_axes_transform_[3], export_object_eval_->obmat[3]);
}

uint OBJMesh::tot_vertices() const
{
  return export_mesh_eval_->totvert;
}

uint OBJMesh::tot_polygons() const
{
  return export_mesh_eval_->totpoly;
}

uint OBJMesh::tot_uv_vertices() const
{
  return tot_uv_vertices_;
}

/**
 * UV vertex indices of one polygon.
 */
Span<uint> OBJMesh::uv_indices(const int poly_index) const
{
  BLI_assert(poly_index < export_mesh_eval_->totpoly);
  BLI_assert(poly_index < uv_indices_.size());
  return uv_indices_[poly_index];
}

uint OBJMesh::tot_edges() const
{
  return export_mesh_eval_->totedge;
}

/**
 * Total materials in the object to export.
 */
short OBJMesh::tot_materials() const
{
  return export_mesh_eval_->totcol;
}

/**
 * Total smooth groups in the object to export.
 */
uint OBJMesh::tot_smooth_groups() const
{
  BLI_assert(tot_smooth_groups_ != -1);
  /* Calculate smooth groups first: `OBJMesh::calc_smooth_groups`. */
  return tot_smooth_groups_;
}

/**
 * Return smooth group of the polygon at the given index.
 */
int OBJMesh::ith_smooth_group(const int poly_index) const
{
  BLI_assert(tot_smooth_groups_ != -1);
  /* Calculate smooth groups first: `OBJMesh::calc_smooth_groups`. */
  BLI_assert(poly_smooth_groups_);
  return poly_smooth_groups_[poly_index];
}

void OBJMesh::ensure_mesh_normals() const
{
  BKE_mesh_ensure_normals(export_mesh_eval_);
  BKE_mesh_calc_normals_split(export_mesh_eval_);
}

void OBJMesh::ensure_mesh_edges() const
{
  BKE_mesh_calc_edges(export_mesh_eval_, true, false);
  BKE_mesh_calc_edges_loose(export_mesh_eval_);
}

/**
 * Calculate smooth groups of a smooth shaded object.
 * \return A polygon aligned array of smooth group numbers or bitflags if export
 * settings specify so.
 */
void OBJMesh::calc_smooth_groups(const bool use_bitflags)
{
  int tot_smooth_groups = 0;
  poly_smooth_groups_ = BKE_mesh_calc_smoothgroups(export_mesh_eval_->medge,
                                                   export_mesh_eval_->totedge,
                                                   export_mesh_eval_->mpoly,
                                                   export_mesh_eval_->totpoly,
                                                   export_mesh_eval_->mloop,
                                                   export_mesh_eval_->totloop,
                                                   &tot_smooth_groups,
                                                   use_bitflags);
  tot_smooth_groups_ = tot_smooth_groups;
}

/**
 * Return mat_nr-th material of the object.
 */
const Material *OBJMesh::get_object_material(const short mat_nr) const
{
  return BKE_object_material_get(export_object_eval_, mat_nr);
}

bool OBJMesh::is_ith_poly_smooth(const uint poly_index) const
{
  return export_mesh_eval_->mpoly[poly_index].flag & ME_SMOOTH;
}

short OBJMesh::ith_poly_matnr(const uint poly_index) const
{
  BLI_assert(poly_index < export_mesh_eval_->totpoly);
  return export_mesh_eval_->mpoly[poly_index].mat_nr;
}

int OBJMesh::ith_poly_totloop(const uint poly_index) const
{
  BLI_assert(poly_index < export_mesh_eval_->totpoly);
  return export_mesh_eval_->mpoly[poly_index].totloop;
}

/**
 * Get object name as it appears in the outliner.
 */
const char *OBJMesh::get_object_name() const
{
  return export_object_eval_->id.name + 2;
}

/**
 * Get object's mesh name.
 */
const char *OBJMesh::get_object_mesh_name() const
{
  return export_mesh_eval_->id.name + 2;
}

/**
 * Get object's material (at the given index) name.
 */
const char *OBJMesh::get_object_material_name(short mat_nr) const
{
  const Material *mat = BKE_object_material_get(export_object_eval_, mat_nr);
#ifdef DEBUG
  if (!mat) {
    std::cerr << "Material not found for mat_nr = " << mat_nr << std::endl;
  }
#endif
  return mat ? mat->id.name + 2 : nullptr;
}

/**
 * Calculate coordinates of a vertex at the given index.
 */
float3 OBJMesh::calc_vertex_coords(const uint vert_index, const float scaling_factor) const
{
  float3 r_coords;
  copy_v3_v3(r_coords, export_mesh_eval_->mvert[vert_index].co);
  mul_m4_v3(world_and_axes_transform_, r_coords);
  mul_v3_fl(r_coords, scaling_factor);
  return r_coords;
}

/**
 * Calculate vertex indices of all vertices of a polygon at the given index.
 */
void OBJMesh::calc_poly_vertex_indices(const uint poly_index,
                                       Vector<uint> &r_poly_vertex_indices) const
{
  const MPoly &mpoly = export_mesh_eval_->mpoly[poly_index];
  const MLoop *mloop = &export_mesh_eval_->mloop[mpoly.loopstart];
  for (uint loop_index = 0; loop_index < mpoly.totloop; loop_index++) {
    r_poly_vertex_indices[loop_index] = mloop[loop_index].v;
  }
}

/**
 * Fill UV vertex coordinates of an object in the given buffer. Also, store the
 * UV vertex indices in the member variable.
 */
void OBJMesh::store_uv_coords_and_indices(Vector<std::array<float, 2>> &r_uv_coords)
{
  const MPoly *mpoly = export_mesh_eval_->mpoly;
  const MLoop *mloop = export_mesh_eval_->mloop;
  const uint totpoly = export_mesh_eval_->totpoly;
  const uint totvert = export_mesh_eval_->totvert;
  const MLoopUV *mloopuv = static_cast<MLoopUV *>(
      CustomData_get_layer(&export_mesh_eval_->ldata, CD_MLOOPUV));
  if (!mloopuv) {
    tot_uv_vertices_ = 0;
    return;
  }
  const float limit[2] = {STD_UV_CONNECT_LIMIT, STD_UV_CONNECT_LIMIT};

  UvVertMap *uv_vert_map = BKE_mesh_uv_vert_map_create(
      mpoly, mloop, mloopuv, totpoly, totvert, limit, false, false);

  uv_indices_.resize(totpoly);
  /* At least total vertices of a mesh will be present in its texture map. So
   * reserve minimum space early. */
  r_uv_coords.reserve(totvert);

  tot_uv_vertices_ = 0;
  for (uint vertex_index = 0; vertex_index < totvert; vertex_index++) {
    const UvMapVert *uv_vert = BKE_mesh_uv_vert_map_get_vert(uv_vert_map, vertex_index);
    for (; uv_vert; uv_vert = uv_vert->next) {
      if (uv_vert->separate) {
        tot_uv_vertices_ += 1;
      }
      if (tot_uv_vertices_ == 0) {
        return;
      }
      const uint vertices_in_poly = (uint)mpoly[uv_vert->poly_index].totloop;

      /* Fill up UV vertex's coordinates. */
      r_uv_coords.resize(tot_uv_vertices_);
      const int loopstart = mpoly[uv_vert->poly_index].loopstart;
      const float(&vert_uv_coords)[2] = mloopuv[loopstart + uv_vert->loop_of_poly_index].uv;
      r_uv_coords[tot_uv_vertices_ - 1][0] = vert_uv_coords[0];
      r_uv_coords[tot_uv_vertices_ - 1][1] = vert_uv_coords[1];

      uv_indices_[uv_vert->poly_index].resize(vertices_in_poly);
      /* Keep indices zero-based and let the writer handle the + 1. */
      uv_indices_[uv_vert->poly_index][uv_vert->loop_of_poly_index] = tot_uv_vertices_ - 1;
    }
  }
  BKE_mesh_uv_vert_map_free(uv_vert_map);
}

/**
 * Calculate face normal of a polygon at given index.
 */
float3 OBJMesh::calc_poly_normal(const uint poly_index) const
{
  float3 r_poly_normal;
  const MPoly &poly_to_write = export_mesh_eval_->mpoly[poly_index];
  const MLoop &mloop = export_mesh_eval_->mloop[poly_to_write.loopstart];
  const MVert &mvert = *(export_mesh_eval_->mvert);
  BKE_mesh_calc_poly_normal(&poly_to_write, &mloop, &mvert, r_poly_normal);
  mul_mat3_m4_v3(world_and_axes_transform_, r_poly_normal);
  return r_poly_normal;
}

/**
 * Calculate loop normals of a polygon at the given index.
 *
 * Should be used if a polygon is shaded smooth.
 */
void OBJMesh::calc_loop_normals(const uint poly_index, Vector<float3> &r_loop_normals) const
{
  r_loop_normals.clear();
  const MPoly &mpoly = export_mesh_eval_->mpoly[poly_index];
  const float(*lnors)[3] = (const float(*)[3])(
      CustomData_get_layer(&export_mesh_eval_->ldata, CD_NORMAL));
  for (int loop_of_poly = 0; loop_of_poly < mpoly.totloop; loop_of_poly++) {
    float3 loop_normal;
    copy_v3_v3(loop_normal, lnors[mpoly.loopstart + loop_of_poly]);
    mul_mat3_m4_v3(world_and_axes_transform_, loop_normal);
    r_loop_normals.append(loop_normal);
  }
}

/**
 * Find the name of the vertex group with the maximum number of vertices in a poly.
 *
 * If no vertex belongs to any group, returned name is "off".
 * If two or more groups have the same number of vertices (maximum), group name depends on the
 * implementation of std::max_element.
 * If the group corresponding to r_last_vertex_group shows up on current polygon, return nullptr so
 * that caller can skip that group.
 *
 * \param r_last_vertex_group stores the index of the vertex group found in last iteration,
 * indexing into Object->defbase.
 */
const char *OBJMesh::get_poly_deform_group_name(const uint poly_index,
                                                short &r_last_vertex_group) const
{
  BLI_assert(poly_index < export_mesh_eval_->totpoly);
  const MPoly &mpoly = export_mesh_eval_->mpoly[poly_index];
  const MLoop *mloop = &export_mesh_eval_->mloop[mpoly.loopstart];
  const uint tot_deform_groups = BLI_listbase_count(&export_object_eval_->defbase);
  /* Indices of the vector index into deform groups of an object; values are the number of vertex
   * members in one deform group. */
  Vector<int> deform_group_members(tot_deform_groups, 0);
  /* Whether at least one vertex in the polygon belongs to any group. */
  bool found_group = false;

  const MDeformVert *dvert_orig = static_cast<MDeformVert *>(
      CustomData_get_layer(&export_mesh_eval_->vdata, CD_MDEFORMVERT));
  if (!dvert_orig) {
    return nullptr;
  }

  const MDeformWeight *curr_weight = nullptr;
  const MDeformVert *dvert = nullptr;
  for (uint loop_index = 0; loop_index < mpoly.totloop; loop_index++) {
    dvert = &dvert_orig[(mloop + loop_index)->v];
    curr_weight = dvert->dw;
    if (curr_weight) {
      bDeformGroup *vertex_group = static_cast<bDeformGroup *>(
          BLI_findlink((&export_object_eval_->defbase), curr_weight->def_nr));
      if (vertex_group) {
        deform_group_members[curr_weight->def_nr] += 1;
        found_group = true;
      }
    }
  }

  if (!found_group) {
    if (r_last_vertex_group == -1) {
      /* No vertex group found in this face, just like in the last iteration. */
      return nullptr;
    }
    /* -1 indicates deform group having no vertices in it. */
    r_last_vertex_group = -1;
    return "off";
  }

  /* Index of the group with maximum vertices. */
  short max_idx = (short)(std::max_element(deform_group_members.begin(),
                                           deform_group_members.end()) -
                          deform_group_members.begin());
  if (max_idx == r_last_vertex_group) {
    /* No need to update the name, this is the same as the group name in the last iteration. */
    return nullptr;
  }

  r_last_vertex_group = max_idx;
  const bDeformGroup &vertex_group = *(
      static_cast<bDeformGroup *>(BLI_findlink(&export_object_eval_->defbase, max_idx)));

  return vertex_group.name;
}

/**
 * Calculate vertex indices of an edge's corners if it is a loose edge.
 */
std::optional<std::array<int, 2>> OBJMesh::calc_loose_edge_vert_indices(
    const uint edge_index) const
{
  const MEdge &edge = export_mesh_eval_->medge[edge_index];
  if (edge.flag & ME_LOOSEEDGE) {
    return std::array<int, 2>{static_cast<int>(edge.v1), static_cast<int>(edge.v2)};
  }
  return {};
}
}  // namespace blender::io::obj
