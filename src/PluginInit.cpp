/*
**                    TIVTC for AviSynth 2.6 interface
**
**   TIVTC includes a field matching filter (TFM) and a decimation
**   filter (TDecimate) which can be used together to achieve an
**   IVTC or for other uses. TIVTC currently supports 8 bit planar YUV and
**   YUY2 colorspaces.
**
**   Copyright (C) 2004-2008 Kevin Stone, additional work (C) 2020 pinterf
**
**   This program is free software; you can redistribute it and/or modify
**   it under the terms of the GNU General Public License as published by
**   the Free Software Foundation; either version 2 of the License, or
**   (at your option) any later version.
**
**   This program is distributed in the hope that it will be useful,
**   but WITHOUT ANY WARRANTY; without even the implied warranty of
**   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**   GNU General Public License for more details.
**
**   You should have received a copy of the GNU General Public License
**   along with this program; if not, write to the Free Software
**   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <VapourSynth.h>
#include <VSHelper.h>

#include "TFM.h"
#include "TFMPP.h"


static void VS_CC tfmInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    (void)in;
    (void)out;
    (void)core;

    TFM *d = (TFM *) *instanceData;

    vsapi->setVideoInfo(d->vi, 1, node);
}


static const VSFrameRef *VS_CC tfmGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    (void)frameData;
    (void)vsapi;

    TFM *d = (TFM *) *instanceData;

    return d->GetFrame(n, activationReason, frameCtx, core);
}


static void VS_CC tfmFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    (void)core;
    (void)vsapi;

    TFM *d = (TFM *)instanceData;

    delete d;
}


static void VS_CC tfmppInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    (void)in;
    (void)out;
    (void)core;

    TFMPP *d = (TFMPP *) *instanceData;

    vsapi->setVideoInfo(d->vi, 1, node);
}


static const VSFrameRef *VS_CC tfmppGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    (void)frameData;
    (void)vsapi;

    TFMPP *d = (TFMPP *) *instanceData;

    return d->GetFrame(n, activationReason, frameCtx, core);
}


static void VS_CC tfmppFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    (void)core;
    (void)vsapi;

    TFMPP *d = (TFMPP *)instanceData;

    delete d;
}


static void VS_CC tfmDisplayFunc(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    VSNodeRef *clip = (VSNodeRef *)userData;

    const VSFrameRef *f = vsapi->propGetFrame(in, "f", 0, nullptr);
    const VSMap *props = vsapi->getFramePropsRO(f);
    const char *text = vsapi->propGetData(props, PROP_TFMDisplay, 0, nullptr);
    int text_size = vsapi->propGetDataSize(props, PROP_TFMDisplay, 0, nullptr);

    VSMap *params = vsapi->createMap();
    vsapi->propSetNode(params, "clip", clip, paReplace); // clip is freed by vapoursynth somewhere.
    vsapi->propSetData(params, "text", text, text_size, paReplace);
    vsapi->freeFrame(f);

    VSPlugin *text_plugin = vsapi->getPluginById("com.vapoursynth.text", core);
    VSMap *ret = vsapi->invoke(text_plugin, "Text", params);
    vsapi->freeMap(params);
    if (vsapi->getError(ret)) {
        char error[512] = { 0 };
        snprintf(error, 512, "TFM: failed to invoke text.Text: %s", vsapi->getError(ret));
        vsapi->freeMap(ret);
        vsapi->setError(out, error);
        return;
    }
    clip = vsapi->propGetNode(ret, "clip", 0, nullptr);
    vsapi->freeMap(ret);
    vsapi->propSetNode(out, "val", clip, paReplace);
    vsapi->freeNode(clip);
}


static void VS_CC tfmCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    (void)userData;

    int err;

    int order = int64ToIntS(vsapi->propGetInt(in, "order", 0, &err));
    if (err)
        order = -1;

    int field = int64ToIntS(vsapi->propGetInt(in, "field", 0, &err));
    if (err)
        field = -1;

    int mode = int64ToIntS(vsapi->propGetInt(in, "mode", 0, &err));
    if (err)
        mode = 1;

    int PP = int64ToIntS(vsapi->propGetInt(in, "PP", 0, &err));
    if (err)
        PP = 6;

    const char *ovr = vsapi->propGetData(in, "ovr", 0, &err);
    if (err)
        ovr = "";

    const char *input = vsapi->propGetData(in, "input", 0, &err);
    if (err)
        input = "";

    const char *output = vsapi->propGetData(in, "output", 0, &err);
    if (err)
        output = "";

    const char *outputC = vsapi->propGetData(in, "outputC", 0, &err);
    if (err)
        outputC = "";

    bool debug = !!vsapi->propGetInt(in, "debug", 0, &err); /// not used for anything at the moment. maybe use logMessage ?
    if (err)
        debug = false;

    bool display = !!vsapi->propGetInt(in, "display", 0, &err);
    if (err)
        display = false;

    int slow = int64ToIntS(vsapi->propGetInt(in, "slow", 0, &err));
    if (err)
        slow = 1;

    bool mChroma = !!vsapi->propGetInt(in, "mChroma", 0, &err);
    if (err)
        mChroma = true;

    int cNum = int64ToIntS(vsapi->propGetInt(in, "cNum", 0, &err));
    if (err)
        cNum = 15;

    int cthresh = int64ToIntS(vsapi->propGetInt(in, "cthresh", 0, &err));
    if (err)
        cthresh = 9;

    int MI = int64ToIntS(vsapi->propGetInt(in, "MI", 0, &err));
    if (err)
        MI = 80;

    bool chroma = !!vsapi->propGetInt(in, "chroma", 0, &err);
    if (err)
        chroma = false;

    int blockx = int64ToIntS(vsapi->propGetInt(in, "blockx", 0, &err));
    if (err)
        blockx = 16;

    int blocky = int64ToIntS(vsapi->propGetInt(in, "blocky", 0, &err));
    if (err)
        blocky = 16;

    int y0 = int64ToIntS(vsapi->propGetInt(in, "y0", 0, &err));
    if (err)
        y0 = 0;

    int y1 = int64ToIntS(vsapi->propGetInt(in, "y1", 0, &err));
    if (err)
        y1 = 0;

    int mthresh = int64ToIntS(vsapi->propGetInt(in, "mthresh", 0, &err));
    if (err)
        mthresh = 5;

    const char *d2v = vsapi->propGetData(in, "d2v", 0, &err);
    if (err)
        d2v = "";

    int ovrDefault = int64ToIntS(vsapi->propGetInt(in, "ovrDefault", 0, &err));
    if (err)
        ovrDefault = 0;

    int flags = int64ToIntS(vsapi->propGetInt(in, "flags", 0, &err));
    if (err)
        flags = 4;

    double scthresh = vsapi->propGetFloat(in, "scthresh", 0, &err);
    if (err)
        scthresh = 12.0;

    int micout = int64ToIntS(vsapi->propGetInt(in, "micout", 0, &err));
    if (err)
        micout = 0;

    int micmatching = int64ToIntS(vsapi->propGetInt(in, "micmatching", 0, &err));
    if (err)
        micmatching = 1;

    const char *trimIn = vsapi->propGetData(in, "trimIn", 0, &err);
    if (err)
        trimIn = "";

    bool hint = !!vsapi->propGetInt(in, "hint", 0, &err);
    if (err)
        hint = true;

    int metric = int64ToIntS(vsapi->propGetInt(in, "metric", 0, &err));
    if (err)
        metric = 0;

    bool batch = !!vsapi->propGetInt(in, "batch", 0, &err);
    if (err)
        batch = false;

    bool ubsco = !!vsapi->propGetInt(in, "ubsco", 0, &err);
    if (err)
        ubsco = true;

    bool mmsco = !!vsapi->propGetInt(in, "mmsco", 0, &err);
    if (err)
        mmsco = true;

    int opt = int64ToIntS(vsapi->propGetInt(in, "opt", 0, &err));
    if (err)
        opt = 4;


    VSNodeRef *clip = vsapi->propGetNode(in, "clip", 0, nullptr);

    TFM *tfm_data;

    try {
        tfm_data = new TFM(clip, order, field, mode, PP, ovr, input, output, outputC, debug, display, slow, mChroma, cNum, cthresh,
                       MI, chroma, blockx, blocky, y0, y1, d2v, ovrDefault, flags, scthresh, micout, micmatching, trimIn, hint,
                       metric, batch, ubsco, mmsco, opt, vsapi, core);
    } catch (const TIVTCError& e) {
        vsapi->setError(out, e.what());

        vsapi->freeNode(clip);

        return;
    }

    int filter_mode = fmParallelRequests; /// It's possible fmParallel could be used in some situations. Study the matter.
    int filter_flags = 0;
    if (mode == 7) {
        // mode 7 requires linear access to function correctly.
        filter_mode = fmSerial;
        filter_flags = nfMakeLinear;
    }
    vsapi->createFilter(in, out, "TFM", tfmInit, tfmGetFrame, tfmFree, filter_mode, filter_flags, tfm_data, core);

    if (vsapi->getError(out))
        return;


    if (PP > 4) {
        VSMap *params = vsapi->createMap();
        VSNodeRef *node = vsapi->propGetNode(out, "clip", 0, nullptr);
        vsapi->propSetNode(params, "clip", node, paReplace);
        vsapi->freeNode(node);
        VSPlugin *std_plugin = vsapi->getPluginById("com.vapoursynth.std", core);
        VSMap *ret = vsapi->invoke(std_plugin, "Cache", params);
        vsapi->freeMap(params);
        if (vsapi->getError(ret)) {
            char error[512] = { 0 };
            snprintf(error, 512, "TFM: failed to invoke std.Cache: %s", vsapi->getError(ret));
            vsapi->freeMap(ret);
            vsapi->setError(out, error);
            return;
        }
        node = vsapi->propGetNode(ret, "clip", 0, nullptr);
        vsapi->freeMap(ret);
        vsapi->propSetNode(out, "clip", node, paReplace);
        vsapi->freeNode(node);
    }

    if (PP > 1) {
        VSNodeRef *clip2 = vsapi->propGetNode(in, "clip2", 0, &err);

        VSNodeRef *node = vsapi->propGetNode(out, "clip", 0, nullptr);

        TFMPP *tfmpp_data;

        try {
            tfmpp_data = new TFMPP(node, PP, mthresh, ovr, display, clip2, hint, opt, vsapi, core);
        } catch (const TIVTCError& e) {
            vsapi->setError(out, e.what());

            vsapi->freeNode(node);
            vsapi->freeNode(clip2);

            return;
        }

        // createFilter uses paAppend when adding the node to the "out" map, so clear the existing node first.
        vsapi->propDeleteKey(out, "clip");

        vsapi->createFilter(in, out, "TFMPP", tfmppInit, tfmppGetFrame, tfmppFree, fmParallelRequests, 0, tfmpp_data, core);
    }

    if (display) {
        // text.FrameProps won't print the TFMDisplay property because it is too long,
        // so we use text.Text with std.FrameEval instead.
        VSMap *params = vsapi->createMap();
        VSNodeRef *node = vsapi->propGetNode(out, "clip", 0, nullptr);
        vsapi->propSetNode(params, "clip", node, paReplace);
        vsapi->propSetNode(params, "prop_src", node, paReplace);
        VSFuncRef *displayFuncRef = vsapi->createFunc(tfmDisplayFunc, vsapi->cloneNodeRef(node), (VSFreeFuncData)vsapi->freeNode, core, vsapi);
        vsapi->freeNode(node);
        vsapi->propSetFunc(params, "eval", displayFuncRef, paReplace);
        vsapi->freeFunc(displayFuncRef);
        VSPlugin *std_plugin = vsapi->getPluginById("com.vapoursynth.std", core);
        VSMap *ret = vsapi->invoke(std_plugin, "FrameEval", params);
        vsapi->freeMap(params);
        if (vsapi->getError(ret)) {
            char error[512] = { 0 };
            snprintf(error, 512, "TFM: failed to invoke std.FrameEval: %s", vsapi->getError(ret));
            vsapi->freeMap(ret);
            vsapi->setError(out, error);
            return;
        }
        node = vsapi->propGetNode(ret, "clip", 0, nullptr);
        vsapi->freeMap(ret);
        vsapi->propSetNode(out, "clip", node, paReplace);
        vsapi->freeNode(node);
    }
}


VS_EXTERNAL_API(void) VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc("com.nodame.tivtc", "tivtc", "Field matching and decimation", (3 << 16) | 5, 1, plugin);
    registerFunc("TFM",
                 "clip:clip;"
                 "order:int:opt;"
                 "field:int:opt;"
                 "mode:int:opt;"
                 "PP:int:opt;"
                 "ovr:data:opt;"
                 "input:data:opt;"
                 "output:data:opt;"
                 "outputC:data:opt;"
                 "debug:int:opt;"
                 "display:int:opt;"
                 "slow:int:opt;"
                 "mChroma:int:opt;"
                 "cNum:int:opt;"
                 "cthresh:int:opt;"
                 "MI:int:opt;"
                 "chroma:int:opt;"
                 "blockx:int:opt;"
                 "blocky:int:opt;"
                 "y0:int:opt;"
                 "y1:int:opt;"
                 "mthresh:int:opt;"
                 "clip2:clip:opt;"
                 "d2v:data:opt;"
                 "ovrDefault:int:opt;"
                 "flags:int:opt;"
                 "scthresh:float:opt;"
                 "micout:int:opt;"
                 "micmatching:int:opt;"
                 "trimIn:data:opt;"
                 "hint:int:opt;"
                 "metric:int:opt;"
                 "batch:int:opt;"
                 "ubsco:int:opt;"
                 "mmsco:int:opt;"
                 "opt:int:opt;"
                 , tfmCreate, nullptr, plugin);
}


//AVSValue __cdecl Create_TFM(AVSValue args, void* user_data, IScriptEnvironment* env);
//AVSValue __cdecl Create_TDecimate(AVSValue args, void* user_data, IScriptEnvironment* env);
//AVSValue __cdecl Create_MergeHints(AVSValue args, void* user_data, IScriptEnvironment* env);
//AVSValue __cdecl Create_FieldDiff(AVSValue args, void* user_data, IScriptEnvironment* env);
//AVSValue __cdecl Create_CFieldDiff(AVSValue args, void* user_data, IScriptEnvironment* env);
//AVSValue __cdecl Create_FrameDiff(AVSValue args, void* user_data, IScriptEnvironment* env);
//AVSValue __cdecl Create_CFrameDiff(AVSValue args, void* user_data, IScriptEnvironment* env);
//AVSValue __cdecl Create_ShowCombedTIVTC(AVSValue args, void* user_data, IScriptEnvironment* env);
//AVSValue __cdecl Create_IsCombedTIVTC(AVSValue args, void* user_data, IScriptEnvironment* env);
//AVSValue __cdecl Create_RequestLinear(AVSValue args, void* user_data, IScriptEnvironment* env);

//#ifdef AVISYNTH_PLUGIN_25
//extern "C" __declspec(dllexport) const char* __stdcall AvisynthPluginInit2(IScriptEnvironment* env) {
//#else
/* New 2.6 requirement!!! */
// Declare and initialise server pointers static storage.
//const AVS_Linkage *AVS_linkage = 0;

