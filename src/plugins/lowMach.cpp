#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nrs.hpp"
#include "nekInterfaceAdapter.hpp"
#include "udf.hpp"

#include "lowMach.hpp"
#include "linAlg.hpp"

void lowMach::setup(nrs_t* nrs)
{
  mesh_t* mesh = nrs->mesh;
  int err = 1;
  if(nrs->options.compareArgs("SCALAR00 IS TEMPERATURE", "TRUE")) err = 0;
  if(err) {
    if(mesh->rank == 0) cout << "lowMach requires solving for temperature!\n";
    ABORT(1);
  } 
  nrs->options.setArgs("LOWMACH", "TRUE"); 
}

// qtl = 1/(rho*cp*T) * (div[k*grad[T] ] + qvol)
void lowMach::qThermalPerfectGasSingleComponent(nrs_t* nrs, dfloat time, dfloat gamma, occa::memory o_div)
{
  cds_t* cds = nrs->cds;
  mesh_t* mesh = nrs->mesh;
  linAlg_t* linAlg = linAlg_t::getInstance();

  nrs->gradientVolumeKernel(
    mesh->Nelements,
    mesh->o_vgeo,
    mesh->o_Dmatrices,
    nrs->fieldOffset,
    cds->o_S,
    cds->o_wrk0);

  oogs::startFinish(cds->o_wrk0, nrs->NVfields, nrs->fieldOffset,ogsDfloat, ogsAdd, nrs->gsh);

  linAlg->axmyVector(
    mesh->Nlocal,
    nrs->fieldOffset,
    0,
    1.0,
    nrs->mesh->o_invLMM,
    cds->o_wrk0);

  if(udf.sEqnSource) {
    timer::tic("udfSEqnSource", 1);
    udf.sEqnSource(nrs, time, cds->o_S, cds->o_wrk3);
    timer::toc("udfSEqnSource");
  } else {
    linAlg->fill(mesh->Nelements * mesh->Np, 0.0, cds->o_wrk3);
  }

  nrs->qtlKernel(
    mesh->Nelements,
    mesh->o_vgeo,
    mesh->o_Dmatrices,
    nrs->fieldOffset,
    cds->o_wrk0,
    cds->o_S,
    cds->o_diff,
    cds->o_rho,
    cds->o_wrk3,
    o_div);

  oogs::startFinish(o_div, 1, nrs->fieldOffset, ogsDfloat, ogsAdd, nrs->gsh);

  linAlg->axmy(
    mesh->Nlocal,
    1.0,
    nrs->mesh->o_invLMM,
    o_div);
}
