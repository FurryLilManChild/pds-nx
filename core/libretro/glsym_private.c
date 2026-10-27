#include "glsym/glsym.h"

#if defined(_OGLES3_)
RGLSYMGLMEMORYBARRIERPROC __rglgen_glMemoryBarrier;
RGLSYMGLBINDFRAGDATALOCATIONPROC __rglgen_glBindFragDataLocation;
RGLSYMGLPATCHPARAMETERIPROC __rglgen_glPatchParameteri;
RGLSYMGLDISPATCHCOMPUTEPROC __rglgen_glDispatchCompute;
RGLSYMGLBINDIMAGETEXTUREPROC __rglgen_glBindImageTexture;
#else
RGLSYMGLTEXTUREBARRIERNVPROC __rglgen_glTextureBarrierNV;
#endif

#if defined(_OGLES3_)
/* pds-nx: nothing in the core resolves these GLES 3.1/3.2 entry points, so the
 * compute-shader rotating background path called through NULL. Called by the
 * Switch frontend once its GL context exists. */
void pdsnx_resolve_private_gl(void *(*get_proc)(const char *))
{
   __rglgen_glMemoryBarrier        = (RGLSYMGLMEMORYBARRIERPROC)get_proc("glMemoryBarrier");
   __rglgen_glBindFragDataLocation = (RGLSYMGLBINDFRAGDATALOCATIONPROC)get_proc("glBindFragDataLocationEXT");
   __rglgen_glPatchParameteri      = (RGLSYMGLPATCHPARAMETERIPROC)get_proc("glPatchParameteri");
   __rglgen_glDispatchCompute      = (RGLSYMGLDISPATCHCOMPUTEPROC)get_proc("glDispatchCompute");
   __rglgen_glBindImageTexture     = (RGLSYMGLBINDIMAGETEXTUREPROC)get_proc("glBindImageTexture");
}
#endif
