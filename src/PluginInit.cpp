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
#include "TDecimate.h"


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


enum DisplayFilters {
    DisplayTFM,
    DisplayTDecimate
};

template <DisplayFilters filter>
static void VS_CC tivtcDisplayFunc(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    VSNodeRef *clip = (VSNodeRef *)userData;

    const char *display_prop = filter == DisplayTFM ? PROP_TFMDisplay : PROP_TDecimateDisplay;

    const VSFrameRef *f = vsapi->propGetFrame(in, "f", 0, nullptr);
    const VSMap *props = vsapi->getFramePropsRO(f);
    const char *text = vsapi->propGetData(props, display_prop, 0, nullptr);
    int text_size = vsapi->propGetDataSize(props, display_prop, 0, nullptr);

    VSMap *params = vsapi->createMap();
    vsapi->propSetNode(params, "clip", clip, paReplace); // clip is freed by vapoursynth somewhere. We don't free it here.
    vsapi->propSetData(params, "text", text, text_size, paReplace);
    vsapi->freeFrame(f);

    VSPlugin *text_plugin = vsapi->getPluginById("com.vapoursynth.text", core);
    VSMap *ret = vsapi->invoke(text_plugin, "Text", params);
    vsapi->freeMap(params);
    if (vsapi->getError(ret)) {
        char error[512] = { 0 };
        snprintf(error, 512, "%s: failed to invoke text.Text: %s", filter == DisplayTFM ? "TFM" : "TDecimate", vsapi->getError(ret));
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
        VSFuncRef *displayFuncRef = vsapi->createFunc(tivtcDisplayFunc<DisplayTFM>, vsapi->cloneNodeRef(node), (VSFreeFuncData)vsapi->freeNode, core, vsapi);
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


static void VS_CC tdecimateInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    (void)in;
    (void)out;
    (void)core;

    TDecimate *d = (TDecimate *) *instanceData;

    vsapi->setVideoInfo(&d->vi, 1, node);
}


static const VSFrameRef *VS_CC tdecimateGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    (void)vsapi;

    TDecimate *d = (TDecimate *) *instanceData;

    return d->GetFrame(n, activationReason, frameData, frameCtx, core);
}


static void VS_CC tdecimateFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    (void)core;
    (void)vsapi;

    TDecimate *d = (TDecimate *)instanceData;

    delete d;
}


