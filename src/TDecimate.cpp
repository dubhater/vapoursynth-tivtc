/*
**                    TIVTC for AviSynth 2.6 interface
**
**   TIVTC includes a field matching filter (TFM) and a decimation
**   filter (TDecimate) which can be used together to achieve an
**   IVTC or for other uses. TIVTC currently supports 8 bit planar YUV and
**   YUY2 colorspaces.
**
**   Copyright (C) 2004-2008 Kevin Stone, additional work (C) 2017-2018 pinterf
**   orgOut addition: (C)2018 8day
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

#include "TDecimate.h"
#include "TDecimateASM.h"
#include "TCommonASM.h"
#include <inttypes.h>
#include <algorithm>

const VSFrameRef *TDecimate::GetFrame(int n, int activationReason, void **frameData, VSFrameContext *frameCtx, VSCore *core)
{
  if (n < 0) n = 0;
  else if (n > nfrmsN) n = nfrmsN;

  const VSFrameRef * dst = nullptr;

  try {
      if (mode < 2) dst = GetFrameMode01(n, activationReason, frameData, frameCtx, core);     // most similar/longest string
      else if (mode == 2) dst = GetFrameMode2(n, activationReason, frameData, frameCtx, core); // arbitrary framerate
      else if (mode == 3) dst = GetFrameMode3(n, activationReason, frameData, frameCtx, core); // single pass mkv-vfr
      else if (mode == 4) dst = GetFrameMode4(n, activationReason, frameCtx, core); // metrics output
      else if (mode == 5 || mode == 6) dst = GetFrameMode56(n, activationReason, frameCtx, core); // second pass of two pass hybrid
    //  else if (mode == 6) dst = GetFrameMode6(n, activationReason, frameCtx, core); // second pass for 120fps to vfr
      else if (mode == 7) dst = GetFrameMode7(n, activationReason, frameData, frameCtx, core); // arbitrary framerate v2
  } catch (const TIVTCError &e) {
      vsapi->setFilterError(e.what(), frameCtx);
      return nullptr;
  }

  return dst;
}


// For modes 0, 1, and 3
enum OutputType {
    SingleFrame = 0,
    TwoFramesBlended,
};

struct OutputInfo {
    OutputType type;
    int f1, f2;
    double a1, a2;
    std::vector<uint64_t> metrics;

    // For display only:
    int requested_frame_number; // requested from TDecimate
    int chosen_frame_number; // chosen from child/clip2 to return
    bool film;

    void set(OutputType _type, int _f1, int _f2, double _a1, double _a2, int _requested, int _chosen, bool _film) {
        type = _type;
        f1 = _f1;
        f2 = _f2;
        a1 = _a1;
        a2 = _a2;
        requested_frame_number = _requested;
        chosen_frame_number = _chosen;
        film = _film;
    }

    void requestFrames(VSNodeRef *clip, VSFrameContext *frameCtx, const VSAPI *vsapi) {
        vsapi->requestFrameFilter(f1, clip, frameCtx);

        if (type == TwoFramesBlended)
            vsapi->requestFrameFilter(f2, clip, frameCtx);
    }
};
////////////////////


// PF 180131 uses usehints! but no problem, its runtime
const VSFrameRef * TDecimate::GetFrameMode01(int n, int activationReason, void **frameData, VSFrameContext *frameCtx, VSCore *core)
{
    if (activationReason != arInitial && activationReason != arAllFramesReady)
        return nullptr;

  int EvalGroup;
  if (hybrid != 3) EvalGroup = ((int)(n / (cycle - cycleR))) * cycle;
  else EvalGroup = ((int)(n / cycle)) * cycle;

  bool first_frame_in_cycle = hybrid != 3 ? n % (cycle - cycleR) == 0
                                          : n % cycle == 0;

  if (activationReason == arInitial) {
      for (int i = EvalGroup - cycle - 1; i < EvalGroup + (cycle * 3); i++)
          vsapi->requestFrameFilter(std::max(0, std::min(i, vi_child->numFrames - 1)), child, frameCtx);

      return nullptr;
  } else if (activationReason == arAllFramesReady && *frameData != nullptr) {
      const OutputInfo *o = (const OutputInfo *)*frameData;

      VSFrameRef *dst = nullptr;
      const VSFrameRef *frame1 = vsapi->getFrameFilter(o->f1, clip2, frameCtx);

      if (o->type == SingleFrame) {
          dst = vsapi->copyFrame(frame1, core);
      } else if (o->type == TwoFramesBlended) {
          const VSFrameRef *frame2 = vsapi->getFrameFilter(o->f2, clip2, frameCtx);
          dst = vsapi->newVideoFrame(vi_clip2->format, vi_clip2->width, vi_clip2->height, frame1, core);
          blendFrames(frame1, frame2, dst, o->a1);
          vsapi->freeFrame(frame2);
      }
      vsapi->freeFrame(frame1);

      if (display)
          displayOutput(dst, o->requested_frame_number, o->chosen_frame_number, o->film, o->a1, o->a2, o->f1, o->f2);

      VSMap *props = vsapi->getFramePropsRW(dst);

      if (first_frame_in_cycle) {
        vsapi->propSetInt(props, PROP_TDecimateCycleStart, EvalGroup, paReplace);
        vsapi->propSetIntArray(props, PROP_TDecimateCycleMaxBlockDiff, (const int64_t *)o->metrics.data(), cycle);
      }
      vsapi->propSetInt(props, PROP_TDecimateOriginalFrame, o->f1, paReplace);

      delete o;

      return dst;
  }

  // rerunFromStart is only executed if all the metrics are already available from the "input" file (fullInfo is true)
  // thus it never requests any frames, it always does calculations from the stored metrics
  if (n != lastn + 1 && EvalGroup >= cycle && fullInfo && (EvalGroup != curr.frame ||
    EvalGroup - cycle != prev.frame || EvalGroup + cycle != next.frame))
    rerunFromStart(EvalGroup, frameCtx, core);

  lastn = n;
//  if (ecf) child->SetCacheHints(EvalGroup, -20);
  if (curr.frame != EvalGroup)
  {
    prev = curr;
    if (prev.frame != EvalGroup - cycle)
    {
      prev.setFrame(EvalGroup - cycle);
      getOvrCycle(prev, false);
      calcMetricCycle(prev, true, true, core, frameCtx);
      if (hybrid > 0)
      {
        checkVideoMatches(prev, prev);
        checkVideoMetrics(prev, vidThresh);
      }
      if (output.size()) addMetricCycle(prev);
    }
    curr = next;
    if (curr.frame != EvalGroup)
    {
      curr.setFrame(EvalGroup);
      getOvrCycle(curr, false);
      calcMetricCycle(curr, true, true, core, frameCtx);
      if (hybrid > 0)
      {
        checkVideoMatches(prev, curr);
        checkVideoMetrics(curr, vidThresh);
      }
      if (output.size()) addMetricCycle(curr);
    }
    next = nbuf;
    if (next.frame != EvalGroup + cycle)
      next.setFrame(EvalGroup + cycle);
    getOvrCycle(next, false);
    calcMetricCycle(next, true, true, core, frameCtx);
    if (hybrid > 0)
    {
      checkVideoMatches(curr, next);
      checkVideoMetrics(next, vidThresh);
    }
    if (output.size()) addMetricCycle(next);
    nbuf.setFrame(EvalGroup + cycle * 2);
    getOvrCycle(nbuf, false);
    if (hybrid > 0 && curr.type > 1)
    {
      int scenetest = curr.sceneDetect(prev, next, sceneThreshU);
      bool isVid = ((curr.type == 2 || curr.type == 4) && !curr.isfilmd2v && // matches
        (prev.type == 5 || (prev.type == 2 && (vidDetect == 0 || vidDetect == 2)) || prev.type == 4 ||
          next.type == 5 || (next.type == 2 && (vidDetect == 0 || vidDetect == 2)) || next.type == 4 ||
          conCycle == 1 || scenetest != -20));
      bool isVid2 = ((curr.type == 3 || curr.type == 4) && !curr.isfilmd2v && // metrics
        (prev.type == 5 || (prev.type == 3 && (vidDetect == 1 || vidDetect == 2)) || prev.type == 4 ||
          next.type == 5 || (next.type == 3 && (vidDetect == 1 || vidDetect == 2)) || next.type == 4 ||
          conCycle == 1 || scenetest != -20));
      if (curr.type == 5 || (vidDetect == 0 && isVid) || (vidDetect == 1 && isVid2) ||
        (vidDetect == 2 && (isVid2 || isVid)) || (vidDetect == 3 && (isVid2 && isVid)))
      {
        int temp = curr.sceneDetect(prev, next, sceneThreshU);
        if (temp != -20 && hybrid != 3)
        {
          for (int p = curr.cycleS; p < curr.cycleE; ++p) curr.decimate[p] = curr.decimate2[p] = 0;
          curr.decimate[temp] = curr.decimate2[temp] = 1;
          curr.blend = 2;
          curr.decSet = true;
        }
        else curr.blend = 1;
      }
      else { goto novidjump; }
    }
    else
    {
    novidjump:
      if (mode == 0)
      {
        mostSimilarDecDecision(prev, curr, next);
      }
      else
      {
        prev.setDups(dupThresh);
        curr.setDups(dupThresh);
        next.setDups(dupThresh);
        findDupStrings(prev, curr, next);
      }
      if (curr.blend == 3)
      {
        int tscene = curr.sceneDetect(prev, next, sceneThreshU);
        if (tscene != -20 && curr.decimate[tscene] == 1 && hybrid != 3)
        {
          curr.decimate[tscene] = curr.decimate2[tscene] = 0;
          curr.blend = 0;
        }
      }
      if (curr.blend != 3) curr.blend = 0;
    }
//    if (debug) debugOutput1(n, curr.blend == 1 ? false : true, curr.blend);
  }
  for (int j = nbuf.cycleS; j < nbuf.cycleE; ++j)
  {
    if (nbuf.diffMetricsU[j] == UINT64_MAX || nbuf.diffMetricsUF[j] == UINT64_MAX ||
      nbuf.match[j] == -20)
    {
      calcMetricPreBuf(next.frameEO - 1 + j, next.frameEO + j, j, vi_child, true, true, frameCtx, core);
      break;
    }
  }
  
  OutputInfo *o = new OutputInfo;
  *frameData = (void *)o;

  if (first_frame_in_cycle) {
      o->metrics.assign(curr.diffMetricsU, curr.diffMetricsU + cycle);
//      o->metrics.resize(cycle);
//          memcpy(o->metrics.data(), curr.diffMetricsU, cycle * sizeof(*o->metrics.data()));
  }

  if (curr.blend == 3)  // 2 dups detected
  {
    if (hybrid == 3)  // blend up-convert (hybrid=3 leaves video untouched)
    {
      bool tsc = false;
      int tscene = curr.sceneDetect(prev, next, sceneThreshU);
      if (tscene == -20)
      {
        tscene = next.sceneDetect(sceneThreshU);
        if (tscene == 0 && next.diffMetricsUF[next.cycleS] > sceneThreshU &&
          curr.sceneDetect(sceneThreshU) == -20)
        {
          tscene = curr.length;
          tsc = true;
        }
        else tscene = -20;
      }
      else if (tscene == 0 && curr.diffMetricsUF[curr.cycleS] > sceneThreshU) tsc = true;
      double a1, a2; // a2 = 1.0 - a1
      int f1, f2;
      calcBlendRatios2(a1, a2, f1, f2, n, prev, curr, next, 2);

      o->type = SingleFrame;

      if (a1 >= 1.0)
      {
        // #1 is 100%
        o->f1 = f1;
      }
      else if (a2 >= 1.0)
      {
        // #2 is 100%
        o->f1 = f2;
      }
      else if (tscene >= 0 &&
        ((!tsc && (f1 == curr.frame + tscene || f2 == curr.frame + tscene + 1)) ||
          (tsc && (f1 == curr.frame + tscene - 1 || f2 == curr.frame + tscene))))
      {
        if (!tsc)
        {
          f1 = curr.frame + tscene;
          f2 = curr.frame + tscene + 1;
        }
        else
        {
          f1 = curr.frame + tscene - 1;
          f2 = curr.frame + tscene;
        }
        a1 = 1.0; // make #1 as 100%
        a2 = 0.0;

        o->f1 = f1;
      }
      else
      {
          o->type = TwoFramesBlended;
          o->f1 = f1;
          o->f2 = f2;
      }
//      if (debug) debugOutput2(n, 0, true, f1, f2, a1, a2);

      o->requested_frame_number = n;
      o->chosen_frame_number = 0;
      o->film = true;
      o->a1 = a1;
      o->a2 = a2;
      o->requestFrames(clip2, frameCtx, vsapi);
      return nullptr;
    }
    // drop one dup and replace the other with a blend of its neighbors
    // (if noblend=false)... or if one is next to a scenechange then just
    // leave it (will be much less noticeable than a blend).
    int ret = n % (cycle - cycleR), y, f1 = 0, f2 = 0, jk;
    double a1 = 0.0, a2 = 0.0;
    int tscene = curr.sceneDetect(prev, next, sceneThreshU);
    if (tscene != -20)
    {
      for (jk = -1, y = curr.cycleS; y < curr.cycleE; ++y)
      {
        if (curr.decimate[y] == 0) ++jk;
        if (y == tscene && jk < ret) ++jk;
        if (ret == jk)
        {
          ret = y;
          break;
        }
      }
    }
    else
    {
      int d1 = -20, d2 = -20;
      for (y = curr.cycleS; y < curr.cycleE; ++y)
      {
        if (curr.decimate[y] == 1 && d1 == -20) d1 = y;
        else if (curr.decimate[y] == 1 && d2 == -20) { d2 = y; break; }
      }
      if (curr.diffMetricsU[d1] > curr.diffMetricsU[d2]) d1 = d2;
      for (jk = 0, y = curr.cycleS; y < curr.cycleE; ++y)
      {
        if (ret == jk && y != d1)
        {
          if (curr.decimate[y] == 1)
          {
            f1 = curr.frameSO + y - 1;
            f2 = curr.frameSO + y + 1;
            a1 = a2 = 0.5; // 50-50%
          }
          else ret = y;
          break;
        }
        if (y != d1) ++jk;
      }
    }

    if (f1 != 0)
    {
//      if (debug) debugOutput2(n, 0, true, f1, f2, a1, a2);
      o->set(TwoFramesBlended, f1, f2, a1, a2, n, 0, true);
    } else {
//    if (debug) debugOutput2(n, curr.frame + ret, true, f1, f2, a1, a2);
      o->set(SingleFrame, curr.frame + ret, -69, a1, a2, n, curr.frame + ret, true);
    }

    o->requestFrames(clip2, frameCtx, vsapi);
    return nullptr;
    // end of curr_blend == 3
  }
  else if (curr.blend != 1)  // normal film (1 dup)
  {
    if (hybrid == 3)  // blend up-convert (hybrid=3 leaves video untouched)
    {
      bool tsc = false;
      int tscene = curr.sceneDetect(prev, next, sceneThreshU);
      if (tscene == -20)
      {
        tscene = next.sceneDetect(sceneThreshU);
        if (tscene == 0 && next.diffMetricsUF[next.cycleS] > sceneThreshU &&
          curr.sceneDetect(sceneThreshU) == -20)
        {
          tscene = curr.length;
          tsc = true;
        }
        else tscene = -20;
      }
      else if (tscene == 0 && curr.diffMetricsUF[curr.cycleS] > sceneThreshU) tsc = true;

      double a1, a2;
      int f1, f2;
      calcBlendRatios2(a1, a2, f1, f2, n, prev, curr, next, 1);

      o->type = SingleFrame;

      if (a1 >= 1.0)
      {
        o->f1 = f1;
      }
      else if (a2 >= 1.0)
      {
        o->f1 = f2;
      }
      else if (tscene >= 0 &&
        ((!tsc && (f1 == curr.frame + tscene || f2 == curr.frame + tscene + 1)) ||
        (tsc && (f1 == curr.frame + tscene - 1 || f2 == curr.frame + tscene))))
      {
        if (!tsc)
        {
          f1 = curr.frame + tscene;
          f2 = curr.frame + tscene + 1;
        }
        else
        {
          f1 = curr.frame + tscene - 1;
          f2 = curr.frame + tscene;
        }
        a1 = 1.0; // make #1 as 100%
        a2 = 0.0;

        o->f1 = f1;
      }
      else
      {
          o->type = TwoFramesBlended;
          o->f1 = f1;
          o->f2 = f2;
      }
//      if (debug) debugOutput2(n, 0, true, f1, f2, a1, a2);

      o->requested_frame_number = n;
      o->chosen_frame_number = 0;
      o->film = true;
      o->a1 = a1;
      o->a2 = a2;
      o->requestFrames(clip2, frameCtx, vsapi);
      return nullptr;
    }
    // normal drop operation
    int ret = curr.getNonDec(n % (cycle - cycleR));
    if (ret == -1)
    {
      curr.debugOutput();
      curr.debugMetrics(curr.length);
      vsapi->setFilterError("TDecimate:  major internal error. Couldn't figure out which frame to return. Please report this ASAP!", frameCtx);
      return nullptr;
    }
//    if (debug) debugOutput2(n, curr.frame + ret, curr.blend == 2 ? false : true, 0, 0, 0.0, 0.0);

    o->set(SingleFrame, curr.frame + ret, -69, 0.0, 0.0, n, curr.frame + ret, curr.blend != 2);
    o->requestFrames(clip2, frameCtx, vsapi);
    return nullptr;
  }
  else  // video (no dups)
  {
    if (hybrid == 3) // return source frame (hybrid=3 leaves video untouched)
    {
//      if (debug) debugOutput2(n, n, false, 0, 0, 0.0, 0.0);

        // So.... did it not drop any frames up to this one? That's the only way output frame n corresponds to input frame n.
      o->set(SingleFrame, n, -69, 0.0, 0.0, n, n, false);
      o->requestFrames(clip2, frameCtx, vsapi);
      return nullptr;
    }
    // blend down-convert (hybrid=1 leaves film untouched)

    double a1, a2;
    int f1, f2;
    calcBlendRatios(a1, a2, f1, f2, n, curr.frame, curr.cycleE - curr.cycleS);

    o->type = SingleFrame;

    if (a1 >= 1.0)
    {
      o->f1 = f1;
    }
    else if (a2 >= 1.0)
    {
      o->f1 = f2;
    }
    else
    {
        o->type = TwoFramesBlended;
        o->f1 = f1;
        o->f2 = f2;
    }

//    if (debug) debugOutput2(n, 0, false, f1, f2, a1, a2);

    o->requested_frame_number = n;
    o->chosen_frame_number = 0;
    o->film = false;
    o->a1 = a1;
    o->a2 = a2;
    o->requestFrames(clip2, frameCtx, vsapi);
    return nullptr;
  }
}

void setBlack(VSFrameRef *dst, const VSAPI *vsapi)
{
    const VSFormat *format = vsapi->getFrameFormat(dst);
  const int np = format->numPlanes;

  for (int b = 0; b < np; ++b)
  {
    const int plane = b;
    uint8_t* dstp = vsapi->getWritePtr(dst, plane);
    const int pitch = vsapi->getStride(dst, plane);
    const size_t height = vsapi->getFrameHeight(dst, plane);

    if (b == 0)
      memset(dstp, 0, pitch * height); // luma
    else {
      // chroma
      const int bits_per_pixel = format->bitsPerSample;
      if (bits_per_pixel == 8)
        memset(dstp, 128, pitch * height);
      else
        std::fill_n((uint16_t*)dstp, pitch * height / sizeof(uint16_t), 128 << (bits_per_pixel - 8));
    }
  }
}

const VSFrameRef * TDecimate::GetFrameMode3(int n, int activationReason, void **frameData, VSFrameContext *frameCtx, VSCore *core)
{
  static int vidC = 0;
  static int filmC = 0;
  static int longestT = 0;
  static int longestV = 0;
  static int countVT = 0;
  static double timestamp = 0.0;

  if (activationReason != arInitial && activationReason != arAllFramesReady)
      return nullptr;

  if (activationReason == arInitial) {
      for (int i = lastCycle - 1; i < lastCycle + (cycle * 4); i++)
          vsapi->requestFrameFilter(std::max(0, std::min(i, vi_child->numFrames - 1)), child, frameCtx);

      return nullptr;
  } else if (activationReason == arAllFramesReady && *frameData != nullptr) {
      const OutputInfo *o = (const OutputInfo *)*frameData;

      VSFrameRef *dst = nullptr;
      const VSFrameRef *frame1 = vsapi->getFrameFilter(o->f1, clip2, frameCtx);

      if (o->type == SingleFrame) {
          dst = vsapi->copyFrame(frame1, core);
      } else if (o->type == TwoFramesBlended) {
          const VSFrameRef *frame2 = vsapi->getFrameFilter(o->f2, clip2, frameCtx);
          dst = vsapi->newVideoFrame(vi_clip2->format, vi_clip2->width, vi_clip2->height, frame1, core);
          blendFrames(frame1, frame2, dst, o->a1);
          vsapi->freeFrame(frame2);
      }
      vsapi->freeFrame(frame1);

      if (display)
          displayOutput(dst, o->requested_frame_number, o->chosen_frame_number, o->film, o->a1, o->a2, o->f1, o->f2);

      delete o;

      return dst;
  }

  if (n == 0)
  {
    vidC = filmC = longestT = longestV = countVT = 0;
    timestamp = 0.0;
  }
  if (linearCount != n) {
      vsapi->setFilterError("TDecimate:  non-linear access detected in mode 3!", frameCtx);
      return nullptr;
  }
  ++linearCount;
  if (n == 0 || n - lastGroup == retFrames)
  {
    lastGroup = n;
    lastCycle += cycle;
//    if (ecf) child->SetCacheHints(lastCycle, -20);
    prev = curr;
    if (prev.frame != lastCycle - cycle)
    {
      prev.setFrame(lastCycle - cycle);
      getOvrCycle(prev, false);
      calcMetricCycle(prev, true, true, core, frameCtx);
      checkVideoMatches(prev, prev);
      checkVideoMetrics(prev, vidThresh);
      if (output.size()) addMetricCycle(prev);
    }
    curr = next;
    if (curr.frame != lastCycle)
    {
      curr.setFrame(lastCycle);
      getOvrCycle(curr, false);
      calcMetricCycle(curr, true, true, core, frameCtx);
      checkVideoMatches(prev, curr);
      checkVideoMetrics(curr, vidThresh);
      if (output.size()) addMetricCycle(curr);
    }
    next = nbuf;
    if (next.frame != lastCycle + cycle)
      next.setFrame(lastCycle + cycle);
    getOvrCycle(next, false);
    calcMetricCycle(next, true, true, core, frameCtx);
    checkVideoMatches(curr, next);
    checkVideoMetrics(next, vidThresh);
    if (output.size()) addMetricCycle(next);

    nbuf.setFrame(lastCycle + cycle * 2);
    getOvrCycle(nbuf, false);
    int scenetest = curr.sceneDetect(prev, next, sceneThreshU);
    bool isVid = ((curr.type == 2 || curr.type == 4) && !curr.isfilmd2v && // matches
      (prev.type == 5 || (prev.type == 2 && (vidDetect == 0 || vidDetect == 2)) || prev.type == 4 ||
        next.type == 5 || (next.type == 2 && (vidDetect == 0 || vidDetect == 2)) || next.type == 4 ||
        conCycle == 1 || scenetest != -20));
    bool isVid2 = ((curr.type == 3 || curr.type == 4) && !curr.isfilmd2v && // metrics
      (prev.type == 5 || (prev.type == 3 && (vidDetect == 1 || vidDetect == 2)) || prev.type == 4 ||
        next.type == 5 || (next.type == 3 && (vidDetect == 1 || vidDetect == 2)) || next.type == 4 ||
        conCycle == 1 || scenetest != -20));
    if (curr.type == 5 || (vidDetect == 0 && isVid) || (vidDetect == 1 && isVid2) ||
      (vidDetect == 2 && (isVid2 || isVid)) || (vidDetect == 3 && (isVid2 && isVid)))
    {
      retFrames = cycle;
      vidC += (curr.frame + cycle <= nfrms ? cycle : nfrms - curr.frame + 1);
      longestT += (curr.frame + cycle <= nfrms ? cycle : nfrms - curr.frame + 1);
      if (!tcfv1)
      {
        int stop = (lastCycle + cycle <= nfrms ? cycle : nfrms - lastCycle + 1);
        for (int u = 0; u < stop; ++u)
        {
          fprintf(mkvOutF, "%3.6f\n", timestamp);
          timestamp += 1000.0 / fps;
        }
      }
    }
    else
    {
      if (vfrDec != 1)
      {
        mostSimilarDecDecision(prev, curr, next);
      }
      else
      {
        prev.setDups(dupThresh);
        curr.setDups(dupThresh);
        next.setDups(dupThresh);
        findDupStrings(prev, curr, next);
      }
      filmC += (curr.frame + cycle <= nfrms ? cycle : nfrms - curr.frame + 1);
      if (retFrames == cycle)
      {
        if (longestT > longestV) longestV = longestT;
        ++countVT;
        longestT = 0;
      }
      if (curr.blend != 3)
      {
        if (!tcfv1)
        {
          int stop = (lastCycle + cycle <= nfrms ? cycle - cycleR : nfrms - lastCycle + 1 - cycleR);
          for (int u = 0; u < stop; ++u)
          {
            fprintf(mkvOutF, "%3.6f\n", timestamp);
            timestamp += 1000.0 / mkvfps;
          }
        }
        retFrames = cycle - cycleR;
      }
      else
      {
        if (lastType > 0 && tcfv1)
          fprintf(mkvOutF, "%d,%d,%4.6f\n", lastGroup - (lastType*(cycle - cycleR)), lastGroup - 1, mkvfps);
        if (!tcfv1)
        {
          int stop = (lastCycle + cycle <= nfrms ? cycle - cycleR - 1 : nfrms - lastCycle + 1 - cycleR - 1);
          for (int u = 0; u < stop; ++u)
          {
            fprintf(mkvOutF, "%3.6f\n", timestamp);
            timestamp += 1000.0 / mkvfps2;
          }
        }
        else fprintf(mkvOutF, "%d,%d,%4.6f\n", lastGroup, lastGroup + cycle - cycleR - 2, mkvfps2);
        retFrames = cycle - cycleR - 1;
      }
    }
    if (retFrames == cycle && lastType > 0 && tcfv1)
      fprintf(mkvOutF, "%d,%d,%4.6f\n", lastGroup - (lastType*(cycle - cycleR)), lastGroup - 1, mkvfps);
    if (retFrames == cycle - cycleR) ++lastType;
    else lastType = 0;
//    if (debug) debugOutput1(n, retFrames == cycle ? false : true, curr.blend);
  }

  for (int j = nbuf.cycleS; j < nbuf.cycleE; ++j)
  {
    if (nbuf.diffMetricsU[j] == UINT64_MAX || nbuf.diffMetricsUF[j] == UINT64_MAX ||
      nbuf.match[j] == -20)
    {
      calcMetricPreBuf(next.frameEO - 1 + j, next.frameEO + j, j, vi_child, true, true, frameCtx, core);
      break;
    }
  }

  if (retFrames == cycle)
  {
    if (lastCycle + (n - lastGroup) > nfrms)
    {
      retFrames = -1;
      lastFrame = n - 1;
      fprintf(mkvOutF, "# TDecimate Mode 3:  Last Frame = %d\n", lastFrame);
    }
    else
    {
//      if (debug) debugOutput2(n, lastCycle + (n - lastGroup), false, 0, 0, 0.0, 0.0);

      OutputInfo *o = new OutputInfo;
      *frameData = (void *)o;

      o->set(SingleFrame, lastCycle + (n - lastGroup), -69, 0.0, 0.0, n, lastCycle + (n - lastGroup), false);
      o->requestFrames(clip2, frameCtx, vsapi);
      return nullptr;
    }
  }
  else if (retFrames == cycle - cycleR || (curr.blend == 3 && retFrames == cycle - cycleR - 1))
  {
    int ret = curr.getNonDec(n - lastGroup);
    if ((ret >= 0 && curr.frame + ret > nfrms) || ret < 0)
    {
      retFrames = -1;
      lastFrame = n - 1;
      if (lastType > 0)
      {
        if (tcfv1) fprintf(mkvOutF, "%d,%d,%4.6f\n", lastGroup - ((lastType - 1)*(cycle - cycleR)), lastFrame, mkvfps);
        lastType = 0;
      }
      fprintf(mkvOutF, "# TDecimate Mode 3:  Last Frame = %d\n", lastFrame);
    }
    else
    {
//      if (debug) debugOutput2(n, curr.frame + ret, true, 0, 0, 0.0, 0.0);

      OutputInfo *o = new OutputInfo;
      *frameData = (void *)o;

      o->set(SingleFrame, curr.frame + ret, -69, 0.0, 0.0, n, curr.frame + ret, true);
      o->requestFrames(clip2, frameCtx, vsapi);
      return nullptr;
    }
  }

  if (retFrames == -1 && mkvOutF != nullptr)
  {
    double filmCf = ((double)(filmC) / (double)(nfrms + 1))*100.0;
    double videoCf = ((double)(vidC) / (double)(nfrms + 1))*100.0;
    fprintf(mkvOutF, "# vfr stats:  %05.2f%c film  %05.2f%c video\n", filmCf, '%', videoCf, '%');
    fprintf(mkvOutF, "# vfr stats:  %d - film  %d - video  %d - total\n", filmC, vidC, nfrms + 1);
    fprintf(mkvOutF, "# vfr stats:  longest vid section - %d frames\n", longestV);
    fprintf(mkvOutF, "# vfr stats:  # of detected vid sections - %d", countVT);
    fclose(mkvOutF);
    mkvOutF = nullptr;
  }

  if (retFrames <= -306 && se) {
      vsapi->setFilterError("TDecimate:  mode 3 finished (early termination)!", frameCtx);
      return nullptr;
  }

  if (retFrames <= -305)
  {
      // I refuse to copy the text drawing code.

      std::string last = "Mode 3:  Last Actual Frame = " + std::to_string(lastFrame);

      VSPlugin *std_plugin = vsapi->getPluginById("com.vapoursynth.std", core);
      VSPlugin *text_plugin = vsapi->getPluginById("com.vapoursynth.text", core);

      VSMap *args = vsapi->createMap();
      vsapi->propSetNode(args, "clip", clip2, paReplace);
      VSMap *ret = vsapi->invoke(std_plugin, "BlankClip", args);
      vsapi->clearMap(args);
      if (vsapi->getError(ret)) {
          std::string msg = "TDecimate: failed to invoke std.BlankClip to show this message: '" + last + "'. " + vsapi->getError(ret);
          vsapi->setFilterError(msg.c_str(), frameCtx);
          vsapi->freeMap(args);
          vsapi->freeMap(ret);
          return nullptr;
      }
      VSNodeRef *node = vsapi->propGetNode(ret, "clip", 0, nullptr);
      vsapi->freeMap(ret);
      vsapi->propSetNode(args, "clip", node, paReplace);
      vsapi->freeNode(node);
      node = nullptr;
      vsapi->propSetData(args, "text", last.c_str(), last.size(), paReplace);
      ret = vsapi->invoke(text_plugin, "Text", args);
      vsapi->freeMap(args);
      if (vsapi->getError(ret)) {
          std::string msg = "TDecimate: failed to invoke text.Text to show this message: '" + last + "'. " + vsapi->getError(ret);
          vsapi->setFilterError(msg.c_str(), frameCtx);
          vsapi->freeMap(ret);
          return nullptr;
      }
      node = vsapi->propGetNode(ret, "clip", 0, nullptr);
      vsapi->freeMap(ret);

      char error[160] = { 0 };
      const VSFrameRef *dst = vsapi->getFrame(0, node, error, 160);
      vsapi->freeNode(node);
      if (dst == nullptr) {
          std::string msg = "TDecimate: failed to generate the frame with this message: '" + last + "'. " + error;
          vsapi->setFilterError(msg.c_str(), frameCtx);
          return nullptr;
      }

      --retFrames;
      return dst;
  }

  return nullptr; // Should not be reachable.
}

const VSFrameRef * TDecimate::GetFrameMode4(int n, int activationReason, VSFrameContext *frameCtx, VSCore *core)
{
  if (activationReason == arInitial) {
      vsapi->requestFrameFilter(n > 0 ? n - 1 : 0, child, frameCtx);
      vsapi->requestFrameFilter(n, child, frameCtx);

      vsapi->requestFrameFilter(n, clip2, frameCtx);

      return nullptr;
  } else if (activationReason != arAllFramesReady) {
      return nullptr;
  }

  const VSFrameRef * prv = vsapi->getFrameFilter(n > 0 ? n - 1 : 0, child, frameCtx);
  const VSFrameRef * src = vsapi->getFrameFilter(n, child, frameCtx);
  int blockN = -20, xblocks;
  uint64_t metricU = UINT64_MAX, metricF = UINT64_MAX;
  getOvrFrame(n, metricU, metricF);
  if (metricU == UINT64_MAX || metricF == UINT64_MAX || display)
    metricU = calcMetric(prv, src, vi_child, blockN, xblocks, metricF, true, core);

  vsapi->freeFrame(prv);

  double metricN = (metricU*100.0) / MAX_DIFF;
//  if (debug)
//  {
//    sprintf(buf, "TDecimate:  frame %d  metric = %3.2f  metricF =  %" PRIu64 " (%3.2f)", n, metricN, metricF,
//      (double)metricF*100.0 / (double)sceneDivU);
//    OutputDebugString(buf);
//  }
  if (output.size() && metricsOutArray.size())
  {
    metricsOutArray[n << 1] = metricU;
    metricsOutArray[(n << 1) + 1] = metricF;
  }

  vsapi->freeFrame(src);
  src = vsapi->getFrameFilter(n, clip2, frameCtx);

  VSFrameRef *dst = vsapi->copyFrame(src, core);
  vsapi->freeFrame(src);

  VSMap *props = vsapi->getFramePropsRW(dst);

  if (display)
  {
//    if (blockN != -20) drawBox(src, blockx, blocky, blockN, xblocks, vi_clip2); /// figure out what drawBox does

#define SZ 160
    char buf[SZ] = { 0 };

    std::string text = "TDecimate " VERSION " by tritical\n";

    text += "Mode: 4 (metrics output)\n";
    snprintf(buf, SZ, "chroma = %s  denoise = %s\n", chroma ? "true" : "false",
      predenoise ? "true" : "false");
    text += buf;
    snprintf(buf, SZ, "Frame %d:  %3.2f  %3.2f\n", n, metricN, (double)metricF*100.0 / (double)sceneDivU);
    text += buf;
#undef SZ

      vsapi->propSetData(props, PROP_TDecimateDisplay, text.c_str(), text.size(), paReplace);
  }
  return dst;
}

const VSFrameRef * TDecimate::GetFrameMode56(int n, int activationReason, VSFrameContext *frameCtx, VSCore *core)
{
  int frame = aLUT[n];

  if (activationReason == arInitial) {
      vsapi->requestFrameFilter(frame, clip2, frameCtx);

      return nullptr;
  } else if (activationReason != arAllFramesReady) {
      return nullptr;
  }

  const VSFrameRef *src = vsapi->getFrameFilter(frame, clip2, frameCtx);

//  if (debug)
//  {
//    sprintf(buf, "TDecimate:  inframe = %d  useframe = %d  (mode = %d)", n, frame, mode);
//    OutputDebugString(buf);
//  }

  if (display)
  {
    VSFrameRef *dst = vsapi->copyFrame(src, core);
    vsapi->freeFrame(src);

#define SZ 160
    char buf[SZ] = { 0 };

    std::string text = "TDecimate " VERSION " by tritical\n";

    if (mode == 5)
        snprintf(buf, SZ, "Mode: %d (vfr)  Hybrid = %d\n", mode, hybrid);
    else
        snprintf(buf, SZ, "Mode: %d (120fps -> vfr)\n", mode);
    text += buf;
    snprintf(buf, SZ, "inframe = %d  useframe = %d\n", n, frame);
    text += buf;
#undef SZ

    VSMap *props = vsapi->getFramePropsRW(dst);
    vsapi->propSetData(props, PROP_TDecimateDisplay, text.c_str(), text.size(), paReplace);

    return dst;
  }
  return src;
}

// PF 180131 uses usehints! but its runtime alreadz, no problem
void TDecimate::rerunFromStart(const int s, VSFrameContext *frameCtx, VSCore *core)
{
  int EvalGroup = 0;
  while (EvalGroup < s)
  {
    prev = curr;
    if (prev.frame != EvalGroup - cycle)
    {
      prev.setFrame(EvalGroup - cycle);
      getOvrCycle(prev, false);
      calcMetricCycle(prev, true, true, core, frameCtx);
      if (hybrid > 0)
      {
        checkVideoMatches(prev, prev);
        checkVideoMetrics(prev, vidThresh);
      }
    }
    curr = next;
    if (curr.frame != EvalGroup)
    {
      curr.setFrame(EvalGroup);
      getOvrCycle(curr, false);
      calcMetricCycle(curr, true, true, core, frameCtx);
      if (hybrid > 0)
      {
        checkVideoMatches(prev, curr);
        checkVideoMetrics(curr, vidThresh);
      }
    }
    next.setFrame(EvalGroup + cycle);
    getOvrCycle(next, false);
    calcMetricCycle(next, true, true, core, frameCtx);
    if (hybrid > 0)
    {
      checkVideoMatches(curr, next);
      checkVideoMetrics(next, vidThresh);
    }
    if (hybrid > 0 && curr.type > 1)
    {
      int scenetest = curr.sceneDetect(prev, next, sceneThreshU);
      bool isVid = ((curr.type == 2 || curr.type == 4) && !curr.isfilmd2v && // matches
        (prev.type == 5 || (prev.type == 2 && (vidDetect == 0 || vidDetect == 2)) || prev.type == 4 ||
          next.type == 5 || (next.type == 2 && (vidDetect == 0 || vidDetect == 2)) || next.type == 4 ||
          conCycle == 1 || scenetest != -20));
      bool isVid2 = ((curr.type == 3 || curr.type == 4) && !curr.isfilmd2v && // metrics
        (prev.type == 5 || (prev.type == 3 && (vidDetect == 1 || vidDetect == 2)) || prev.type == 4 ||
          next.type == 5 || (next.type == 3 && (vidDetect == 1 || vidDetect == 2)) || next.type == 4 ||
          conCycle == 1 || scenetest != -20));
      if (curr.type == 5 || (vidDetect == 0 && isVid) || (vidDetect == 1 && isVid2) ||
        (vidDetect == 2 && (isVid2 || isVid)) || (vidDetect == 3 && (isVid2 && isVid)))
      {
        int temp = curr.sceneDetect(prev, next, sceneThreshU);
        if (temp != -20 && hybrid != 3)
        {
          for (int p = curr.cycleS; p < curr.cycleE; ++p) curr.decimate[p] = curr.decimate2[p] = 0;
          curr.decimate[temp] = curr.decimate2[temp] = 1;
          curr.blend = 2;
          curr.decSet = true;
        }
        else curr.blend = 1;
      }
      else { goto novidjump; }
    }
    else
    {
    novidjump:
      if (mode == 0) mostSimilarDecDecision(prev, curr, next);
      else
      {
        prev.setDups(dupThresh);
        curr.setDups(dupThresh);
        next.setDups(dupThresh);
        findDupStrings(prev, curr, next);
      }
      if (curr.blend == 3)
      {
        int tscene = curr.sceneDetect(prev, next, sceneThreshU);
        if (tscene != -20 && curr.decimate[tscene] == 1 && hybrid != 3)
        {
          curr.decimate[tscene] = curr.decimate2[tscene] = 0;
          curr.blend = 0;
        }
      }
      if (curr.blend != 3) curr.blend = 0;
    }
    EvalGroup += cycle;
  }
}

void TDecimate::calcMetricPreBuf(int n1, int n2, int pos, const VSVideoInfo *vit, bool scene,
  bool gethint, VSFrameContext *frameCtx, VSCore *core)
{
  if (n2 > nbuf.maxFrame || n2 < 0) return;
//  if (n2 < nbuf.frameSO || n2 >= nbuf.frameEO || n1 != n2 - 1 ||
//    nbuf.frameSO + pos != n2)
//    env->ThrowError("TDecimate:  internal error during pre-buffering (n1=%d,n2=%d,pos=%d,nbuf.FrameSO=%d,nBuf.frameEO=%d)!",
//      n1, n2, pos, nbuf.frameSO, nbuf.frameEO);
  if (n2 == 0) n1 = 0;
  int blockNI, xblocksI;
  uint64_t metricF;
  const VSFrameRef *src = nullptr;
  if (nbuf.diffMetricsU[pos] == UINT64_MAX ||
    (nbuf.diffMetricsUF[pos] == UINT64_MAX && scene))
  {
    src = vsapi->getFrameFilter(n2, child, frameCtx);
    const VSFrameRef *frame = vsapi->getFrameFilter(n1, child, frameCtx);
    nbuf.diffMetricsU[pos] = calcMetric(frame, src, vit, blockNI, xblocksI, metricF, scene, core);
    vsapi->freeFrame(frame);
    nbuf.diffMetricsN[pos] = (nbuf.diffMetricsU[pos] * 100.0) / MAX_DIFF;
    if (scene) nbuf.diffMetricsUF[pos] = metricF;
  }
  if (gethint && nbuf.match[pos] == -20)
  {
    if (!usehints) nbuf.match[pos] = -200;
    else
    {
      if (!src)
        src = vsapi->getFrameFilter(n2, child, frameCtx);

      nbuf.match[pos] = getTFMFrameProperties(src, nbuf.filmd2v[pos]);

    }
  }
  vsapi->freeFrame(src);
}

void CalcMetricsExtracted(const VSFrameRef *prevt, const VSFrameRef *currt, CalcMetricData& d, VSCore *core, const VSAPI *vsapi)
{
  VSFrameRef *prev = nullptr, *curr = nullptr;

  if (d.predenoise)
  {
    prev = vsapi->newVideoFrame(d.vi.format, d.vi.width, d.vi.height, nullptr, core);
    curr = vsapi->newVideoFrame(d.vi.format, d.vi.width, d.vi.height, nullptr, core);
    blurFrame(prevt, prev, 2, d.chroma, d.cpuFlags, core, vsapi);
    blurFrame(currt, curr, 2, d.chroma, d.cpuFlags, core, vsapi);
  }
  else
  {
    prev = vsapi->copyFrame(prevt, core);
    curr = vsapi->copyFrame(currt, core);
  }

  // core start

  const uint8_t* prvp, * curp;
  int prv_pitch, cur_pitch, width, height;

  int xblocks = ((d.vi.width + d.blockx_half) >> d.blockx_shift) + 1;
  int xblocks4 = xblocks << 2;
  int yblocks = ((d.vi.height + d.blocky_half) >> d.blocky_shift) + 1;
  int arraysize = (xblocks * yblocks) << 2;

  const bool use_sse2 = d.cpuFlags->sse2;

  memset(d.diff, 0, arraysize * sizeof(uint64_t));

  const int stop = !d.chroma ? 1 : d.vi.format->numPlanes; // luma only (!chroma) only 1 planar planes

  const int pixelsize = d.vi.format->bytesPerSample;

  for (int b = 0; b < stop; ++b)
  {
    const int plane = b;
    prvp = vsapi->getReadPtr(prev, plane);
    prv_pitch = vsapi->getStride(prev, plane) / pixelsize;
    width = vsapi->getFrameWidth(prev, plane);
    height = vsapi->getFrameHeight(prev, plane);
    curp = vsapi->getReadPtr(curr, plane);
    cur_pitch = vsapi->getStride(curr, plane) / pixelsize;

    // sum is gathered in uint64_t diff
    // diff[] entries are normalized back to 8 bit

    if (pixelsize == 1 && d.blockx == 32 && d.blocky == 32 && d.nt <= 0)
    {
      if (d.ssd && use_sse2)
        calcDiffSSD_32x32_SSE2(prvp, curp, prv_pitch, cur_pitch, width, height, plane, xblocks4, d.diff, d.chroma, &d.vi);
      else if (!d.ssd && use_sse2)
        calcDiffSAD_32x32_SSE2(prvp, curp, prv_pitch, cur_pitch, width, height, plane, xblocks4, d.diff, d.chroma, &d.vi);
      else { goto use_c; }
    }
    else if (pixelsize == 1 && d.blockx >= 16 && d.blocky >= 16 && d.nt <= 0)
    {
      // YUY2 block size 8 is really 16 in width because luma + chroma
      if (d.ssd && use_sse2)
        calcDiffSSD_Generic_SSE2(prvp, curp, prv_pitch, cur_pitch, width, height, plane, xblocks4, d.diff, d.chroma, d.blockx_shift, d.blocky_shift, d.blockx_half, d.blocky_half, &d.vi);
      else if (!d.ssd && use_sse2)
        calcDiffSAD_Generic_SSE2(prvp, curp, prv_pitch, cur_pitch, width, height, plane, xblocks4, d.diff, d.chroma, d.blockx_shift, d.blocky_shift, d.blockx_half, d.blocky_half, &d.vi);
      else { goto use_c; }
    }
    else
    {
      // fixme: have calcDiffSSD uint16_t to SIMD.
    use_c:
      if (pixelsize == 1) {
        if (!d.ssd) {
          // SAD
            calcDiff_SADorSSD_Generic_c<uint8_t, true, 1>(prvp, curp, prv_pitch, cur_pitch, width, height, plane, xblocks4, d.diff, d.chroma, d.blockx_shift, d.blocky_shift, d.blockx_half, d.blocky_half, d.nt, &d.vi);
        }
        else {
          // SSD
            calcDiff_SADorSSD_Generic_c<uint8_t, false, 1>(prvp, curp, prv_pitch, cur_pitch, width, height, plane, xblocks4, d.diff, d.chroma, d.blockx_shift, d.blocky_shift, d.blockx_half, d.blocky_half, d.nt, &d.vi);
        }
      }
      else {
        // pixelsize == 2, 10-16 bits
        if (!d.ssd) {
          // SAD
          calcDiff_SADorSSD_Generic_c<uint16_t, true, 1>((const uint16_t *)prvp, (const uint16_t*)curp, prv_pitch, cur_pitch, width, height, plane, xblocks4, d.diff, d.chroma, d.blockx_shift, d.blocky_shift, d.blockx_half, d.blocky_half, d.nt, &d.vi);
        }
        else {
          // SSD
          calcDiff_SADorSSD_Generic_c<uint16_t, false, 1>((const uint16_t*)prvp, (const uint16_t*)curp, prv_pitch, cur_pitch, width, height, plane, xblocks4, d.diff, d.chroma, d.blockx_shift, d.blocky_shift, d.blockx_half, d.blocky_half, d.nt, &d.vi);
        }
      }
    }

    if (d.metricF_needed) { // called from TDecimate. from FrameDiff:false
      if (b == 0) // luma
      {
        *d.metricF = 0;
        if (d.scene)
        {
          // planar or YUY2 luma+chroma
          if (true)
          // fix in v18: v17 was: !d.chroma instead of d.chroma
          {
            for (int x = 0; x < arraysize; x += 4)
              *d.metricF += d.diff[x];
            // d.diff entries are normalized back to 8 bit video world, done inside calcDiff_SADorSSD_Generic_c
          }
        }
      }
    }
  }

  vsapi->freeFrame(prev);
  vsapi->freeFrame(curr);
}

uint64_t TDecimate::calcMetric(const VSFrameRef *prevt, const VSFrameRef *currt, const VSVideoInfo *vit, int &blockNI,
  int &xblocksI, uint64_t &metricF, bool scene, VSCore *core) const
{
  uint64_t highestDiff = 0;

  struct CalcMetricData d;
  //d.np = np;
  d.predenoise = predenoise;
  d.vi = *vit;
  d.chroma = chroma;
  d.cpuFlags = &cpuFlags;
  d.blockx = blockx;
  d.blockx_half = blockx_half;
  d.blockx_shift = blockx_shift;
  d.blocky = blocky;
  d.blocky_half = blocky_half;
  d.blocky_shift = blocky_shift;
  d.diff = diff.get();
  d.nt = nt;
  d.ssd = ssd;

  d.metricF_needed = true;
  d.metricF = &metricF;
  d.scene = scene; 

  CalcMetricsExtracted(prevt, currt, d, core, vsapi);

  int xblocks = ((d.vi.width + d.blockx_half) >> d.blockx_shift) + 1;
  int xblocks4 = xblocks << 2;
  int yblocks = ((d.vi.height + d.blocky_half) >> d.blocky_shift) + 1;
  int arraysize = (xblocks * yblocks) << 2;

  // output parameters
  blockNI = -20;
  xblocksI = xblocks4;

  for (int x = 0; x < arraysize; ++x)
  {
    if (diff.get()[x] > highestDiff)
    {
      highestDiff = diff.get()[x];
      blockNI = x;
    }
  }
  if (blockNI == -20) blockNI = 0;
  if (ssd)
  {
    highestDiff = (uint64_t)(sqrt((double)(highestDiff)));
    metricF = (uint64_t)(sqrt((double)(metricF)));
  }
  return highestDiff;
}

// PF 180131 uses usehints!
void TDecimate::calcMetricCycle(Cycle &current, bool scene, bool hnt, VSCore *core, VSFrameContext *frameCtx) const
{
  if (current.mSet || current.cycleS == current.cycleE) 
    return;
  
  int i, w;
  uint64_t highestDiff;
  int next_num = -20, next_numd = -20;

  VSFrameRef *prv = nullptr, *nxt = nullptr;
  const VSFrameRef *prevt = nullptr, *nextt = nullptr;
  if (predenoise)
  {
    prv = vsapi->newVideoFrame(vi_child->format, vi_child->width, vi_child->height, nullptr, core);
    nxt = vsapi->newVideoFrame(vi_child->format, vi_child->width, vi_child->height, nullptr, core);
  }

  for (w = current.frameSO, i = current.cycleS; i < current.cycleE; ++i, ++w)
  {
    if ((current.match[i] != -20 || !hnt) && current.diffMetricsU[i] != UINT64_MAX &&
      (current.diffMetricsUF[i] != UINT64_MAX || !scene)) continue;
    if (predenoise)
    {
      if (current.diffMetricsU[i] != UINT64_MAX &&
        (current.diffMetricsUF[i] != UINT64_MAX || !scene))
      {
        if (current.match[i] == -20 && hnt)
        {
          if (!usehints) current.match[i] = -200;
          else
          {
              vsapi->freeFrame(nextt);
            if (frameCtx)
                nextt = vsapi->getFrameFilter(w, child, frameCtx);
            else
                nextt = vsapi->getFrame(w, child, nullptr, 0);
            next_num = w;
            current.match[i] = getTFMFrameProperties(nextt, current.filmd2v[i]);
          }
        }
        continue;
      }
      
      vsapi->freeFrame(prevt);
      if (next_num == w - 1)
        prevt = vsapi->cloneFrameRef(nextt);
      else {
          if (frameCtx)
            prevt = vsapi->getFrameFilter(w > 0 ? w - 1 : 0, child, frameCtx);
          else
            prevt = vsapi->getFrame(w > 0 ? w - 1 : 0, child, nullptr, 0);
      }

      vsapi->freeFrame(nextt);
      if (frameCtx)
        nextt = vsapi->getFrameFilter(w, child, frameCtx);
      else
        nextt = vsapi->getFrame(w, child, nullptr, 0);
      next_num = w;
      if (current.match[i] == -20 && hnt)
      {
        if (!usehints) current.match[i] = -200;
        else current.match[i] = getTFMFrameProperties(nextt, current.filmd2v[i]);
      }
      if (next_numd == w - 1) 
        copyFrame(prv, nxt, vsapi);
      else 
        blurFrame(prevt, prv, 2, chroma, &cpuFlags, core, vsapi);
      
      blurFrame(nextt, nxt, 2, chroma, &cpuFlags, core, vsapi);
      next_numd = w;
    }
    else
    {
      if (current.diffMetricsU[i] != UINT64_MAX &&
        (current.diffMetricsUF[i] != UINT64_MAX || !scene))
      {
        if (current.match[i] == -20 && hnt)
        {
          if (!usehints) current.match[i] = -200;
          else
          {
            const VSFrameRef *tmp;
            if (frameCtx)
                tmp = vsapi->getFrameFilter(w, child, frameCtx);
            else
                tmp = vsapi->getFrame(w, child, nullptr, 0);
            vsapi->freeFrame(nxt);
            nxt = vsapi->copyFrame(tmp, core);
            vsapi->freeFrame(tmp);
            next_num = w;
            current.match[i] = getTFMFrameProperties(nxt, current.filmd2v[i]);
          }
        }
        continue;
      }

      vsapi->freeFrame(prv);
      if (next_num == w - 1) 
        prv = vsapi->copyFrame(nxt, core);
      else {
        const VSFrameRef *tmp;
        if (frameCtx)
            tmp = vsapi->getFrameFilter(w > 0 ? w - 1 : 0, child, frameCtx);
        else
            tmp = vsapi->getFrame(w > 0 ? w - 1 : 0, child, nullptr, 0);
        prv = vsapi->copyFrame(tmp, core);
        vsapi->freeFrame(tmp);
      }
      const VSFrameRef *tmp;
      if (frameCtx)
          tmp = vsapi->getFrameFilter(w, child, frameCtx);
      else
          tmp = vsapi->getFrame(w, child, nullptr, 0);
      vsapi->freeFrame(nxt);
      nxt = vsapi->copyFrame(tmp, core);
      vsapi->freeFrame(tmp);
      next_num = w;
      if (current.match[i] == -20 && hnt)
      {
        if (!usehints) current.match[i] = -200;
        else current.match[i] = getTFMFrameProperties(nxt, current.filmd2v[i]);
      }
    }

    struct CalcMetricData d;
    //d.np = np;
    d.predenoise = false; // done earlier
    d.vi = *vi_child;
    d.chroma = chroma;
    d.cpuFlags = &cpuFlags;
    d.blockx = blockx;
    d.blockx_half = blockx_half;
    d.blockx_shift = blockx_shift;
    d.blocky = blocky;
    d.blocky_half = blocky_half;
    d.blocky_shift = blocky_shift;
    d.diff = diff.get();
    d.nt = nt;
    d.ssd = ssd;

    // here we need metrics and has scene
    d.metricF_needed = true;
    d.metricF = &current.diffMetricsUF[i];
    d.scene = scene;

    CalcMetricsExtracted(prv, nxt, d, core, vsapi);

    int xblocks = ((d.vi.width + d.blockx_half) >> d.blockx_shift) + 1;
    int yblocks = ((d.vi.height + d.blocky_half) >> d.blocky_shift) + 1;
    int arraysize = (xblocks * yblocks) << 2;

    highestDiff = 0;
    for (int x = 0; x < arraysize; ++x)
    {
      if (diff.get()[x] > highestDiff)
        highestDiff = diff.get()[x];
    }
    if (ssd)
    {
      highestDiff = (uint64_t)(sqrt((double)(highestDiff)));
      current.diffMetricsUF[i] = (uint64_t)(sqrt((double)(current.diffMetricsUF[i])));
    }
    current.diffMetricsU[i] = highestDiff;
    current.diffMetricsN[i] = (highestDiff * 100.0) / MAX_DIFF;
  }

  vsapi->freeFrame(prevt);
  vsapi->freeFrame(nextt);
  vsapi->freeFrame(prv);
  vsapi->freeFrame(nxt);

  current.mSet = true;
  current.setIsFilmD2V();
}

template<bool SAD>
void calcLumaDiffYUY2_SADorSSD_c(const uint8_t* prvp, const uint8_t* nxtp,
  int width, int height, int prv_pitch, int nxt_pitch, int nt, uint64_t& diff) {

  if (width <= 0)
    return;
  for (int y = 0; y < height; ++y)
  {
    for (int x = 0; x < width; x += 2)
    {
      int temp;
      if constexpr (SAD)
        temp = abs(prvp[x] - nxtp[x]); // SAD
      else {
        temp = prvp[x] - nxtp[x];
        temp *= temp; // SSD
      }
      if (temp > nt) diff += temp;
      diff += temp;
    }
    prvp += prv_pitch;
    nxtp += nxt_pitch;
  }
}

//template<bool SAD>
//uint64_t calcLumaDiffYUY2_SADorSSD(const uint8_t* prvp, const uint8_t* nxtp,
//  int width, int height, int prv_pitch, int nxt_pitch, int nt, int cpuFlags)
//{
//  uint64_t diff = 0;

//  const bool use_sse2 = (cpuFlags & CPUF_SSE2) ? true : false;

//  int widtha;

//  if (use_sse2 && (nt == 0) && width >= 16) {
//    widtha = (width / 16) * 16;
//    if constexpr(SAD)
//      calcLumaDiffYUY2SAD_SSE2_16(prvp, nxtp, widtha, height, prv_pitch, nxt_pitch, diff);
//    else
//      calcLumaDiffYUY2SSD_SSE2_16(prvp, nxtp, widtha, height, prv_pitch, nxt_pitch, diff);

//    calcLumaDiffYUY2_SADorSSD_c<SAD>(prvp + widtha, nxtp + widtha, width - widtha, height, prv_pitch, nxt_pitch, nt, diff);
//  }
//  else
//  {
//    calcLumaDiffYUY2_SADorSSD_c<SAD>(prvp, nxtp, width, height, prv_pitch, nxt_pitch, nt, diff);
//  }

//  return diff;
//}

//uint64_t calcLumaDiffYUY2_SAD(const uint8_t* prvp, const uint8_t* nxtp,
//  int width, int height, int prv_pitch, int nxt_pitch, int nt, int cpuFlags)
//{
//  return calcLumaDiffYUY2_SADorSSD<true>(prvp, nxtp, width, height, prv_pitch, nxt_pitch, nt, cpuFlags);
//}

//uint64_t calcLumaDiffYUY2_SSD(const uint8_t* prvp, const uint8_t* nxtp,
//  int width, int height, int prv_pitch, int nxt_pitch, int nt, int cpuFlags)
//{
//  return calcLumaDiffYUY2_SADorSSD<false>(prvp, nxtp, width, height, prv_pitch, nxt_pitch, nt, cpuFlags);
//}

int TDecimate::getTFMFrameProperties(const VSFrameRef *src, int& d2vfilm) const
{
    const VSMap *props = vsapi->getFramePropsRO(src);
    int err;

  int match = int64ToIntS(vsapi->propGetInt(props, PROP_TFMMATCH, 0, &err));
  if (err)
      match = -200;

  d2vfilm = int64ToIntS(vsapi->propGetInt(props, PROP_TFMD2VFilm, 0, &err));
  if (err)
      d2vfilm = 0;

  int field = int64ToIntS(vsapi->propGetInt(props, PROP_TFMField, 0, &err));
  if (err)
      field = 0;

  if (match != -200 && field != 0)
  {
    if (match == 0) match = 3;
    else if (match == 2) match = 4;
    else if (match == 3) match = 0;
    else if (match == 4) match = 2;
  }

  return match;
}

/*
**  This function checks to see if there is a single match dup in the
**  current cycle and if that frame also has the lowest metric in the
**  cycle.  If those conditions are true, then it checks to see if
**  there is also such a frame in the previous or next cycle.  If
**  there is, and if it is in the same position as the one in the
**  current cycle, then the frame in the current cycle is chosen for
**  decimation.
**
**  This function is only used by longest string.
*/
bool TDecimate::checkForObviousDecFrame(Cycle &p, Cycle &c, Cycle &n)
{
  int i, v, dups = 0, mc = ISC, mp = ISC, saved = -20, saved2 = -20;
  uint64_t lowest_metric = UINT64_MAX;
  for (i = c.cycleS; i < c.cycleE; ++i)
  {
    mp = i == c.cycleS ? (p.cycleE > 0 ? p.match[p.cycleE - 1] : -20)
                       : mc;
    mc = c.match[i];
    if (checkMatchDup(mp, mc)) ++dups;
    if (dups > 1) return false;
    if (dups == 1 && saved == -20) saved = i;
    if (c.diffMetricsU[i] < lowest_metric)
    {
      lowest_metric = c.diffMetricsU[i];
      saved2 = i;
    }
  }
  if (dups != 1) return false;
  if (saved != saved2 || saved2 == -20) return false;
  lowest_metric = UINT64_MAX;
  int cp = -20, cn = -20;
  for (dups = 0, v = -1, i = p.cycleS; i < p.cycleE; ++i)
  {
    mp = i == p.cycleS ? -20 : mc;
    mc = p.match[i];
    if (checkMatchDup(mp, mc)) { ++dups; cp = i; }
    if (p.diffMetricsU[i] < lowest_metric) { v = i; lowest_metric = p.diffMetricsU[i]; }
  }
  if (dups == 0 && v == p.cycleS) { dups = 1; cp = v; }
  if (dups != 1 || cp != v) cp = -20;
  if (cp == -20)
  {
    lowest_metric = UINT64_MAX;
    for (dups = 0, v = -1, i = n.cycleS; i < n.cycleE; ++i)
    {
      mp = i == n.cycleS ? c.match[c.cycleE - 1] : mc;
      mc = n.match[i];
      if (checkMatchDup(mp, mc)) { ++dups; cn = i; }
      if (n.diffMetricsU[i] < lowest_metric) { v = i; lowest_metric = n.diffMetricsU[i]; }
    }
    if (dups != 1 || cn != v) return false;
  }
  if (saved != cp && saved != cn) return false;
//  if (debug)
//  {
//    sprintf(buf, "TDecimate:  obvious dec frame found  %d - %d!\n", saved, c.frameSO);
//    OutputDebugString(buf);
//  }
  c.decimate[saved] = c.decimate2[saved] = 1;
  c.decSet = true;
  return true;
}

