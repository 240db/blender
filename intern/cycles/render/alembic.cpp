/*
 * Copyright 2011-2018 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "render/alembic.h"

#include "render/camera.h"
#include "render/curves.h"
#include "render/mesh.h"
#include "render/object.h"
#include "render/scene.h"
#include "render/shader.h"

#include "util/util_foreach.h"
#include "util/util_progress.h"
#include "util/util_transform.h"
#include "util/util_vector.h"

#ifdef WITH_ALEMBIC

using namespace Alembic::AbcGeom;

CCL_NAMESPACE_BEGIN

/* TODO(@kevindietrich): motion blur support, requires persistent data for final renders, or at
 * least a way to tell which frame data to load, so we do not load the entire archive for a few
 * frames. */

/* TODO(@kevindietrich): arrays are emptied when passed to the sockets, so for now we copy the
 * arrays to avoid reloading the data */

static float3 make_float3_from_yup(const Imath::Vec3<float> &v)
{
  return make_float3(v.x, -v.z, v.y);
}

static M44d convert_yup_zup(const M44d &mtx)
{
  V3d scale, shear, rotation, translation;
  extractSHRT(mtx, scale, shear, rotation, translation);

  M44d rot_mat, scale_mat, trans_mat;
  rot_mat.setEulerAngles(V3d(rotation.x, -rotation.z, rotation.y));
  scale_mat.setScale(V3d(scale.x, scale.z, scale.y));
  trans_mat.setTranslation(V3d(translation.x, -translation.z, translation.y));

  return scale_mat * rot_mat * trans_mat;
}

static void transform_decompose(const Imath::M44d &mat,
                                Imath::V3d &scale,
                                Imath::V3d &shear,
                                Imath::Quatd &rotation,
                                Imath::V3d &translation)
{
  Imath::M44d mat_remainder(mat);

  /* extract scale and shear */
  Imath::extractAndRemoveScalingAndShear(mat_remainder, scale, shear);

  /* extract translation */
  translation.x = mat_remainder[3][0];
  translation.y = mat_remainder[3][1];
  translation.z = mat_remainder[3][2];

  /* extract rotation */
  rotation = extractQuat(mat_remainder);
}

static M44d transform_compose(const Imath::V3d &scale,
                              const Imath::V3d &shear,
                              const Imath::Quatd &rotation,
                              const Imath::V3d &translation)
{
  Imath::M44d scale_mat, shear_mat, rot_mat, trans_mat;

  scale_mat.setScale(scale);
  shear_mat.setShear(shear);
  rot_mat = rotation.toMatrix44();
  trans_mat.setTranslation(translation);

  return scale_mat * shear_mat * rot_mat * trans_mat;
}

/* get the matrix for the specified time, or return the identity matrix if there is no exact match
 */
static M44d get_matrix_for_time(const MatrixSampleMap &samples, chrono_t time)
{
  MatrixSampleMap::const_iterator iter = samples.find(time);
  if (iter != samples.end()) {
    return iter->second;
  }

  return M44d();
}

/* get the matrix for the specified time, or interpolate between samples if there is no exact match
 */
static M44d get_interpolated_matrix_for_time(const MatrixSampleMap &samples, chrono_t time)
{
  if (samples.empty()) {
    return M44d();
  }

  /* see if exact match */
  MatrixSampleMap::const_iterator iter = samples.find(time);
  if (iter != samples.end()) {
    return iter->second;
  }

  if (samples.size() == 1) {
    return samples.begin()->second;
  }

  if (time <= samples.begin()->first) {
    return samples.begin()->second;
  }

  if (time >= samples.rbegin()->first) {
    return samples.rbegin()->second;
  }

  /* find previous and next time sample to interpolate */
  chrono_t prev_time = samples.begin()->first;
  chrono_t next_time = samples.rbegin()->first;

  for (MatrixSampleMap::const_iterator I = samples.begin(); I != samples.end(); ++I) {
    chrono_t current_time = (*I).first;

    if (current_time > prev_time && current_time <= time) {
      prev_time = current_time;
    }

    if (current_time > next_time && current_time >= time) {
      next_time = current_time;
    }
  }

  const M44d prev_mat = get_matrix_for_time(samples, prev_time);
  const M44d next_mat = get_matrix_for_time(samples, next_time);

  V3d prev_scale, next_scale;
  V3d prev_shear, next_shear;
  V3d prev_translation, next_translation;
  Quatd prev_rotation, next_rotation;

  transform_decompose(prev_mat, prev_scale, prev_shear, prev_rotation, prev_translation);
  transform_decompose(next_mat, next_scale, next_shear, next_rotation, next_translation);

  chrono_t t = (time - prev_time) / (next_time - prev_time);

  /* ensure rotation around the shortest angle  */
  if ((prev_rotation ^ next_rotation) < 0) {
    next_rotation = -next_rotation;
  }

  return transform_compose(Imath::lerp(prev_scale, next_scale, t),
                           Imath::lerp(prev_shear, next_shear, t),
                           Imath::slerp(prev_rotation, next_rotation, t),
                           Imath::lerp(prev_translation, next_translation, t));
}

