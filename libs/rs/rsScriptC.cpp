/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "rsContext.h"
#include "rsScriptC.h"
#include "rsMatrix.h"
#include "../../compile/libbcc/include/bcc/bcc.h"
#include "utils/Timers.h"
#include "utils/StopWatch.h"
extern "C" {
#include "libdex/ZipArchive.h"
}

#include <GLES/gl.h>
#include <GLES/glext.h>

using namespace android;
using namespace android::renderscript;

#define GET_TLS()  Context::ScriptTLSStruct * tls = \
    (Context::ScriptTLSStruct *)pthread_getspecific(Context::gThreadTLSKey); \
    Context * rsc = tls->mContext; \
    ScriptC * sc = (ScriptC *) tls->mScript


ScriptC::ScriptC(Context *rsc) : Script(rsc) {
    LOGD(">>>> ScriptC ctor called, obj=%x", this);
    mBccScript = NULL;
    memset(&mProgram, 0, sizeof(mProgram));
}

ScriptC::~ScriptC() {
    LOGD(">>>> ~ScriptC()");
    if (mBccScript) {
        bccDeleteScript(mBccScript);
        LOGD(">>>> ~ScriptC(mBCCScript)");
    }
    free(mEnviroment.mScriptText);
    mEnviroment.mScriptText = NULL;
}

void ScriptC::setupScript(Context *rsc) {
    mEnviroment.mStartTimeMillis
                = nanoseconds_to_milliseconds(systemTime(SYSTEM_TIME_MONOTONIC));

    for (uint32_t ct=0; ct < mEnviroment.mFieldCount; ct++) {
        if (mSlots[ct].get() && !mTypes[ct].get()) {
            mTypes[ct].set(mSlots[ct]->getType());
        }

        if (!mTypes[ct].get())
            continue;
        void *ptr = NULL;
        if (mSlots[ct].get()) {
            ptr = mSlots[ct]->getPtr();
        }
        void **dest = ((void ***)mEnviroment.mFieldAddress)[ct];

        if (rsc->props.mLogScripts) {
            if (mSlots[ct].get() != NULL) {
                LOGV("%p ScriptC::setupScript slot=%i  dst=%p  src=%p  type=%p", rsc, ct, dest, ptr, mSlots[ct]->getType());
            } else {
                LOGV("%p ScriptC::setupScript slot=%i  dst=%p  src=%p  type=null", rsc, ct, dest, ptr);
            }
        }

        if (dest) {
            *dest = ptr;
        }
    }
}

const Allocation *ScriptC::ptrToAllocation(const void *ptr) const {
    if (!ptr) {
        return NULL;
    }
    for (uint32_t ct=0; ct < mEnviroment.mFieldCount; ct++) {
        if (!mSlots[ct].get())
            continue;
        if (mSlots[ct]->getPtr() == ptr) {
            return mSlots[ct].get();
        }
    }
    LOGE("ScriptC::ptrToAllocation, failed to find %p", ptr);
    return NULL;
}

Script * ScriptC::setTLS(Script *sc) {
    Context::ScriptTLSStruct * tls = (Context::ScriptTLSStruct *)
                                  pthread_getspecific(Context::gThreadTLSKey);
    rsAssert(tls);
    Script *old = tls->mScript;
    tls->mScript = sc;
    return old;
}

void ScriptC::setupGLState(Context *rsc) {
    if (mEnviroment.mFragmentStore.get()) {
        rsc->setProgramStore(mEnviroment.mFragmentStore.get());
    }
    if (mEnviroment.mFragment.get()) {
        rsc->setProgramFragment(mEnviroment.mFragment.get());
    }
    if (mEnviroment.mVertex.get()) {
        rsc->setProgramVertex(mEnviroment.mVertex.get());
    }
    if (mEnviroment.mRaster.get()) {
        rsc->setProgramRaster(mEnviroment.mRaster.get());
    }
}

uint32_t ScriptC::run(Context *rsc) {
    if (mProgram.mRoot == NULL) {
        rsc->setError(RS_ERROR_BAD_SCRIPT, "Attempted to run bad script");
        return 0;
    }

    setupGLState(rsc);
    setupScript(rsc);

    uint32_t ret = 0;
    Script * oldTLS = setTLS(this);

    if (rsc->props.mLogScripts) {
        LOGV("%p ScriptC::run invoking root,  ptr %p", rsc, mProgram.mRoot);
    }

    ret = mProgram.mRoot();

    if (rsc->props.mLogScripts) {
        LOGV("%p ScriptC::run invoking complete, ret=%i", rsc, ret);
    }

    setTLS(oldTLS);
    return ret;
}

