// Copyright Matt Overby 2020.
// Distributed under the MIT License.

#include "admmpd_linsolve.h"
#include <numeric>
#include <iostream>
#include <set>
#include <unordered_set>
#include <random>
#include "BLI_assert.h"
#include "BLI_task.h"

namespace admmpd {
using namespace Eigen;

// Makes full n3 x n3 matrix
static inline void make_n3(
		const RowSparseMatrix<double> &A,
		RowSparseMatrix<double> &A3)
{
	int na = A.rows();
	SparseMatrix<double> A_cm = A; // A to CM
	SparseMatrix<double> A3_cm(na*3, na*3);
	A3_cm.reserve(A_cm.nonZeros()*3);
	int cols = A_cm.rows();
	for(int c=0; c<cols; ++c)
	{
		for(int dim=0; dim<3; ++dim)
		{
			int col = c*3+dim;
			A3_cm.startVec(col);
			for(SparseMatrix<double>::InnerIterator itA(A_cm,c); itA; ++itA)
				A3_cm.insertBack((itA.row()*3+dim), col) = itA.value();
		}
	}
	A3_cm.finalize();
	A3_cm.makeCompressed();
	A3 = A3_cm; // to RowMajor
} // end make_n3

void ConjugateGradients::solve(
		const Options *options,
		SolverData *data,
		Collision *collision)
{
	(void)(collision); // unused
	BLI_assert(data != NULL);
	BLI_assert(options != NULL);
	int nx = data->x.rows();
	BLI_assert(nx > 0);
	BLI_assert(data->A.rows() == nx);
	BLI_assert(data->A.cols() == nx);
	BLI_assert(data->b.rows() == nx);
	BLI_assert(data->C.cols() == nx*3);
	BLI_assert(data->d.rows() > 0);
	BLI_assert(data->C.rows() == data->d.rows());
	BLI_assert(data->PtP.cols() == nx*3);
	BLI_assert(data->PtP.rows() == nx*3);

	// If we don't have any constraints, we don't need to perform CG
	data->b.noalias() = data->M_xbar + data->DtW2*(data->z-data->u);
	if (data->C.nonZeros()==0 && data->PtP.nonZeros()==0)
	{
		data->x = data->ldltA.solve(data->b);
		return;
	}

	// Resize data associated with Conjugate-Gradient
	admmpd::SolverData::GlobalStepData *gsdata = &data->gsdata;
	if (gsdata->r.rows() != nx*3)
	{
		gsdata->r.resize(nx*3);
		gsdata->z.resize(nx*3);
		gsdata->p.resize(nx*3);
		gsdata->A3p.resize(nx*3);
		gsdata->b3_Ctd_Ptx.resize(nx*3);
		make_n3(data->A, gsdata->A3);
	}

	double col_k = options->mult_ck * data->A_diag_max;
	gsdata->CtC = col_k * data->C.transpose()*data->C;
	gsdata->Ctd = col_k * data->C.transpose()*data->d;
	gsdata->A3_CtC_PtP = gsdata->A3 + gsdata->CtC + data->PtP;
	VectorXd x3(nx*3);
	for (int i=0; i<nx; ++i)
	{
		gsdata->b3_Ctd_Ptx.segment<3>(i*3) =
			data->b.row(i).transpose() + 
			gsdata->Ctd.segment<3>(i*3) +
			data->Ptq.segment<3>(i*3);
		x3.segment<3>(i*3) = data->x.row(i);
	}

	gsdata->r.noalias() = gsdata->b3_Ctd_Ptx - gsdata->A3_CtC_PtP*x3;
	solve_Ax_b(data,&gsdata->z,&gsdata->r);
	gsdata->p = gsdata->z;

	for (int iter=0; iter<options->max_cg_iters; ++iter)
	{
		gsdata->A3p.noalias() = gsdata->A3_CtC_PtP*gsdata->p;
		double p_dot_Ap = gsdata->p.dot(gsdata->A3p);
		if (p_dot_Ap==0.0)
			break;

		double zk_dot_rk = gsdata->z.dot(gsdata->r);
		if (zk_dot_rk==0.0)
			break;

		double alpha = zk_dot_rk / p_dot_Ap;
		x3.noalias() += alpha * gsdata->p;
		gsdata->r.noalias() -= alpha * gsdata->A3p;
		double r_norm = gsdata->r.lpNorm<Infinity>();
		if (r_norm < options->min_res)
			break;

		solve_Ax_b(data,&gsdata->z,&gsdata->r);
		double zk1_dot_rk1 = gsdata->z.dot(gsdata->r);
		double beta = zk1_dot_rk1 / zk_dot_rk;
		gsdata->p = gsdata->z + beta*gsdata->p;
	}

	for (int i=0; i<nx; ++i)
		data->x.row(i) = x3.segment<3>(i*3);

} // end ConjugateGradients solve

// Solve Ax = b in parallel and apply
// dimensionality mapping with temporaries
void ConjugateGradients::solve_Ax_b(
	SolverData *data_,
	VectorXd *x_,
	VectorXd *b_)
{
	struct LinSolveThreadData {
		SolverData *data;
		VectorXd *ls_x;
		VectorXd *ls_b;
	};

	auto parallel_lin_solve = [](
		void *__restrict userdata,
		const int i,
		const TaskParallelTLS *__restrict UNUSED(tls))->void
	{
		LinSolveThreadData *td = (LinSolveThreadData*)userdata;
		int nx = td->ls_x->rows()/3;
		VectorXd b(nx);
		for (int j=0; j<nx; ++j)
			b[j] = td->ls_b->operator[](j*3+i);
		VectorXd x = td->data->ldltA.solve(b);
		for (int j=0; j<nx; ++j)
			td->ls_x->operator[](j*3+i) = x[j];
	};

	LinSolveThreadData thread_data = {.data=data_, .ls_x=x_, .ls_b=b_};
	TaskParallelSettings settings;
	BLI_parallel_range_settings_defaults(&settings);
	BLI_task_parallel_range(0, 3, &thread_data, parallel_lin_solve, &settings);

} // end solve Ax=b

void GaussSeidel::solve(
		const Options *options,
		SolverData *data,
		Collision *collision)
{
	init_solve(options,data,collision);
	MatrixXd dx(data->x.rows(),3);
	dx.setZero();

	struct GaussSeidelThreadData {
		int iter;
		int color;
		std::vector<std::vector<int> > *colors;
		const Options *options;
		SolverData *data;
		Collision *collision;
		MatrixXd *dx;
	};

	GaussSeidelThreadData thread_data = {
		.iter = 0,
		.color = 0,
		.colors = &data->gsdata.A3_plus_CtC_colors,
		.options = options,
		.data = data,
		.collision = collision,
		.dx = &dx };

	// Inner iteration of Gauss-Seidel
	auto parallel_gs_sweep = [](void *__restrict userdata, const int i_,
		const TaskParallelTLS *__restrict UNUSED(tls)) -> void
	{
		GaussSeidelThreadData *td = (GaussSeidelThreadData*)userdata;
		int idx = td->colors->at(td->color)[i_];
		double omega = td->options->gs_omega;
		typedef RowSparseMatrix<double>::InnerIterator InnerIter;

		// Loop over expanded A matrix, i.e. segment update
		Vector3d LUx(0,0,0);
		Vector3d inv_aii(0,0,0);
		for (int j=0; j<3; ++j)
		{
			InnerIter rit(td->data->gsdata.A3_CtC_PtP, idx*3+j);
			for (; rit; ++rit)
			{
				double v = rit.value();
				if (v==0.0)
					continue;

				int r = rit.row();
				int c = rit.col();
				if (r==c) // Diagonal
				{
					inv_aii[j] = 1.0/v;
					continue;
				}

				double xj = td->data->x(c/3,j);
				LUx[j] += v*xj;
			}

		} // end loop segment

		// Update x
		Vector3d bi = td->data->gsdata.b3_Ctd_Ptx.segment<3>(idx*3);

		//Vector3d bi = td->data->b.row(idx);
		Vector3d xi = td->data->x.row(idx);
		Vector3d xi_new = (bi-LUx);
		for (int j=0; j<3; ++j)
			xi_new[j] *= inv_aii[j];

		if (xi_new.norm()>1000 || xi_new.norm()<-1000)
		{
			std::cout << "idx: " << idx << std::endl;
			std::cout << "xi: " << xi_new.transpose() << std::endl;
			std::cout << "bi+ctd: " << bi.transpose() << std::endl;
			std::cout << "bi: " << td->data->b.row(idx) << std::endl;
			std::cout << "LUx: " << LUx.transpose() << std::endl;
			std::cout << "aii: " << inv_aii.transpose() << std::endl;
			throw std::runtime_error("Gauss Seidel exploded");
		}

		td->data->x.row(idx) = xi*(1.0-omega) + xi_new*omega;

		// Check fast-query constraints
//		double floor_z = td->collision->get_floor();
//		if (td->data->x(idx,2) < floor_z)
//			td->data->x(idx,2) = floor_z;

		// Update deltas
		td->dx->row(idx) = td->data->x.row(idx)-xi.transpose();

	};

	TaskParallelSettings thrd_settings;
	BLI_parallel_range_settings_defaults(&thrd_settings);

	// Outer iteration loop
	int n_colors = data->gsdata.A3_plus_CtC_colors.size();
	int iter = 0;
	for (; iter < options->max_gs_iters; ++iter)
	{
		for (int color=0; color<n_colors; ++color)
		{
			thread_data.color = color;
			int n_inds = data->gsdata.A3_plus_CtC_colors[color].size();
			thrd_settings.use_threading = false;
			BLI_task_parallel_range(0, n_inds, &thread_data, parallel_gs_sweep, &thrd_settings);
		} // end loop colors

		double dxn = dx.rowwise().lpNorm<Infinity>().maxCoeff();
		if (dxn < options->min_res)
			break;
	} // end loop GS iters

} // end solve with constraints

void GaussSeidel::init_solve(
		const Options *options,
		SolverData *data,
		Collision *collision)
{

	// TODO:
	//
	// When it comes to improving run time of Gauss-Seidel after
	// the instability issues have been addressed, reducing the
	// matrix-vector mults that occur here should be a priority.
	// Many of them are unnecessary and can be done
	// within the Gauss-Seidel sweeps!

	BLI_assert(options != nullptr);
	BLI_assert(data != nullptr);
	BLI_assert(collision != nullptr);
	int nx = data->x.rows();
	BLI_assert(nx>0);
	BLI_assert(data->x.cols()==3);
	data->b.noalias() = data->M_xbar + data->DtW2*(data->z-data->u);
	BLI_assert(data->b.rows()==nx);
	BLI_assert(data->b.cols()==data->x.cols());

	// Do we need to color the default colorings?
	if (data->gsdata.A_colors.size() == 0)
	{
		std::vector<std::set<int> > c_graph;
		compute_colors(data->energies_graph, c_graph, data->gsdata.A_colors);
	}

	// Create large A if we haven't already.
	if (data->gsdata.A3.nonZeros()==0)
		make_n3(data->A, data->gsdata.A3);

	// TODO
	// Eventually we'll replace KtK with the full-dof matrix.
	// For now use z and test collisions against ground plane.
	bool has_constraints = data->C.nonZeros()>0;

	// Finally, the new global matrix and rhs
	if (has_constraints)
	{
		double col_k = options->mult_ck * data->A_diag_max;
		data->gsdata.CtC = col_k * data->C.transpose()*data->C;
		data->gsdata.Ctd.noalias() = col_k * data->C.transpose()*data->d;
		data->gsdata.A3_CtC_PtP = data->gsdata.A3 + data->gsdata.CtC;
		data->gsdata.b3_Ctd_Ptx.resize(nx*3);
		for (int i=0; i<nx; ++i)
		{
			data->gsdata.b3_Ctd_Ptx[i*3+0] = data->b(i,0)+data->gsdata.Ctd[i*3+0];
			data->gsdata.b3_Ctd_Ptx[i*3+1] = data->b(i,1)+data->gsdata.Ctd[i*3+1];
			data->gsdata.b3_Ctd_Ptx[i*3+2] = data->b(i,2)+data->gsdata.Ctd[i*3+2];
		}
		std::vector<std::set<int> > c_graph;
		collision->graph(c_graph);
		compute_colors(data->energies_graph, c_graph, data->gsdata.A3_plus_CtC_colors);
	}
	else
	{
		if (data->gsdata.CtC.rows() != nx*3)
			data->gsdata.CtC.resize(nx*3, nx*3);
		data->gsdata.CtC.setZero();
		if (data->gsdata.Ctd.rows() != nx*3)
			data->gsdata.Ctd.resize(nx*3);
		data->gsdata.Ctd.setZero();
		data->gsdata.A3_CtC_PtP = data->gsdata.A3;
		data->gsdata.b3_Ctd_Ptx.resize(nx*3);
		for (int i=0; i<nx; ++i)
		{
			data->gsdata.b3_Ctd_Ptx[i*3+0] = data->b(i,0);
			data->gsdata.b3_Ctd_Ptx[i*3+1] = data->b(i,1);
			data->gsdata.b3_Ctd_Ptx[i*3+2] = data->b(i,2);
		}
		data->gsdata.A3_plus_CtC_colors = data->gsdata.A_colors;
	}

} // end init solve

// Rehash of graph coloring from
// https://github.com/mattoverby/mclscene/blob/master/include/MCL/GraphColor.hpp
void GaussSeidel::compute_colors(
		const std::vector<std::set<int> > &vertex_energies_graph,
		const std::vector<std::set<int> > &vertex_constraints_graph,
		std::vector<std::vector<int> > &colors)
{
	int n_nodes = vertex_energies_graph.size();
	BLI_assert(n_nodes>0);
	BLI_assert(
		vertex_constraints_graph.size()==0 ||
		(int)vertex_constraints_graph.size()==n_nodes);

	{
		colors.clear();
		colors.resize(n_nodes, std::vector<int>());
		for (int i=0; i<n_nodes; ++i)
			colors[i].emplace_back(i);
		return;
	}
	
	// Graph color settings
	int init_palette_size = 6;

	// Graph coloring tmp data
	std::vector<std::set<int> > palette(n_nodes, std::set<int>());
	std::vector<int> conflict(n_nodes,1);
	std::vector<int> node_colors(n_nodes,-1);
	std::vector<int> node_queue(n_nodes);
	std::iota(node_queue.begin(), node_queue.end(), 0);
    std::random_device rd;
    std::mt19937 mt(rd());
	std::uniform_int_distribution<int> dist(0, n_nodes);

	struct GraphColorThreadData {
		int iter;
		int init_palette_size;
		const std::vector<std::set<int> > *e_graph; // energies
		const std::vector<std::set<int> > *c_graph; // constraints
		std::vector<std::set<int> > *palette;
		std::vector<int> *conflict;
		std::vector<int> *node_colors;
		std::vector<int> *node_queue;
    	std::mt19937 *mt;
		std::uniform_int_distribution<int> *dist;
	};
	GraphColorThreadData thread_data = {
		.iter = 0,
		.init_palette_size = init_palette_size,
		.e_graph = &vertex_energies_graph,
		.c_graph = &vertex_constraints_graph,
		.palette = &palette,
		.conflict = &conflict,
		.node_colors = &node_colors,
		.node_queue = &node_queue,
		.mt = &mt,
		.dist = &dist };
	TaskParallelSettings thrd_settings;
	BLI_parallel_range_settings_defaults(&thrd_settings);

	//
	// Step 1)
	// Graph color initialization
	//
	auto init_graph = [](void *__restrict userdata, const int i,
		const TaskParallelTLS *__restrict UNUSED(tls)) -> void
	{
		GraphColorThreadData *td = (GraphColorThreadData*)userdata;
		for( int j=0; j<td->init_palette_size; ++j ) // init colors
			td->palette->at(i).insert(j); 
	};
	BLI_task_parallel_range(0, n_nodes, &thread_data, init_graph, &thrd_settings);

	//
	// Step 2)
	// Stochastic Graph coloring
	//
	int max_iters = n_nodes;
	for (int rand_iter=0; n_nodes>0 && rand_iter<max_iters; ++rand_iter)
	{
		thread_data.iter = rand_iter;

		// Generate a random color
		auto generate_color = [](void *__restrict userdata, const int i,
			const TaskParallelTLS *__restrict UNUSED(tls)) -> void
		{
			GraphColorThreadData *td = (GraphColorThreadData*)userdata;
			int idx = td->node_queue->at(i);
			if (td->palette->at(idx).size()<2) // Feed the hungry
					td->palette->at(idx).insert(td->init_palette_size+td->iter);
			int c_idx = td->dist->operator()(*td->mt) % td->palette->at(idx).size();
			td->node_colors->at(idx) = *std::next(td->palette->at(idx).begin(), c_idx);
		};
		BLI_task_parallel_range(0, n_nodes, &thread_data, generate_color, &thrd_settings);

		// Detect conflicts
		auto detect_conflicts = [](void *__restrict userdata, const int i,
			const TaskParallelTLS *__restrict UNUSED(tls)) -> void
		{
			GraphColorThreadData *td = (GraphColorThreadData*)userdata;
			int idx = td->node_queue->at(i);
			int curr_c = td->node_colors->at(idx);
			bool curr_conflict = false;
			for (std::set<int>::iterator e_it = td->e_graph->at(idx).begin();
				e_it != td->e_graph->at(idx).end() && !curr_conflict; ++e_it)
			{
				int adj_idx = *e_it;
				if (adj_idx<=idx)
					continue; // Hungarian heuristic
				int adj_c = td->node_colors->at(adj_idx);
				if (curr_c==adj_c)
					curr_conflict = true;
			}
			if ((int)td->c_graph->size() > idx)
			{
				for (std::set<int>::iterator c_it = td->c_graph->at(idx).begin();
					c_it != td->c_graph->at(idx).end() && !curr_conflict; ++c_it)
				{
					int adj_idx = *c_it;
					if (adj_idx<=idx)
						continue; // Hungarian heuristic
					int adj_c = td->node_colors->at(adj_idx);
					if (curr_c==adj_c)
						curr_conflict = true;
				}
			}
			td->conflict->at(idx) = curr_conflict;
		};
		BLI_task_parallel_range(0, n_nodes, &thread_data, detect_conflicts, &thrd_settings);

		// Resolve conflicts and update queue
		std::vector<int> new_queue;
		for (int i=0; i<n_nodes; ++i)
		{
			int idx = node_queue[i];
			if (conflict[idx])
				new_queue.emplace_back(idx);
			else
			{
				int curr_color = node_colors[idx];
				// Remove color from neighbor palletes
				for (std::set<int>::iterator e_it = vertex_energies_graph[idx].begin();
					e_it != vertex_energies_graph[idx].end(); ++e_it)
				{
					int adj_idx = *e_it;
					if (conflict[adj_idx]) // still in the set?
						palette[adj_idx].erase(curr_color);
				}
				if ((int)vertex_constraints_graph.size() > idx)
				{
					for (std::set<int>::iterator c_it = vertex_constraints_graph[idx].begin();
						c_it != vertex_constraints_graph[idx].end(); ++c_it)
					{
						int adj_idx = *c_it;
						if (conflict[adj_idx]) // still in the set?
							palette[adj_idx].erase(curr_color);
					}
				}
			}
		}

		node_queue = new_queue;
		n_nodes = node_queue.size();

	} // end color loop

	//
	// Step 3)
	// Map per-vertex colors
	//
	colors.clear();
	colors.resize(14,std::vector<int>());
	n_nodes = node_colors.size();
	for( int i=0; i<n_nodes; ++i ){
		int color = node_colors[i];
		if (color<0)
			throw std::runtime_error("GaussSeidel: Error with coloring");
		while( color >= (int)colors.size() )
			colors.emplace_back(std::vector<int>());
		colors[color].emplace_back(i);
	}

	// Remove empty color groups
	for (std::vector<std::vector<int> >::iterator it = colors.begin(); it != colors.end();)
		it->size() == 0 ? it = colors.erase(it) : it++;

} // end compute colors

void GaussSeidel::verify_colors(SolverData *data)
{
	// TODO check constraints too
	// Verify color groups are correct
	std::cout << "TESTING " << data->tets.rows() << " tets" << std::endl;
	std::cout << "num colors: " << data->gsdata.A_colors.size() << std::endl;
	int nt = data->tets.rows();
	int nc = data->gsdata.A_colors.size();
	for (int i=0; i<nc; ++i)
	{
		// Each vertex in the color should not
		// be a part of a tet with a vertex in the same color
		const std::vector<int> &grp = data->gsdata.A_colors[i];
		int n_grp = grp.size();
		for (int j=0; j<n_grp; ++j)
		{
			int p_idx = grp[j];
			auto in_tet = [](int idx, const RowVector4i &t)
			{
				return (t[0]==idx||t[1]==idx||t[2]==idx||t[3]==idx);
			};

			for (int k=0; k<nt; ++k)
			{
				RowVector4i t = data->tets.row(k);
				if (!in_tet(p_idx,t))
					continue;

				for (int l=0; l<n_grp; ++l)
				{
					int q_idx = grp[l];
					if (p_idx==q_idx)
						continue;

					if (in_tet(q_idx,t))
					{
						std::cerr << "p: " << p_idx << ", q: " << q_idx <<
							", tet (" << k << "): " << t <<
							", color: " << i <<
							std::endl;
					}
				}
			}
		}
	}
} // end verify colors

} // namespace admmpd