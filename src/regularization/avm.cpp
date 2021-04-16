#include "cds.hpp"
#include "avm.hpp"
#include "udf.hpp"
#include <limits>
#include <string>
#include <array>

/**
 * Persson's artificial viscosity method (http://persson.berkeley.edu/pub/persson06shock.pdf) with P1
 **/

namespace avm {


static occa::kernel evaluateShockSensorKernel;
static occa::kernel applyAVMKernel;
static occa::kernel computeMaxViscKernel;
static occa::kernel interpolateP1Kernel;

static occa::memory o_artVisc;
static occa::memory o_vertexIds;
static occa::memory o_r;
static occa::memory o_s;
static occa::memory o_t;
static occa::memory o_diffOld; // diffusion from initial state
namespace
{

void allocateMemory(cds_t* cds)
{
  mesh_t * mesh = cds->mesh[0];
  if(udf.properties == nullptr) o_diffOld = platform->device.malloc(cds->fieldOffsetSum, sizeof(dfloat), cds->o_diff);
  o_vertexIds = platform->device.malloc(mesh->Nverts * sizeof(int), mesh->vertexNodes);
  o_r = platform->device.malloc(mesh->Np * sizeof(dfloat), mesh->r);
  o_s = platform->device.malloc(mesh->Np * sizeof(dfloat), mesh->s);
  o_t = platform->device.malloc(mesh->Np * sizeof(dfloat), mesh->t);

  o_artVisc = platform->device.malloc(cds->NSfields * cds->mesh[0]->Np, sizeof(dfloat));
  platform->linAlg->fill(cds->NSfields * cds->mesh[0]->Np, 1.0, o_artVisc);
}

void compileKernels(cds_t* cds)
{
  mesh_t* mesh = cds->mesh[0];
  std::string install_dir;
  install_dir.assign(getenv("NEKRS_INSTALL_DIR"));
  const std::string oklpath = install_dir + "/okl/cds/regularization/";
  std::string filename = oklpath + "evaluateShockSensor.okl";
  occa::properties info = platform->kernelInfo;
  info["defines/" "p_Nq"] = cds->mesh[0]->Nq;
  info["defines/" "p_Np"] = cds->mesh[0]->Np;
  evaluateShockSensorKernel =
    platform->device.buildKernel(filename,
                             "evaluateShockSensor",
                             info);

  filename = oklpath + "applyAVM.okl";
  applyAVMKernel =
    platform->device.buildKernel(filename,
                             "applyAVM",
                             info);

  filename = oklpath + "computeMaxVisc.okl";
  computeMaxViscKernel =
    platform->device.buildKernel(filename,
                             "computeMaxVisc",
                             info);
  filename = oklpath + "interpolateP1.okl";
  interpolateP1Kernel =
    platform->device.buildKernel(filename,
      "interpolateP1",
      info);
}

}
void setup(cds_t* cds)
{
  allocateMemory(cds);
  compileKernels(cds);

  const dfloat alpha = -std::log(std::numeric_limits<dfloat>::epsilon());
  auto superGaussian = [alpha](const dfloat x, const dfloat lambda)
  {
    return std::exp(-alpha * std::pow(x, 2.0 * lambda));
  };

  dfloat* artVisc = (dfloat*) calloc(cds->mesh[0]->Np, sizeof(dfloat));
  for(dlong is = 0 ; is < cds->NSfields; ++is){
    if(cds->options[is].compareArgs("AVM C0", "TRUE")){
      dfloat lambda = 1.0;
      cds->options[is].getArgs("AVM LAMBDA", lambda);
      for(dlong point = 0; point < cds->mesh[is]->Np; ++point){
        const dfloat r = cds->mesh[is]->r[point];
        const dfloat s = cds->mesh[is]->s[point];
        const dfloat t = cds->mesh[is]->t[point];
        dfloat visc = superGaussian(r, lambda);
        visc *= superGaussian(s, lambda);
        visc *= superGaussian(t, lambda);
        visc = cbrt(visc);
        artVisc[point] = visc;
      }
      o_artVisc.copyFrom(artVisc, cds->mesh[is]->Np * sizeof(dfloat), is * cds->mesh[is]->Np * sizeof(dfloat));
    }
  }

  free(artVisc);

}

occa::memory computeEps(cds_t* cds, const dfloat time, const dlong scalarIndex, occa::memory o_S)
{
  mesh_t* mesh = cds->mesh[scalarIndex];
  int Nblock = (cds->mesh[scalarIndex]->Nlocal+BLOCKSIZE-1)/BLOCKSIZE;

  occa::memory& o_logShockSensor = platform->o_mempool.slice0;

  // artificial viscosity magnitude
  occa::memory o_epsilon = platform->o_mempool.slice1;
  platform->linAlg->fill(cds->fieldOffset[scalarIndex], 0.0, o_epsilon);

  const dfloat p = mesh->N;
  
  evaluateShockSensorKernel(
    mesh->Nelements,
    scalarIndex,
    cds->fieldOffsetScan[scalarIndex],
    cds->o_filterMT,
    mesh->o_LMM,
    o_S,
    o_logShockSensor
  );

  const dfloat logReferenceSensor = -4.0 * log10(p);

  dfloat coeff = 0.5;
  cds->options[scalarIndex].getArgs("VISMAX COEFF", coeff);

  dfloat rampParameter = 1.0;
  cds->options[scalarIndex].getArgs("RAMP CONSTANT", rampParameter);

  computeMaxViscKernel(
    mesh->Nelements,
    cds->vFieldOffset,
    logReferenceSensor,
    rampParameter,
    coeff,
    mesh->o_x,
    mesh->o_y,
    mesh->o_z,
    cds->o_U,
    o_logShockSensor,
    o_epsilon // max(|df/du|) <- max visc
  );

  int makeP1Cont = cds->options[scalarIndex].compareArgs("AVM P1", "TRUE");
  if(makeP1Cont){
    oogs_t* gsh;
    if(scalarIndex){
      gsh = cds->gsh;
    } else {
      gsh = cds->gshT;
    }
    oogs::startFinish(o_epsilon, 1, cds->fieldOffset[scalarIndex], ogsDfloat, ogsMax, gsh);
    interpolateP1Kernel(
      mesh->Nelements,
      o_vertexIds,
      o_r,
      o_s,
      o_t,
      o_epsilon
    );
  }

  return o_epsilon;

}

void apply(cds_t* cds, const dfloat time, const dlong scalarIndex, occa::memory o_S)
{
  const int verbose = platform->options.compareArgs("VERBOSE", "TRUE");
  mesh_t* mesh = cds->mesh[scalarIndex];
  const dlong scalarOffset = cds->fieldOffsetScan[scalarIndex];
  if(udf.properties == nullptr){ // restore viscosity from previous state
    if(verbose && platform->comm.mpiRank == 0) printf("Resetting properties...\n");
    cds->o_diff.copyFrom(o_diffOld,
      cds->fieldOffset[scalarIndex] * sizeof(dfloat),
      cds->fieldOffsetScan[scalarIndex] * sizeof(dfloat),
      cds->fieldOffsetScan[scalarIndex] * sizeof(dfloat)
    );
  }
  occa::memory o_eps = computeEps(cds, time, scalarIndex, o_S);
  if(verbose && platform->comm.mpiRank == 0){
    const dfloat maxEps = platform->linAlg->max(
      mesh->Nlocal,
      o_eps,
      platform->comm.mpiComm
    );
    const dfloat minEps = platform->linAlg->min(
      mesh->Nlocal,
      o_eps,
      platform->comm.mpiComm
    );
    occa::memory o_S_slice = cds->o_diff + cds->fieldOffsetScan[scalarIndex] * sizeof(dfloat);
    const dfloat maxDiff = platform->linAlg->max(
      mesh->Nlocal,
      o_S_slice,
      platform->comm.mpiComm
    );
    const dfloat minDiff = platform->linAlg->min(
      mesh->Nlocal,
      o_S_slice,
      platform->comm.mpiComm
    );
    printf("Applying a min/max artificial viscosity of (%f,%f) to a field with min/max visc (%f,%f) (field = %d)\n",
      minEps, maxEps, minDiff, maxDiff, scalarIndex);
  }

  applyAVMKernel(
    mesh->Nelements,
    scalarOffset,
    scalarIndex,
    o_artVisc,
    o_eps,
    cds->o_diff
  );
  if(verbose && platform->comm.mpiRank == 0){
    occa::memory o_S_slice = cds->o_diff + cds->fieldOffsetScan[scalarIndex] * sizeof(dfloat);
    const dfloat maxDiff = platform->linAlg->max(
      mesh->Nlocal,
      o_S_slice,
      platform->comm.mpiComm
    );
    const dfloat minDiff = platform->linAlg->min(
      mesh->Nlocal,
      o_S_slice,
      platform->comm.mpiComm
    );
    printf("Field (%d) now has a min/max visc: (%f,%f)\n", scalarIndex, minDiff, maxDiff);
  }

}

} // namespace