typedef struct {
    Context *rsc;
    ScriptC *script;
    const Allocation * ain;
    Allocation * aout;
    const void * usr;

    uint32_t mSliceSize;
    volatile int mSliceNum;

    const uint8_t *ptrIn;
    uint32_t eStrideIn;
    uint8_t *ptrOut;
    uint32_t eStrideOut;

    uint32_t xStart;
    uint32_t xEnd;
    uint32_t yStart;
    uint32_t yEnd;
    uint32_t zStart;
    uint32_t zEnd;
    uint32_t arrayStart;
    uint32_t arrayEnd;

    uint32_t dimX;
    uint32_t dimY;
    uint32_t dimZ;
    uint32_t dimArray;
} MTLaunchStruct;
typedef int (*rs_t)(const void *, void *, const void *, uint32_t, uint32_t, uint32_t, uint32_t);

static void wc_xy(void *usr, uint32_t idx) {
    MTLaunchStruct *mtls = (MTLaunchStruct *)usr;

    while (1) {
        uint32_t slice = (uint32_t)android_atomic_inc(&mtls->mSliceNum);
        uint32_t yStart = mtls->yStart + slice * mtls->mSliceSize;
        uint32_t yEnd = yStart + mtls->mSliceSize;
        yEnd = rsMin(yEnd, mtls->yEnd);
        if (yEnd <= yStart) {
            return;
        }

        //LOGE("usr idx %i, x %i,%i  y %i,%i", idx, mtls->xStart, mtls->xEnd, yStart, yEnd);
        //LOGE("usr ptr in %p,  out %p", mtls->ptrIn, mtls->ptrOut);
        for (uint32_t y = yStart; y < yEnd; y++) {
            uint32_t offset = mtls->dimX * y;
            uint8_t *xPtrOut = mtls->ptrOut + (mtls->eStrideOut * offset);
            const uint8_t *xPtrIn = mtls->ptrIn + (mtls->eStrideIn * offset);

            for (uint32_t x = mtls->xStart; x < mtls->xEnd; x++) {
                ((rs_t)mtls->script->mProgram.mRoot) (xPtrIn, xPtrOut, mtls->usr, x, y, 0, 0);
                xPtrIn += mtls->eStrideIn;
                xPtrOut += mtls->eStrideOut;
            }
        }
    }
}

static void wc_x(void *usr, uint32_t idx) {
    MTLaunchStruct *mtls = (MTLaunchStruct *)usr;

    while (1) {
        uint32_t slice = (uint32_t)android_atomic_inc(&mtls->mSliceNum);
        uint32_t xStart = mtls->xStart + slice * mtls->mSliceSize;
        uint32_t xEnd = xStart + mtls->mSliceSize;
        xEnd = rsMin(xEnd, mtls->xEnd);
        if (xEnd <= xStart) {
            return;
        }

        //LOGE("usr idx %i, x %i,%i  y %i,%i", idx, mtls->xStart, mtls->xEnd, yStart, yEnd);
        //LOGE("usr ptr in %p,  out %p", mtls->ptrIn, mtls->ptrOut);
        uint8_t *xPtrOut = mtls->ptrOut + (mtls->eStrideOut * xStart);
        const uint8_t *xPtrIn = mtls->ptrIn + (mtls->eStrideIn * xStart);
        for (uint32_t x = xStart; x < xEnd; x++) {
            ((rs_t)mtls->script->mProgram.mRoot) (xPtrIn, xPtrOut, mtls->usr, x, 0, 0, 0);
            xPtrIn += mtls->eStrideIn;
            xPtrOut += mtls->eStrideOut;
        }
    }
}