static void concatenate_xform_samples(const MatrixSampleMap &parent_samples,
                                      const MatrixSampleMap &local_samples,
                                      MatrixSampleMap &output_samples)
{
  set<chrono_t> union_of_samples;

  for (const std::pair<chrono_t, M44d> pair : parent_samples) {
    union_of_samples.insert(pair.first);
  }

  for (const std::pair<chrono_t, M44d> pair : local_samples) {
    union_of_samples.insert(pair.first);
  }

  foreach (chrono_t time, union_of_samples) {
    M44d parent_matrix = get_interpolated_matrix_for_time(parent_samples, time);
    M44d local_matrix = get_interpolated_matrix_for_time(local_samples, time);

    output_samples[time] = local_matrix * parent_matrix;
  }
}

static Transform make_transform(const M44d &a)
{
  M44d m = convert_yup_zup(a);
  Transform trans;
  for (int j = 0; j < 3; j++) {
    for (int i = 0; i < 4; i++) {
      trans[j][i] = static_cast<float>(m[i][j]);
    }
  }
  return trans;
}

static void read_default_uvs(const IV2fGeomParam &uvs, CachedData &cached_data)
{
  CachedData::CachedAttribute &attr = cached_data.add_attribute(ustring(uvs.getName()));

  for (size_t i = 0; i < uvs.getNumSamples(); ++i) {
    const ISampleSelector iss = ISampleSelector(index_t(i));
    const IV2fGeomParam::Sample sample = uvs.getExpandedValue(iss);

    const double time = uvs.getTimeSampling()->getSampleTime(index_t(i));

    switch (uvs.getScope()) {
      case kFacevaryingScope: {
        const IV2fGeomParam::Sample uvsample = uvs.getIndexedValue(iss);

        if (!uvsample.valid()) {
          continue;
        }

        const array<int3> *triangles = cached_data.triangles.data_for_time_no_check(time);
        const array<int3> *triangles_loops = cached_data.triangles_loops.data_for_time_no_check(
            time);

        if (!triangles || !triangles_loops) {
          continue;
        }

        attr.std = ATTR_STD_UV;

        array<char> data;
        data.resize(triangles->size() * 3 * sizeof(float2));

        float2 *data_float2 = reinterpret_cast<float2 *>(data.data());

        const unsigned int *indices = uvsample.getIndices()->get();
        const V2f *values = uvsample.getVals()->get();

        for (const int3 &loop : *triangles_loops) {
          unsigned int v0 = indices[loop.x];
          unsigned int v1 = indices[loop.y];
          unsigned int v2 = indices[loop.z];

          data_float2[0] = make_float2(values[v0][0], values[v0][1]);
          data_float2[1] = make_float2(values[v1][0], values[v1][1]);
          data_float2[2] = make_float2(values[v2][0], values[v2][1]);
          data_float2 += 3;
        }

        attr.data.add_data(data, time);

        break;
      }
      default: {
        // not supported
        break;
      }
    }
  }
}

static void add_normals(const Int32ArraySamplePtr face_indices,
                        const IN3fGeomParam &normals,
                        double time,
                        CachedData &cached_data)
{

  switch (normals.getScope()) {
    case kFacevaryingScope: {
      const ISampleSelector iss = ISampleSelector(time);
      const IN3fGeomParam::Sample sample = normals.getExpandedValue(iss);

      if (!sample.valid()) {
        return;
      }

      CachedData::CachedAttribute &attr = cached_data.add_attribute(ustring(normals.getName()));
      attr.std = ATTR_STD_VERTEX_NORMAL;

      const array<float3> *vertices = cached_data.vertices.data_for_time_no_check(time);

      if (!vertices) {
        return;
      }

      array<char> data;
      data.resize(vertices->size() * sizeof(float3));

      float3 *data_float3 = reinterpret_cast<float3 *>(data.data());

      const int *face_indices_array = face_indices->get();
      const N3fArraySamplePtr values = sample.getVals();

      for (size_t i = 0; i < face_indices->size(); ++i) {
        int point_index = face_indices_array[i];
        /* polygon winding order in Alembic follows the RenderMan convention, which is the
         * reverse of Cycle, so the normal has to be flipped
         * this code also assumes that the vertices of each polygon corresponding to the same
         * point have the same normal (that we do not have split normals, which is not
         * supported in Cycles anyway) */
        data_float3[point_index] = -make_float3_from_yup(values->get()[i]);
      }

      attr.data.add_data(data, time);
      break;
    }
    case kVaryingScope:
    case kVertexScope: {
      const ISampleSelector iss = ISampleSelector(time);
      const IN3fGeomParam::Sample sample = normals.getExpandedValue(iss);

      if (!sample.valid()) {
        return;
      }

      CachedData::CachedAttribute &attr = cached_data.add_attribute(ustring(normals.getName()));
      attr.std = ATTR_STD_VERTEX_NORMAL;

      const array<float3> *vertices = cached_data.vertices.data_for_time_no_check(time);

      if (!vertices) {
        return;
      }

      array<char> data;
      data.resize(vertices->size() * sizeof(float3));

      float3 *data_float3 = reinterpret_cast<float3 *>(data.data());

      const Imath::V3f *values = sample.getVals()->get();

      for (size_t i = 0; i < vertices->size(); ++i) {
        data_float3[i] = make_float3_from_yup(values[i]);
      }

      attr.data.add_data(data, time);

      break;
    }
    default: {
      break;
    }
  }
}

