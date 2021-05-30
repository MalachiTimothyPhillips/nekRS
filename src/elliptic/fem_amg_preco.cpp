/*
 * Low-Order finite element preconditioner computed with HYPRE's AMG solver
*/

#include <platform.hpp>
#include <math.h>
#include "_hypre_utilities.h"
#include "HYPRE_parcsr_ls.h"
#include "_hypre_parcsr_ls.h"
#include "HYPRE.h"

#include "gslib.h"
#include "fem_amg_preco.hpp"
#include <map>
#include <set>
#include <cassert>

namespace{

occa::memory scratchOrAllocateMemory(int nWords, int sizeT, void* src, long long& bytesRemaining, long long& byteOffset, long long& bytesAllocated, bool& allocated);
static occa::kernel computeStiffnessMatrixKernel;
static occa::memory o_stiffness;
static occa::memory o_x;
static occa::memory o_y;
static occa::memory o_z;

int bisection_search_index(long long* sortedArr, long long value, long long start, long long end);

void build_kernel();

void fem_assembly_host();
void fem_assembly_device();

void matrix_distribution();
void fem_assembly();
void quadrature_rule(double[4][3], double[4]);
void mesh_connectivity(int[8][3], int[8][4]);
void x_map(double[3], double[4][3], double[3][4], int);
void J_xr_map(double[3][3], double[4][3], double[3][4]);

double phi_3D_1(double q_r[4][3], int q);
double phi_3D_2(double q_r[4][3], int q);
double phi_3D_3(double q_r[4][3], int q);
double phi_3D_4(double q_r[4][3], int q);
void dphi(double deriv[3], int q);

double determinant(double[3][3]);
void inverse(double[3][3], double[3][3]);
long long maximum(long long, long long);

static constexpr int n_dim = 3;
static int n_x, n_y, n_z, n_elem;
static int n_xyz, n_xyze;
static double *x_m, *y_m, *z_m;
static long long *glo_num;
static double *pmask;
static int num_loc_dofs;
static long long *dof_map;
static long long row_start;
static long long row_end;
static HYPRE_IJMatrix A_bc;
static HYPRE_IJMatrix A_test;
static int rank;

}

static struct comm comm;
struct gs_data {
  struct comm comm;
};
static struct gs_data *gsh;