void ScriptC::runForEach(Context *rsc,
                         const Allocation * ain,
                         Allocation * aout,
                         const void * usr,
                         const RsScriptCall *sc) {
    MTLaunchStruct mtls;
    memset(&mtls, 0, sizeof(mtls));
    Context::PushState ps(rsc);

    if (ain) {
        mtls.dimX = ain->getType()->getDimX();
        mtls.dimY = ain->getType()->getDimY();
        mtls.dimZ = ain->getType()->getDimZ();
        //mtls.dimArray = ain->getType()->getDimArray();
    } else if (aout) {
        mtls.dimX = aout->getType()->getDimX();
        mtls.dimY = aout->getType()->getDimY();
        mtls.dimZ = aout->getType()->getDimZ();
        //mtls.dimArray = aout->getType()->getDimArray();
    } else {
        rsc->setError(RS_ERROR_BAD_SCRIPT, "rsForEach called with null allocations");
        return;
    }

    if (!sc || (sc->xEnd == 0)) {
        mtls.xEnd = mtls.dimX;
    } else {
        rsAssert(sc->xStart < mtls.dimX);
        rsAssert(sc->xEnd <= mtls.dimX);
        rsAssert(sc->xStart < sc->xEnd);
        mtls.xStart = rsMin(mtls.dimX, sc->xStart);
        mtls.xEnd = rsMin(mtls.dimX, sc->xEnd);
        if (mtls.xStart >= mtls.xEnd) return;
    }

    if (!sc || (sc->yEnd == 0)) {
        mtls.yEnd = mtls.dimY;
    } else {
        rsAssert(sc->yStart < mtls.dimY);
        rsAssert(sc->yEnd <= mtls.dimY);
        rsAssert(sc->yStart < sc->yEnd);
        mtls.yStart = rsMin(mtls.dimY, sc->yStart);
        mtls.yEnd = rsMin(mtls.dimY, sc->yEnd);
        if (mtls.yStart >= mtls.yEnd) return;
    }

    mtls.xEnd = rsMax((uint32_t)1, mtls.xEnd);
    mtls.yEnd = rsMax((uint32_t)1, mtls.yEnd);
    mtls.zEnd = rsMax((uint32_t)1, mtls.zEnd);
    mtls.arrayEnd = rsMax((uint32_t)1, mtls.arrayEnd);

    rsAssert(ain->getType()->getDimZ() == 0);

    setupGLState(rsc);
    setupScript(rsc);
    Script * oldTLS = setTLS(this);

    mtls.rsc = rsc;
    mtls.ain = ain;
    mtls.aout = aout;
    mtls.script = this;
    mtls.usr = usr;
    mtls.mSliceSize = 10;
    mtls.mSliceNum = 0;

    mtls.ptrIn = NULL;
    mtls.eStrideIn = 0;
    if (ain) {
        mtls.ptrIn = (const uint8_t *)ain->getPtr();
        mtls.eStrideIn = ain->getType()->getElementSizeBytes();
    }

    mtls.ptrOut = NULL;
    mtls.eStrideOut = 0;
    if (aout) {
        mtls.ptrOut = (uint8_t *)aout->getPtr();
        mtls.eStrideOut = aout->getType()->getElementSizeBytes();
    }

    if ((rsc->getWorkerPoolSize() > 1) && mEnviroment.mIsThreadable) {
        if (mtls.dimY > 1) {
            rsc->launchThreads(wc_xy, &mtls);
        } else {
            rsc->launchThreads(wc_x, &mtls);
        }

        //LOGE("launch 1");
    } else {
        //LOGE("launch 3");
        for (uint32_t ar = mtls.arrayStart; ar < mtls.arrayEnd; ar++) {
            for (uint32_t z = mtls.zStart; z < mtls.zEnd; z++) {
                for (uint32_t y = mtls.yStart; y < mtls.yEnd; y++) {
                    uint32_t offset = mtls.dimX * mtls.dimY * mtls.dimZ * ar +
                                      mtls.dimX * mtls.dimY * z +
                                      mtls.dimX * y;
                    uint8_t *xPtrOut = mtls.ptrOut + (mtls.eStrideOut * offset);
                    const uint8_t *xPtrIn = mtls.ptrIn + (mtls.eStrideIn * offset);

                    for (uint32_t x = mtls.xStart; x < mtls.xEnd; x++) {
                        ((rs_t)mProgram.mRoot) (xPtrIn, xPtrOut, usr, x, y, z, ar);
                        xPtrIn += mtls.eStrideIn;
                        xPtrOut += mtls.eStrideOut;
                    }
                }
            }
        }
    }

    setTLS(oldTLS);
}

void ScriptC::Invoke(Context *rsc, uint32_t slot, const void *data, uint32_t len) {
    if ((slot >= mEnviroment.mInvokeFunctionCount) ||
        (mEnviroment.mInvokeFunctions[slot] == NULL)) {
        rsc->setError(RS_ERROR_BAD_SCRIPT, "Calling invoke on bad script");
        return;
    }
    setupScript(rsc);
    Script * oldTLS = setTLS(this);

    if (rsc->props.mLogScripts) {
        LOGV("%p ScriptC::Invoke invoking slot %i,  ptr %p", rsc, slot, mEnviroment.mInvokeFunctions[slot]);
    }
    ((void (*)(const void *, uint32_t))
        mEnviroment.mInvokeFunctions[slot])(data, len);
    if (rsc->props.mLogScripts) {
        LOGV("%p ScriptC::Invoke complete", rsc);
    }

    setTLS(oldTLS);
}

