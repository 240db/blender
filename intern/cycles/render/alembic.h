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

#pragma once

#include "graph/node.h"
#include "render/attribute.h"
#include "render/procedural.h"
#include "util/util_set.h"
#include "util/util_transform.h"
#include "util/util_vector.h"

#ifdef WITH_ALEMBIC

#  include <Alembic/AbcCoreFactory/All.h>
#  include <Alembic/AbcGeom/All.h>

CCL_NAMESPACE_BEGIN

class Geometry;
class Object;
class Progress;
class Shader;

using MatrixSampleMap = std::map<Alembic::Abc::chrono_t, Alembic::Abc::M44d>;

/* Helpers to detect if some type is a ccl::array. */
template<typename> struct is_array : public std::false_type {
};

template<typename T> struct is_array<array<T>> : public std::true_type {
};

/* Store the data set for an animation at every time points, or at the begining of the animation
 * for constant data.
 *
 * The data is supposed to be stored in chronological order, and is looked up using the current
 * animation time in seconds using the TimeSampling from the Alembic property. */
template<typename T> class DataStore {
  struct DataTimePair {
    double time = 0;
    T data{};
  };

  vector<DataTimePair> data{};
  Alembic::AbcCoreAbstract::TimeSampling time_sampling{};

  double last_loaded_time = std::numeric_limits<double>::max();

 public:
  void set_time_sampling(Alembic::AbcCoreAbstract::TimeSampling time_sampling_)
  {
    time_sampling = time_sampling_;
  }

  Alembic::AbcCoreAbstract::TimeSampling get_time_sampling() const
  {
    return time_sampling;
  }

  /* Get the data for the specified time.
   * Return nullptr if there is no data or if the data for this time was already loaded. */
  T *data_for_time(double time)
  {
    if (size() == 0) {
      return nullptr;
    }

    std::pair<size_t, Alembic::Abc::chrono_t> index_pair;
    index_pair = time_sampling.getNearIndex(time, data.size());
    DataTimePair &data_pair = data[index_pair.first];

    if (last_loaded_time == data_pair.time) {
      return nullptr;
    }

    last_loaded_time = data_pair.time;

    return &data_pair.data;
  }

  /* get the data for the specified time, but do not check if the data was already loaded for this
   * time return nullptr if there is no data */
  T *data_for_time_no_check(double time)
  {
    if (size() == 0) {
      return nullptr;
    }

    std::pair<size_t, Alembic::Abc::chrono_t> index_pair;
    index_pair = time_sampling.getNearIndex(time, data.size());
    DataTimePair &data_pair = data[index_pair.first];
    return &data_pair.data;
  }

  void add_data(T &data_, double time)
  {
    if constexpr (is_array<T>::value) {
      data.emplace_back();
      data.back().data.steal_data(data_);
      data.back().time = time;
      return;
    }

    data.push_back({time, data_});
  }

  bool is_constant() const
  {
    return data.size() <= 1;
  }

  size_t size() const
  {
    return data.size();
  }

  void clear()
  {
    last_loaded_time = std::numeric_limits<double>::max();
    data.clear();
  }
};

/* Actual cache for the stored data.
 * This caches the topological, transformation, and attribute data for a Mesh node or a Hair node
 * inside of DataStores.
 */
struct CachedData {
  DataStore<Transform> transforms{};

  /* mesh data */
  DataStore<array<float3>> vertices;
  DataStore<array<int3>> triangles{};
  /* triangle "loops" are the polygons' vertices indices used for indexing face varying attributes
   * (like UVs) */
  DataStore<array<int3>> triangles_loops{};
  DataStore<array<int>> shader{};

  /* hair data */
  DataStore<array<float3>> curve_keys;
  DataStore<array<float>> curve_radius;
  DataStore<array<int>> curve_first_key;
  DataStore<array<int>> curve_shader;

  struct CachedAttribute {
    AttributeStandard std;
    AttributeElement element;
    TypeDesc type_desc;
    ustring name;
    DataStore<array<char>> data{};
  };

  vector<CachedAttribute> attributes{};

  void clear()
  {
    vertices.clear();
    triangles.clear();
    triangles_loops.clear();
    transforms.clear();
    attributes.clear();
    curve_keys.clear();
    curve_radius.clear();
    curve_first_key.clear();
    curve_shader.clear();
    shader.clear();
  }

  CachedAttribute &add_attribute(const ustring &name,
                                 const Alembic::Abc::TimeSampling &time_sampling)
  {
    for (auto &attr : attributes) {
      if (attr.name == name) {
        return attr;
      }
    }

    auto &attr = attributes.emplace_back();
    attr.name = name;
    attr.data.set_time_sampling(time_sampling);
    return attr;
  }

  bool is_constant() const
  {
    if (!vertices.is_constant()) {
      return false;
    }

    if (!triangles.is_constant()) {
      return false;
    }

    if (!transforms.is_constant()) {
      return false;
    }

    if (!curve_keys.is_constant()) {
      return false;
    }

    if (!curve_radius.is_constant()) {
      return false;
    }

    if (!curve_first_key.is_constant()) {
      return false;
    }

    if (!curve_shader.is_constant()) {
      return false;
    }

    if (!shader.is_constant()) {
      return false;
    }

    for (const CachedAttribute &attr : attributes) {
      if (!attr.data.is_constant()) {
        return false;
      }
    }

    return true;
  }
};