static void add_positions(const P3fArraySamplePtr positions, double time, CachedData &cached_data)
{
  if (!positions) {
    return;
  }

  array<float3> vertices;
  vertices.reserve(positions->size());

  for (size_t i = 0; i < positions->size(); i++) {
    Imath::Vec3<float> f = positions->get()[i];
    vertices.push_back_reserved(make_float3_from_yup(f));
  }

  cached_data.vertices.add_data(vertices, time);
}

static void add_triangles(const Int32ArraySamplePtr face_counts,
                          const Int32ArraySamplePtr face_indices,
                          double time,
                          CachedData &cached_data,
                          const array<int> &polygon_to_shader)
{
  if (!face_counts || !face_indices) {
    return;
  }

  const size_t num_faces = face_counts->size();
  const int *face_counts_array = face_counts->get();
  const int *face_indices_array = face_indices->get();

  size_t num_triangles = 0;
  for (size_t i = 0; i < face_counts->size(); i++) {
    num_triangles += face_counts_array[i] - 2;
  }

  array<int> shader;
  array<int3> triangles;
  array<int3> triangles_loops;
  shader.reserve(num_triangles);
  triangles.reserve(num_triangles);
  triangles_loops.reserve(num_triangles);
  int index_offset = 0;

  for (size_t i = 0; i < num_faces; i++) {
    int current_shader = 0;

    if (!polygon_to_shader.empty()) {
      current_shader = polygon_to_shader[i];
    }

    for (int j = 0; j < face_counts_array[i] - 2; j++) {
      int v0 = face_indices_array[index_offset];
      int v1 = face_indices_array[index_offset + j + 1];
      int v2 = face_indices_array[index_offset + j + 2];

      shader.push_back_reserved(current_shader);
      triangles.push_back_reserved(make_int3(v0, v1, v2));
      triangles_loops.push_back_reserved(
          make_int3(index_offset, index_offset + j + 1, index_offset + j + 2));
    }

    index_offset += face_counts_array[i];
  }

  cached_data.triangles.add_data(triangles, time);
  cached_data.triangles_loops.add_data(triangles_loops, time);
  cached_data.shader.add_data(shader, time);
}

NODE_DEFINE(AlembicObject)
{
  NodeType *type = NodeType::add("alembic_object", create);
  SOCKET_STRING(path, "Alembic Path", ustring());
  SOCKET_NODE_ARRAY(used_shaders, "Used Shaders", &Shader::node_type);

  return type;
}

AlembicObject::AlembicObject() : Node(node_type)
{
}

AlembicObject::~AlembicObject()
{
}

void AlembicObject::set_object(Object *object_)
{
  object = object_;
}

Object *AlembicObject::get_object()
{
  return object;
}

bool AlembicObject::has_data_loaded() const
{
  return data_loaded;
}

void AlembicObject::read_face_sets(IPolyMeshSchema &schema, array<int> &polygon_to_shader)
{
  /* TODO(@kevindietrich) at the moment this is only supported for meshes whose topology remains
   * constant (with possible vertex animation) */
  if (schema.getTopologyVariance() == kHeterogenousTopology) {
    return;
  }

  std::vector<std::string> face_sets;
  schema.getFaceSetNames(face_sets);

  if (face_sets.empty()) {
    return;
  }

  const Int32ArraySamplePtr face_counts = schema.getFaceCountsProperty().getValue();

  polygon_to_shader.resize(face_counts->size());

  foreach (const std::string &face_set_name, face_sets) {
    int shader_index = 0;

    foreach (Node *node, get_used_shaders()) {
      if (node->name == face_set_name) {
        break;
      }

      ++shader_index;
    }

    if (shader_index >= get_used_shaders().size()) {
      /* use the first shader instead if none was found */
      shader_index = 0;
    }

    const IFaceSet face_set = schema.getFaceSet(face_set_name);

    if (!face_set.valid()) {
      continue;
    }

    const IFaceSetSchema face_schem = face_set.getSchema();
    const IFaceSetSchema::Sample face_sample = face_schem.getValue(ISampleSelector(index_t(0)));
    const Int32ArraySamplePtr group_faces = face_sample.getFaces();
    const size_t num_group_faces = group_faces->size();

    for (size_t l = 0; l < num_group_faces; l++) {
      size_t pos = (*group_faces)[l];

      if (pos >= polygon_to_shader.size()) {
        continue;
      }

      polygon_to_shader[pos] = shader_index;
    }
  }
}

