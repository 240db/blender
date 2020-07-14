// Copyright Matt Overby 2020.
// Distributed under the MIT License.

#ifndef ADMMPD_BVH_H_
#define ADMMPD_BVH_H_ 1

#include "admmpd_bvh_traverse.h"
#include "admmpd_geom.h"
#include "BLI_assert.h"
#include "BLI_math_geom.h"

namespace admmpd {
using namespace Eigen;

template <typename T>
RayClosestHit<T>::RayClosestHit(
		const VecType &orig_,
		const VecType &dir_,
		const MatrixXType *prim_verts_,
		const Eigen::MatrixXi *tri_inds_,
		T eps_,
		T t_min_,
		T t_max_) :
	o(orig_),
	d(dir_),
	prim_verts(prim_verts_),
	tri_inds(tri_inds_),
	eps(eps_),
	t_min(t_min_)
{
	output.t_max = t_max_;
	BLI_assert(prim_verts != NULL);
	BLI_assert(tri_inds != NULL);
	BLI_assert(prim_verts->cols()==3);
	BLI_assert(tri_inds->cols()==3);
}

template <typename T>
void RayClosestHit<T>::traverse(
	const AABB &left_aabb, bool &go_left,
	const AABB &right_aabb, bool &go_right,
	bool &go_left_first)
{
	go_left = geom::ray_aabb<T>(o,d,left_aabb,t_min,output.t_max) ||
		(eps > 0 ? left_aabb.exteriorDistance(o) < eps : false);
	go_right = geom::ray_aabb<T>(o,d,right_aabb,t_min,output.t_max) ||
		(eps > 0 ? right_aabb.exteriorDistance(o) < eps : false);
}

template <typename T>
bool RayClosestHit<T>::stop_traversing(const AABB &aabb, int prim)
{
	BLI_assert(prim > 0);
	BLI_assert(prim < tri_inds->rows());
	if (!geom::ray_aabb<T>(o,d,aabb,t_min,output.t_max))
		return true;
	RowVector3i p = tri_inds->row(prim);
	VecType v[3] = {
		prim_verts->row(p[0]),
		prim_verts->row(p[1]),
		prim_verts->row(p[2])
	};
	T t_max = output.t_max;
	VecType bary;
	bool hit = geom::ray_triangle<T>(o,d,v[0],v[1],v[2],t_min,t_max,&bary);
	if (hit)
	{
		output.prim = prim;
		output.t_max = t_max;
		output.barys = bary;
		return false;
	}
	if (eps > 0)
	{
		VecType o_on_tri = geom::point_on_triangle<T>(o,v[0],v[1],v[2]);
		T dist = (o-o_on_tri).norm();
		if (dist < eps)
		{
			output.prim = prim;
			output.t_max = dist;
			output.barys = geom::point_triangle_barys<T>(o_on_tri,v[0],v[1],v[2]);
		}
	}
	return false;
}


template <typename T>
void PointInTetMeshTraverse<T>::traverse(
	const AABB &left_aabb, bool &go_left,
	const AABB &right_aabb, bool &go_right,
	bool &go_left_first )
{
	if (left_aabb.contains(point))
		go_left = true;
	if (right_aabb.contains(point))
		go_right = true;
	(void)(go_left_first); // doesn't matter for point-in-tet
}

template <typename T>
bool PointInTetMeshTraverse<T>::stop_traversing(
		const AABB &aabb, int prim )
{
	BLI_assert(prim_verts->cols()==3);
	BLI_assert(prim_inds->cols()==4);

	if (!aabb.contains(point))
		return false;

	RowVector4i t = prim_inds->row(prim);
	VecType v[4] = {
		prim_verts->row(t[0]),
		prim_verts->row(t[1]),
		prim_verts->row(t[2]),
		prim_verts->row(t[3])
	};

	bool hit = geom::point_in_tet(
		point.template cast<double>(),
		v[0].template cast<double>(),
		v[1].template cast<double>(),
		v[2].template cast<double>(),
		v[3].template cast<double>());

	if (hit)
		output.prim = prim;

	return hit; // stop traversing if hit
}

template <typename T>
PointInTriangleMeshTraverse<T>::PointInTriangleMeshTraverse(
	const VecType &point_,
	const MatrixXType *prim_verts_,
	const Eigen::MatrixXi *prim_inds_) :
	point(point_),
	dir(0,0,1),
	prim_verts(prim_verts_),
	prim_inds(prim_inds_)
{
	BLI_assert(prim_verts->rows()>=0);
	BLI_assert(prim_inds->rows()>=0);
	BLI_assert(prim_inds->cols()==3);
	dir.normalize(); // TODO random unit vector
	for (int i=0; i<3; ++i)
	{
		o[i] = (float)point[i];
		d[i] = (float)dir[i];
	}
}

template <typename T>
void PointInTriangleMeshTraverse<T>::traverse(
	const AABB &left_aabb, bool &go_left,
	const AABB &right_aabb, bool &go_right,
	bool &go_left_first )
{
	float tmin = 0;
	float tmax = std::numeric_limits<float>::max();
	float l_bb_min[3] = { (float)left_aabb.min()[0], (float)left_aabb.min()[1], (float)left_aabb.min()[2] };
	float l_bb_max[3] = { (float)left_aabb.max()[0], (float)left_aabb.max()[1], (float)left_aabb.max()[2] };
	go_left = isect_ray_aabb_v3_simple(o,d,l_bb_min,l_bb_max,&tmin,&tmax);
	tmin = 0;
	tmax = std::numeric_limits<float>::max();
	float r_bb_min[3] = { (float)right_aabb.min()[0], (float)right_aabb.min()[1], (float)right_aabb.min()[2] };
	float r_bb_max[3] = { (float)right_aabb.max()[0], (float)right_aabb.max()[1], (float)right_aabb.max()[2] };
	go_right = isect_ray_aabb_v3_simple(o,d,r_bb_min,r_bb_max,&tmin,&tmax);
} // end point in mesh traverse

template <typename T>
bool PointInTriangleMeshTraverse<T>::stop_traversing(
		const AABB &aabb, int prim )
{
	// Check if the tet box doesn't intersect the triangle box
	{
		float tmin = 0;
		float tmax = std::numeric_limits<float>::max();
		float bb_min[3] = { (float)aabb.min()[0], (float)aabb.min()[1], (float)aabb.min()[2] };
		float bb_max[3] = { (float)aabb.max()[0], (float)aabb.max()[1], (float)aabb.max()[2] };
		if (!isect_ray_aabb_v3_simple(o,d,bb_min,bb_max,&tmin,&tmax))
			return false;
	}

	// Get the vertices of the face in float arrays
	// to interface with Blender kernels.
	BLI_assert(prim >= 0 && prim < prim_inds->rows());
	RowVector3i q_f = prim_inds->row(prim);
	BLI_assert(q_f[0] < prim_verts->rows());
	BLI_assert(q_f[1] < prim_verts->rows());
	BLI_assert(q_f[2] < prim_verts->rows());
	float q0[3], q1[3], q2[3];
	for (int i=0; i<3; ++i)
	{
		q0[i] = (float)prim_verts->operator()(q_f[0],i);
		q1[i] = (float)prim_verts->operator()(q_f[1],i);
		q2[i] = (float)prim_verts->operator()(q_f[2],i);
	}

	// If we didn't have a triangle-triangle intersection
	// then record if it was a ray-hit.
	float lambda = 0;
	float uv[2] = {0,0};
	bool hit = isect_ray_tri_watertight_v3_simple(
		o, d, q0, q1, q2, &lambda, uv);

	if (hit)
		output.hits.emplace_back(std::make_pair(prim,lambda));

	return false; // multi-hit, so keep traversing

} // end point in mesh stop traversing

template <typename T>
void NearestTriangleTraverse<T>::traverse(
	const AABB &left_aabb, bool &go_left,
	const AABB &right_aabb, bool &go_right,
	bool &go_left_first)
{
	T l_d2 = left_aabb.exteriorDistance(point);
	go_left = l_d2 < output.dist;
	T r_d2 = right_aabb.exteriorDistance(point);
	go_right = r_d2 < output.dist;
	go_left_first = go_left < go_right;
}

template <typename T>
bool NearestTriangleTraverse<T>::stop_traversing(const AABB &aabb, int prim)
{
	BLI_assert(prim >= 0);
	BLI_assert(prim < prim_inds->rows());
	BLI_assert(prim_inds->cols()==3);

	T b_dist = aabb.exteriorDistance(point);
	if (b_dist > output.dist)
		return false;

	float p[3] = { (float)point[0], (float)point[1], (float)point[2] };
	float r[3] = { p[0], p[1], p[2] };
	RowVector3i tri = prim_inds->row(prim);
	float t1[3], t2[3], t3[3];
	for (int i=0; i<3; ++i)
	{
		t1[i] = (float)prim_verts->operator()(tri[0],i);
		t2[i] = (float)prim_verts->operator()(tri[1],i);
		t3[i] = (float)prim_verts->operator()(tri[2],i);
	}

	// I was hoping there would be kernels that are a bit faster
	// to get point-triangle distance, but I guess this does what I need.
	closest_on_tri_to_point_v3(r, p, t1, t2, t3);
	VecType pt_on_tri((T)r[0], (T)r[1], (T)r[2]);
	double d2 = (point-pt_on_tri).norm();
	if (d2 < output.dist)
	{
		output.prim = prim;
		output.dist = d2;
		output.pt_on_tri = pt_on_tri;		
	}

	return false;
}

template <typename T>
TetIntersectsMeshTraverse<T>::TetIntersectsMeshTraverse(
	const VecType points_[4],
	const MatrixXType *prim_verts_,
	const Eigen::MatrixXi *prim_inds_) :
		prim_verts(prim_verts_),
		prim_inds(prim_inds_)
{
	for (int i=0; i<4; ++i)
		points[i] = points_[i];

	BLI_assert(prim_verts->cols()==3);
	BLI_assert(prim_inds->cols()==3);

	for(int i=0; i<3; ++i)
	{
		p0[i] = (float)points[0][i];
		p1[i] = (float)points[1][i];
		p2[i] = (float)points[2][i];
		p3[i] = (float)points[3][i];
	}

	tet_faces.resize(4,std::vector<float*>());
	tet_faces[0] = {p0,p1,p2};
	tet_faces[1] = {p0,p2,p3};
	tet_faces[2] = {p0,p3,p1};
	tet_faces[3] = {p1,p2,p3};

	tet_aabb.setEmpty();
	for (int i=0; i<4; ++i)
		tet_aabb.extend(points[i]);

} // end point in mesh constructor

template <typename T>
void TetIntersectsMeshTraverse<T>::traverse(
	const AABB &left_aabb, bool &go_left,
	const AABB &right_aabb, bool &go_right,
	bool &go_left_first )
{
	go_left = false;
	go_right = false;

	if (tet_aabb.intersects(left_aabb))
		go_left = true;
	if (tet_aabb.intersects(right_aabb))
		go_right = true;

	go_left_first = true;
	if (go_right && !go_left)
		go_left_first = false;

} // end point in mesh traverse

template <typename T>
bool TetIntersectsMeshTraverse<T>::stop_traversing(
		const AABB &aabb, int prim )
{
	bool tet_hits_aabb = tet_aabb.intersects(aabb);
	if(!tet_hits_aabb)
	{
		return false;
	}

	// Get the vertices of the face in float arrays
	// to interface with Blender kernels.
	BLI_assert(prim >= 0 && prim < prim_inds->rows());
	RowVector3i q_f = prim_inds->row(prim);
	float q0[3], q1[3], q2[3];
	for (int i=0; i<3; ++i)
	{
		q0[i] = (float)prim_verts->operator()(q_f[0],i);
		q1[i] = (float)prim_verts->operator()(q_f[1],i);
		q2[i] = (float)prim_verts->operator()(q_f[2],i);
	}

	// If the tet-aabb intersects the triangle-aabb, then test
	// the four faces of the tet against the triangle.
	for (int i=0; i<4; ++i)
	{
		float r_i1[3] = {0,0,0};
		float r_i2[3] = {0,0,0};
		const std::vector<float*> &f = tet_faces[i];
		bool hit = isect_tri_tri_epsilon_v3(
			f[0], f[1], f[2], q0, q1, q2, r_i1, r_i2, 1e-8);
		if (hit)
		{
			output.hit_face = prim;
			return true;
		}
	}

	return false; // multi-hit, so keep traversing

} // end point in mesh stop traversing

// Compile template types
template class admmpd::RayClosestHit<double>;
template class admmpd::RayClosestHit<float>;
template class admmpd::PointInTetMeshTraverse<double>;
template class admmpd::PointInTetMeshTraverse<float>;
template class admmpd::PointInTriangleMeshTraverse<double>;
template class admmpd::PointInTriangleMeshTraverse<float>;
template class admmpd::NearestTriangleTraverse<double>;
template class admmpd::NearestTriangleTraverse<float>;
template class admmpd::TetIntersectsMeshTraverse<double>;
template class admmpd::TetIntersectsMeshTraverse<float>;

} // namespace admmpd

#endif // ADMMPD_BVH_H_