/* New 2.6 requirement!!! */
// DLL entry point called from LoadPlugin() to setup a user plugin.
//extern "C" __declspec(dllexport) const char* __stdcall
//AvisynthPluginInit3(IScriptEnvironment* env, const AVS_Linkage* const vectors) {

  /* New 2.6 requirment!!! */
  // Save the server pointers.
//  AVS_linkage = vectors;
//#endif
/*
  env->AddFunction("TFM", "c[1order]i[2field]i[3mode]i[4PP]i[5ovr]s[6input]s[7output]s[8outputC]s" \
    "[9debug]b[10display]b[11slow]i[12mChroma]b[13cNum]i[14cthresh]i[15MI]i" \
    "[16chroma]b[17blockx]i[18blocky]i[19y0]i[20y1]i[21mthresh]i[22clip2]c[23d2v]s" \
    "[24ovrDefault]i[25flags]i[26scthresh]f[27micout]i[28micmatching]i[29trimIn]s" \
    "[30hint]b[31metric]i[32batch]b[33ubsco]b[34mmsco]b[35opt]i", Create_TFM, 0);
  env->AddFunction("TDecimate", "c[mode]i[cycleR]i[cycle]i[rate]f[dupThresh]f[vidThresh]f" \
    "[sceneThresh]f[hybrid]i[vidDetect]i[conCycle]i[conCycleTP]i" \
    "[ovr]s[output]s[input]s[tfmIn]s[mkvOut]s[nt]i[blockx]i" \
    "[blocky]i[debug]b[display]b[vfrDec]i[batch]b[tcfv1]b[se]b" \
    "[chroma]b[exPP]b[maxndl]i[m2PA]b[denoise]b[noblend]b[ssd]b" \
    "[hint]b[clip2]c[sdlim]i[opt]i[orgOut]s", Create_TDecimate, 0);
  env->AddFunction("MergeHints", "c[hintClip]c[debug]b", Create_MergeHints, 0);
  env->AddFunction("FieldDiff", "c[nt]i[chroma]b[display]b[debug]b[sse]b[opt]i",
    Create_FieldDiff, 0);
  env->AddFunction("CFieldDiff", "c[nt]i[chroma]b[debug]b[sse]b[opt]i", Create_CFieldDiff, 0);
  env->AddFunction("FrameDiff", "c[mode]i[prevf]b[nt]i[blockx]i[blocky]i[chroma]b[thresh]f" \
    "[display]i[debug]b[norm]b[denoise]b[ssd]b[opt]i", Create_FrameDiff, 0);
  env->AddFunction("CFrameDiff", "c[mode]i[prevf]b[nt]i[blockx]i[blocky]i[chroma]b[debug]b" \
    "[norm]b[denoise]b[ssd]b[rpos]b[opt]i", Create_CFrameDiff, 0);
  env->AddFunction("ShowCombedTIVTC", "c[cthresh]i[chroma]b[MI]i[blockx]i[blocky]i[metric]i" \
    "[debug]b[display]i[fill]b[opt]i", Create_ShowCombedTIVTC, 0);
  env->AddFunction("IsCombedTIVTC", "c[cthresh]i[MI]i[chroma]b[blockx]i[blocky]i[metric]i" \
    "[opt]i", Create_IsCombedTIVTC, 0);
  env->AddFunction("RequestLinear", "c[rlim]i[clim]i[elim]i[rall]b[debug]b",
  */
//    Create_RequestLinear, 0);
//  return 0;
//}