void AlembicObject::load_all_data(IPolyMeshSchema &schema, Progress &progress)
{
  cached_data.clear();

  AttributeRequestSet requested_attributes = get_requested_attributes();

  const TimeSamplingPtr time_sampling = schema.getTimeSampling();
  cached_data.vertices.set_time_sampling(*time_sampling);
  cached_data.triangles.set_time_sampling(*time_sampling);
  cached_data.triangles_loops.set_time_sampling(*time_sampling);

  /* start by reading the face sets (per face shader), as we directly split polygons to triangles
   */
  array<int> polygon_to_shader;
  read_face_sets(schema, polygon_to_shader);

  const IN3fGeomParam &normals = schema.getNormalsParam();

  /* read topology */
  for (size_t i = 0; i < schema.getNumSamples(); ++i) {
    if (progress.get_cancel()) {
      return;
    }

    const ISampleSelector iss = ISampleSelector(static_cast<index_t>(i));
    const IPolyMeshSchema::Sample sample = schema.getValue(iss);

    const double time = time_sampling->getSampleTime(static_cast<index_t>(i));

    add_positions(sample.getPositions(), time, cached_data);

    add_triangles(
        sample.getFaceCounts(), sample.getFaceIndices(), time, cached_data, polygon_to_shader);

    if (normals.valid()) {
      add_normals(sample.getFaceIndices(), normals, time, cached_data);
    }

    foreach (const AttributeRequest &attr, requested_attributes.requests) {
      read_attribute(schema.getArbGeomParams(), iss, attr.name);
    }
  }

  if (progress.get_cancel()) {
    return;
  }

  const IV2fGeomParam &uvs = schema.getUVsParam();

  if (uvs.valid()) {
    read_default_uvs(uvs, cached_data);
  }

  if (progress.get_cancel()) {
    return;
  }

  setup_transform_cache();

  data_loaded = true;
}

void AlembicObject::load_all_data(const ICurvesSchema &schema,
                                  Progress &progress,
                                  float default_radius)
{
  cached_data.clear();

  const TimeSamplingPtr time_sampling = schema.getTimeSampling();
  cached_data.curve_keys.set_time_sampling(*time_sampling);
  cached_data.curve_radius.set_time_sampling(*time_sampling);
  cached_data.curve_first_key.set_time_sampling(*time_sampling);
  cached_data.curve_shader.set_time_sampling(*time_sampling);

  for (size_t i = 0; i < schema.getNumSamples(); ++i) {
    if (progress.get_cancel()) {
      return;
    }

    const ISampleSelector iss = ISampleSelector(index_t(i));
    const ICurvesSchema::Sample sample = schema.getValue(iss);

    const double time = time_sampling->getSampleTime(index_t(i));

    const Int32ArraySamplePtr curves_num_vertices = sample.getCurvesNumVertices();
    const P3fArraySamplePtr position = sample.getPositions();

    const IFloatGeomParam widths_param = schema.getWidthsParam();
    FloatArraySamplePtr radiuses;

    if (widths_param.valid()) {
      IFloatGeomParam::Sample wsample = widths_param.getExpandedValue(iss);
      radiuses = wsample.getVals();
    }

    const bool do_radius = (radiuses != nullptr) && (radiuses->size() > 1);
    float radius = (radiuses && radiuses->size() == 1) ? (*radiuses)[0] : default_radius;

    array<float3> curve_keys;
    array<float> curve_radius;
    array<int> curve_first_key;
    array<int> curve_shader;

    curve_keys.reserve(position->size());
    curve_radius.reserve(position->size());
    curve_first_key.reserve(curves_num_vertices->size());
    curve_shader.reserve(curves_num_vertices->size());

    int offset = 0;
    for (size_t i = 0; i < curves_num_vertices->size(); i++) {
      const int num_vertices = curves_num_vertices->get()[i];

      for (int j = 0; j < num_vertices; j++) {
        const V3f &f = position->get()[offset + j];
        curve_keys.push_back_reserved(make_float3_from_yup(f));

        if (do_radius) {
          radius = (*radiuses)[offset + j];
        }

        curve_radius.push_back_reserved(radius);
      }

      curve_first_key.push_back_reserved(offset);
      curve_shader.push_back_reserved(0);

      offset += num_vertices;
    }

    cached_data.curve_keys.add_data(curve_keys, time);
    cached_data.curve_radius.add_data(curve_radius, time);
    cached_data.curve_first_key.add_data(curve_first_key, time);
    cached_data.curve_shader.add_data(curve_shader, time);
  }

  // TODO(@kevindietrich): attributes, but I need example files

  setup_transform_cache();

  data_loaded = true;
}