/*
**  This function checks to see if there is a single frame in the
**  current cycle marked as a d2v duplicate and if that frame is
**  either a match dup or has the lowest metric.  If so, that frame
**  is chosen for decimation.  If mode=1 (or vfrDec=1), the prev and
**  next cycles must have a d2v duplicate in the same position.
**
**  This function is used by both longest string and most similar.
*/
int TDecimate::checkForD2VDecFrame(Cycle &p, Cycle &c, Cycle &n)
{
  int i, v = 0, mp, savedV = -20, savedL = -20, savedM = -20;
  uint64_t lowest = UINT64_MAX;
  for (i = c.cycleS; i < c.cycleE; ++i)
  {
    if (c.filmd2v[i] == 1 && (mode == 0 || (mode > 1 && vfrDec == 0) ||
      (p.filmd2v[i] == 1 && n.filmd2v[i] == 1)))
    {
      ++v;
      if (v > 1) return -20;
      savedV = i;
      mp = i == c.cycleS ? (p.cycleE > 0 ? p.match[p.cycleE - 1] : -20) : c.match[i - 1];
      if (checkMatchDup(mp, c.match[i])) savedM = i;
    }
    if (c.diffMetricsU[i] < lowest)
    {
      lowest = c.diffMetricsU[i];
      savedL = i;
    }
  }
  if (v != 1 || (savedV != savedM && savedV != savedL)) return -20;
//  if (debug)
//  {
//    sprintf(buf, "TDecimate:  d2v dec frame found  %d - %d!\n", savedV, c.frameSO);
//    OutputDebugString(buf);
//  }
  return savedV;
}

