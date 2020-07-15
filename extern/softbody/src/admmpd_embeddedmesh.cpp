// Copyright Matt Overby 2020.
// Distributed under the MIT License.

#include "admmpd_embeddedmesh.h"
#include "admmpd_geom.h"
#include "admmpd_bvh.h"
#include "admmpd_bvh_traverse.h"

#include <iostream>
#include <fstream>
#include <unordered_map>
#include <set>
#include <numeric>

#include "BLI_task.h" // threading
#include "BLI_assert.h"

namespace admmpd {
using namespace Eigen;

typedef struct KeepTetThreadData {
	const AABBTree<double,3> *tree; // of embedded faces
	const MatrixXd *pts; // of embedded verts
	const MatrixXi *faces; // embedded faces
	const std::vector<Vector3d> *tet_x;
	const std::vector<Vector4i> *tets;
	std::vector<int> *keep_tet; // 0 or 1
} KeepTetThreadData;

static void parallel_keep_tet(
	void *__restrict userdata,
	const int i,
	const TaskParallelTLS *__restrict UNUSED(tls))
{
	KeepTetThreadData *td = (KeepTetThreadData*)userdata;
	RowVector4i tet = td->tets->at(i);
	Vector3d tet_pts[4] = {
		td->tet_x->at(tet[0]),
		td->tet_x->at(tet[1]),
		td->tet_x->at(tet[2]),
		td->tet_x->at(tet[3])
	};

	// Returns true if the tet intersects the
	// surface mesh. Even if it doesn't, we want to keep
	// the tet if it's totally enclosed in the mesh.
	TetIntersectsMeshTraverse<double> tet_hits_mesh(
		tet_pts, td->pts, td->faces);
	bool hit = td->tree->traverse(tet_hits_mesh);
	if (!hit)
	{
		// We only need to check if one vertex of the
		// tet is inside the mesh. If a subset of
		// vertices are inside the mesh, then there would
		// be tri/tri intersection.
		PointInTriangleMeshTraverse<double> pt_in_mesh(
			tet_pts[0], td->pts, td->faces);
		td->tree->traverse(pt_in_mesh);
		if (pt_in_mesh.output.num_hits() % 2 == 1)
		{
			hit = true;
		}
	}

	if (hit) { td->keep_tet->at(i) = 1; }
	else { td->keep_tet->at(i) = 0; }

} // end parallel test if keep tet

// Gen lattice with subdivision
struct LatticeData {
	//SDF<double> *emb_sdf;
	const Eigen::MatrixXd *V;
	const Eigen::MatrixXi *F;
	std::vector<Vector3d> verts;
	std::vector<Vector4i> tets;
};

static inline void merge_close_vertices(LatticeData *data, double eps=1e-12)
{
	int nv = data->verts.size();
	std::vector<Vector3d> new_v(nv); // new verts
	std::vector<int> idx(nv,0); // index mapping
	std::vector<int> visited(nv,0);
	int count = 0;
	for (int i=0; i<nv; ++i)
	{
		if(!visited[i])
		{
			visited[i] = 1;
			new_v[count] = data->verts[i];
			idx[i] = count;
			Vector3d vi = data->verts[i];
			for (int j = i+1; j<nv; ++j)
			{
				if((data->verts[j]-vi).norm() < eps)
				{
					visited[j] = 1;
					idx[j] = count;
				}
			}
			count++;
		}
	}
	new_v.resize(count);
	data->verts = new_v;
	int nt = data->tets.size();
	for (int i=0; i<nt; ++i)
	{
		for (int j=0; j<4; ++j)
		{
			data->tets[i][j] = idx[data->tets[i][j]];
		}
	}
}

static inline void subdivide_cube(
	LatticeData *data,
	const std::vector<Vector3d> &cv,
	const std::vector<int> &faces,
	int level)
{
	BLI_assert((int)cv.size()==8);
	auto add_tets_from_box = [&]()
	{
		Vector3d max = cv[5];
		Vector3d min = cv[3];
		std::vector<Vector3d> v = {
			// Top plane, clockwise looking down
			max,
			Vector3d(min[0], max[1], max[2]),
			Vector3d(min[0], max[1], min[2]),
			Vector3d(max[0], max[1], min[2]),
			// Bottom plane, clockwise looking down
			Vector3d(max[0], min[1], max[2]),
			Vector3d(min[0], min[1], max[2]),
			min,
			Vector3d(max[0], min[1], min[2])
		};
		// Add vertices and get indices of the box
		std::vector<int> b;
		for(int i=0; i<8; ++i)
		{
			b.emplace_back(data->verts.size());
			data->verts.emplace_back(v[i]);
		}
		// From the box, create five new tets
		std::vector<Vector4i> new_tets = {
			Vector4i( b[0], b[5], b[7], b[4] ),
			Vector4i( b[5], b[7], b[2], b[0] ),
			Vector4i( b[5], b[0], b[2], b[1] ),
			Vector4i( b[7], b[2], b[0], b[3] ),
			Vector4i( b[5], b[2], b[7], b[6] )
		};
		for(int i=0; i<5; ++i)
			data->tets.emplace_back(new_tets[i]);
	};

	// Add this cube because we're at bottom
	if (level==0)
	{
		add_tets_from_box();
		return;
	}

	// Only subdivide if the box contains the surface.
	AlignedBox<double,3> aabb;
	aabb.extend(cv[3]); aabb.extend(cv[5]);
	aabb.extend(aabb.min()-Vector3d::Ones()*1e-8);
	aabb.extend(aabb.max()+Vector3d::Ones()*1e-8);
	std::vector<int> new_faces;
	int nf = faces.size();
	for (int i=0; i<nf; ++i)
	{
		int f_idx = faces[i];
		RowVector3i f = data->F->row(f_idx);
		Vector3d v0 = data->V->row(f[0]);
		Vector3d v1 = data->V->row(f[1]);
		Vector3d v2 = data->V->row(f[2]);
		if (geom::aabb_triangle_intersect(aabb.min(),aabb.max(),v0,v1,v2))
			new_faces.emplace_back(f_idx);
	}
	if (new_faces.size()==0)
	{
		add_tets_from_box();
		return;
	}

	// cv are the cube vertices, listed clockwise
	// with the bottom plane first, then top plane.
	// This is basically a dumb version of an octree.
	Vector3d vfront = 0.25*(cv[0]+cv[1]+cv[5]+cv[4]); // front (+z)
	Vector3d vback = 0.25*(cv[3]+cv[2]+cv[6]+cv[7]); // back (-z)
	Vector3d vleft = 0.25*(cv[0]+cv[3]+cv[7]+cv[4]); // left (-x)
	Vector3d vright = 0.25*(cv[1]+cv[2]+cv[6]+cv[5]); // right (+x)
	Vector3d vtop = 0.25*(cv[4]+cv[5]+cv[6]+cv[7]); // top (+y)
	Vector3d vbot = 0.25*(cv[0]+cv[1]+cv[2]+cv[3]); // bot (-y)
	Vector3d vcent = 0.125*(cv[0]+cv[1]+cv[2]+cv[3]+cv[4]+cv[5]+cv[6]+cv[7]); // center
	Vector3d v01 = 0.5*(cv[0]+cv[1]);
	Vector3d v03 = 0.5*(cv[0]+cv[3]);
	Vector3d v04 = 0.5*(cv[0]+cv[4]);
	Vector3d v12 = 0.5*(cv[1]+cv[2]);
	Vector3d v15 = 0.5*(cv[1]+cv[5]);
	Vector3d v23 = 0.5*(cv[2]+cv[3]);
	Vector3d v26 = 0.5*(cv[2]+cv[6]);
	Vector3d v37 = 0.5*(cv[3]+cv[7]);
	Vector3d v45 = 0.5*(cv[4]+cv[5]);
	Vector3d v56 = 0.5*(cv[5]+cv[6]);
	Vector3d v67 = 0.5*(cv[6]+cv[7]);
	Vector3d v47 = 0.5*(cv[4]+cv[7]);
	subdivide_cube(data, { cv[0], v01, vbot, v03, v04, vfront, vcent, vleft }, new_faces, level-1);
	subdivide_cube(data, { v01, cv[1], v12, vbot, vfront, v15, vright, vcent }, new_faces, level-1);
	subdivide_cube(data, { vbot, v12, cv[2], v23, vcent, vright, v26, vback }, new_faces, level-1);
	subdivide_cube(data, { v03, vbot, v23, cv[3], vleft, vcent, vback, v37 }, new_faces, level-1);
	subdivide_cube(data, { v04, vfront, vcent, vleft, cv[4], v45, vtop, v47 }, new_faces, level-1);
	subdivide_cube(data, { vfront, v15, vright, vcent, v45, cv[5], v56, vtop }, new_faces, level-1);
	subdivide_cube(data, { vcent, vright, v26, vback, vtop, v56, cv[6], v67 }, new_faces, level-1);
	subdivide_cube(data, { vleft, vcent, vback, v37, v47, vtop, v67, cv[7] }, new_faces, level-1);

}

bool EmbeddedMesh::generate(
	const Eigen::MatrixXd &V, // embedded verts
	const Eigen::MatrixXi &F, // embedded faces
	bool trim_lattice,
	int subdiv_levels)
{
	emb_faces = F;
	emb_rest_x = V;

	if (F.rows()==0 || V.rows()==0)
		throw std::runtime_error("EmbeddedMesh::generate Error: Missing data");

	AlignedBox<double,3> aabb;
	int nev = V.rows();
	for (int i=0; i<nev; ++i)
		aabb.extend(V.row(i).transpose());

	// Create initial box
	aabb.extend(aabb.min()-Vector3d::Ones()*1e-4);
	aabb.extend(aabb.max()+Vector3d::Ones()*1e-4);
	Vector3d min = aabb.min();
	Vector3d max = aabb.max();
	std::vector<Vector3d> b0 = {
		Vector3d(min[0], min[1], max[2]),
		Vector3d(max[0], min[1], max[2]),
		Vector3d(max[0], min[1], min[2]),
		min,
		Vector3d(min[0], max[1], max[2]),
		max,
		Vector3d(max[0], max[1], min[2]),
		Vector3d(min[0], max[1], min[2])
	};

	std::vector<int> faces_in_box(F.rows());
	std::iota(faces_in_box.begin(), faces_in_box.end(), 0);

	LatticeData data;
	data.V = &V;
	data.F = &F;
	subdivide_cube(&data,b0,faces_in_box,subdiv_levels);
	merge_close_vertices(&data);

	// We only want to keep tets that are either
	// a) intersecting the surface mesh
	// b) totally inside the surface mesh
	std::set<int> refd_verts;
	{
		int nf = F.rows();
		std::vector<AlignedBox<double,3> > face_aabb(nf);
		for (int i=0; i<nf; ++i)
		{
			RowVector3i f = F.row(i);
			face_aabb[i].setEmpty();
			for (int j=0; j<3; ++j)
				face_aabb[i].extend(V.row(f[j]).transpose());
		}

		int nt0 = data.tets.size();
		std::vector<int> keep_tet(nt0,1);
		emb_rest_tree.init(face_aabb);
		KeepTetThreadData thread_data = {
			.tree = &emb_rest_tree,
			.pts = &V,
			.faces = &F,
			.tet_x = &data.verts,
			.tets = &data.tets,
			.keep_tet = &keep_tet
		};
		if (trim_lattice)
		{
			TaskParallelSettings settings;
			BLI_parallel_range_settings_defaults(&settings);
			BLI_task_parallel_range(0, nt0, &thread_data, parallel_keep_tet, &settings);
		}

		// Loop over tets and remove as needed.
		// Mark referenced vertices to compute a mapping.
		int tet_idx = 0;
		for (std::vector<Vector4i>::iterator it = data.tets.begin();
			it != data.tets.end(); ++tet_idx)
		{
			bool keep = keep_tet[tet_idx];
			if (keep)
			{
				const Vector4i &t = *it;
				refd_verts.emplace(t[0]);
				refd_verts.emplace(t[1]);
				refd_verts.emplace(t[2]);
				refd_verts.emplace(t[3]);
				++it;
			}
			else { it = data.tets.erase(it); }
		}

	} // end remove unnecessary tets

	// Copy data into matrices and remove unreferenced
	{
		// Computing a mapping of vertices from old to new
		// Delete any unreferenced vertices
		std::unordered_map<int,int> vtx_old_to_new;
		int ntv0 = data.verts.size(); // original num verts
		int ntv1 = refd_verts.size(); // reduced num verts
		BLI_assert(ntv1 <= ntv0);
		lat_rest_x.resize(ntv1,3);
		int vtx_count = 0;
		for (int i=0; i<ntv0; ++i)
		{
			if (refd_verts.count(i)>0)
			{
				for(int j=0; j<3; ++j){
					lat_rest_x(vtx_count,j) = data.verts[i][j];
				}
				vtx_old_to_new[i] = vtx_count;
				vtx_count++;
			}
		}

		// Copy tets to matrix data and update vertices
		int nt = data.tets.size();
		lat_tets.resize(nt,4);
		for(int i=0; i<nt; ++i){
			for(int j=0; j<4; ++j){
				int old_idx = data.tets[i][j];
				BLI_assert(vtx_old_to_new.count(old_idx)>0);
				lat_tets(i,j) = vtx_old_to_new[old_idx];
			}
		}
	}

	// Now compute the baryweighting for embedded vertices
	bool embed_success = compute_embedding();

	// Export the mesh for funsies
	std::ofstream of("v_lattice.txt"); of << lat_rest_x; of.close();
	std::ofstream of2("t_lattice.txt"); of2 << lat_tets; of2.close();

	return embed_success;

} // end gen lattice

void EmbeddedMesh::compute_masses(
	Eigen::VectorXd *masses_tets, // masses of the lattice verts
	double density_kgm3)
{
	BLI_assert(masses_tets != NULL);
	BLI_assert(density_kgm3 > 0);

	// TODO
	// map the area of the surface to the tet vertices

	// Source: https://github.com/mattoverby/mclscene/blob/master/include/MCL/TetMesh.hpp
	// Computes volume-weighted masses for each vertex
	// density_kgm3 is the unit-volume density
	int nx = lat_rest_x.rows();
	masses_tets->resize(nx);
	masses_tets->setZero();
	int n_tets = lat_tets.rows();
	for (int t=0; t<n_tets; ++t)
	{
		RowVector4i tet = lat_tets.row(t);
		RowVector3d tet_v0 = lat_rest_x.row(tet[0]);
		Matrix3d edges;
		edges.col(0) = lat_rest_x.row(tet[1]) - tet_v0;
		edges.col(1) = lat_rest_x.row(tet[2]) - tet_v0;
		edges.col(2) = lat_rest_x.row(tet[3]) - tet_v0;
		double vol = std::abs((edges).determinant()/6.f);
		double tet_mass = density_kgm3 * vol;
		masses_tets->operator[](tet[0]) += tet_mass / 4.f;
		masses_tets->operator[](tet[1]) += tet_mass / 4.f;
		masses_tets->operator[](tet[2]) += tet_mass / 4.f;
		masses_tets->operator[](tet[3]) += tet_mass / 4.f;
	}

	// Verify masses
	for (int i=0; i<nx; ++i)
	{
		if (masses_tets->operator[](i) <= 0.0)
		{
			printf("**EmbeddedMesh::compute_masses Error: unreferenced vertex\n");
			masses_tets->operator[](i)=1;
		}
	}
} // end compute masses

typedef struct FindTetThreadData {
	AABBTree<double,3> *tree;
	EmbeddedMesh *emb_mesh; // thread sets vtx_to_tet and barys
} FindTetThreadData;

static void parallel_point_in_tet(
	void *__restrict userdata,
	const int i,
	const TaskParallelTLS *__restrict UNUSED(tls))
{
	FindTetThreadData *td = (FindTetThreadData*)userdata;
	Vector3d pt = td->emb_mesh->emb_rest_x.row(i);
	PointInTetMeshTraverse<double> traverser(
			pt,
			&td->emb_mesh->lat_rest_x,
			&td->emb_mesh->lat_tets);
	bool success = td->tree->traverse(traverser);
	int tet_idx = traverser.output.prim;
	if (success && tet_idx >= 0)
	{
		RowVector4i tet = td->emb_mesh->lat_tets.row(tet_idx);
		Vector3d t[4] = {
			td->emb_mesh->lat_rest_x.row(tet[0]),
			td->emb_mesh->lat_rest_x.row(tet[1]),
			td->emb_mesh->lat_rest_x.row(tet[2]),
			td->emb_mesh->lat_rest_x.row(tet[3])
		};
		td->emb_mesh->emb_vtx_to_tet[i] = tet_idx;
		Vector4d b = geom::point_tet_barys(pt,t[0],t[1],t[2],t[3]);
		td->emb_mesh->emb_barys.row(i) = b;
	}
} // end parallel lin solve

bool EmbeddedMesh::compute_embedding()
{
	int nv = emb_rest_x.rows();
	if (nv==0)
	{
		printf("**EmbeddedMesh::compute_embedding: No embedded vertices");
		return false;
	}

	emb_barys.resize(nv,4);
	emb_barys.setOnes();
	emb_vtx_to_tet.resize(nv);
	int nt = lat_tets.rows();

	// BVH tree for finding point-in-tet and computing
	// barycoords for each embedded vertex
	std::vector<AlignedBox<double,3> > tet_aabbs;
	tet_aabbs.resize(nt);
	Vector3d veta = Vector3d::Ones()*1e-12;
	for (int i=0; i<nt; ++i)
	{
		tet_aabbs[i].setEmpty();
		RowVector4i tet = lat_tets.row(i);
		for (int j=0; j<4; ++j)
			tet_aabbs[i].extend(lat_rest_x.row(tet[j]).transpose());

		tet_aabbs[i].extend(tet_aabbs[i].min()-veta);
		tet_aabbs[i].extend(tet_aabbs[i].max()+veta);
	}

	AABBTree<double,3> tree;
	tree.init(tet_aabbs);

	FindTetThreadData thread_data = {
		.tree = &tree,
		.emb_mesh = this
	};
	TaskParallelSettings settings;
	BLI_parallel_range_settings_defaults(&settings);
	BLI_task_parallel_range(0, nv, &thread_data, parallel_point_in_tet, &settings);

	// Double check we set (valid) barycoords for every embedded vertex
	const double eps = 1e-8;
	for (int i=0; i<nv; ++i)
	{
		RowVector4d b = emb_barys.row(i);
		if (b.minCoeff() < -eps)
		{
			printf("**Lattice::generate Error: negative barycoords\n");
			return false;
		}
		if (b.maxCoeff() > 1 + eps)
		{
			printf("**Lattice::generate Error: max barycoord > 1\n");
			return false;
		}
		if (b.sum() > 1 + eps)
		{
			printf("**Lattice::generate Error: barycoord sum > 1\n");
			return false;
		}
	}

	return true;

} // end compute vtx to tet mapping

Eigen::Vector3d EmbeddedMesh::get_mapped_vertex(
	const Eigen::MatrixXd *x_data, int idx) const
{
    int t_idx = emb_vtx_to_tet[idx];
    RowVector4i tet = lat_tets.row(t_idx);
    RowVector4d b = emb_barys.row(idx);
    return Vector3d(
		x_data->row(tet[0]) * b[0] +
		x_data->row(tet[1]) * b[1] +
		x_data->row(tet[2]) * b[2] +
		x_data->row(tet[3]) * b[3]);
}

} // namespace admmpd