void AlembicObject::setup_transform_cache()
{
  if (xform_samples.size() == 0) {
    Transform tfm = transform_identity();
    cached_data.transforms.add_data(tfm, 0.0);
  }
  else {
    /* It is possible for a leaf node of the hierarchy to have multiple samples for its transforms
     * if a sibling has animated transforms. So check if we indeed have animated transformations.
     */
    M44d first_matrix = xform_samples.begin()->first;
    bool has_animation = false;
    for (const std::pair<chrono_t, M44d> pair : xform_samples) {
      if (pair.second != first_matrix) {
        has_animation = true;
        break;
      }
    }

    if (!has_animation) {
      Transform tfm = make_transform(first_matrix);
      cached_data.transforms.add_data(tfm, 0.0);
    }
    else {
      for (const std::pair<chrono_t, M44d> pair : xform_samples) {
        Transform tfm = make_transform(pair.second);
        cached_data.transforms.add_data(tfm, pair.first);
      }
    }
  }

  // TODO(@kevindietrich) : proper time sampling, but is it possible for the hierarchy to have
  // different time sampling for each xform ?
  cached_data.transforms.set_time_sampling(cached_data.vertices.get_time_sampling());
}

AttributeRequestSet AlembicObject::get_requested_attributes()
{
  AttributeRequestSet requested_attributes;

  Geometry *geometry = object->get_geometry();
  assert(geometry);

  // TODO(@kevindietrich) : check for attribute changes in the shaders
  foreach (Node *node, geometry->get_used_shaders()) {
    Shader *shader = static_cast<Shader *>(node);

    foreach (const AttributeRequest &attr, shader->attributes.requests) {
      if (attr.name != "") {
        requested_attributes.add(attr.name);
      }
    }
  }

  return requested_attributes;
}

void AlembicObject::read_attribute(const ICompoundProperty &arb_geom_params,
                                   const ISampleSelector &iss,
                                   const ustring &attr_name)
{
  const index_t index = iss.getRequestedIndex();
  CachedData::CachedAttribute &attribute = cached_data.add_attribute(attr_name);

  for (size_t i = 0; i < arb_geom_params.getNumProperties(); ++i) {
    const PropertyHeader &prop = arb_geom_params.getPropertyHeader(i);

    if (prop.getName() != attr_name) {
      continue;
    }

    if (IV2fProperty::matches(prop.getMetaData()) && Alembic::AbcGeom::isUV(prop)) {
      const IV2fGeomParam &param = IV2fGeomParam(arb_geom_params, prop.getName());

      IV2fGeomParam::Sample sample;
      param.getIndexed(sample, iss);

      const chrono_t time = param.getTimeSampling()->getSampleTime(index);

      if (param.getScope() == kFacevaryingScope) {
        V2fArraySamplePtr values = sample.getVals();
        UInt32ArraySamplePtr indices = sample.getIndices();

        attribute.std = ATTR_STD_NONE;
        attribute.element = ATTR_ELEMENT_CORNER;
        attribute.type_desc = TypeFloat2;

        const array<int3> *triangles = cached_data.triangles.data_for_time_no_check(time);
        const array<int3> *triangles_loops = cached_data.triangles_loops.data_for_time_no_check(
            time);

        if (!triangles || !triangles_loops) {
          continue;
        }

        array<char> data;
        data.resize(triangles->size() * 3 * sizeof(float2));

        float2 *data_float2 = reinterpret_cast<float2 *>(data.data());

        for (const int3 &loop : *triangles_loops) {
          unsigned int v0 = (*indices)[loop.x];
          unsigned int v1 = (*indices)[loop.y];
          unsigned int v2 = (*indices)[loop.z];

          data_float2[0] = make_float2((*values)[v0][0], (*values)[v0][1]);
          data_float2[1] = make_float2((*values)[v1][0], (*values)[v1][1]);
          data_float2[2] = make_float2((*values)[v2][0], (*values)[v2][1]);
          data_float2 += 3;
        }

        attribute.data.set_time_sampling(*param.getTimeSampling());
        attribute.data.add_data(data, time);
      }
    }
    else if (IC3fProperty::matches(prop.getMetaData())) {
      const IC3fGeomParam &param = IC3fGeomParam(arb_geom_params, prop.getName());

      IC3fGeomParam::Sample sample;
      param.getIndexed(sample, iss);

      const chrono_t time = param.getTimeSampling()->getSampleTime(index);

      C3fArraySamplePtr values = sample.getVals();

      attribute.std = ATTR_STD_NONE;

      if (param.getScope() == kVaryingScope) {
        attribute.element = ATTR_ELEMENT_CORNER_BYTE;
        attribute.type_desc = TypeRGBA;

        const array<int3> *triangles = cached_data.triangles.data_for_time_no_check(time);

        if (!triangles) {
          continue;
        }

        array<char> data;
        data.resize(triangles->size() * 3 * sizeof(uchar4));

        uchar4 *data_uchar4 = reinterpret_cast<uchar4 *>(data.data());

        int offset = 0;
        for (const int3 &tri : *triangles) {
          Imath::C3f v = (*values)[tri.x];
          data_uchar4[offset + 0] = color_float_to_byte(make_float3(v.x, v.y, v.z));

          v = (*values)[tri.y];
          data_uchar4[offset + 1] = color_float_to_byte(make_float3(v.x, v.y, v.z));

          v = (*values)[tri.z];
          data_uchar4[offset + 2] = color_float_to_byte(make_float3(v.x, v.y, v.z));

          offset += 3;
        }

        attribute.data.set_time_sampling(*param.getTimeSampling());
        attribute.data.add_data(data, time);
      }
    }
    else if (IC4fProperty::matches(prop.getMetaData())) {
      const IC4fGeomParam &param = IC4fGeomParam(arb_geom_params, prop.getName());

      IC4fGeomParam::Sample sample;
      param.getIndexed(sample, iss);

      const chrono_t time = param.getTimeSampling()->getSampleTime(index);

      C4fArraySamplePtr values = sample.getVals();

      attribute.std = ATTR_STD_NONE;

      if (param.getScope() == kVaryingScope) {
        attribute.element = ATTR_ELEMENT_CORNER_BYTE;
        attribute.type_desc = TypeRGBA;

        const array<int3> *triangles = cached_data.triangles.data_for_time_no_check(time);

        if (!triangles) {
          continue;
        }

        array<char> data;
        data.resize(triangles->size() * 3 * sizeof(uchar4));

        uchar4 *data_uchar4 = reinterpret_cast<uchar4 *>(data.data());

        int offset = 0;
        for (const int3 &tri : *triangles) {
          Imath::C4f v = (*values)[tri.x];
          data_uchar4[offset + 0] = color_float4_to_uchar4(make_float4(v.r, v.g, v.b, v.a));

          v = (*values)[tri.y];
          data_uchar4[offset + 1] = color_float4_to_uchar4(make_float4(v.r, v.g, v.b, v.a));

          v = (*values)[tri.z];
          data_uchar4[offset + 2] = color_float4_to_uchar4(make_float4(v.r, v.g, v.b, v.a));

          offset += 3;
        }

        attribute.data.set_time_sampling(*param.getTimeSampling());
        attribute.data.add_data(data, time);
      }
    }
  }
}