/*  This function checks for the two duplicate due to ivtc pattern
**  change case. It checks the prev and next cycles for single match
**  duplicate with the lowest metric in different positions and the
**  current cycle for 2 match duplicates with the 2 lowest metrics.
**  The two dups must match the positions of dups in the prev and
**  next cycles.  Finally, it requires that no other frames in the
**  current cycle were detected as metric duplicates.
**
**  This function is only used by longest string.
*/
bool TDecimate::checkForTwoDropLongestString(Cycle &p, Cycle &c, Cycle &n)
{
  int dupsP = 0, savedp = -20, dupsN = 0, savedn = -20;
  int c1 = -20, c2 = -20, i, v, mp, mc = ISC;
  uint64_t lowest = UINT64_MAX;
  for (v = -1, i = p.cycleS; i < p.cycleE; ++i)
  {
    mp = i == p.cycleS ? -20 : mc;
    mc = p.match[i];
    if (checkMatchDup(mp, mc)) { ++dupsP; savedp = i; }
    if (p.diffMetricsU[i] < lowest) { lowest = p.diffMetricsU[i]; v = i; }
  }
  if (dupsP == 0 && v == p.cycleS) { dupsP = 1; savedp = v; }
  if (dupsP != 1 || savedp != v) return false;
  lowest = UINT64_MAX;
  for (v = -1, i = n.cycleS; i < n.cycleE; ++i)
  {
    mp = i == n.cycleS ? c.match[c.cycleE - 1] : mc;
    mc = n.match[i];
    if (checkMatchDup(mp, mc)) { ++dupsN; savedn = i; }
    if (n.diffMetricsU[i] < lowest) { lowest = n.diffMetricsU[i]; v = i; }
  }
  if (dupsN != 1 || savedn != v || savedn == savedp) return false;
  uint64_t lowest1 = UINT64_MAX;
  uint64_t lowest2 = UINT64_MAX;
  int cl1 = -20, cl2 = -20;
  for (v = 0, c1 = -1, c2 = -1, i = c.cycleS; i < c.cycleE; ++i)
  {
    mp = i == c.cycleS ? (p.cycleE > 0 ? p.match[p.cycleE - 1] : -20) : mc;
    mc = c.match[i];
    if (checkMatchDup(mp, mc))
    {
      ++v;
      if (c1 == -1) c1 = i;
      else if (c2 == -1) c2 = i;
    }
    if (c.diffMetricsU[i] < lowest1)
    {
      lowest2 = lowest1;
      cl2 = cl1;
      lowest1 = c.diffMetricsU[i];
      cl1 = i;
    }
    else if (c.diffMetricsU[i] < lowest2)
    {
      lowest2 = c.diffMetricsU[i];
      cl2 = i;
    }
  }
  if (v != 2 || c1 == -1 || c2 == -1 ||
    (c1 != cl1 && c1 != cl2) || (c2 != cl1 && c2 != cl2))
    return false;
  if (c1 != savedp || c2 != savedn) return false;
  if (abs(c1 - c2) <= 1) return false;
  for (i = c.cycleS; i < c.cycleE; ++i)
  {
    if (c.dupArray[i] == 1 && c1 != i && c2 != i)
      return false;
  }
  if ((c1 == c.cycleS && (p.dupArray[p.cycleE > 0 ? p.cycleE - 1 : 0] == 1 || c.dupArray[c1 + 1] == 1)) ||
    (c1 != c.cycleS && (c.dupArray[c1 - 1] == 1 || c.dupArray[c1 + 1] == 1))) return false;
  if ((c2 == c.cycleE - 1 && (n.dupArray[n.cycleS] == 1 || c.dupArray[c2 - 1] == 1)) ||
    (c2 != c.cycleE - 1 && (c.dupArray[c2 - 1] == 1 || c.dupArray[c2 + 1] == 1))) return false;
  if (hybrid == 0 && noblend)
  {
    if (c.diffMetricsU[c1] <= c.diffMetricsU[c2])
      c.decimate[c1] = c.decimate2[c1] = 1;
    else
      c.decimate[c2] = c.decimate2[c2] = 1;
    c.decSet = true;
  }
  else
  {
    c.blend = 3;
    c.decimate[c1] = c.decimate2[c1] = 1;
    c.decimate[c2] = c.decimate2[c2] = 1;
    c.decSet = true;
//    if (debug)
//    {
//      sprintf(buf, "TDecimate:  drop two frames longest string  %d:%d - %d!\n", c1, c2, c.frameSO);
//      OutputDebugString(buf);
//    }
  }
  return true;
}