ScriptCState::ScriptCState() {
    mScript.clear();
}

ScriptCState::~ScriptCState() {
    mScript.clear();
}

void ScriptCState::init(Context *rsc) {
    clear(rsc);
}

void ScriptCState::clear(Context *rsc) {
    rsAssert(rsc);
    mScript.clear();
    mScript.set(new ScriptC(rsc));
}

static BCCvoid* symbolLookup(BCCvoid* pContext, const BCCchar* name) {
    const ScriptCState::SymbolTable_t *sym;
    ScriptC *s = (ScriptC *)pContext;
    if (!strcmp(name, "__isThreadable")) {
      return (BCCvoid*) s->mEnviroment.mIsThreadable;
    } else if (!strcmp(name, "__clearThreadable")) {
      s->mEnviroment.mIsThreadable = false;
      return NULL;
    }
    sym = ScriptCState::lookupSymbol(name);
    if (!sym) {
        sym = ScriptCState::lookupSymbolCL(name);
    }
    if (!sym) {
        sym = ScriptCState::lookupSymbolGL(name);
    }
    if (sym) {
        s->mEnviroment.mIsThreadable &= sym->threadable;
        return sym->mPtr;
    }
    LOGE("ScriptC sym lookup failed for %s", name);
    return NULL;
}

extern const char rs_runtime_lib_bc[];
extern unsigned rs_runtime_lib_bc_size;

void ScriptCState::runCompiler(Context *rsc,
                               ScriptC *s,
                               long modWhen,
                               long crc32,
                               const char *resName,
                               const char *cacheDir) {
    {
        s->mBccScript = bccCreateScript();
        s->mEnviroment.mIsThreadable = true;
        bccRegisterSymbolCallback(s->mBccScript, symbolLookup, s);
        // bccReadBC() reads in the BitCode, if no cache file corresponding to
        // the resName is found. Otherwise, bccReadBC() returns a negative value
        // and the "else" branch will be taken.
        if (bccReadBC(s->mBccScript,
                      s->mEnviroment.mScriptText,
                      s->mEnviroment.mScriptTextLength,
                      modWhen,
                      crc32,
                      resName,
                      cacheDir) >= 0) {
          //bccLinkBC(s->mBccScript, rs_runtime_lib_bc, rs_runtime_lib_bc_size);
          bccCompileBC(s->mBccScript);
        } else {
          // bccReadBC returns a neagative value: Didn't read any script,
          // So, use cached binary instead
          if (bccLoadBinary(s->mBccScript)) {  // LoadBinary fails ==> Recompile
            bccReadBC(s->mBccScript,
                      s->mEnviroment.mScriptText,
                      s->mEnviroment.mScriptTextLength,
                      modWhen,
                      crc32,
                      resName,
                      cacheDir);
            bccCompileBC(s->mBccScript);
          }
        }
        bccGetScriptLabel(s->mBccScript, "root", (BCCvoid**) &s->mProgram.mRoot);
        bccGetScriptLabel(s->mBccScript, "init", (BCCvoid**) &s->mProgram.mInit);
    }
    LOGV("%p ScriptCState::runCompiler root %p,  init %p", rsc, s->mProgram.mRoot, s->mProgram.mInit);

    if (s->mProgram.mInit) {
        s->mProgram.mInit();
    }

    bccGetExportFuncs(s->mBccScript, (BCCsizei*) &s->mEnviroment.mInvokeFunctionCount, 0, NULL);
    if (s->mEnviroment.mInvokeFunctionCount <= 0)
        s->mEnviroment.mInvokeFunctions = NULL;
    else {
        s->mEnviroment.mInvokeFunctions = (Script::InvokeFunc_t*) calloc(s->mEnviroment.mInvokeFunctionCount, sizeof(Script::InvokeFunc_t));
        bccGetExportFuncs(s->mBccScript, NULL, s->mEnviroment.mInvokeFunctionCount, (BCCvoid **) s->mEnviroment.mInvokeFunctions);
    }

    bccGetExportVars(s->mBccScript, (BCCsizei*) &s->mEnviroment.mFieldCount, 0, NULL);
    if (s->mEnviroment.mFieldCount <= 0)
        s->mEnviroment.mFieldAddress = NULL;
    else {
        s->mEnviroment.mFieldAddress = (void **) calloc(s->mEnviroment.mFieldCount, sizeof(void *));
        bccGetExportVars(s->mBccScript, NULL, s->mEnviroment.mFieldCount, (BCCvoid **) s->mEnviroment.mFieldAddress);
        s->initSlots();
    }

    s->mEnviroment.mFragment.set(rsc->getDefaultProgramFragment());
    s->mEnviroment.mVertex.set(rsc->getDefaultProgramVertex());
    s->mEnviroment.mFragmentStore.set(rsc->getDefaultProgramStore());
    s->mEnviroment.mRaster.set(rsc->getDefaultProgramRaster());

    if (s->mProgram.mRoot) {
        const static int pragmaMax = 16;
        BCCsizei pragmaCount;
        BCCchar * str[pragmaMax];
        bccGetPragmas(s->mBccScript, &pragmaCount, pragmaMax, &str[0]);

        for (int ct=0; ct < pragmaCount; ct+=2) {
            //LOGE("pragme %s %s", str[ct], str[ct+1]);
            if (!strcmp(str[ct], "version")) {
                continue;
            }

            if (!strcmp(str[ct], "stateVertex")) {
                if (!strcmp(str[ct+1], "default")) {
                    continue;
                }
                if (!strcmp(str[ct+1], "parent")) {
                    s->mEnviroment.mVertex.clear();
                    continue;
                }
                LOGE("Unreconized value %s passed to stateVertex", str[ct+1]);
            }

            if (!strcmp(str[ct], "stateRaster")) {
                if (!strcmp(str[ct+1], "default")) {
                    continue;
                }
                if (!strcmp(str[ct+1], "parent")) {
                    s->mEnviroment.mRaster.clear();
                    continue;
                }
                LOGE("Unreconized value %s passed to stateRaster", str[ct+1]);
            }

            if (!strcmp(str[ct], "stateFragment")) {
                if (!strcmp(str[ct+1], "default")) {
                    continue;
                }
                if (!strcmp(str[ct+1], "parent")) {
                    s->mEnviroment.mFragment.clear();
                    continue;
                }
                LOGE("Unreconized value %s passed to stateFragment", str[ct+1]);
            }

            if (!strcmp(str[ct], "stateStore")) {
                if (!strcmp(str[ct+1], "default")) {
                    continue;
                }
                if (!strcmp(str[ct+1], "parent")) {
                    s->mEnviroment.mFragmentStore.clear();
                    continue;
                }
                LOGE("Unreconized value %s passed to stateStore", str[ct+1]);
            }

        }


    } else {
        // Deal with an error.
    }
}