NODE_DEFINE(AlembicProcedural)
{
  NodeType *type = NodeType::add("alembic", create);

  SOCKET_STRING(filepath, "Filename", ustring());
  SOCKET_FLOAT(frame, "Frame", 1.0f);
  SOCKET_FLOAT(frame_rate, "Frame Rate", 24.0f);
  SOCKET_FLOAT(default_curves_radius, "Default Curve Radius", 0.01f);

  SOCKET_NODE_ARRAY(objects, "Objects", &AlembicObject::node_type);

  return type;
}

AlembicProcedural::AlembicProcedural() : Procedural(node_type)
{
  frame = 1.0f;
  frame_rate = 24.0f;
  default_curves_radius = 0.01f;
}

AlembicProcedural::~AlembicProcedural()
{
  for (size_t i = 0; i < objects.size(); ++i) {
    delete objects[i];
  }
}

void AlembicProcedural::generate(Scene *scene, Progress &progress)
{
  if (!is_modified()) {
    return;
  }

  if (!archive.valid()) {
    Alembic::AbcCoreFactory::IFactory factory;
    factory.setPolicy(Alembic::Abc::ErrorHandler::kQuietNoopPolicy);
    archive = factory.getArchive(filepath.c_str());

    if (!archive.valid()) {
      /* avoid potential infinite update loops in viewport synchronization */
      filepath.clear();
      clear_modified();
      return;
    }
  }

  if (!objects_loaded || objects_is_modified()) {
    load_objects(progress);
    objects_loaded = true;
  }

  const chrono_t frame_time = (chrono_t)(frame / frame_rate);

  foreach (Node *node, objects) {
    AlembicObject *object = static_cast<AlembicObject *>(node);

    if (progress.get_cancel()) {
      return;
    }

    /* skip constant objects */
    if (object->has_data_loaded() && object->is_constant()) {
      continue;
    }

    if (IPolyMesh::matches(object->iobject.getHeader())) {
      read_mesh(scene, object, frame_time, progress);
    }
    else if (ICurves::matches(object->iobject.getHeader())) {
      read_curves(scene, object, frame_time, progress);
    }
  }

  clear_modified();
}

void AlembicProcedural::add_object(AlembicObject *object)
{
  objects.push_back_slow(object);
  tag_objects_modified();
}

void AlembicProcedural::tag_update(Scene *scene)
{
  if (is_modified()) {
    scene->procedural_manager->tag_update();
  }
}

void AlembicProcedural::load_objects(Progress &progress)
{
  unordered_map<string, AlembicObject *> object_map;

  foreach (Node *node, objects) {
    AlembicObject *object = static_cast<AlembicObject *>(node);

    /* only consider newly added objects */
    if (object->get_object() == nullptr) {
      object_map.insert({object->get_path().c_str(), object});
    }
  }

  IObject root = archive.getTop();

  for (size_t i = 0; i < root.getNumChildren(); ++i) {
    walk_hierarchy(root, root.getChildHeader(i), nullptr, object_map, progress);
  }
}