/* Interface definition */
SEMFEMData* fem_amg_setup(const int N_, const int n_elem_,
                          double *x_m_, double *y_m_, double *z_m_,
                          double *pmask_, MPI_Comm mpiComm,
                          long long int *gatherGlobalNodes,
                          occa::memory _o_x,
                          occa::memory _o_y,
                          occa::memory _o_z
                          )
{
  o_x = _o_x;
  o_y = _o_y;
  o_z = _o_z;
  n_x = N_;
  n_y = N_;
  n_z = N_;
  n_elem = n_elem_;
  x_m = x_m_;
  y_m = y_m_;
  z_m = z_m_;
  pmask = pmask_;

  n_xyz = n_x * n_y * n_z;
  n_xyze = n_x * n_y * n_z * n_elem;
  int NuniqueBases = n_xyze;

  {
    comm_ext world;
    world = (comm_ext)mpiComm; // MPI_COMM_WORLD;
    comm_init(&comm, world);
    gsh = gs_setup(gatherGlobalNodes, NuniqueBases, &comm, 0, gs_pairwise,
                   /* mode */ 0);
  }

  MPI_Comm_rank(mpiComm, &rank);

  build_kernel();

  matrix_distribution();

  fem_assembly();

  SEMFEMData* data;

  {

    const int numRows = row_end - row_start + 1;
    HYPRE_BigInt *ownedRows = (HYPRE_BigInt*) calloc(numRows, sizeof(HYPRE_BigInt));
    int ctr = 0;
    for(long long row = row_start; row <= row_end; ++row)
      ownedRows[ctr++] = row;
  
    HYPRE_Int *ncols = (HYPRE_Int*) calloc(numRows, sizeof(HYPRE_Int));
    HYPRE_IJMatrixGetRowCounts(A_bc,
      numRows,
      ownedRows,
      ncols);

    int nnz = 0;
    for(int i = 0; i < numRows; ++i)
      nnz += ncols[i];
  
    // construct COO matrix from Hypre matrix
    HYPRE_BigInt *hAj = (HYPRE_BigInt*) calloc(nnz, sizeof(HYPRE_BigInt));
    HYPRE_Real   *hAv = (HYPRE_Real*) calloc(nnz, sizeof(HYPRE_Real));
    HYPRE_IJMatrixGetValues(A_bc,
      -numRows,
      ncols,
      ownedRows,
      &hAj[0],
      &hAv[0]);

    long long *Ai = (long long*) calloc(nnz, sizeof(long long));
    long long *Aj = (long long*) calloc(nnz, sizeof(long long));
    double    *Av = (double*) calloc(nnz, sizeof(double));
    for(int n = 0; n < nnz; ++n) {
       Aj[n] = hAj[n];
       Av[n] = hAv[n];
    } 
    ctr = 0;
    for(int i = 0; i < numRows; ++i){
      long long row = ownedRows[i];
      for(int col = 0; col < ncols[i]; ++col)
        Ai[ctr++] = row;
    }

    free(hAj);
    free(hAv);
    free(ownedRows);
    free(ncols);
    HYPRE_IJMatrixDestroy(A_bc);
    HYPRE_IJMatrixDestroy(A_test);

    data = (SEMFEMData*) malloc(sizeof(SEMFEMData));
    data->Ai = Ai;
    data->Aj = Aj;
    data->Av = Av;
    data->nnz = nnz;
    data->rowStart = row_start;
    data->rowEnd = row_end;
    data->dofMap = dof_map;

  }

  return data;
}