static void VS_CC tdecimateCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    (void)userData;

    int err;

    VSNodeRef *clip = vsapi->propGetNode(in, "clip", 0, nullptr); /// move lower if possible

    int mode = int64ToIntS(vsapi->propGetInt(in, "mode", 0, &err));
    if (err)
        mode = 0;

    int cycleR = int64ToIntS(vsapi->propGetInt(in, "cycleR", 0, &err));
    if (err)
        cycleR = 1;

    int cycle = int64ToIntS(vsapi->propGetInt(in, "cycle", 0, &err));
    if (err)
        cycle = 5;

    double rate = vsapi->propGetFloat(in, "rate", 0, &err);
    if (err)
        rate = 23.976;

    bool chroma = !!vsapi->propGetInt(in, "chroma", 0, &err);
    if (err)
        chroma = true;

    {
        const VSVideoInfo *vi = vsapi->getVideoInfo(clip);
        if (vi->format && vi->format->colorFamily == cmGray)
            chroma = false;
    }

    double dupThresh = vsapi->propGetFloat(in, "dupThresh", 0, &err);
    if (err)
        dupThresh = mode == 7 ? (chroma ? 0.4 : 0.5)
                              : (chroma ? 1.1 : 1.4);

    double vidThresh = vsapi->propGetFloat(in, "vidThresh", 0, &err);
    if (err)
        vidThresh = mode == 7 ? (chroma ? 3.5 : 4.0)
                              : (chroma ? 1.1 : 1.4);

    double sceneThresh = vsapi->propGetFloat(in, "sceneThresh", 0, &err);
    if (err)
        sceneThresh = 15;

    int hybrid = int64ToIntS(vsapi->propGetInt(in, "hybrid", 0, &err));
    if (err)
        hybrid = 0;

    int vidDetect = int64ToIntS(vsapi->propGetInt(in, "vidDetect", 0, &err));
    if (err)
        vidDetect = 3;

    int conCycle = int64ToIntS(vsapi->propGetInt(in, "conCycle", 0, &err));
    if (err)
        conCycle = vidDetect >= 3 ? 1 : 2;

    int conCycleTP = int64ToIntS(vsapi->propGetInt(in, "conCycleTP", 0, &err));
    if (err)
        conCycleTP = vidDetect >= 3 ? 1 : 2;

    const char *ovr = vsapi->propGetData(in, "ovr", 0, &err);
    if (err)
        ovr = "";

    const char *output = vsapi->propGetData(in, "output", 0, &err);
    if (err)
        output = "";

    const char *input = vsapi->propGetData(in, "input", 0, &err);
    if (err)
        input = "";

    const char *tfmIn = vsapi->propGetData(in, "tfmIn", 0, &err);
    if (err)
        tfmIn = "";

    const char *mkvOut = vsapi->propGetData(in, "mkvOut", 0, &err);
    if (err)
        mkvOut = "";

    int nt = int64ToIntS(vsapi->propGetInt(in, "nt", 0, &err));
    if (err)
        nt = 0;

    int blockx = int64ToIntS(vsapi->propGetInt(in, "blockx", 0, &err));
    if (err)
        blockx = 32;

    int blocky = int64ToIntS(vsapi->propGetInt(in, "blocky", 0, &err));
    if (err)
        blocky = 32;

    bool debug = !!vsapi->propGetInt(in, "debug", 0, &err);
    if (err)
        debug = false;

    bool display = !!vsapi->propGetInt(in, "display", 0, &err);
    if (err)
        display = false;

    int vfrDec = int64ToIntS(vsapi->propGetInt(in, "vfrDec", 0, &err));
    if (err)
        vfrDec = 1;

    bool batch = !!vsapi->propGetInt(in, "batch", 0, &err);
    if (err)
        batch = false;

    bool tcfv1 = !!vsapi->propGetInt(in, "tcfv1", 0, &err);
    if (err)
        tcfv1 = true;

    bool se = !!vsapi->propGetInt(in, "se", 0, &err);
    if (err)
        se = false;

    bool exPP = !!vsapi->propGetInt(in, "exPP", 0, &err);
    if (err)
        exPP = false;

    int maxndl = int64ToIntS(vsapi->propGetInt(in, "maxndl", 0, &err));
    if (err)
        maxndl = -200;

    bool m2PA = !!vsapi->propGetInt(in, "m2PA", 0, &err);
    if (err)
        m2PA = false;

    bool denoise = !!vsapi->propGetInt(in, "denoise", 0, &err);
    if (err)
        denoise = false;

    bool noblend = !!vsapi->propGetInt(in, "noblend", 0, &err);
    if (err)
        noblend = true;

    bool ssd = !!vsapi->propGetInt(in, "ssd", 0, &err);
    if (err)
        ssd = false;

    bool hint = !!vsapi->propGetInt(in, "hint", 0, &err);
    if (err)
        hint = true;

    VSNodeRef *clip2 = vsapi->propGetNode(in, "clip2", 0, &err);
    if (err)
        clip2 = vsapi->cloneNodeRef(clip); // simplifies the code in the getframe functions

    int sdlim = int64ToIntS(vsapi->propGetInt(in, "sdlim", 0, &err));
    if (err)
        sdlim = 0;

    int opt = int64ToIntS(vsapi->propGetInt(in, "opt", 0, &err));
    if (err)
        opt = 4;

    const char *orgOut = vsapi->propGetData(in, "orgOut", 0, &err);
    if (err)
        orgOut = "";


    TDecimate *tdecimate_data;

    try {
        tdecimate_data = new TDecimate(clip, mode, cycleR, cycle, rate, dupThresh, vidThresh, sceneThresh, hybrid, vidDetect, conCycle, conCycleTP, ovr, output, input, tfmIn, mkvOut, nt, blockx, blocky, debug, display, vfrDec, batch, tcfv1, se, chroma, exPP, maxndl, m2PA, denoise, noblend, ssd, hint, clip2, sdlim, opt, orgOut, vsapi, core);
    } catch (const TIVTCError& e) {
        vsapi->setError(out, e.what());

        vsapi->freeNode(clip);
        vsapi->freeNode(clip2);

        return;
    }

    int filter_modes[8] = {
        fmParallelRequests,
        fmParallelRequests,
        fmUnordered, // Either fmUnordered or fmParallelRequests. I figured out which one but I didn't write it down and forgot.
        fmSerial,
        fmParallel,
        fmParallel,
        fmParallel,
        fmUnordered
    };
    int filter_flags[8] = {
        0,
        0,
        0,
        nfMakeLinear,
        0,
        0,
        0,
        0
    };

    vsapi->createFilter(in, out, "TDecimate", tdecimateInit, tdecimateGetFrame, tdecimateFree, filter_modes[mode], filter_flags[mode], tdecimate_data, core);

    if (vsapi->getError(out))
        return;


    if (display) {
        // text.FrameProps won't print the TDecimateDisplay property because it is too long,
        // so we use text.Text with std.FrameEval instead.
        VSMap *params = vsapi->createMap();
        VSNodeRef *node = vsapi->propGetNode(out, "clip", 0, nullptr);
        vsapi->propSetNode(params, "clip", node, paReplace);
        vsapi->propSetNode(params, "prop_src", node, paReplace);
        VSFuncRef *displayFuncRef = vsapi->createFunc(tivtcDisplayFunc<DisplayTDecimate>, vsapi->cloneNodeRef(node), (VSFreeFuncData)vsapi->freeNode, core, vsapi);
        vsapi->freeNode(node);
        vsapi->propSetFunc(params, "eval", displayFuncRef, paReplace);
        vsapi->freeFunc(displayFuncRef);
        VSPlugin *std_plugin = vsapi->getPluginById("com.vapoursynth.std", core);
        VSMap *ret = vsapi->invoke(std_plugin, "FrameEval", params);
        vsapi->freeMap(params);
        if (vsapi->getError(ret)) {
            char error[512] = { 0 };
            snprintf(error, 512, "TDecimate: failed to invoke std.FrameEval: %s", vsapi->getError(ret));
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

    registerFunc("TDecimate",
                 "clip:clip;"
                 "mode:int:opt;"
                 "cycleR:int:opt;"
                 "cycle:int:opt;"
                 "rate:float:opt;"
                 "dupThresh:float:opt;"
                 "vidThresh:float:opt;"
                 "sceneThresh:float:opt;"
                 "hybrid:int:opt;"
                 "vidDetect:int:opt;"
                 "conCycle:int:opt;"
                 "conCycleTP:int:opt;"
                 "ovr:data:opt;"
                 "output:data:opt;"
                 "input:data:opt;"
                 "tfmIn:data:opt;"
                 "mkvOut:data:opt;"
                 "nt:int:opt;"
                 "blockx:int:opt;"
                 "blocky:int:opt;"
                 "debug:int:opt;"
                 "display:int:opt;"
                 "vfrDec:int:opt;"
                 "batch:int:opt;"
                 "tcfv1:int:opt;"
                 "se:int:opt;"
                 "chroma:int:opt;"
                 "exPP:int:opt;"
                 "maxndl:int:opt;"
                 "m2PA:int:opt;"
                 "denoise:int:opt;"
                 "noblend:int:opt;"
                 "ssd:int:opt;"
                 "hint:int:opt;"
                 "clip2:clip:opt;"
                 "sdlim:int:opt;"
                 "opt:int:opt;"
                 "orgOut:data:opt;"
                 , tdecimateCreate, nullptr, plugin);
}