void TDecimate::mostSimilarDecDecision(Cycle &p, Cycle &c, Cycle &n)
{
  if (!c.lowSet) c.setLowest(false);
  if (cycleR != 1)
  {
    c.setDecimateLow(c.frameEO - c.frameSO == cycle ? cycleR :
      std::max(int(cycleR*(c.frameEO - c.frameSO) / double(cycle)), 1));
    return;
  }
  if (!p.dupsSet) p.setDupsMatches(p, ovrArray);
  if (!c.dupsSet) c.setDupsMatches(p, ovrArray);
  if (!n.dupsSet) n.setDupsMatches(c, ovrArray);
  int d2vdecf = checkForD2VDecFrame(p, c, n);
  if (c.dupCount <= 0 && d2vdecf == -20)
  {
    c.setDecimateLow(cycleR);
    return;
  }
  int i, ovrdups;
  for (ovrdups = 0, i = c.cycleS; i < c.cycleE; ++i)
  {
    if (c.decimate[i] != 1) c.decimate[i] = c.decimate2[i] = 0;
    else ++ovrdups;
  }
  if (ovrdups != 0)
  {
    c.setDecimateLow(cycleR);
    return;
  }
  for (i = 0; i < c.cycleS; ++i) c.decimate[i] = c.decimate2[i] = -20;
  for (i = c.cycleE; i < c.length; ++i) c.decimate[i] = c.decimate2[i] = -20;
  if (d2vdecf != -20)
  {
    c.decimate[d2vdecf] = c.decimate2[d2vdecf] = 1;
    c.decSet = true;
    return;
  }
  if (c.dupCount == 1)
  {
    uint64_t lowest = UINT64_MAX, lowest2 = UINT64_MAX;
    int savedc = -1, savedp = -1, savedn = -1, v;
    for (v = -1, i = c.cycleS; i < c.cycleE; ++i)
    {
      if (c.dupArray[i] == 1) savedc = i;
      if (c.diffMetricsU[i] < lowest) { v = i; lowest = c.diffMetricsU[i]; }
    }
    if (savedc == v || (double)(c.diffMetricsU[savedc] * 0.9) <= (double)lowest ||
      fabs(((double)lowest*100.0 / (double)MAX_DIFF) - c.diffMetricsN[savedc]) < 0.20)
    {
      c.decimate[savedc] = c.decimate2[savedc] = 1;
      c.decSet = true;
      return;
    }
    if (p.dupCount == 1 && n.dupCount == 1)
    {
      for (lowest2 = UINT64_MAX, i = p.cycleS; i < p.cycleE; ++i)
      {
        if (p.dupArray[i] == 1) savedp = i;
        if (p.diffMetricsU[i] < lowest2) { v = i; lowest2 = p.diffMetricsU[i]; }
      }
      if (savedp != v)
      {
        c.setDecimateLow(cycleR);
        return;
      }
      for (lowest2 = UINT64_MAX, i = n.cycleS; i < n.cycleE; ++i)
      {
        if (n.dupArray[i] == 1) savedn = i;
        if (n.diffMetricsU[i] < lowest2) { v = i; lowest2 = n.diffMetricsU[i]; }
      }
      if (savedn != v || savedp != savedn || savedp != savedc)
      {
        c.setDecimateLow(cycleR);
        return;
      }
      c.decimate[savedc] = c.decimate2[savedc] = 1;
      c.decSet = true;
      return;
    }
    c.setDecimateLow(cycleR);
    return;
  }
  else
  {
    uint64_t lowestp = UINT64_MAX, lowestn = UINT64_MAX;
    int savedp = -1, savedn = -1, savedc1, savedc2, v;
    if (c.dupCount == 2 && p.dupCount == 1 && n.dupCount == 1)
    {
      for (v = -1, i = p.cycleS; i < p.cycleE; ++i)
      {
        if (p.dupArray[i] == 1) savedp = i;
        if (p.diffMetricsU[i] < lowestp) { v = i; lowestp = p.diffMetricsU[i]; }
      }
      if (savedp != v || v == -1) goto tryother;
      for (v = -1, i = n.cycleS; i < n.cycleE; ++i)
      {
        if (n.dupArray[i] == 1) savedn = i;
        if (n.diffMetricsU[i] < lowestn) { v = i; lowestn = n.diffMetricsU[i]; }
      }
      if (savedn != v || v == -1 || savedn == savedp) goto tryother;
      for (savedc1 = -1, savedc2 = -1, i = c.cycleS; i < c.cycleE; ++i)
      {
        if (c.dupArray[i] == 1)
        {
          if (savedc1 == -1) savedc1 = i;
          else if (savedc2 == -1) savedc2 = i;
        }
      }
      if (savedc1 != savedp || savedc2 != savedn) goto tryother;
      if (savedc1 != c.lowest[0] && savedc1 != c.lowest[1]) goto tryother;
      if (savedc2 != c.lowest[0] && savedc2 != c.lowest[1]) goto tryother;
      if (abs(savedc1 - savedc2) <= 1) goto tryother;
      if (hybrid == 0 && noblend)
      {
        if (c.diffMetricsU[savedc1] <= c.diffMetricsU[savedc2])
          c.decimate[savedc1] = c.decimate2[savedc1] = 1;
        else
          c.decimate[savedc2] = c.decimate2[savedc2] = 1;
        c.decSet = true;
      }
      else
      {
        c.blend = 3;
        c.decimate[savedc1] = c.decimate2[savedc1] = 1;
        c.decimate[savedc2] = c.decimate2[savedc2] = 1;
        c.decSet = true;
//        if (debug)
//        {
//          sprintf(buf, "TDecimate:  drop two frames most similar  %d:%d - %d!\n", savedc1, savedc2, c.frameSO);
//          OutputDebugString(buf);
//        }
      }
      return;
    }
  tryother:
    int savedc = -1;
    uint64_t metricP, metricN, metricPt, metricNt;
    for (v = 0, i = c.cycleS; i < c.cycleE; ++i)
    {
      if (c.dupArray[i] == 1)
      {
        if (((i == c.cycleS && p.dupArray[p.cycleE > 0 ? p.cycleE - 1 : 0] == 0) || (i != c.cycleS && c.dupArray[i - 1] == 0)) &&
          ((i == c.cycleE - 1 && n.dupArray[n.cycleS] == 0) || (i != c.cycleE - 1 && c.dupArray[i + 1] == 0)))
        {
          ++v;
          metricPt = i == c.cycleS ? p.diffMetricsU[p.cycleE > 0 ? p.cycleE - 1 : 0] : c.diffMetricsU[i - 1];
          metricNt = i == c.cycleE - 1 ? n.diffMetricsU[n.cycleS] : c.diffMetricsU[i + 1];
          if (savedc == -1 || (metricPt + metricNt > metricP + metricN &&
            metricPt > c.diffMetricsU[i] && metricNt > c.diffMetricsU[i] &&
            fabs(c.diffMetricsN[i] - c.diffMetricsN[c.lowest[0]]) < 1.0))
          {
            savedc = i;
            metricP = metricPt;
            metricN = metricNt;
          }
        }
      }
    }
    bool check = false;
    for (i = 0; i < v; ++i)
    {
      if (savedc == c.lowest[i]) { check = true; break; }
    }
    if (!check || savedc == -1)
    {
      c.setDecimateLow(cycleR);
      return;
    }
    c.decimate[savedc] = c.decimate2[savedc] = 1;
    c.decSet = true;
    return;
  }
  c.setDecimateLow(cycleR);
}

