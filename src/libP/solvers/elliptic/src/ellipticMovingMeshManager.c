/*

   The MIT License (MIT)

   Copyright (c) 2017 Tim Warburton, Noel Chalmers, Jesse Chan, Ali Karakus

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.

 */
#include "elliptic.h"
#include "ellipticMovingMeshManager.h"
#include <iostream>
#include <timer.hpp>
MovingMeshManager::MovingMeshManager(elliptic_t* meshSolver)
{
}
void MovingMeshManager::update_system(elliptic_t * elliptic)
{
  // TODO: implement
}
void MovingMeshManager::area3(){}
void MovingMeshManager::move_boundary(elliptic_t * elliptic)
{
  linAlg->fill(Ntotal*ndim, 0.0, o_Rn);
  // TODO:
  // o_Un has to be filled here, which comes from nek5000 area3
  // What's the nekRS equivalent?
  area3();
  updateFaceVectorKernel(Nelements, Nfaces, o_Rn, o_Un);
  oogs::startFinish(o_Rn, ndim, 0, ogsDfloat, ogsAdd, oogs);
  unitVecKernel(Ntotal, o_Rn);

  linAlg->fill(Ntotal * ndim, 0.0, o_W);
  linAlg->fill(Ntotal * ndim, 0.0, o_Wt);

  constexpr int nsweep = 2;
  occa::memory o_vx = o_U + 0 * Ntotal * sizeof(dfloat);
  occa::memory o_vy = o_U + 1 * Ntotal * sizeof(dfloat);
  occa::memory o_vz = o_U + 2 * Ntotal * sizeof(dfloat);
  for(int sweep = 0 ; sweep < nsweep; ++sweep){
    scaleFaceKernel(Nelements, Nfaces, o_wvx, o_wvy, o_wvz, o_vx, o_vy, o_vz);
    oogs::startFinish(o_WV, ndim, 0, ogsDfloat, opType, oogs);
    linAlg->axmy(Ntotal, 1.0, o_invDegree, o_wvx);
    linAlg->axmy(Ntotal, 1.0, o_invDegree, o_wvy);
    linAlg->axmy(Ntotal, 1.0, o_invDegree, o_wvz);

    if(!initialized)
      o_W.copyFrom(o_WV, Ntotal * ndim * sizeof(dfloat));

    // TODO: some additional stuff for conjugate ht transfer
    // TODO: symmetry boundary condition
    // TODO: fix boundary condition

    const char* opType = (sweep == 0) ? ogsMax : ogsMin;
    oogs::startFinish(o_V, ndim, 0, ogsDfloat, opType, oogs);
  }

  //TODO: mask and add solution back in
  initialized = true;
}
void MovingMeshManager::meshSolve(ins_t* ins, dfloat time, occa::memory o_Utmp)
{
  o_U = o_Utmp;

  // elastic material constants
  double vnu = 0.0;
  const double eps = 1e-8;
  ins->meshOptions.getArgs("MESH VISCOSITY", vnu);
  if(std::abs(vnu) < eps)
    vnu = 0.4;
  vnu = std::abs(vnu);
  vnu = std::min(0.499,vnu);
  const double Ce = 1.0 / ( 1.0 + vnu);
  const double C2 = vnu * Ce / (1.0 - 2.0 * vnu);
  const double C3 = 0.5 * Ce;

  update_system(ins->meshSolver);
  move_boundary(ins->meshSolver);

  linAlg->fill(ins->meshSolver->Ntotal, C2, o_h1);
  linAlg->fill(ins->meshSolver->Ntotal, C3, o_h2);

  const double toleranceForMeshSolve = 1e-12;

  cartesianVectorDotProdKernel(Nlocal, o_W, o_wrk);
  const dfloat maxDiffPos = sqrt(linAlg->max(Nlocal, o_wrk, ins->mesh->comm));

  if(maxDiffPos < toleranceForMeshSolve){
    return;
  }
  // nek5000 changes the mesh tolerances based on TOLAB * maxDiffPos * sqrt(min(lambda(A))),
  // but I don't want to do that.

  occa::memory & o_AW = ins->o_wrk3;

  ellipticOperator(ins->meshSolver, o_W, o_AW, dfloatString);
  ins->linAlg->scale(ins->meshSolver->Ntotal*ndim, -1.0, o_AW);

  occa::memory & o_solution = ins->o_wrk0;
  ins->NiterMeshSolve = ellipticSolve(ins->meshSolver, ins->meshTOL, o_AW, o_solution);
  ins->meshSolver->scaledAdd(Nlocal, fieldOffset, 1.0, o_solution, 1.0,o_W);
  oogs::startFinish(o_W, ndim, 0, ogsDfloat, ogsAdd, oogs);

  linAlg->axmy(Ntotal, 1.0, o_invDegree, o_wx);
  linAlg->axmy(Ntotal, 1.0, o_invDegree, o_wy);
  linAlg->axmy(Ntotal, 1.0, o_invDegree, o_wz);
}