void AlembicProcedural::read_mesh(Scene *scene,
                                  AlembicObject *abc_object,
                                  Abc::chrono_t frame_time,
                                  Progress &progress)
{
  IPolyMesh polymesh(abc_object->iobject, Alembic::Abc::kWrapExisting);

  Mesh *mesh = nullptr;

  /* create a mesh node in the scene if not already done */
  if (!abc_object->get_object()) {
    mesh = scene->create_node<Mesh>();
    mesh->name = abc_object->iobject.getName();

    array<Node *> used_shaders = abc_object->get_used_shaders();
    mesh->set_used_shaders(used_shaders);

    /* create object*/
    Object *object = scene->create_node<Object>();
    object->set_geometry(mesh);
    object->set_tfm(abc_object->xform);
    object->name = abc_object->iobject.getName();

    abc_object->set_object(object);
  }
  else {
    mesh = static_cast<Mesh *>(abc_object->get_object()->get_geometry());
  }

  IPolyMeshSchema schema = polymesh.getSchema();

  if (!abc_object->has_data_loaded()) {
    abc_object->load_all_data(schema, progress);
  }

  CachedData &cached_data = abc_object->get_cached_data();

  Transform *tfm = cached_data.transforms.data_for_time(frame_time);
  if (tfm) {
    Object *object = abc_object->get_object();
    object->set_tfm(*tfm);
  }

  array<float3> *vertices = cached_data.vertices.data_for_time(frame_time);
  if (vertices) {
    array<float3> new_vertices = *vertices;
    mesh->set_verts(new_vertices);
  }

  array<int3> *triangle_data = cached_data.triangles.data_for_time(frame_time);
  if (triangle_data) {
    array<int> triangles;
    array<bool> smooth;

    triangles.reserve(triangle_data->size() * 3);
    smooth.reserve(triangle_data->size());

    for (size_t i = 0; i < triangle_data->size(); ++i) {
      int3 tri = (*triangle_data)[i];
      triangles.push_back_reserved(tri.x);
      triangles.push_back_reserved(tri.y);
      triangles.push_back_reserved(tri.z);
      smooth.push_back_reserved(1);
    }

    mesh->set_triangles(triangles);
    mesh->set_smooth(smooth);
  }

  array<int> *shader = cached_data.shader.data_for_time(frame_time);
  if (shader) {
    array<int> new_shader = *shader;
    mesh->set_shader(new_shader);
  }

  for (CachedData::CachedAttribute &attribute : cached_data.attributes) {
    const array<char> *attr_data = attribute.data.data_for_time(frame_time);

    if (!attr_data) {
      continue;
    }

    Attribute *attr = nullptr;
    if (attribute.std != ATTR_STD_NONE) {
      attr = mesh->attributes.add(attribute.std, attribute.name);
    }
    else {
      attr = mesh->attributes.add(attribute.name, attribute.type_desc, attribute.element);
    }
    assert(attr);

    attr->modified = true;
    memcpy(attr->data(), attr_data->data(), attr_data->size());
  }

  /* we don't yet support arbitrary attributes, for now add vertex
   * coordinates as generated coordinates if requested */
  if (mesh->need_attribute(scene, ATTR_STD_GENERATED)) {
    Attribute *attr = mesh->attributes.add(ATTR_STD_GENERATED);
    memcpy(
        attr->data_float3(), mesh->get_verts().data(), sizeof(float3) * mesh->get_verts().size());
  }

  if (mesh->is_modified()) {
    bool need_rebuild = mesh->triangles_is_modified();
    mesh->tag_update(scene, need_rebuild);
  }
}