void TDecimate::findDupStrings(Cycle &p, Cycle &c, Cycle &n)
{
  if (!p.dupsSet) p.setDups(dupThresh);
  if (!c.dupsSet) c.setDups(dupThresh);
  if (!n.dupsSet) n.setDups(dupThresh);
  const int dcnt = (cycle + 1) >> 1;
  uint64_t lowest;
  int temp, i, g, b, f, forward, back, v, w = 0, j;
  int temp1, temp2, temp3, y, dups, ovrdups = 0, d2vdecf = -20;
  if (cycleR == 1) d2vdecf = checkForD2VDecFrame(p, c, n);
  for (temp = 0, i = c.cycleS; i < c.cycleE; ++i) temp += c.dupArray[i];
  if (temp == 0 && d2vdecf == -20)
  {
    if (!c.lowSet) c.setLowest(false);
    c.setDecimateLow(c.frameEO - c.frameSO == cycle ? cycleR :
      std::max(int(cycleR*(c.frameEO - c.frameSO) / double(cycle)), 1));
    return;
  }
  for (ovrdups = 0, i = c.cycleS; i < c.cycleE; ++i)
  {
    if (c.decimate[i] != 1) c.decimate[i] = c.decimate2[i] = 0;
    else ++ovrdups;
  }
  for (i = 0; i < c.cycleS; ++i) c.decimate[i] = c.decimate2[i] = -20;
  for (i = c.cycleE; i < c.length; ++i) c.decimate[i] = c.decimate2[i] = -20;
  int cycleRt = c.frameEO - c.frameSO == cycle ? cycleR :
    std::max(int(cycleR*(c.frameEO - c.frameSO) / double(cycle)), 1);
  if (ovrdups >= cycleRt) { c.decSet = true; return; }
  if (cycleR == 1 && checkForObviousDecFrame(p, c, n)) return;
  if (cycleR == 1 && checkForTwoDropLongestString(p, c, n)) return;
  if (cycleR == 1 && cycle > 2 && ovrdups == 0 && c.dupCount > 1 &&
    (p.dupCount == 1 || p.dupCount == 0 || n.dupCount == 1 || n.dupCount == 0))
  {
    int p1 = -20, c1 = -20, c2 = -20, n1 = -20, dupcp = 0, usecp = 0;
    for (dupcp = 0, i = c.cycleS; i < c.cycleE && dupcp == 0; ++i)
    {
      if (c.dupArray[i] == 1 &&
        ((i > c.cycleS && c.dupArray[i - 1] == 0) || (i == c.cycleS && p.dupArray[p.cycleE > 0 ? p.cycleE - 1 : 0] == 0)) &&
        ((i < c.cycleE - 1 && c.dupArray[i + 1] == 0) || ((i == c.cycleE - 1 && n.dupArray[n.cycleS] == 0))))
      {
        c1 = i; break;
      }
      if (c.dupArray[i] == 1) ++dupcp;
    }
    for (dupcp = 0, i = c.cycleE - 1; i >= c.cycleS && dupcp == 0; --i)
    {
      if (c.dupArray[i] == 1 &&
        ((i > c.cycleS && c.dupArray[i - 1] == 0) || (i == c.cycleS && p.dupArray[p.cycleE > 0 ? p.cycleE - 1 : 0] == 0)) &&
        ((i < c.cycleE - 1 && c.dupArray[i + 1] == 0) || ((i == c.cycleE - 1 && n.dupArray[n.cycleS] == 0))))
      {
        c2 = i; break;
      }
      if (c.dupArray[i] == 1) ++dupcp;
    }
    bool ct1 = false, ct2 = false;
    if ((p.dupCount == 1 || p.dupCount == 0) && c1 != -20)
    {
      if (p.dupCount == 0 && (p.cycleE - p.cycleS == p.length ||
        n.dupCount != c.dupCount)) {
        p1 = c1; ct1 = true;
      }
      else
      {
        for (i = p.cycleS; i < p.cycleE; ++i)
        {
          if (p.dupArray[i] == 1) { p1 = i; break; }
        }
      }
      if (p1 == c1) usecp += 1;
    }
    if ((n.dupCount == 1 || n.dupCount == 0) && c2 != -20)
    {
      if (n.dupCount == 0 && (n.cycleE - n.cycleS == n.length ||
        p.dupCount != c.dupCount)) {
        n1 = c2; ct2 = true;
      }
      else
      {
        for (i = n.cycleS; i < n.cycleE; ++i)
        {
          if (n.dupArray[i] == 1) { n1 = i; break; }
        }
      }
      if (n1 == c2) usecp += 5;
    }
    if (hybrid == 0 && noblend && usecp == 6)
    {
      if (ct1 && !ct2) usecp = 5;
      else if (!ct1 && ct2) usecp = 1;
      else
      {
        if (c.diffMetricsU[c1] <= c.diffMetricsU[c2]) usecp = 1;
        else usecp = 5;
      }
    }
    if (usecp == 1 || usecp == 5)
    {
      if (usecp == 5) c1 = c2;
      c.decimate[c1] = c.decimate2[c1] = 1;
//      if (debug)
//      {
//        sprintf(buf, "TDecimate:  usecp case %d - %d!\n", usecp, c.frameSO);
//        OutputDebugString(buf);
//      }
      c.decSet = true;
      return;
    }
    else if (usecp == 6)
    {
      c.blend = 3;
      c.decimate[c1] = c.decimate2[c1] = 1;
      c.decimate[c2] = c.decimate2[c2] = 1;
//      if (debug)
//      {
//        sprintf(buf, "TDecimate:  usecp case %d - %d!\n", usecp, c.frameSO);
//        OutputDebugString(buf);
//      }
      c.decSet = true;
      return;
    }
  }
  if (d2vdecf != -20)
  {
    c.decimate[d2vdecf] = c.decimate2[d2vdecf] = 1;
    c.decSet = true;
    return;
  }
  int **dupStrings = (int**)malloc(dcnt * sizeof(int*));
  for (int z = 0; z < dcnt; ++z)
    dupStrings[z] = (int*)malloc(3 * sizeof(int));
  for (i = 0; i < dcnt; ++i)
    dupStrings[i][0] = dupStrings[i][1] = dupStrings[i][2] = -20;
  for (w = 0, i = c.cycleS; i < c.cycleE; ++i)
  {
    if (c.dupArray[i] == 0) continue;
    f = b = i;
    forward = back = 0;
    while (c.dupArray[f] == 1 && f < c.cycleE)
    {
      ++forward;
      ++f;
    }
    if (f == c.cycleE)
    {
      g = n.cycleS;
      while (n.dupArray[g] == 1 && g < n.cycleE)
      {
        ++g;
        ++forward;
      }
    }
    while (c.dupArray[b] == 1 && b >= c.cycleS)
    {
      ++back;
      --b;
    }
    if (b < 0)
    {
      g = p.cycleE - 1;
      while ((p.dupArray[g] == 1 && p.decimate2[g] != 1) && g >= p.cycleS)
      {
        ++back;
        --g;
      }
    }
    i = f;
    ++b;
    dupStrings[w][0] = back + forward - 1;
    dupStrings[w][1] = b;
    dupStrings[w][2] = f;
    ++w;
  }
  if (ovrArray.size())
  {
    for (i = c.cycleS; i < c.cycleE; ++i)
    {
      if (c.decimate[i] == 1)
      {
        for (v = 0; v < w; ++v)
        {
          if (i >= dupStrings[v][1] && i < dupStrings[v][2])
          {
            if (dupStrings[v][2] - dupStrings[v][1] - 1 <= 0) dupStrings[v][0] = -20;
            else --dupStrings[v][0];
          }
        }
      }
    }
  }
  for (i = 1; i < w; ++i)
  {
    j = i;
    temp1 = dupStrings[i][0];
    temp2 = dupStrings[i][1];
    temp3 = dupStrings[i][2];
    while (j > 0 && (dupStrings[j - 1][0] < temp1 || (dupStrings[j - 1][0] == temp1 &&
      dupStrings[j - 1][1] > temp2)))
    {
      dupStrings[j][0] = dupStrings[j - 1][0];
      dupStrings[j][1] = dupStrings[j - 1][1];
      dupStrings[j][2] = dupStrings[j - 1][2];
      --j;
    }
    dupStrings[j][0] = temp1;
    dupStrings[j][1] = temp2;
    dupStrings[j][2] = temp3;
  }
  for (v = 0; v < c.dupCount && v < cycleRt - ovrdups; ++v)
  {
    if (dupStrings[0][0] < 1) break;
    lowest = UINT64_MAX;
    f = dupStrings[0][1];
    for (dups = 0, i = dupStrings[0][1]; i < dupStrings[0][2]; ++i)
    {
      if (c.diffMetricsU[i] < lowest && c.decimate[i] == 0)
      {
        lowest = c.diffMetricsU[i];
        f = i;
      }
      if (c.decimate[i] == 1) ++dups;
    }
    c.decimate[f] = 1;
    y = dupStrings[0][1];
    while (c.decimate2[y] == 1) ++y;
    c.decimate2[y] = 1;
    if (dupStrings[0][2] - dupStrings[0][1] - dups - 1 <= 0) dupStrings[0][0] = -20;
    else --dupStrings[0][0];
    j = 0;
    temp1 = dupStrings[0][0];
    temp2 = dupStrings[0][1];
    temp3 = dupStrings[0][2];
    while (j < w - 1 && (dupStrings[j + 1][0] > temp1 || (dupStrings[j + 1][0] == temp1 &&
      dupStrings[j + 1][1] < temp2)))
    {
      dupStrings[j][0] = dupStrings[j + 1][0];
      dupStrings[j][1] = dupStrings[j + 1][1];
      dupStrings[j][2] = dupStrings[j + 1][2];
      ++j;
    }
    dupStrings[j][0] = temp1;
    dupStrings[j][1] = temp2;
    dupStrings[j][2] = temp3;
  }
  c.decSet = true;
  if (v < cycleRt - ovrdups)
  {
    c.setLowest(true);
    c.setDecimateLowP(cycleRt - ovrdups - v);
  }
  for (int z = 0; z < dcnt; ++z)
    free(dupStrings[z]);
  free(dupStrings);
}

void TDecimate::checkVideoMatches(Cycle &p, Cycle &c)
{
  if (!p.mSet || !c.mSet || (c.type != 3 && c.type > 0)) return;
  int dups = 0, mp, mc, i;
  for (i = c.cycleS; i < c.cycleE && dups <= 0; ++i)
  {
    if (i == c.cycleS)
    {
      if (p.frame != c.frame) mp = p.cycleE > 0 ? p.match[p.cycleE - 1] : -20;
      else mp = -20;
    }
    else mp = c.match[i - 1];
    mc = c.match[i];
    if (mp == 0 && mc == 3) ++dups;
    else if (mp == 1 && (mc == 0 || mc == 3)) ++dups;
    else if (mp == 2 && (mc == 1 || mc == 3 || mc == 4 || mc == 6)) ++dups;
    else if (mp == 3 && mc == 0) ++dups;
    else if (mp == 4 && (mc == 0 || mc == 1 || mc == 2 || mc == 5)) ++dups;
    else if (mp == 5 && mc == 3) ++dups;
    else if (mp == 6 && mc == 0) ++dups;
    else if (mc < 0) ++dups;
  }
  if (dups == 0)
  {
    if (c.type == -1) c.type = 2;
    else if (c.type == 0) c.type = 2;
    else if (c.type == 3) c.type = 4;
  }
  else if (c.type == -1) c.type = 0;
}

bool TDecimate::checkMatchDup(int mp, int mc)
{
  if (mp == 0 && mc == 3) return true;
  else if (mp == 1 && (mc == 0 || mc == 3)) return true;
  else if (mp == 2 && (mc == 1 || mc == 3 || mc == 4 || mc == 6)) return true;
  else if (mp == 3 && mc == 0) return true;
  else if (mp == 4 && (mc == 0 || mc == 1 || mc == 2 || mc == 5)) return true;
  else if (mp == 5 && mc == 3) return true;
  else if (mp == 6 && mc == 0) return true;
  else if (mc < 0) return true;
  return false;
}

void TDecimate::checkVideoMetrics(Cycle &c, double thresh)
{
  if (!c.mSet || (c.type > 0 && c.type != 2)) return;
  int dups = 0, f = c.cycleS, i;
  double min = 999999.0, max = -999999.0, temp;
  if (c.frame == 0) ++f;
  for (i = f; i < c.cycleE; ++i)
  {
    temp = c.diffMetricsN[i];
    if (temp <= thresh) ++dups;
    if (temp < min) min = temp;
    if (temp > max) max = temp;
  }
  if (min == 0.0) min = 0.0001;
  if (dups == 0 || (cve && max / min < 1.6 && max - min < 2.0 && max >= 0.3))
  {
    if (c.type == -1) c.type = 3;
    else if (c.type == 0) c.type = 3;
    else if (c.type == 2) c.type = 4;
  }
  else if (c.type == -1) c.type = 0;
}

// PF 180131 uses usehints!
void TDecimate::getOvrCycle(Cycle &current, bool mode2)
{
  if (mode2) current.dupCount = 0;
  if (ovrArray.empty() && metricsArray.empty() && metricsOutArray.empty()) return;
  int b = current.cycleS, v = 0, i, p = 0, d = 0, value;
  int numr = current.frameEO - current.frameSO == cycle ? cycleR :
    std::max(int(cycleR*(current.frameEO - current.frameSO) / double(cycle)), 1);
  for (i = current.frameSO; i < current.frameEO; ++i, ++b)
  {
    if (ovrArray.size())
    {
      value = ovrArray[i];
      if (value&DROP_FRAME && (d < numr || mode2))
      {
        if (mode2)
        {
          current.dupArray[b] = 1;
          ++current.dupCount;
        }
        else
        {
          current.decimate[b] = current.decimate2[b] = 1;
          ++d;
          current.type = 1;
        }
      }
      if (value&VIDEO) ++v;
      if (value&FILM) ++p;
      if (usehints)
      {
        if (value&ISD2VFILM) current.filmd2v[b] = 1;
        if ((value&ISMATCH) != 0x70)
        {
          value = (value&ISMATCH) >> 4;
          if (value == ISC) current.match[b] = ISC;
          else if (value == ISP) current.match[b] = ISP;
          else if (value == ISN) current.match[b] = ISN;
          else if (value == ISB) current.match[b] = ISB;
          else if (value == ISU) current.match[b] = ISU;
          else if (value == ISDB) current.match[b] = ISDB;
          else if (value == ISDT) current.match[b] = ISDT;
        }
      }
    }
    bool foundM = false;
    if (metricsArray.size())
    {
      if (metricsArray[i << 1] != UINT64_MAX)
      {
        current.diffMetricsU[b] = metricsArray[i << 1];
        current.diffMetricsN[b] = (metricsArray[i << 1] * 100.0) / MAX_DIFF;
        foundM = true;
      }
      if (metricsArray[(i << 1) + 1] != UINT64_MAX)
        current.diffMetricsUF[b] = metricsArray[(i << 1) + 1];
    }
    if (metricsOutArray.size() && !foundM)
    {
      if (metricsOutArray[i << 1] != UINT64_MAX)
      {
        current.diffMetricsU[b] = metricsOutArray[i << 1];
        current.diffMetricsN[b] = (metricsOutArray[i << 1] * 100.0) / MAX_DIFF;
      }
      if (metricsOutArray[(i << 1) + 1] != UINT64_MAX)
        current.diffMetricsUF[b] = metricsOutArray[(i << 1) + 1];
    }
  }
  if (v > 0 && v == current.cycleE - current.cycleS && current.type != 1)
    current.type = 5;
  if (p > 0 && p == current.cycleE - current.cycleS && current.type != 5)
    current.type = 1;
  current.setIsFilmD2V();
}

void TDecimate::getOvrFrame(int n, uint64_t &metricU, uint64_t &metricF) const
{
  metricU = metricF = UINT64_MAX;
  if (metricsArray.size())
  {
    if (metricsArray[n << 1] != UINT64_MAX)
      metricU = metricsArray[n << 1];
    if (metricsArray[(n << 1) + 1] != UINT64_MAX)
      metricF = metricsArray[(n << 1) + 1];
  }
  
  if (metricU != UINT64_MAX && metricF != UINT64_MAX)
    return;

  if (metricsOutArray.size())
  {
    if (metricU == UINT64_MAX && metricsOutArray[n << 1] != UINT64_MAX) 
      metricU = metricsOutArray[n << 1];
    if (metricF == UINT64_MAX && metricsOutArray[(n << 1) + 1] != UINT64_MAX)
      metricF = metricsOutArray[(n << 1) + 1];
  }
}

void TDecimate::calcBlendRatios(double &amount1, double &amount2, int &frame1, int &frame2, int n,
  int bframe, int cycleI)
{
  double stepsize = ((double)cycleI) / ((double)(cycleI - cycleR));
  double offset = ((cycleI - 1) - (stepsize*(cycleI - cycleR - 1)))*0.5;
  double pos = bframe + (n % (cycle - cycleR))*stepsize + offset;
  double posf = pos - (int)(pos);
  frame1 = (int)(pos);
  frame2 = (int)(pos + 1.0);
  amount1 = 1.0 - posf;
  amount2 = posf;
}