namespace{

/* FEM Assembly definition */
void matrix_distribution() {
  /*
   * Ranks the global numbering array after removing the Dirichlet nodes
   * which is then used in the assembly of the matrices to map degrees of
   * freedom to rows of the matrix
   */

  int idx;
  buffer my_buffer;
  long long idx_start = n_xyze;
  long long scan_out[2], scan_buf[2];
  comm_scan(scan_out, &comm, gs_long_long, gs_add, &idx_start, 1, scan_buf);
  idx_start = scan_out[0];

  glo_num = (long long*) malloc(n_xyze * sizeof(long long));

  for (idx = 0; idx < n_xyze; idx++) {
    if (pmask[idx] > 0.0)
      glo_num[idx] = idx_start + (long long)idx;
    else
      glo_num[idx] = -1;
  }

  gs(glo_num, gs_long_long, gs_min, 0, gsh, 0);

  /* Rank ids */
  long long maximum_value_local = 0;
  long long maximum_value = 0;

  for (idx = 0; idx < n_xyze; idx++) {
    maximum_value_local = (glo_num[idx] > maximum_value_local)
                              ? glo_num[idx]
                              : maximum_value_local;
  }

  comm_allreduce(&comm, gs_long_long, gs_max, &maximum_value_local, 1,
                 &maximum_value);
  const long long nstar = maximum_value / comm.np + 1;

  struct ranking_tuple {
    long long rank;
    unsigned int proc;
    unsigned int idx;
  };

  struct array ranking_transfer;
  array_init(ranking_tuple, &ranking_transfer, n_xyze);
  ranking_transfer.n = n_xyze;
  struct ranking_tuple *ranking_tuple_array =
      (struct ranking_tuple *)ranking_transfer.ptr;

  for (idx = 0; idx < ranking_transfer.n; idx++) {
    ranking_tuple_array[idx].rank = glo_num[idx];
    ranking_tuple_array[idx].proc = glo_num[idx] / nstar;
    ranking_tuple_array[idx].idx = idx;
  }

  struct crystal crystal_router_handle;
  crystal_init(&crystal_router_handle, &comm);
  sarray_transfer(ranking_tuple, &ranking_transfer, proc, 1,
                  &crystal_router_handle);
  ranking_tuple_array = (struct ranking_tuple *)ranking_transfer.ptr;

  buffer_init(&my_buffer, 1);
  sarray_sort(ranking_tuple, ranking_transfer.ptr, ranking_transfer.n, rank, 1,
              &my_buffer);

  long long current_rank = ranking_tuple_array[0].rank;
  long long current_count = 0;
  ranking_tuple_array[0].rank = current_count;

  for (idx = 1; idx < ranking_transfer.n; idx++) {

    if (ranking_tuple_array[idx].rank > current_rank) {
      current_count++;
      current_rank = ranking_tuple_array[idx].rank;
      ranking_tuple_array[idx].rank = current_count;
    } else if (ranking_tuple_array[idx].rank == current_rank) {
      ranking_tuple_array[idx].rank = current_count;
    } else {
      break;
    }
  }

  current_count += 1;

  long long rank_start;
  comm_scan(scan_out, &comm, gs_long_long, gs_add, &current_count, 1, scan_buf);
  rank_start = scan_out[0];

  for (idx = 0; idx < ranking_transfer.n; idx++) {
    ranking_tuple_array[idx].rank += rank_start;
  }

  sarray_transfer(ranking_tuple, &ranking_transfer, proc, 1,
                  &crystal_router_handle);
  ranking_tuple_array = (struct ranking_tuple *)ranking_transfer.ptr;

  buffer_init(&my_buffer, 1);
  sarray_sort(ranking_tuple, ranking_transfer.ptr, ranking_transfer.n, idx, 0,
              &my_buffer);

  for (idx = 0; idx < n_xyze; idx++) {
    glo_num[idx] = ranking_tuple_array[idx].rank;
  }

  array_free(&ranking_transfer);
  crystal_free(&crystal_router_handle);
}
void fem_assembly_host() {
  /* Set quadrature rule */
  constexpr int n_quad = 4;
  double q_r[4][3];
  double q_w[4];

  quadrature_rule(q_r, q_w);

  /* Mesh connectivity (Can be changed to fill-out or one-per-vertex) */
  constexpr int num_fem = 8;
  int v_coord[8][3];
  int t_map[8][4];

  mesh_connectivity(v_coord, t_map);

  /* Finite element assembly */

  double A_loc[4][4];
  double J_xr[3][3];
  double J_rx[3][3];
  double x_t[3][4];
  double q_x[3];

  int s_x, s_y, s_z;
  int E_x = n_x - 1;
  int E_y = n_y - 1;
  int E_z = n_z - 1;

  for (int e = 0; e < n_elem; e++) {
    /* Cycle through collocated quads/hexes */
    for (int s_z = 0; s_z < E_z; s_z++) {
      for (int s_y = 0; s_y < E_y; s_y++) {
        for (int s_x = 0; s_x < E_x; s_x++) {
          /* Get indices */
          int s[n_dim];

          s[0] = s_x;
          s[1] = s_y;
          s[2] = s_z;

          int idx[(int)(pow(2, n_dim))];

          for (int i = 0; i < pow(2, n_dim); i++) {
            idx[i] = 0;

            for (int d = 0; d < n_dim; d++) {
              idx[i] += (s[d] + v_coord[i][d]) * pow(n_x, d);
            }
          }

          /* Cycle through collocated triangles/tets */
          for (int t = 0; t < num_fem; t++) {
            /* Get vertices */
            for (int i = 0; i < n_dim + 1; i++) {
                x_t[0][i] = x_m[idx[t_map[t][i]] + e * n_xyz];
                x_t[1][i] = y_m[idx[t_map[t][i]] + e * n_xyz];
                x_t[2][i] = z_m[idx[t_map[t][i]] + e * n_xyz];
            }

            /* Local FEM matrices */
            /* Reset local stiffness and mass matrices */
            for (int i = 0; i < n_dim + 1; i++) {
              for (int j = 0; j < n_dim + 1; j++) {
                A_loc[i][j] = 0.0;
              }
            }

            /* Build local stiffness matrices by applying quadrature rules */
            J_xr_map(J_xr, q_r, x_t);
            inverse(J_rx, J_xr);
            const double det_J_xr = determinant(J_xr);
            for (int q = 0; q < n_quad; q++) {
              /* From r to x */
              x_map(q_x, q_r, x_t, q);

              /* Integrand */
              for (int i = 0; i < n_dim + 1; i++) {
                double deriv_i[3];
                dphi(deriv_i, i);
                for (int j = 0; j < n_dim + 1; j++) {
                  double deriv_j[3];
                  dphi(deriv_j, j);
                  int alpha, beta;
                  double func = 0.0;

                  for (alpha = 0; alpha < n_dim; alpha++) {
                    double a = 0.0, b = 0.0;

                    for (beta = 0; beta < n_dim; beta++) {
                      a += deriv_i[beta] * J_rx[beta][alpha];

                      b += deriv_j[beta] * J_rx[beta][alpha];
                    }

                    func += a * b;
                  }

                  A_loc[i][j] += func * det_J_xr * q_w[q];
                }
              }
            }
            for (int i = 0; i < n_dim + 1; i++) {
              for (int j = 0; j < n_dim + 1; j++) {
                if ((pmask[idx[t_map[t][i]] + e * n_xyz] > 0.0) &&
                    (pmask[idx[t_map[t][j]] + e * n_xyz] > 0.0)) {
                  HYPRE_BigInt row = glo_num[idx[t_map[t][i]] + e * n_xyz];
                  HYPRE_BigInt col = glo_num[idx[t_map[t][j]] + e * n_xyz];
                  HYPRE_Real A_val = A_loc[i][j];
                  HYPRE_Int ncols = 1;
                  double tol = 1e-7;
                  int err = 0;

                  if (fabs(A_val) > tol) 
                    err = HYPRE_IJMatrixAddToValues(A_bc, 1, &ncols, &row, &col, &A_val);
                  if (err != 0) {
                    if (comm.id == 0)
                      printf("There was an error with entry A(%lld, %lld) = %f\n",
                             row, col, A_val);
                    exit(EXIT_FAILURE);
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}

void fem_assembly_device() {
  /* Set quadrature rule */
  constexpr int n_quad = 4;
  double q_r[4][3];
  double q_w[4];

  quadrature_rule(q_r, q_w);

  /* Mesh connectivity (Can be changed to fill-out or one-per-vertex) */
  constexpr int num_fem = 8;
  int v_coord[8][3];
  int t_map[8][4];

  mesh_connectivity(v_coord, t_map);

  /* Finite element assembly */

  double A_loc[4][4];
  double J_xr[3][3];
  double J_rx[3][3];
  double x_t[3][4];
  double q_x[3];

  int E_x = n_x - 1;
  int E_y = n_y - 1;
  int E_z = n_z - 1;

  std::map<long long, std::set<long long>> graph;
  double tStart = MPI_Wtime();
  for (int e = 0; e < n_elem; e++) {
    /* Cycle through collocated quads/hexes */
    for (int s_z = 0; s_z < E_z; s_z++) {
      for (int s_y = 0; s_y < E_y; s_y++) {
        for (int s_x = 0; s_x < E_x; s_x++) {
          /* Get indices */
          int s[n_dim];

          s[0] = s_x;
          s[1] = s_y;
          s[2] = s_z;

          int idx[8];

          for (int i = 0; i < 8; i++) {
            idx[i] = 0;

            for (int d = 0; d < n_dim; d++) {
              idx[i] += (s[d] + v_coord[i][d]) * pow(n_x, d);
            }
          }
          for (int t = 0; t < num_fem; t++) {
            for (int i = 0; i < n_dim + 1; i++) {
              for (int j = 0; j < n_dim + 1; j++) {
                if ((pmask[idx[t_map[t][i]] + e * n_xyz] > 0.0) &&
                    (pmask[idx[t_map[t][j]] + e * n_xyz] > 0.0)) {
                  HYPRE_BigInt row = glo_num[idx[t_map[t][i]] + e * n_xyz];
                  HYPRE_BigInt col = glo_num[idx[t_map[t][j]] + e * n_xyz];
                  if(graph.count(row) == 0){
                    graph[row] = {{col}};
                  } else {
                    graph[row].insert(col);
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  const long long nrows = graph.size();
  long long * rows = (long long*) malloc(nrows * sizeof(long long));
  long long * rowOffsets = (long long*) malloc((nrows+1) * sizeof(long long));
  long long * ncols = (long long*) malloc(nrows * sizeof(long long));
  long long nnz = 0;
  long long ctr = 0;
  rowOffsets[0] = 0;
  for(auto && row_and_colset : graph){
    const auto size = row_and_colset.second.size();
    const auto row = row_and_colset.first;
    rows[ctr] = row_and_colset.first;
    ncols[ctr] = size;
    rowOffsets[ctr+1] = rowOffsets[ctr] + size;
    nnz += size;
    ctr++;
  }

  long long * cols = (long long*) malloc(nnz * sizeof(long long));
  ctr = 0;
  for(auto && row_and_colset : graph){
    for(auto&& col : row_and_colset.second){
      cols[ctr++] = col;
    }
  }
  double* vals = (double*) calloc(nnz,sizeof(double));
  
  if(platform->comm.mpiRank == 0) printf("Symbolic graph construction took: (%f)s\n", MPI_Wtime() - tStart);

  tStart = MPI_Wtime();
  struct AllocationTracker{
    bool o_maskAlloc;
    bool o_glo_numAlloc;
    bool o_rowOffsetsAlloc;
    bool o_rowsAlloc;
    bool o_colsAlloc;
    bool o_valsAlloc;
  };
  AllocationTracker allocations;
  long long bytesRemaining = platform->o_mempool.bytesAllocated;
  long long byteOffset = 0;
  long long bytesAllocated = 0;
  occa::memory o_mask = scratchOrAllocateMemory(
    n_xyze,
    sizeof(double),
    pmask,
    bytesRemaining,
    byteOffset,
    bytesAllocated,
    allocations.o_maskAlloc
  );
  occa::memory o_glo_num = scratchOrAllocateMemory(
    n_xyze,
    sizeof(long long),
    glo_num,
    bytesRemaining,
    byteOffset,
    bytesAllocated,
    allocations.o_glo_numAlloc
  );
  occa::memory o_rows = scratchOrAllocateMemory(
    nrows,
    sizeof(long long),
    rows,
    bytesRemaining,
    byteOffset,
    bytesAllocated,
    allocations.o_rowsAlloc
  );
  occa::memory o_rowOffsets = scratchOrAllocateMemory(
    nrows+1,
    sizeof(long long),
    rowOffsets,
    bytesRemaining,
    byteOffset,
    bytesAllocated,
    allocations.o_rowOffsetsAlloc
  );
  occa::memory o_cols = scratchOrAllocateMemory(
    nnz,
    sizeof(long long),
    cols,
    bytesRemaining,
    byteOffset,
    bytesAllocated,
    allocations.o_colsAlloc
  );
  occa::memory o_vals = scratchOrAllocateMemory(
    nnz,
    sizeof(double),
    vals,
    bytesRemaining,
    byteOffset,
    bytesAllocated,
    allocations.o_valsAlloc
  );

  MPI_Allreduce(MPI_IN_PLACE, &bytesAllocated, 1, MPI_LONG_LONG, MPI_SUM, platform->comm.mpiComm);
  double bytesTotal = (double) bytesAllocated / 1e9;
  double bytesPerProc = bytesTotal / platform->comm.mpiCommSize;

  if(platform->comm.mpiRank == 0) printf("Allocated in total %f GB, or %f GB/proc\n", bytesTotal, bytesPerProc);

  computeStiffnessMatrixKernel(
    n_elem,
    (int)nrows,
    o_x,
    o_y,
    o_z,
    o_mask,
    o_glo_num,
    o_rows,
    o_rowOffsets,
    o_cols,
    o_vals
  );
  o_vals.copyTo(vals, nnz * sizeof(double));

  if(allocations.o_maskAlloc) o_mask.free();
  if(allocations.o_glo_numAlloc) o_glo_num.free();
  if(allocations.o_rowOffsetsAlloc) o_rowOffsets.free();
  if(allocations.o_rowsAlloc) o_rows.free();
  if(allocations.o_colsAlloc) o_cols.free();
  if(allocations.o_valsAlloc) o_vals.free();

  int err = HYPRE_IJMatrixAddToValues(A_bc, nrows, ncols, rows, cols, vals);
  if (err != 0) {
    if (comm.id == 0)
      printf("err!\n");
    exit(EXIT_FAILURE);
  }
  if(platform->comm.mpiRank == 0) printf("Actual graph construction took: (%f)s\n", MPI_Wtime() - tStart);

  free(rows);
  free(rowOffsets);
  free(ncols);
  free(cols);
  free(vals);
}

void fem_assembly() {
  /*
   * Assembles the low-order FEM matrices from the spectral element mesh
   *
   * Returns A_fem and B_fem
   */

  /* Variables */
  int i, j, k, e, d, t, q;
  int idx;
  long long row;

  row_start = 0;
  row_end = 0;

  for (idx = 0; idx < n_xyze; idx++)
    if (glo_num[idx] >= 0)
      row_end = maximum(row_end, glo_num[idx]);

  long long scan_out[2], scan_buf[2];
  comm_scan(scan_out, &comm, gs_long_long, gs_max, &row_end, 1, scan_buf);
  if (comm.id > 0)
    row_start = scan_out[0] + 1;

  num_loc_dofs = row_end - row_start + 1;

  dof_map = (long long *) malloc(num_loc_dofs * sizeof(long long));

  for (idx = 0; idx < n_xyze; idx++) {
    if ((row_start <= glo_num[idx]) && (glo_num[idx] <= row_end)) {
      dof_map[glo_num[idx] - row_start] = idx;
    }
  }

  /* Assemble FE matrices with boundary conditions applied */
  HYPRE_IJMatrixCreate(comm.c, row_start, row_end, row_start, row_end, &A_bc);
  HYPRE_IJMatrixSetObjectType(A_bc, HYPRE_PARCSR);
  HYPRE_IJMatrixInitialize(A_bc);

  HYPRE_IJMatrixCreate(comm.c, row_start, row_end, row_start, row_end, &A_test);
  HYPRE_IJMatrixSetObjectType(A_test, HYPRE_PARCSR);
  HYPRE_IJMatrixInitialize(A_test);

  //fem_assembly_host();
  fem_assembly_device();



  HYPRE_IJMatrixAssemble(A_bc);
  //HYPRE_IJMatrixAssemble(A_test);

  //HYPRE_IJMatrixPrint(A_bc, "realMatrix");
  //HYPRE_IJMatrixPrint(A_test, "testMatrix");

  free(glo_num);

}

 void quadrature_rule(double q_r[4][3], double q_w[4]) {
    double a = (5.0 + 3.0 * sqrt(5.0)) / 20.0;
    double b = (5.0 - sqrt(5.0)) / 20.0;

    q_r[0][0] = a;
    q_r[0][1] = b;
    q_r[0][2] = b;
    q_r[1][0] = b;
    q_r[1][1] = a;
    q_r[1][2] = b;
    q_r[2][0] = b;
    q_r[2][1] = b;
    q_r[2][2] = a;
    q_r[3][0] = b;
    q_r[3][1] = b;
    q_r[3][2] = b;

    q_w[0] = 1.0 / 24.0;
    q_w[1] = 1.0 / 24.0;
    q_w[2] = 1.0 / 24.0;
    q_w[3] = 1.0 / 24.0;
}

void mesh_connectivity(int v_coord[8][3], int t_map[8][4]) {

  (v_coord)[0][0] = 0;
  (v_coord)[0][1] = 0;
  (v_coord)[0][2] = 0;
  (v_coord)[1][0] = 1;
  (v_coord)[1][1] = 0;
  (v_coord)[1][2] = 0;
  (v_coord)[2][0] = 0;
  (v_coord)[2][1] = 1;
  (v_coord)[2][2] = 0;
  (v_coord)[3][0] = 1;
  (v_coord)[3][1] = 1;
  (v_coord)[3][2] = 0;
  (v_coord)[4][0] = 0;
  (v_coord)[4][1] = 0;
  (v_coord)[4][2] = 1;
  (v_coord)[5][0] = 1;
  (v_coord)[5][1] = 0;
  (v_coord)[5][2] = 1;
  (v_coord)[6][0] = 0;
  (v_coord)[6][1] = 1;
  (v_coord)[6][2] = 1;
  (v_coord)[7][0] = 1;
  (v_coord)[7][1] = 1;
  (v_coord)[7][2] = 1;

  (t_map)[0][0] = 0;
  (t_map)[0][1] = 2;
  (t_map)[0][2] = 1;
  (t_map)[0][3] = 4;
  (t_map)[1][0] = 1;
  (t_map)[1][1] = 0;
  (t_map)[1][2] = 3;
  (t_map)[1][3] = 5;
  (t_map)[2][0] = 2;
  (t_map)[2][1] = 6;
  (t_map)[2][2] = 3;
  (t_map)[2][3] = 0;
  (t_map)[3][0] = 3;
  (t_map)[3][1] = 2;
  (t_map)[3][2] = 7;
  (t_map)[3][3] = 1;
  (t_map)[4][0] = 4;
  (t_map)[4][1] = 5;
  (t_map)[4][2] = 6;
  (t_map)[4][3] = 0;
  (t_map)[5][0] = 5;
  (t_map)[5][1] = 7;
  (t_map)[5][2] = 4;
  (t_map)[5][3] = 1;
  (t_map)[6][0] = 6;
  (t_map)[6][1] = 7;
  (t_map)[6][2] = 2;
  (t_map)[6][3] = 4;
  (t_map)[7][0] = 7;
  (t_map)[7][1] = 3;
  (t_map)[7][2] = 6;
  (t_map)[7][3] = 5;
}

 void x_map(double x[3], double q_r[4][3], double x_t[3][4], int q) {
  int i, d;

  for (d = 0; d < n_dim; d++) {
    x[d] = x_t[d][0] * phi_3D_1(q_r, q);
    x[d] += x_t[d][1] * phi_3D_2(q_r, q);
    x[d] += x_t[d][2] * phi_3D_3(q_r, q);
    x[d] += x_t[d][3] * phi_3D_4(q_r, q);
  }
}

 void J_xr_map(double J_xr[3][3], double q_r[4][3], double x_t[3][4]){
  int i, j, k;
  double deriv[3];

  for (i = 0; i < n_dim; i++) {
    for (j = 0; j < n_dim; j++) {
      J_xr[i][j] = 0.0;

      for (k = 0; k < n_dim + 1; k++) {
        dphi(deriv, k);

        J_xr[i][j] += x_t[i][k] * deriv[j];
      }
    }
  }
}

/* Basis functions and derivatives in 3D */
 double phi_3D_1(double q_r[4][3], int q) { return q_r[q][0]; }
 double phi_3D_2(double q_r[4][3], int q) { return q_r[q][1]; }
 double phi_3D_3(double q_r[4][3], int q) { return q_r[q][2]; }
 double phi_3D_4(double q_r[4][3], int q) { return 1.0 - q_r[q][0] - q_r[q][1] - q_r[q][2]; }
 void dphi(double deriv[3], int q)
{
  if(q==0){
    deriv[0] = 1.0;
    deriv[1] = 0.0;
    deriv[2] = 0.0;
  }

  if(q==1){
    deriv[0] = 0.0;
    deriv[1] = 1.0;
    deriv[2] = 0.0;
  }

  if(q==2){
    deriv[0] = 0.0;
    deriv[1] = 0.0;
    deriv[2] = 1.0;
  }

  if(q==3){
    deriv[0] = -1.0;
    deriv[1] = -1.0;
    deriv[2] = -1.0;
  }
}

/* Math functions */
long long maximum(long long a, long long b) { return a > b ? a : b; }

double determinant(double A[3][3]) {
  /*
   * Computes the determinant of a matrix
   */

  double d_1 = A[0][0] * (A[1][1] * A[2][2] - A[2][1] * A[1][2]);
  double d_2 = A[0][1] * (A[1][0] * A[2][2] - A[2][0] * A[1][2]);
  double d_3 = A[0][2] * (A[1][0] * A[2][1] - A[2][0] * A[1][1]);

  return d_1 - d_2 + d_3;
}

void inverse(double invA[3][3], double A[3][3]) {
  /*
   * Computes the inverse of a matrix
   */
  double det_A = determinant(A);
  invA[0][0] = (1.0 / det_A) * (A[1][1] * A[2][2] - A[2][1] * A[1][2]);
  invA[0][1] = (1.0 / det_A) * (A[0][2] * A[2][1] - A[2][2] * A[0][1]);
  invA[0][2] = (1.0 / det_A) * (A[0][1] * A[1][2] - A[1][1] * A[0][2]);
  invA[1][0] = (1.0 / det_A) * (A[1][2] * A[2][0] - A[2][2] * A[1][0]);
  invA[1][1] = (1.0 / det_A) * (A[0][0] * A[2][2] - A[2][0] * A[0][2]);
  invA[1][2] = (1.0 / det_A) * (A[0][2] * A[1][0] - A[1][2] * A[0][0]);
  invA[2][0] = (1.0 / det_A) * (A[1][0] * A[2][1] - A[2][0] * A[1][1]);
  invA[2][1] = (1.0 / det_A) * (A[0][1] * A[2][0] - A[2][1] * A[0][0]);
  invA[2][2] = (1.0 / det_A) * (A[0][0] * A[1][1] - A[1][0] * A[0][1]);
}

void build_kernel(){
  std::string install_dir;
  install_dir.assign(getenv("NEKRS_INSTALL_DIR"));
  std::string oklpath = install_dir + "/okl/";
  occa::properties stiffnessKernelInfo = platform->kernelInfo;
  std::string filename = oklpath + "elliptic/ellipticSEMFEMStiffness.okl";
  stiffnessKernelInfo["defines/" "p_Nq"] = n_x;
  stiffnessKernelInfo["defines/" "p_Np"] = n_x * n_x * n_x;

  computeStiffnessMatrixKernel = platform->device.buildKernel(
    filename,
    "computeStiffnessMatrix",
    stiffnessKernelInfo
  );
}

int bisection_search_index(long long* sortedArr, long long value, long long start, long long end)
{
  int fail = -1;
  long long L = start;
  long long R = end-1;
  while (L <= R){
    const long long m = (L+R)/2;
    if(sortedArr[m] < value){
      L = m + 1;
    } else if (sortedArr[m] > value){
      R = m - 1;
    } else {
      return m;
    }
  }
  return fail;
}
occa::memory scratchOrAllocateMemory(int nWords, int sizeT, void* src, long long& bytesRemaining, long long& byteOffset, long long& bytesAllocated, bool& allocated)
{
  occa::memory o_mem;
  if(nWords * sizeT < bytesRemaining){
    o_mem = platform->o_mempool.o_ptr.slice(byteOffset);
    o_mem.copyFrom(src, nWords * sizeT);
    bytesRemaining -= nWords * sizeT;
    byteOffset += nWords * sizeT;
    allocated = false;
  } else {
    o_mem = platform->device.malloc(nWords * sizeT, src);
    allocated = true;
    bytesAllocated += nWords * sizeT;
  }
  return o_mem;
}

}