void AlembicProcedural::read_curves(Scene *scene,
                                    AlembicObject *abc_object,
                                    Abc::chrono_t frame_time,
                                    Progress &progress)
{
  ICurves curves(abc_object->iobject, Alembic::Abc::kWrapExisting);
  Hair *hair;

  /* create a hair node in the scene if not already done */
  if (!abc_object->get_object()) {
    hair = scene->create_node<Hair>();
    hair->name = abc_object->iobject.getName();

    array<Node *> used_shaders = abc_object->get_used_shaders();
    hair->set_used_shaders(used_shaders);

    /* create object*/
    Object *object = scene->create_node<Object>();
    object->set_geometry(hair);
    object->set_tfm(abc_object->xform);
    object->name = abc_object->iobject.getName();

    abc_object->set_object(object);
  }
  else {
    hair = static_cast<Hair *>(abc_object->get_object()->get_geometry());
  }

  if (!abc_object->has_data_loaded() || default_curves_radius_is_modified()) {
    ICurvesSchema schema = curves.getSchema();
    abc_object->load_all_data(schema, progress, default_curves_radius);
  }

  CachedData &cached_data = abc_object->get_cached_data();

  Transform *tfm = cached_data.transforms.data_for_time(frame_time);
  if (tfm) {
    Object *object = abc_object->get_object();
    object->set_tfm(*tfm);
  }

  array<float3> *curve_keys = cached_data.curve_keys.data_for_time(frame_time);
  if (curve_keys) {
    array<float3> new_curve_keys = *curve_keys;
    hair->set_curve_keys(new_curve_keys);
  }

  array<float> *curve_radius = cached_data.curve_radius.data_for_time(frame_time);
  if (curve_radius) {
    array<float> new_curve_radius = *curve_radius;
    hair->set_curve_radius(new_curve_radius);
  }

  array<int> *curve_first_key = cached_data.curve_first_key.data_for_time(frame_time);
  if (curve_first_key) {
    array<int> new_curve_first_key = *curve_first_key;
    hair->set_curve_first_key(new_curve_first_key);
  }

  array<int> *curve_shader = cached_data.curve_shader.data_for_time(frame_time);
  if (curve_shader) {
    array<int> new_curve_shader = *curve_shader;
    hair->set_curve_shader(new_curve_shader);
  }

  for (CachedData::CachedAttribute &attribute : cached_data.attributes) {
    const array<char> *attr_data = attribute.data.data_for_time(frame_time);

    if (!attr_data) {
      continue;
    }

    Attribute *attr = nullptr;
    if (attribute.std != ATTR_STD_NONE) {
      attr = hair->attributes.add(attribute.std, attribute.name);
    }
    else {
      attr = hair->attributes.add(attribute.name, attribute.type_desc, attribute.element);
    }
    assert(attr);

    memcpy(attr->data(), attr_data->data(), attr_data->size());
  }

  /* we don't yet support arbitrary attributes, for now add first keys as generated coordinates if
   * requested */
  if (hair->need_attribute(scene, ATTR_STD_GENERATED)) {
    Attribute *attr_generated = hair->attributes.add(ATTR_STD_GENERATED);
    float3 *generated = attr_generated->data_float3();

    for (size_t i = 0; i < hair->num_curves(); i++) {
      generated[i] = hair->get_curve_keys()[hair->get_curve(i).first_key];
    }
  }

  const bool rebuild = (hair->curve_keys_is_modified() || hair->curve_radius_is_modified());
  hair->tag_update(scene, rebuild);
}

void AlembicProcedural::walk_hierarchy(
    IObject parent,
    const ObjectHeader &header,
    MatrixSampleMap *xform_samples,
    const unordered_map<std::string, AlembicObject *> &object_map,
    Progress &progress)
{
  if (progress.get_cancel()) {
    return;
  }

  IObject next_object;

  MatrixSampleMap concatenated_xform_samples;

  if (IXform::matches(header)) {
    IXform xform(parent, header.getName());

    IXformSchema &xs = xform.getSchema();

    if (xs.getNumOps() > 0) {
      TimeSamplingPtr ts = xs.getTimeSampling();
      MatrixSampleMap local_xform_samples;

      MatrixSampleMap *temp_xform_samples = nullptr;
      if (xform_samples == nullptr) {
        /* If there is no parent transforms, fill the map directly. */
        temp_xform_samples = &concatenated_xform_samples;
      }
      else {
        /* use a temporary map */
        temp_xform_samples = &local_xform_samples;
      }

      for (size_t i = 0; i < xs.getNumSamples(); ++i) {
        chrono_t sample_time = ts->getSampleTime(index_t(i));
        XformSample sample = xs.getValue(ISampleSelector(sample_time));
        temp_xform_samples->insert({sample_time, sample.getMatrix()});
      }

      if (xform_samples != nullptr) {
        concatenate_xform_samples(*xform_samples, local_xform_samples, concatenated_xform_samples);
      }

      xform_samples = &concatenated_xform_samples;
    }

    next_object = xform;
  }
  else if (ISubD::matches(header)) {
    // TODO(@kevindietrich): we could support reading SubD objects, but we would need a way to set
    // the dicing parameters
  }
  else if (IPolyMesh::matches(header)) {
    IPolyMesh mesh(parent, header.getName());

    unordered_map<std::string, AlembicObject *>::const_iterator iter;
    iter = object_map.find(mesh.getFullName());

    if (iter != object_map.end()) {
      AlembicObject *abc_object = iter->second;
      abc_object->iobject = mesh;

      if (xform_samples) {
        abc_object->xform_samples = *xform_samples;
      }
    }

    next_object = mesh;
  }
  else if (ICurves::matches(header)) {
    ICurves curves(parent, header.getName());

    unordered_map<std::string, AlembicObject *>::const_iterator iter;
    iter = object_map.find(curves.getFullName());

    if (iter != object_map.end()) {
      AlembicObject *abc_object = iter->second;
      abc_object->iobject = curves;

      if (xform_samples) {
        abc_object->xform_samples = *xform_samples;
      }
    }

    next_object = curves;
  }
  else if (IFaceSet::matches(header)) {
    // ignore the face set, it will be read along with the data
  }
  else {
    // unsupported type for now (Points, NuPatch)
    next_object = parent.getChild(header.getName());
  }

  if (next_object.valid()) {
    for (size_t i = 0; i < next_object.getNumChildren(); ++i) {
      walk_hierarchy(
          next_object, next_object.getChildHeader(i), xform_samples, object_map, progress);
    }
  }
}

CCL_NAMESPACE_END

#endif