void TDecimate::calcBlendRatios2(double &amount1, double &amount2, int &frame1, int &frame2, int tf,
  Cycle &p, Cycle &c, Cycle &n, int remove)
{
  int i, b, k;
  int cycleI = c.cycleE - c.cycleS;
  int cycleD = cycleI - remove;
  int *lutf = (int *)malloc((cycleI + 2) * sizeof(int));
  for (i = 0; i < cycleI + 2; ++i) lutf[i] = -20;
  double stepsize = ((double)cycleD) / ((double)(cycleI));
  double offset = (cycleI - 1)*stepsize;
  offset = (offset - int(offset))*0.5;
  double pos = 1 + (tf%p.length)*stepsize - offset;
  double posf = pos - (int)(pos);
  for (b = p.frameEO - 1, i = p.cycleE - 1; i >= p.cycleS; --i, --b)
  {
    if (p.decimate[i] != 1)
    {
      lutf[0] = b;
      break;
    }
  }
  for (k = 1, b = c.frameSO, i = c.cycleS; i < c.cycleE; ++i, ++b)
  {
    if (c.decimate[i] != 1)
    {
      lutf[k] = b;
      ++k;
    }
  }
  for (b = n.frameSO, i = c.cycleS; i < c.cycleE; ++i, ++b)
  {
    if (c.decimate[i] != 1)
    {
      lutf[k] = b;
      break;
    }
  }
  for (i = 0; i < cycleI + 2; ++i)
  {
    if (lutf[i] < 0) lutf[i] = 0;
    else if (lutf[i] > nfrms) lutf[i] = nfrms;
  }
  frame1 = lutf[(int)(pos)];
  frame2 = lutf[(int)(pos + 1.0)];
  amount1 = 1.0 - posf;
  amount2 = posf;
  // amount 1 and 2 sum is always 1.0, some routines know this and use only amount1
  free(lutf);
}

// used in GetFrameMode01
// hbd ready
void TDecimate::blendFrames(const VSFrameRef *src1, const VSFrameRef *src2, VSFrameRef *dst,
  double amount1)
{
  const uint8_t *srcp1, *srcp2;
  uint8_t *dstp;
  int width, height;
  int s1_pitch, dst_pitch, s2_pitch;

  const float weight_f = (float)amount1;

  // 15 bit arithmetic (used as 16 bi at 8 bit case)
  const int weight_i = (int)(weight_f * 32768.0f + 0.5f);

  if (weight_i >= 32768)
  {
    copyFrame(dst, src1, vsapi); // 1000% src1
    return;
  }
  if (weight_i <= 0)
  {
    copyFrame(dst, src2, vsapi); // 100% src2
    return;
  }

  const VSFormat *format = vsapi->getFrameFormat(dst);

  const int np = format->numPlanes;
  const int bits_per_pixel = format->bitsPerSample;

  for (int b = 0; b < np; ++b)
  {
    const int plane = b;
    srcp1 = vsapi->getReadPtr(src1, plane);
    s1_pitch = vsapi->getStride(src1, plane);
    width = vsapi->getFrameWidth(src1, plane);
    height = vsapi->getFrameHeight(src1, plane);
    srcp2 = vsapi->getReadPtr(src2, plane);
    s2_pitch = vsapi->getStride(src2, plane);
    dstp = vsapi->getWritePtr(dst, plane);
    dst_pitch = vsapi->getStride(dst, plane);

    dispatch_blend(dstp, srcp1, srcp2, width, height, dst_pitch, s1_pitch, s2_pitch, weight_i, bits_per_pixel, &cpuFlags);
  }
}


/*
** I've copied the following functions:  float_to_frac, reduce_float,
** and FloatToFPS from fps.cpp, which can be obtained from avisynth's
** cvs (avisynth2.cvs.sourceforge.net).
*/

static bool float_to_frac(float input, unsigned &num, unsigned &den)
{
  union { float f; unsigned i; } value;
  unsigned mantissa;
  int exponent;
  value.f = input;
  mantissa = (value.i & 0x7FFFFF) + 0x800000;  // add implicit bit on the left
  exponent = ((value.i & 0x7F800000) >> 23) - 127 - 23;  // remove decimal pt
  while (!(mantissa & 1)) { mantissa >>= 1; exponent += 1; }
  if (exponent < -31) {
    return float_to_frac(float(1.0 / input), den, num);
  }
  while ((exponent > 0) && !(mantissa & 0x80000000)) {
    mantissa <<= 1; exponent -= 1;
  }
  if (exponent > 0) {  // number too large
    num = 0xffffffff;
    den = 1;
    return true; // Out of range!
  }
  num = mantissa;
  den = 1 << (-exponent);
  return false;
}

static bool reduce_float(float value, unsigned &num, unsigned &den)
{
  if (float_to_frac(value, num, den)) return true;
  unsigned n0 = 0, n1 = 1, n2, nx = num;  // numerators
  unsigned d0 = 1, d1 = 0, d2, dx = den;  // denominators
  unsigned a2, ax, amin;  // integer parts of quotients
  unsigned f1 = 0, f2;    // fractional parts of quotients
  while (1)  // calculate convergents
  {
    a2 = nx / dx;
    f2 = nx % dx;
    n2 = n0 + n1 * a2;
    d2 = d0 + d1 * a2;
    if (f2 == 0) break;  // no more convergents (n2 / d2 == input)
    float f = (float)((double)n2 / d2);
    if (f == value) break;
    n0 = n1; n1 = n2;
    d0 = d1; d1 = d2;
    nx = dx; dx = f1 = f2;
  }
  if (d2 == 1)
  {
    num = n2;
    den = d2;
  }
  else { // we have been through the loop at least twice
    if ((a2 % 2 == 0) && (d0 * f1 > f2 * d1))
      amin = a2 / 2;  // passed 1/2 a_k admissibility test
    else
      amin = a2 / 2 + 1;
    union { float f; unsigned i; } eps; eps.f = value;

#ifndef UInt32x32To64
#define UInt32x32To64(a, b) ((uint64_t)(((uint64_t)((uint32_t)(a))) * ((uint32_t)(b))))
#endif
    if (UInt32x32To64(n1, den) > UInt32x32To64(num, d1))
      eps.i -= 1;
    else
      eps.i += 1;

    double r2 = eps.f;
    r2 += value;
    r2 /= 2;
    double yn = n0 - r2*d0;
    double yd = r2*d1 - n1;
    ax = (unsigned)((yn + yd) / yd); // ceiling value
    if (ax < amin) ax = amin;
    num = n0 + n1 * ax;
    den = d0 + d1 * ax;
  }
  return false;
}

static bool FloatToFPS(double n, unsigned &num, unsigned &den)
{
    /// check the rate in the caller
//  if (n <= 0)
//    env->ThrowError("TDecimate:  rate must be greater than 0.\n");
  float x;
  unsigned u = (unsigned)(n * 1001 + 0.5);
  x = float((u / 30000 * 30000) / 1001.0);
  if (x == (float)n) { num = u; den = 1001; return false; }
  x = float((u / 24000 * 24000) / 1001.0);
  if (x == (float)n) { num = u; den = 1001; return false; }
  if (n < 14.986) {
    u = (unsigned)(30000 / n + 0.5);
    x = float(30000.0 / (u / 1001 * 1001));
    if (x == (float)n) { num = 30000; den = u; return false; }
    u = (unsigned)(24000 / n + 0.5);
    x = float(24000.0 / (u / 1001 * 1001));
    if (x == (float)n) { num = 24000; den = u; return false; }
  }
  /// make the caller check the return value and throw the error if true
  return reduce_float(float(n), num, den);
//    env->ThrowError("TDecimate:  rate value is out of range.\n");
}



void TDecimate::init_mode_5(VSCore *core) {
  FILE *f = nullptr;

  mkvfps = (fps*(cycle - cycleR)) / cycle;
  mkvfps2 = (fps*(cycle - cycleR - 1)) / cycle;
  std::vector<int> input_magic_numbers(vi.numFrames, 0);

  Cycle prevM(5, sdlim), currM(5, sdlim), nextM(5, sdlim);
  if (cycle > 5)
  {
    prevM.setSize(cycle);
    currM.setSize(cycle);
    nextM.setSize(cycle);
  }
  prevM.length = currM.length = nextM.length = cycle;
  prevM.maxFrame = currM.maxFrame = nextM.maxFrame = nfrms;
  bool vid, prevVid;
  int i, h, w, firstkv, countprev, filmC, videoC, longestT, longestV, countVT;
  int count, b, passThrough = 0;
twopassrun:
  ++passThrough;
#if 0
  if ((f = fopen("debug.txt", "a")) != nullptr) {
    fprintf(f, "passThrough=%d cycle=%d nfrms=%d vidThresh=%f np=%d\n", passThrough, cycle, nfrms, (float)vidThresh, np);
    fclose(f);
    f = nullptr;
  }
#endif
  count = 0;
  for (b = 0; b <= nfrms; b += cycle)
  {
    if (b == 0)
    {
      currM.setFrame(0);
      getOvrCycle(currM, false); // PF 180131 uses usehints!
      calcMetricCycle(currM, true, true, core);
      checkVideoMatches(currM, currM);
      checkVideoMetrics(currM, vidThresh);
    }
    else
    {
      prevM = currM;
      currM = nextM;
    }
    nextM.setFrame(b + cycle);
    getOvrCycle(nextM, false); // PF 180131 uses usehints!
    calcMetricCycle(nextM, true, true, core); // PF 180131 uses usehints!
    checkVideoMatches(currM, nextM);
    checkVideoMetrics(nextM, vidThresh);
    if (passThrough == 1)
    {
      if (currM.type == 5 || (!currM.isfilmd2v && ((currM.type == 2 && (vidDetect == 0 || vidDetect == 2)) ||
        (currM.type == 3 && (vidDetect == 1 || vidDetect == 2)) || (currM.type == 4 && vidDetect == 3))))
      {
        if (currM.type == 5) input_magic_numbers[b] = 8;
        if (currM.sceneDetect(prevM, nextM, sceneThreshU) != -20) input_magic_numbers[b] = 8;
      }
      else
      {
        if (vfrDec != 1)
        {
          mostSimilarDecDecision(prevM, currM, nextM);
        }
        else
        {
          prevM.setDups(dupThresh);
          currM.setDups(dupThresh);
          nextM.setDups(dupThresh);
          findDupStrings(prevM, currM, nextM);
        }
        for (w = 0, i = b; i < b + cycle && i <= nfrms; ++i, ++w)
        {
          if (currM.decimate[w] == 1) input_magic_numbers[i] = 2;
        }
      }
    } // passthrough == 1
    else
    { // passthrough != 1
      for (vid = true, i = b; i <= nfrms && i < b + cycle; ++i)
      {
        if (input_magic_numbers[i] == 2) vid = false;
      }
      if (!vid)
      {
        if (vfrDec != 1)
        {
          mostSimilarDecDecision(prevM, currM, nextM);
        }
        else
        {
          prevM.setDups(dupThresh);
          currM.setDups(dupThresh);
          nextM.setDups(dupThresh);
          findDupStrings(prevM, currM, nextM);
        }
        for (w = 0, i = b; i < b + cycle && i <= nfrms; ++i, ++w)
        {
          if (currM.decimate[w] == 1)
          {
            input_magic_numbers[i] = 2;
            ++count;
#if 0
            if ((f = fopen("debug.txt", "a")) != nullptr) {
              fprintf(f, "count=%03d b=%d w=%d i=%d \n", count, b, w, i);
              fclose(f);
              f = nullptr;
            }
#endif
          }
          else input_magic_numbers[i] = 0;
        }
      }
      else
      {
        for (i = b; i < b + cycle && i <= nfrms; ++i) input_magic_numbers[i] = 0;
      }
    } // passthrough != 1
  }
  if (passThrough == 2) { goto finishTP; }
  for (w = 0, h = 0; h <= nfrms; h += cycle)
  {
    for (vid = true, i = h; i < h + cycle && i <= nfrms; ++i)
    {
      if (input_magic_numbers[i] == 2) vid = false;
    }
    if (vid) ++w;
    else
    {
      if (w > 0 && w < conCycleTP)
      {
        for (i = std::max(0, h - w * cycle); i < h && i <= nfrms; i += cycle)
        {
          if (input_magic_numbers[i] != 8) input_magic_numbers[i] = 2;
        }
      }
      w = 0;
    }
  }
  if (w > 0 && w < conCycleTP)
  {
    for (i = h - w * cycle; i < h && i <= nfrms; i += cycle)
    {
      if (input_magic_numbers[i] != 8) input_magic_numbers[i] = 2;
    }
  }
  goto twopassrun;
finishTP:
    metricsArray.resize(0);

  if (ovrArray.size())
  {
    ovrArray.resize(0);
  }

#if 0
  if ((f = fopen("debug.txt", "a")) != nullptr) {
    fprintf(f, "new_num_frames=%d vi.numFrames=%d count=%d\n", vi.numFrames - count, vi.numFrames, count);
    fclose(f);
    f = nullptr;
  }
#endif

  muldivRational(&vi.fpsNum, &vi.fpsDen, vi.numFrames - count, vi.numFrames);
  vi.numFrames = vi.numFrames - count;
  if ((f = fopen(mkvOut.c_str(), "w")) != nullptr)
  {
    double timestamp = 0.0;
    double sample1 = 1000.0 / fps;
    double sample2 = 1000.0 / mkvfps;
    double sample3 = 1000.0 / mkvfps2;
    int ddup;
    if (tcfv1)
    {
      fprintf(f, "# timecode format v1\n");
      fprintf(f, "Assume %4.6f\n", fps);
    }
    else fprintf(f, "# timecode format v2\n");
    fprintf(f, "# TDecimate %s by tritical\n", VERSION);
    fprintf(f, "# Mode 5 - Auto-generated mkv timecodes file\n");
    firstkv = countprev = 0;
    vid = prevVid = true;
    filmC = videoC = longestT = longestV = countVT = 0;
    for (count = 0, b = 0; b <= nfrms; b += cycle)
    {
      prevVid = vid;
      countprev = count;
      vid = true;
      for (i = b, ddup = 0; i < b + cycle && i <= nfrms; ++i)
      {
        if (input_magic_numbers[i] == 2)
        {
          ++ddup;
          if (ddup < 2) filmC += (b + cycle <= nfrms ? cycle : nfrms - b + 1);
          vid = false;
        }
        else ++count;
      }
      if (vid)
      {
        if (!tcfv1)
        {
          int stop = (b + cycle <= nfrms ? cycle : nfrms - b + 1);
          for (int x = 0; x < stop; ++x)
          {
            fprintf(f, "%3.6f\n", timestamp);
            timestamp += sample1;
          }
        }
        videoC += (b + cycle <= nfrms ? cycle : nfrms - b + 1);
        longestT += (b + cycle <= nfrms ? cycle : nfrms - b + 1);
      }
      else if (!tcfv1)
      {
        if (ddup == 1)
        {
          int stop = (b + cycle <= nfrms ? cycle - cycleR : nfrms - b + 1 - cycleR);
          for (int x = 0; x < stop; ++x)
          {
            fprintf(f, "%3.6f\n", timestamp);
            timestamp += sample2;
          }
        }
        else if (ddup == 2)
        {
          int stop = (b + cycle <= nfrms ? cycle - cycleR - 1 : nfrms - b + 1 - cycleR - 1);
          for (int x = 0; x < stop; ++x)
          {
            fprintf(f, "%3.6f\n", timestamp);
            timestamp += sample3;
          }
        }
        else throw TIVTCError("TDecimate:  unknown mode 5 error (tc file creation)!");
      }
      else if (ddup == 2)
      {
        if (!prevVid) fprintf(f, "%d,%d,%4.6f\n", firstkv, countprev - 1, mkvfps);
        fprintf(f, "%d,%d,%4.6f\n", countprev, countprev + cycle - cycleR - 2, mkvfps2);
        firstkv = countprev + cycle - cycleR - 1;
      }
      if (prevVid != vid && countprev != 0 && ddup != 2 && countprev > firstkv)
      {
        if (!prevVid && tcfv1) fprintf(f, "%d,%d,%4.6f\n", firstkv, countprev - 1, mkvfps);
        firstkv = countprev;
      }
      else if (prevVid != vid && ddup != 2) firstkv = countprev;
      if (prevVid != vid && prevVid && countprev != 0)
      {
        if (longestT > longestV) longestV = longestT;
        ++countVT;
        longestT = 0;
      }
    }
    if (!vid && tcfv1) fprintf(f, "%d,%d,%4.6f\n", firstkv, count - 1, mkvfps);
    double filmCf = ((double)(filmC) / (double)(nfrms + 1))*100.0;
    double videoCf = ((double)(videoC) / (double)(nfrms + 1))*100.0;
    fprintf(f, "# vfr stats:  %05.2f%c film  %05.2f%c video\n", filmCf, '%', videoCf, '%');
    fprintf(f, "# vfr stats:  %d - film  %d - video  %d - total\n", filmC, videoC, nfrms + 1);
    fprintf(f, "# vfr stats:  longest vid section - %d frames\n", longestV);
    fprintf(f, "# vfr stats:  # of detected vid sections - %d", countVT);
    fclose(f);
    f = nullptr;
  }
  else
  {
    throw TIVTCError("TDecimate:  mkvOut file output error (cannot create file)!");
  }
  if (aLUT.size())
    aLUT.resize(0);

  aLUT.resize(vi.numFrames + 1, 0);

  i = w = 0;
  while (i <= nfrms && w <= vi.numFrames - 1)
  {
    if (input_magic_numbers[i] != 2)
    {
      aLUT[w] = i;
      ++w;
    }
    ++i;
  }
  input_magic_numbers.resize(0);
  nfrmsN = vi.numFrames - 1;

  if (f != nullptr) fclose(f);

  //nfrms and nfrmsN may give some hints as well.
  //8day
  if (orgOut.size())
  {
    if (aLUT.empty())
      throw TIVTCError("TDecimate: aLUT is nullptr!");
    FILE *orgOutF = fopen(orgOut.c_str(), "w");
    if (orgOutF == nullptr)
      throw TIVTCError("TDecimate: cannot create orgOut file!");
    for (int n = 0; n<vi.numFrames; ++n)
    {
      fprintf(orgOutF, "%d\n", aLUT[n]);
    }
    fclose(orgOutF);
  }

} // init mode 5