/* Representation of an Alembic object for the AlembicProcedural.
 *
 * The AlembicObject holds the path to the Alembic IObject inside of the archive that is desired
 * for rendering, as well as the list of shaders that it is using.
 *
 * The names of the shaders should correspond to the names of the FaceSets inside of the Alembic
 * archive for per-triangle shader association. If there is no FaceSets, or the names do not
 * match, the first shader is used for rendering for all triangles.
 */
class AlembicObject : public Node {
 public:
  NODE_DECLARE

  /* Path to the IObject inside of the archive. */
  NODE_SOCKET_API(ustring, path)

  /* Shaders used for rendering. */
  NODE_SOCKET_API_ARRAY(array<Node *>, used_shaders)

  AlembicObject();
  ~AlembicObject();

 private:
  friend class AlembicProcedural;

  void set_object(Object *object);
  Object *get_object();

  void load_all_data(Alembic::AbcGeom::IPolyMeshSchema &schema, Progress &progress);
  void load_all_data(const Alembic::AbcGeom::ICurvesSchema &schema,
                     Progress &progress,
                     float default_radius);

  bool has_data_loaded() const;

  MatrixSampleMap xform_samples;
  Alembic::AbcGeom::IObject iobject;
  Transform xform;

  CachedData &get_cached_data()
  {
    return cached_data;
  }

  bool is_constant() const
  {
    return cached_data.is_constant();
  }

  Object *object = nullptr;

  bool data_loaded = false;

  CachedData cached_data;

  void read_attribute(const Alembic::AbcGeom::ICompoundProperty &arb_geom_params,
                      const Alembic::AbcGeom::ISampleSelector &iss,
                      const ustring &attr_name);

  void read_face_sets(Alembic::AbcGeom::IPolyMeshSchema &schema, array<int> &polygon_to_shader);

  void setup_transform_cache();

  AttributeRequestSet get_requested_attributes();
};

/* Procedural to render objects from a single Alembic archive.
 *
 * Every object desired to be rendered should be passed as an AlembicObject through the objects
 * socket.
 *
 * This procedural will load the data set for the entire animation in memory on the first frame,
 * and directly set the data for the new frames on the created Nodes if needed. This allows for
 * faster updates between frames as it avoids reseeking the data on disk.
 */
class AlembicProcedural : public Procedural {
  Alembic::AbcGeom::IArchive archive;
  bool objects_loaded;
  Scene *scene_;

 public:
  NODE_DECLARE

  /* The filepath to the archive */
  NODE_SOCKET_API(ustring, filepath)

  /* The current frame to render. */
  NODE_SOCKET_API(float, frame)

  /* The frame rate used for rendering. */
  NODE_SOCKET_API(float, frame_rate)

  /* List of AlembicObjects to render. */
  NODE_SOCKET_API_ARRAY(array<Node *>, objects)

  /* Set the default radius to use for curves when the Alembic Curves Schemas do not have radius
   * information. */
  NODE_SOCKET_API(float, default_curves_radius)

  AlembicProcedural();
  ~AlembicProcedural();

  /* Populates the Cycles scene with Nodes for every contained AlembicObject on the first
   * invocation, and updates the data on subsequent invocations if the frame changed. */
  void generate(Scene *scene, Progress &progress);

  /* Add an object to our list of objects, and tag the socket as modified. */
  void add_object(AlembicObject *object);

  /* Tag for an update only if something was modified. */
  void tag_update(Scene *scene);

  /* Returns true if an object with the given path exists in this procedural. */
  bool has_object(const ustring &path) const;

 private:
  /* Load the data for all the objects whose data has not yet been loaded. */
  void load_objects(Progress &progress);

  /* Traverse the Alembic hierarchy to lookup the IObjects for the AlembicObjects that were
   * specified in our objects socket, and accumulate all of the transformations samples along the
   * way for each IObject. */
  void walk_hierarchy(Alembic::AbcGeom::IObject parent,
                      const Alembic::AbcGeom::ObjectHeader &ohead,
                      MatrixSampleMap *xform_samples,
                      const unordered_map<string, AlembicObject *> &object_map,
                      Progress &progress);

  /* Read the data for an IPolyMesh at the specified frame_time. Creates corresponding Geometry and
   * Object Nodes in the Cycles scene if none exist yet. */
  void read_mesh(Scene *scene,
                 AlembicObject *abc_object,
                 Alembic::AbcGeom::Abc::chrono_t frame_time,
                 Progress &progress);

  /* Read the data for an ICurves at the specified frame_time. Creates corresponding Geometry and
   * Object Nodes in the Cycles scene if none exist yet. */
  void read_curves(Scene *scene,
                   AlembicObject *abc_object,
                   Alembic::AbcGeom::Abc::chrono_t frame_time,
                   Progress &progress);
};

CCL_NAMESPACE_END

#endif