namespace android {
namespace renderscript {

void rsi_ScriptCBegin(Context * rsc) {
    ScriptCState *ss = &rsc->mScriptC;
    ss->clear(rsc);
}

void rsi_ScriptCSetText(Context *rsc, const char *text, uint32_t len) {
    ScriptCState *ss = &rsc->mScriptC;

    char *t = (char *)malloc(len + 1);
    memcpy(t, text, len);
    t[len] = 0;
    ss->mScript->mEnviroment.mScriptText = t;
    ss->mScript->mEnviroment.mScriptTextLength = len;
}


RsScript rsi_ScriptCCreate(Context *rsc,
                           const char *packageName,
                           const char *resName,
                           const char *cacheDir)
{
    ScriptCState *ss = &rsc->mScriptC;

    ObjectBaseRef<ScriptC> s(ss->mScript);
    ss->mScript.clear();
    s->incUserRef();

    // Open the apk and return the ZipArchive:
    //  int dexZipOpenArchive(const char* fileName, ZipArchive* pArchive)
    ZipArchive archive;
    long modWhen;
    long crc32;
    if (!dexZipOpenArchive(packageName, &archive)) {  // Success
      ZipEntry entry = dexZipFindEntry(&archive, resName);

      int method;
      size_t uncompLen;
      size_t compLen;
      off_t offset;
      if (!dexZipGetEntryInfo(&archive,
                              entry,
                              &method,
                              &uncompLen,
                              &compLen,
                              &offset,
                              &modWhen,
                              &crc32)) {
      } else {
        LOGI("Coudn't get entry info for the bitcode in an apk");
      }
    } else {
      LOGI("Couldn't open the archive and read the bitcode");
    }

    ss->runCompiler(rsc, s.get(), modWhen, crc32, resName, cacheDir);
    ss->clear(rsc);
    return s.get();
}

}
}