TDecimate::TDecimate(VSNodeRef *_child, int _mode, int _cycleR, int _cycle, double _rate,
  double _dupThresh, double _vidThresh, double _sceneThresh, int _hybrid,
  int _vidDetect, int _conCycle, int _conCycleTP, const char* _ovr,
  const char* _output, const char* _input, const char* _tfmIn, const char* _mkvOut,
  int _nt, int _blockx, int _blocky, bool _debug, bool _display, int _vfrDec,
  bool _batch, bool _tcfv1, bool _se, bool _chroma, bool _exPP, int _maxndl, bool _m2PA,
  bool _predenoise, bool _noblend, bool _ssd, bool _usehints, VSNodeRef *_clip2,
  int _sdlim, int _opt, const char* _orgOut, const VSAPI *_vsapi, VSCore *core)
    : vsapi(_vsapi), child(_child),
  mode(_mode),
  cycleR(_cycleR), cycle(_cycle), rate(_rate), dupThresh(_dupThresh),
  hybrid(_hybrid), vidThresh(_vidThresh),
  conCycleTP(_conCycleTP), vidDetect(_vidDetect), sceneThresh(_sceneThresh),
  conCycle(_conCycle), ovr(_ovr), input(_input),
  nt(_nt), output(_output), mkvOut(_mkvOut), tfmIn(_tfmIn), blockx(_blockx), blocky(_blocky),
  vfrDec(_vfrDec), debug(_debug), display(_display), batch(_batch), tcfv1(_tcfv1), se(_se),
  maxndl(_maxndl), chroma(_chroma), m2PA(_m2PA), exPP(_exPP),
  noblend(_noblend), predenoise(_predenoise), ssd(_ssd), sdlim(_sdlim),
  opt(_opt), clip2(_clip2), orgOut(_orgOut),
  prev(5, 0), curr(5, 0), next(5, 0), nbuf(5, 0), usehints(_usehints), diff(nullptr, nullptr)
{
    vi_child = vsapi->getVideoInfo(child);
    vi = *vi_child;

  mkvOutF = nullptr;
  FILE *f = nullptr;
  char linein[1024], *linep, *linet;
  
  bool tfmFullInfo = false, metricsFullInfo = false;
  
  fps = (double)vi.fpsNum / (double)vi.fpsDen;

  cpuFlags = *getCPUFeatures();
  if (opt == 0) memset(&cpuFlags, 0, sizeof(cpuFlags));

  if (!vi.format)
      throw TIVTCError("TDecimate: the clip must have constant format.");

  if (vi.width == 0 || vi.height == 0)
      throw TIVTCError("TDecimate: the clip must have constant dimensions.");

  if (vi.format->bitsPerSample > 16)
    throw TIVTCError("TDecimate:  only 8-16 bit formats supported!");
  if (vi.format->colorFamily != cmYUV)
    throw TIVTCError("TDecimate:  YUV colorspaces only!");
  if (mode < 0 || mode > 7)
    throw TIVTCError("TDecimate:  mode must be set to 0, 1, 2, 3, 4, 5, 6, or 7!");
  if (mode == 3 && mkvOut.empty())
    throw TIVTCError("TDecimate:  an mkvOut file must be specified in mode 3!");
  if (mode == 5 && mkvOut.empty())
    throw TIVTCError("TDecimate:  an mkvOut file must be specified in mode 5!");
  if (mode == 6 && mkvOut.empty())
    throw TIVTCError("TDecimate:  an mkvOut file must be specified in mode 6!");
  if (hybrid < 0 || hybrid > 3)
    throw TIVTCError("TDecimate:  hybrid must be set to 0, 1, 2, or 3!");
  if (mode == 3 && hybrid != 2)
    throw TIVTCError("TDecimate:  mode 3 can only be used with hybrid = 2!");
  if (mode == 5 && hybrid != 2)
    throw TIVTCError("TDecimate:  mode 5 can only be used with hybrid = 2!");
  if (mode == 6 && hybrid != 2)
    throw TIVTCError("TDecimate:  mode 6 can only be used with hybrid = 2!");
  if (hybrid == 3 && mode > 1)
    throw TIVTCError("TDecimate:  hybrid = 3 can only be used with modes 0 and 1!");
  if (hybrid == 1 && mode > 1)
    throw TIVTCError("TDecimate:  hybrid = 1 can only be used with modes 0 and 1!");
  if (hybrid > 0 && cycleR > 1)
    throw TIVTCError("TDecimate:  hybrid processing is currently limited to cycleR=1 cases only!");
  if (mode < 2 && hybrid > 1 && hybrid != 3)
    throw TIVTCError("TDecimate:  only hybrid = 0, 1, or 3 is supported in modes 0 and 1!");
  if (cycleR >= cycle || cycleR <= 0)
    throw TIVTCError("TDecimate:  cycleR must be greater than 0 and less than cycle!");
  if (cycle < 2 || cycle > vi.numFrames)
    throw TIVTCError("TDecimate:  cycle must be at least 2 and less than or equal to the number of frames in the clip!");
  if (sceneThresh < 0.0 || sceneThresh > 100.0)
    throw TIVTCError("TDecimate:  sceneThresh must be in the range 0 to 100!");
  if (rate >= fps && (mode == 2 || mode == 7))
    throw TIVTCError("TDecimate:  mode 2 and 7 - new rate must be less than current rate!");
  if (vidDetect < 0 || vidDetect > 4)
    throw TIVTCError("TDecimate:  vidDetect must be set to 0, 1, 2, 3, or 4!");
  if (conCycle > 2)
    throw TIVTCError("TDecimate:  conCycle cannot be greater than 2!");
  if (mode == 4 && (ovr.size() || tfmIn.size()))
    throw TIVTCError("TDecimate:  cannot use an ovr or tfmIn file when in mode 4!");
  if (vfrDec != 0 && vfrDec != 1)
    throw TIVTCError("TDecimate:  vfrDec must be set to 0 or 1!");
  if (output.size() && (mode == 5 || mode == 6))
    throw TIVTCError("TDecimate:  output not supported in mode 5 and 6 (you should already have the metrics)!");
  if (blockx != 4 && blockx != 8 && blockx != 16 && blockx != 32 && blockx != 64 &&
    blockx != 128 && blockx != 256 && blockx != 512 && blockx != 1024 && blockx != 2048)
    throw TIVTCError("TDecimate:  illegal blockx size!");
  if (blocky != 4 && blocky != 8 && blocky != 16 && blocky != 32 && blocky != 64 &&
    blocky != 128 && blocky != 256 && blocky != 512 && blocky != 1024 && blocky != 2048)
    throw TIVTCError("TDecimate:  illegal blocky size!");
  if (mode == 2 && maxndl != -200 && (maxndl < 1 || maxndl > 99))
    throw TIVTCError("TDecimate:  maxndl must be set to a value between 1 and 99 inclusive!");
  if ((mode != 0 && mode != 1 && mode != 3) || cycleR == 1)
    sdlim = 0;
  if ((abs(sdlim) + 1)*(cycleR - 1) >= cycle) {
      char msg[160] = { 0 };
    snprintf(msg, 160, "TDecimate:  invalid sdlim setting (%d through %d (inclusive) are allowed)!", 0, int(ceil(cycle / double(cycleR - 1))) - 2);
    throw TIVTCError(msg);
  }
  if (opt < 0 || opt > 4)
    throw TIVTCError("TDecimate:  opt must be set to 0, 1, 2, 3, or 4!");

  vi_clip2 = vsapi->getVideoInfo(clip2);

  if (vi.numFrames != vi_clip2->numFrames)
    throw TIVTCError("TDecimate:  clip2 must have the same number of frames as the input clip!");
  if (vi_clip2->format->colorFamily != cmYUV)
    throw TIVTCError("TDecimate:  clip2 must be YUV colorspace!");
  if (vi_clip2->format->bitsPerSample > 16)
    throw TIVTCError("TDecimate:  clip2: only 8-16 bit formats supported!");

//  if (debug)
//  {
//    sprintf(buf, "TDecimate:  %s by tritical\n", VERSION);
//    OutputDebugString(buf);
//  }
  ecf = false;
  if (cycle > 5 && mode != 4 && mode != 6 && mode != 7)
  {
    prev.setSize(cycle);
    curr.setSize(cycle);
    next.setSize(cycle);
    nbuf.setSize(cycle);
  }
  if (sdlim)
  {
    prev.sdlim = sdlim;
    curr.sdlim = sdlim;
    next.sdlim = sdlim;
    nbuf.sdlim = sdlim;
  }
  if (mode == 4 || mode == 5 || mode == 6) {
//    child->SetCacheHints(CACHE_GENERIC, 3);
  }
  else if (mode != 2 && mode != 7)
  {
    int cacheRange = cycle * 4 + 1;
    if (cacheRange < 1) cacheRange = 1;
    if (input.size() || cycle >= 26)
    {
//      if (cacheRange > 100)
//        child->SetCacheHints(CACHE_GENERIC, 100);
//      else
//        child->SetCacheHints(CACHE_GENERIC, cacheRange);
    }
    else
    {
      ecf = true; // ecf is not used anywhere. It had to do with the cache manipulation, which we can't do in VapourSynth.
//      child->SetCacheHints(0, -20);
    }
  }

  if (vidDetect == 4)
  {
    vidDetect = 3;
    cve = true;
  }
  else cve = false;
  lastn = -1;
  fullInfo = false;
  same_thresh = diff_thresh = 0;
  linearCount = -342;
  mode2_num = mode2_den = mode2_numCycles = -20;
  memset(mode2_cfs, 0, 10 * sizeof(int));
  nfrms = nfrmsN = vi.numFrames - 1;
  prev.length = curr.length = next.length = nbuf.length = cycle;
  prev.maxFrame = curr.maxFrame = next.maxFrame = nbuf.maxFrame = nfrms;
  blockx_shift = blockx == 4 ? 2 : blockx == 8 ? 3 : blockx == 16 ? 4 : blockx == 32 ? 5 :
    blockx == 64 ? 6 : blockx == 128 ? 7 : blockx == 256 ? 8 : blockx == 512 ? 9 :
    blockx == 1024 ? 10 : 11;
  blocky_shift = blocky == 4 ? 2 : blocky == 8 ? 3 : blocky == 16 ? 4 : blocky == 32 ? 5 :
    blocky == 64 ? 6 : blocky == 128 ? 7 : blocky == 256 ? 8 : blocky == 512 ? 9 :
    blocky == 1024 ? 10 : 11;
  blocky_half = blocky >> 1;
  blockx_half = blockx >> 1;

  char error[512] = "TDecimate: Couldn't fetch the first frame from the input clip to read TFM's PP value. Reason: ";
  size_t len = strlen(error);

  const VSFrameRef *first_frame = vsapi->getFrame(0, child, error + len, 512 - len);
  if (first_frame == nullptr)
      throw TIVTCError(error);

  const VSMap *props = vsapi->getFramePropsRO(first_frame);

  int err;
  int64_t TFMPP = vsapi->propGetInt(props, PROP_TFMPP, 0, &err);
  vsapi->freeFrame(first_frame);
  if (err)
      useTFMPP = false;
  else
      useTFMPP = TFMPP > 1;

  if (exPP) useTFMPP = true;


    if (chroma)
    {
      const int blockx_chroma = blockx >> vi.format->subSamplingW;
      const int blocky_chroma = blocky >> vi.format->subSamplingH;
      if (ssd) 
        MAX_DIFF = (uint64_t)(sqrt(219.0*219.0*blockx*blocky + 224.0*224.0* blockx_chroma * blocky_chroma *2.0));
      else 
        MAX_DIFF = (uint64_t)(219.0*blockx*blocky + 224.0*blockx_half*blocky_half*2.0);
    }
    else
    {
      if (ssd) 
        MAX_DIFF = (uint64_t)(sqrt(219.0*219.0*blockx*blocky));
      else
        MAX_DIFF = (uint64_t)(219.0*blockx*blocky);
    }
    if (ssd)
    {
      sceneThreshU = (uint64_t)((sceneThresh*sqrt(219.0*219.0*vi.height*vi.width)) / 100.0);
      sceneDivU = (uint64_t)(sqrt(219.0*219.0*vi.width*vi.height));
    }
    else
    {
      sceneThreshU = (uint64_t)((sceneThresh*219.0*vi.height*vi.width) / 100.0);
      sceneDivU = (uint64_t)(219.0*vi.width*vi.height);
    }


  if (mode <= 5 || mode == 7)
  {
    diff = decltype(diff) (vs_aligned_malloc<uint64_t>((((vi.width + blockx_half) >> blockx_shift) + 1)*(((vi.height + blocky_half) >> blocky_shift) + 1) * 4 * sizeof(uint64_t), 16), &vs_aligned_free);
    if (diff == nullptr) throw TIVTCError("TDecimate:  malloc failure (diff)!");
  }
  if (output.size())
  {
    if ((f = fopen(output.c_str(), "w")) != nullptr)
    {
      _fullpath(outputFull, output.c_str(), MAX_PATH);
      calcCRC(child, 15, outputCrc, vsapi);
      fclose(f);
      f = nullptr;
      metricsOutArray.resize(vi.numFrames * 2, UINT64_MAX);
    }
    else throw TIVTCError("TDecimate:  output error (cannot create output file)!");
  }
  if (input.size())
  {
    metricsArray.resize(vi.numFrames * 2);

    for (int h = 0; h < vi.numFrames * 2; ++h)
    {
      if (!batch || (mode != 5 && mode != 6)) metricsArray[h] = UINT64_MAX;
      else metricsArray[h] = 0;
    }
    if ((f = fopen(input.c_str(), "r")) != nullptr)
    {
      uint64_t metricU, metricF;
      int w;
      while (fgets(linein, 1024, f) != nullptr)
      {
        if (linein[0] == 0 || linein[0] == '\n' || linein[0] == '\r' || linein[0] == '#' || linein[0] == ';')
          continue;
        linep = linein;
        while (*linep != ' ' && *linep != 0 && *linep != 'c') linep++;
        if (*linep == 'c')
        {
          if (_strnicmp(linein, "crc32 = ", 8) == 0)
          {
            linet = linein;
            while (*linet != ' ') linet++;
            linet++;
            while (*linet != ' ') linet++;
            linet++;
            unsigned int z, tempCrc;
            sscanf(linet, "%x", &z);
            calcCRC(child, 15, tempCrc, vsapi);
            if (tempCrc != z && !batch)
            {
              fclose(f);
              f = nullptr;
              char msg[160] = { 0 };
              snprintf(msg, 160, "TDecimate:  crc32 in input file does not match that of the current clip (%#x vs %#x)!",
                z, tempCrc);
              throw TIVTCError(msg);
            }
            linep = linein;
            while (*linep != ',' && linep != 0) linep++;
            if (*linep == 0) continue;
            linep++; linep++;
            int j;
            if (_strnicmp(linep, "blockx = ", 9) == 0)
            {
              while (*linep != '=') linep++;
              linep++; linep++;
              sscanf(linep, "%d", &j);
              if (j != blockx)
              {
                fclose(f);
                f = nullptr;
                throw TIVTCError("TDecimate:  current blockx value does not match" \
                  " that which was used to create the given input file!");
              }
            }
            linep = linein;
            while (*linep != ',' && *linep != 0) linep++;
            if (*linep == 0) continue;
            linep++;
            while (*linep != ',' && *linep != 0) linep++;
            if (*linep == 0) continue;
            linep++; linep++;
            if (_strnicmp(linep, "blocky = ", 9) == 0)
            {
              while (*linep != '=') linep++;
              linep++; linep++;
              sscanf(linep, "%d", &j);
              if (j != blocky)
              {
                fclose(f);
                f = nullptr;
                throw TIVTCError("TDecimate:  current blocky value does not match" \
                  " that which was used to create the given input file!");
              }
            }
            linep = linein;
            while (*linep != ',' && *linep != 0) linep++;
            if (*linep == 0) continue;
            linep++;
            while (*linep != ',' && *linep != 0) linep++;
            if (*linep == 0) continue;
            linep++;
            while (*linep != ',' && *linep != 0) linep++;
            if (*linep == 0) continue;
            linep++; linep++;

            char ch;
            if (_strnicmp(linep, "chroma = ", 9) == 0)
            {
              while (*linep != '=') linep++;
              linep++; linep++;
              sscanf(linep, "%c", &ch);
              if (((ch == 'T' || ch == 't') && !chroma) || ((ch == 'F' || ch == 'f') && chroma))
              {
                fclose(f);
                f = nullptr;
                throw TIVTCError("TDecimate:  current chroma setting does not match" \
                  " that which was used to create the given input file!");
              }
            }
          }
        }
        else if (*linep == ' ' && *(linep + 1) != 0 && *(linep + 1) != ' ')
        {
          sscanf(linein, "%d %" PRIu64 " %" PRIu64 "", &w, &metricU, &metricF);
          if (w < 0 || w > nfrms)
          {
            fclose(f);
            f = nullptr;
            throw TIVTCError("TDecimate:  input error (out of range frame #)!");
          }
          metricsArray[w * 2] = metricU;
          metricsArray[w * 2 + 1] = metricF;
        }
      }
      fclose(f);
      f = nullptr;
      metricsFullInfo = true;
      for (int h = 0; h < vi.numFrames * 2; h += 2)
      {
        if (metricsArray[h] == UINT64_MAX)
        {
          metricsFullInfo = false;
          if ((mode == 5 || mode == 6) && !batch)
          {
            throw TIVTCError("TDecimate:  input error (mode 5 and 6, all frames must have entries)!");
          }
        }
      }
    }
    else
    {
      throw TIVTCError("TDecimate:  input error (cannot open input file)!");
    }
  }
  else if (mode == 5)
  {
    metricsArray.resize(vi.numFrames * 2);

    for (int h = 0; h < vi.numFrames * 2; h += 2)
    {
      metricsArray[h + 0] = UINT64_MAX - 1;
      metricsArray[h + 1] = 0;
    }
  }
  if (ovr.size())
  {
    if ((f = fopen(ovr.c_str(), "r")) != nullptr)
    {
      if (ovrArray.empty())
      {
        ovrArray.resize(vi.numFrames);
        if (!batch || (mode != 5 && mode != 6)) memset(ovrArray.data(), 112, vi.numFrames);
        else memset(ovrArray.data(), 0, vi.numFrames);
      }
      int q, w, z, count = 0;
      while (fgets(linein, 1024, f) != 0)
      {
        if (linein[0] == 0 || linein[0] == '\n' || linein[0] == '\r' || linein[0] == ';' || linein[0] == '#')
          continue;
        linep = linein;
        while (*linep != 0 && *linep != ' ' && *linep != ',') linep++;
        if (*linep == ' ')
        {
          linet = linein;
          while (*linet != 0)
          {
            if (*linet != ' ' && *linet != 10) break;
            linet++;
          }
          if (*linet == 0) continue;
          linep++;
          if (*linep == '-' || *linep == '+')
          {
            sscanf(linein, "%d", &z);
            if (z<0 || z>nfrms)
            {
              fclose(f);
              f = nullptr;
              throw TIVTCError("TDecimate:  ovr file error (out of range frame #)!");
            }
            linep = linein;
            while (*linep != ' ' && *linep != 0) linep++;
            if (*linep != 0)
            {
              linep++;
              q = *linep;
              if (q == 45) q = DROP_FRAME;
              else if (q == 43) q = KEEP_FRAME;
              else
              {
                fclose(f);
                f = nullptr;
                throw TIVTCError("TDecimate:  ovr file error (invalid specifier)!");
              }
              ovrArray[z] &= 0xFC;
              ovrArray[z] |= q;
            }
          }
          else if (*linep == 'f' || *linep == 'v')
          {
            sscanf(linein, "%d", &z);
            if (z<0 || z>nfrms)
            {
              fclose(f);
              f = nullptr;
              throw TIVTCError("TDecimate:  ovr file error (out of range frame #)!");
            }
            linep = linein;
            while (*linep != ' ' && *linep != 0) linep++;
            if (*linep != 0)
            {
              linep++;
              q = *linep;
              if (q == 102) q = FILM;
              else if (q == 118) q = VIDEO;
              else
              {
                fclose(f);
                f = nullptr;
                throw TIVTCError("TDecimate:  ovr file error (invalid symbol)!");
              }
              ovrArray[z] &= 0xF3;
              ovrArray[z] |= q;
            }
          }
        }
        else if (*linep == ',')
        {
          while (*linep != ' ' && *linep != 0) linep++;
          if (*linep == 0) continue;
          linep++;
          if (*linep == 'f' || *linep == 'v')
          {
            sscanf(linein, "%d,%d", &z, &w);
            if (w == 0) w = nfrms;
            if (z<0 || z>nfrms || w<0 || w>nfrms || w < z)
            {
              fclose(f);
              f = nullptr;
              throw TIVTCError("TDecimate:  input file error (out of range frame #)!");
            }
            linep = linein;
            while (*linep != ' ' && *linep != 0) linep++;
            if (*linep != 0)
            {
              linep++;
              q = *linep;
              if (q == 102) q = FILM;
              else if (q == 118) q = VIDEO;
              else
              {
                fclose(f);
                f = nullptr;
                throw TIVTCError("TDecimate:  input file error (invalid specifier)!");
              }
              while (z <= w)
              {
                ovrArray[z] &= 0xF3;
                ovrArray[z] |= q;
                ++z;
              }
            }
          }
          else if (*linep == '-' || *linep == '+')
          {
            sscanf(linein, "%d,%d", &z, &w);
            if (w == 0) w = nfrms;
            if (z<0 || z>nfrms || w<0 || w>nfrms || w < z)
            {
              fclose(f);
              f = nullptr;
              throw TIVTCError("TDecimate:  input file error (out of range frame #)!");
            }
            linep = linein;
            while (*linep != ' ' && *linep != 0) linep++;
            linep++;
            if (*(linep + 1) == '-' || *(linep + 1) == '+')
            {
              count = 0;
              while ((*linep == '-' || *linep == '+') && (z + count <= w))
              {
                q = *linep;
                if (q == 45) q = DROP_FRAME;
                else if (q == 43) q = KEEP_FRAME;
                else
                {
                  fclose(f);
                  f = nullptr;
                  throw TIVTCError("TDecimate:  input file error (invalid specifier)!");
                }
                ovrArray[z + count] &= 0xFC;
                ovrArray[z + count] |= q;
                ++count;
                linep++;
              }
              while (z + count <= w)
              {
                ovrArray[z + count] &= 0xFC;
                ovrArray[z + count] |= (ovrArray[z] & 0x03);
                ++z;
              }
            }
            else
            {
              q = *linep;
              if (q == 45) q = DROP_FRAME;
              else if (q == 43) q = KEEP_FRAME;
              else
              {
                fclose(f);
                f = nullptr;
                throw TIVTCError("TDecimatee:  input file error (invalid specifier)!");
              }
              while (z <= w)
              {
                ovrArray[z] &= 0xFC;
                ovrArray[z] |= q;
                ++z;
              }
            }
          }
        }
      }
      fclose(f);
      f = nullptr;
    }
    else throw TIVTCError("TDecimate:  ovr error (could not open ovr file)!");
  }
  if (tfmIn.size())
  {
    bool d2vmarked, micmarked;
    if ((f = fopen(tfmIn.c_str(), "r")) != nullptr)
    {
      int fieldt, firstLine, z, q, r;
      if (ovrArray.empty())
      {
        ovrArray.resize(vi.numFrames);
        if (!batch || mode != 5) memset(ovrArray.data(), 112, vi.numFrames);
        else memset(ovrArray.data(), 0, vi.numFrames);
      }
      fieldt = firstLine = 0;
      while (fgets(linein, 1024, f) != nullptr)
      {
        if (linein[0] == 0 || linein[0] == '\n' || linein[0] == '\r' || linein[0] == ';' || linein[0] == '#')
          continue;
        ++firstLine;
        linep = linein;
        while (*linep != 'f' && *linep != 'F' && *linep != 0 && *linep != ' ' && *linep != 'c') linep++;
        if (*linep == 'f' || *linep == 'F')
        {
          if (firstLine == 1)
          {
            if (_strnicmp(linein, "field = top", 11) == 0) fieldt = 1;
            else if (_strnicmp(linein, "field = bottom", 14) == 0) fieldt = 0;
          }
        }
        else if (*linep == ' ')
        {
          linet = linein;
          while (*linet != 0)
          {
            if (*linet != ' ' && *linet != 10) break;
            linet++;
          }
          if (*linet == 0) { --firstLine; continue; }
          sscanf(linein, "%d", &z);
          linep = linein;
          while (*linep != 'p' && *linep != 'c' && *linep != 'n' && *linep != 'u' &&
            *linep != 'b' && *linep != 'l' && *linep != 'h' && *linep != 0) linep++;
          if (*linep != 0)
          {
            if (z<0 || z>nfrms)
            {
              fclose(f);
              f = nullptr;
              throw TIVTCError("TDecimate:  tfmIn file error (out of range frame #)!");
            }
            linep = linein;
            while (*linep != ' ' && *linep != 0) linep++;
            if (*linep != 0)
            {
              linep++;
              q = *linep;
              if (q == 112) q = 0;
              else if (q == 99) q = 1;
              else if (q == 110) q = 2;
              else if (q == 98) q = 3;
              else if (q == 117) q = 4;
              else if (q == 108) q = 5;
              else if (q == 104) q = 6;
              else
              {
                fclose(f);
                f = nullptr;
                throw TIVTCError("TDecimate:  tfmIn file error (invalid match specifier)!");
              }
              if (fieldt != 0)
              {
                if (q == 0) q = 3;
                else if (q == 2) q = 4;
                else if (q == 3) q = 0;
                else if (q == 4) q = 2;
              }
              d2vmarked = micmarked = false;
              linep++;
              while (*linep == ' ' && *linep != 0 && *linep != 10) linep++;
              if (*linep != 0 && *linep != 10)
              {
                r = *linep;
                if (r == 45 && useTFMPP)
                {
                  // intentional noop q = q; 
                }
                else if (r == 43 && q < 5 && useTFMPP)
                {
                  if (fieldt == 0) q = 5;
                  else q = 6;
                }
                else if (r == '1') d2vmarked = true;
                else if (r == '[') micmarked = true;
                else if (r != 43 && r != 45)
                {
                  fclose(f);
                  f = nullptr;
                  throw TIVTCError("TDecimate:  tfmIn file error (invalid specifier)!");
                }
              }
              if (!d2vmarked && !micmarked && *linep != 0 && *linep != 10)
              {
                linep++;
                while (*linep == ' ' && *linep != 0 && *linep != 10) linep++;
                if (*linep != 0 && *linep != 10)
                {
                  r = *linep;
                  if (r == '1') d2vmarked = true;
                }
              }
              if (d2vmarked) ovrArray[z] |= ISD2VFILM;
              ovrArray[z] |= 0x70;
              ovrArray[z] &= ((q << 4) | 0x8F);
            }
          }
        }
      }
      fclose(f);
      f = nullptr;
      tfmFullInfo = true;
      for (int h = 0; h < vi.numFrames; ++h)
      {
        if ((ovrArray[h] & ISMATCH) == 0x70)
        {
          tfmFullInfo = false;
          if (mode == 5 && !batch)
          {
            throw TIVTCError("TDecimate:  tfmIn error (mode 5, all frames must have an entry)!");
          }
        }
      }
    }
    else throw TIVTCError("TDecimate:  tfmIn file error (could not open file)!");
  }
  else if (mode == 5)
  {
    if (ovrArray.empty())
    {
      ovrArray.resize(vi.numFrames, 16);
    }
    else
    {
      for (int i = 0; i < vi.numFrames; ++i)
      {
        ovrArray[i] |= 0x70;
        ovrArray[i] &= ((1 << 4) | 0x8F);
      }
    }
  }

  if (metricsFullInfo && (tfmFullInfo || !usehints)) fullInfo = true;
  else fullInfo = false;

  if (mode < 2)
  {
    if (hybrid != 3)
    {
      vi.numFrames = (vi.numFrames * (cycle - cycleR)) / cycle;
      nfrmsN = vi.numFrames - 1;
      muldivRational(&vi.fpsNum, &vi.fpsDen, cycle - cycleR, cycle);
    }
    else nfrmsN = vi.numFrames - 1;
  }
  else if (mode == 2)
  {
    if (metricsOutArray.empty())
    {
      metricsOutArray.resize(vi.numFrames * 2, UINT64_MAX);
    }
    mode2_decA.resize(vi.numFrames, -20);

    double arate = buildDecStrategy();
    if (mode2_numCycles > 0)
    {
      if (curr.length < 0)
        throw TIVTCError("TDecimate:  unknown error with mode 2!");
//      if (curr.length <= 50)
//        child->SetCacheHints(CACHE_GENERIC, (curr.length * 2) + 1);
//      else
//        child->SetCacheHints(CACHE_GENERIC, 100);
      mode2_order.resize(std::max(curr.length + 10, 100));
      mode2_metrics.resize(std::max(curr.length + 10, 100));
    }
    else {
//      child->SetCacheHints(CACHE_GENERIC, 3);  // fixed to diameter (07/30/2005)
    }
    unsigned int num, den;
    if (FloatToFPS(arate, num, den))
        throw TIVTCError("TDecimate:  rate value is out of range.");
    vi.fpsNum = num;
    vi.fpsDen = den;
    vi.numFrames = (int)(vi.numFrames * (arate / fps));
    nfrmsN = vi.numFrames - 1;
  }
  else if (mode == 7)
  {
    if (metricsOutArray.empty())
    {
      metricsOutArray.resize(vi.numFrames * 2, UINT64_MAX);
      metricsOutArray[0] = 0;
    }
    if (aLUT.size()) aLUT.resize(0);
    aLUT.resize(vi.numFrames, -20);

    if (rate <= 0)
        throw TIVTCError("TDecimate:  rate must be greater than 0.");
    unsigned int num, den;
    if (FloatToFPS(rate, num, den))
        throw TIVTCError("TDecimate:  rate value is out of range.");
    vi.fpsNum = num;
    vi.fpsDen = den;
    vi.numFrames = (int)(vi.numFrames * (rate / fps));
    nfrmsN = vi.numFrames - 1;
    mode2_decA.resize(vi.numFrames, -20);

//    child->SetCacheHints(CACHE_GENERIC, int((fps / rate) + 1.0) * 2 + 3);  // fixed to diameter (07/30/2005)
    diff_thresh = uint64_t((vidThresh*MAX_DIFF) / 100.0);
    same_thresh = uint64_t((dupThresh*MAX_DIFF) / 100.0);
  }
  else if (mode == 3)
  {
    mkvfps = (fps*(cycle - cycleR)) / cycle;
    mkvfps2 = (fps*(cycle - cycleR - 1)) / cycle;
    lastGroup = -1;
    lastCycle = -cycle;
    retFrames = -200;
    lastType = linearCount = 0;
    if ((mkvOutF = fopen(mkvOut.c_str(), "w")) != nullptr)
    {
      if (tcfv1)
      {
        fprintf(mkvOutF, "# timecode format v1\n");
        fprintf(mkvOutF, "Assume %4.6f\n", fps);
      }
      else fprintf(mkvOutF, "# timecode format v2\n");
      fprintf(mkvOutF, "# TDecimate %s by tritical\n", VERSION);
      fprintf(mkvOutF, "# Mode 3 - Auto-generated mkv timecodes file\n");
    }
    else throw TIVTCError("TDecimate:  mode 3 error (cannot create mkvOut file)!");
  }
  else if (mode == 5)
  {
    init_mode_5(core);
    diff = nullptr; // mode 5 is using diff buffer only at init
  } // mode 5
  else if (mode == 6)
  {
    std::vector<int> input_magic_numbers(vi.numFrames, 0);

    int j = 0, k = 0, dups;
    double timestamp = 0.0;
    int lastt = 0, lastf = 0;
    if ((f = fopen(mkvOut.c_str(), "w")) == nullptr)
    {
      throw TIVTCError("TDecimate:  unable to create mkvOut file!");
    }
    if (tcfv1)
    {
      fprintf(f, "# timecode format v1\n");
      fprintf(f, "Assume 23.976024\n");
    }
    else fprintf(f, "# timecode format v2\n");
    fprintf(f, "# TDecimate %s by tritical\n", VERSION);
    fprintf(f, "# Mode 6 - Auto-generated mkv timecodes file\n");
    while (j < vi.numFrames)
    {
      dups = 1;
      ++j;
      while (j < vi.numFrames && metricsArray[j * 2] == 0)
      {
        ++dups;
        ++j;
      }
      while (dups > 0)
      {
        if (dups == 1) // 119.88012
        {
          if (!tcfv1)
          {
            fprintf(f, "%3.6f\n", timestamp*1000.0);
            timestamp += 0.00834166665833;
          }
          else if (lastt != 1 && lastt > 0)
          {
            if (lastt != 5) fprintf(f, "%d,%d,%s\n", lastf, k - 1, cfps(lastt));
            lastt = 1;
            lastf = k;
          }
          else if (lastt <= 0) lastt = 1;
          input_magic_numbers[j - 1] = 2;
          dups = 0;
          ++k;
        }
        else if (dups == 2) // 59.94006
        {
          if (!tcfv1)
          {
            fprintf(f, "%3.6f\n", timestamp*1000.0);
            timestamp += 0.01668333331665;
          }
          else if (lastt != 2 && lastt > 0)
          {
            if (lastt != 5) fprintf(f, "%d,%d,%s\n", lastf, k - 1, cfps(lastt));
            lastt = 2;
            lastf = k;
          }
          else if (lastt <= 0) lastt = 2;
          input_magic_numbers[j - 2] = 2;
          dups = 0;
          ++k;
        }
        else if (dups == 3) // 39.96004
        {
          if (!tcfv1)
          {
            fprintf(f, "%3.6f\n", timestamp*1000.0);
            timestamp += 0.02502499997498;
          }
          else if (lastt != 3 && lastt > 0)
          {
            if (lastt != 5) fprintf(f, "%d,%d,%s\n", lastf, k - 1, cfps(lastt));
            lastt = 3;
            lastf = k;
          }
          else if (lastt <= 0) lastt = 3;
          input_magic_numbers[j - 3] = 2;
          dups = 0;
          ++k;
        }
        else if ((dups % 4) == 0) // 29.97003
        {
          if (!tcfv1)
          {
            int i, repeat = dups >> 2;
            for (i = 0; i < repeat; ++i)
            {
              fprintf(f, "%3.6f\n", timestamp*1000.0);
              timestamp += 0.03336666663330;
            }
          }
          else if (lastt != 4 && lastt > 0)
          {
            if (lastt != 5) fprintf(f, "%d,%d,%s\n", lastf, k - 1, cfps(lastt));
            lastt = 4;
            lastf = k;
          }
          else if (lastt <= 0) lastt = 4;
          k += (dups >> 2);
          for (int i = 0; i < dups; i += 4) input_magic_numbers[j - dups + i] = 2;
          dups = 0;
        }
        else if ((dups % 5) == 0) // 23.97602
        {
          if (!tcfv1)
          {
            int i, repeat = dups / 5;
            for (i = 0; i < repeat; ++i)
            {
              fprintf(f, "%3.6f\n", timestamp*1000.0);
              timestamp += 0.04170834024997;
            }
          }
          else if (lastt != 5 && lastt > 0)
          {
            fprintf(f, "%d,%d,%s\n", lastf, k - 1, cfps(lastt));
            lastt = 5;
            lastf = k;
          }
          else if (lastt <= 0) lastt = 5;
          k += (dups / 5);
          for (int i = 0; i < dups; i += 5) input_magic_numbers[j - dups + i] = 2;
          dups = 0;
        }
        else if (dups > 5)
        {
          if (!tcfv1)
          {
            fprintf(f, "%3.6f\n", timestamp*1000.0);
            timestamp += 0.04170834024997;
          }
          else if (lastt != 5 && lastt > 0)
          {
            fprintf(f, "%d,%d,%s\n", lastf, k - 1, cfps(lastt));
            lastt = 5;
            lastf = k;
          }
          else if (lastt <= 0) lastt = 5;
          input_magic_numbers[j - dups] = 2;
          dups -= 5;
          ++k;
        }
      }
    }
    if (tcfv1 && lastt != 5) fprintf(f, "%d,%d,%s\n", lastf, k - 1, cfps(lastt));
    vi.numFrames = k;
    if (aLUT.size()) { aLUT.resize(0); }
    aLUT.resize(vi.numFrames + 1, 0);

    k = j = 0;
    while (k <= nfrms && j <= vi.numFrames - 1)
    {
      if (input_magic_numbers[k] == 2)
      {
        aLUT[j] = k;
        ++j;
      }
      ++k;
    }

    fclose(f);
    f = nullptr;
    nfrmsN = vi.numFrames - 1;
  } // mode 6
  if (f != nullptr) fclose(f);

    vi.width = vi_clip2->width;
    vi.height = vi_clip2->height;
    vi.format = vi_clip2->format;
}

TDecimate::~TDecimate()
{
  if (metricsOutArray.size())
  {
    if (output.size())
    {
      FILE *f = nullptr;
      if ((f = fopen(outputFull, "w")) != nullptr)
      {
        uint64_t metricU, metricF;
        fprintf(f, "#TDecimate %s by tritical\n", VERSION);
        fprintf(f, "crc32 = %x, blockx = %d, blocky = %d, chroma = %c\n", outputCrc, blockx, blocky,
          chroma ? 'T' : 'F');
        for (int h = 0; h < (nfrms + 1) * 2; h += 2)
        {
          metricU = metricF = UINT64_MAX;
          if (metricsOutArray[h] != UINT64_MAX) metricU = metricsOutArray[h];
          if (metricsOutArray[h + 1] != UINT64_MAX) metricF = metricsOutArray[h + 1];
          if (metricU != UINT64_MAX || metricF != UINT64_MAX)
            fprintf(f, "%d %" PRIu64 " %" PRIu64 "\n", h >> 1, metricU, metricF);
        }
        fclose(f);
        f = nullptr;
      }
      if (f != nullptr) fclose(f);
    }
  }
  if (mkvOutF != nullptr) fclose(mkvOutF);

  vsapi->freeNode(child);
  vsapi->freeNode(clip2);
}
