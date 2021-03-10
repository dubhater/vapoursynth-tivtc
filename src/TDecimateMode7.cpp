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

#include "TDecimate.h"
#include <inttypes.h>
#include <algorithm>

const VSFrameRef * TDecimate::GetFrameMode7(int n, int activationReason, void **frameData, VSFrameContext *frameCtx, VSCore *core)
{
    if (activationReason != arInitial && activationReason != arAllFramesReady)
        return nullptr;

  double ratio = fps / rate;
  int prev_f = int(double(n - 1)*ratio + 1.0);
  if (prev_f < 0) prev_f = 0;
  int curr1_f = int(double(n)*ratio);
  if (curr1_f > nfrms) mode2_decA[n] = nfrms;
  int curr2_f = int(double(n)*ratio + 1.0);
  if (curr2_f > nfrms) mode2_decA[n] = nfrms;
  int next_f = int(double(n + 1)*ratio);
  if (next_f > nfrms) next_f = nfrms;
  int curr_real = mode2_decA[n];
  int chosen = 0;

  if (curr_real == -20) {
    int prev_real = n == 0 ? -20 : mode2_decA[n - 1];
    if (prev_real != -20) prev_f = prev_real;
    bool rup = double(n) * ratio - double(curr1_f) >= 0.5 ? true : false;
    bool requesting_frames = false;
    for (int i = std::max(prev_f - 3, 1); i <= std::min(next_f + 2, nfrms); ++i)
    {
      if (metricsOutArray[i << 1] == UINT64_MAX)
      {
        if (metricsArray.size() && metricsArray[i << 1] != UINT64_MAX)
          metricsOutArray[i << 1] = metricsArray[i << 1];
        else
        {
            if (activationReason == arInitial) {
                vsapi->requestFrameFilter(i - 1, child, frameCtx);
                vsapi->requestFrameFilter(i, child, frameCtx);
                requesting_frames = true;
            } else {
              int blockNI, blocksI;
              uint64_t metricF;
              const VSFrameRef *frame1 = vsapi->getFrameFilter(i - 1, child, frameCtx);
              const VSFrameRef *frame2 = vsapi->getFrameFilter(i, child, frameCtx);
              metricsOutArray[i << 1] =
                calcMetric(frame1, frame2,
                  vi_child, blockNI, blocksI, metricF, false, core);
              vsapi->freeFrame(frame1);
              vsapi->freeFrame(frame2);
            }
        }
      }
    }
    if (requesting_frames)
        return nullptr;

    try {
        if (same_group(curr1_f, curr2_f))
        {
          if (next_f - curr2_f > 1 && similar_group(prev_f, curr2_f) &&
            diff_group(next_f, next_f + 1) && diff_group(curr2_f, curr2_f + 1))
            chosen = 4;
          else chosen = 2;
        }
        else if (same_group(prev_f, curr1_f)) chosen = 1;
        else if (similar_group(prev_f, curr1_f))
        {
          if (similar_group(curr1_f, curr2_f) && !same_group(curr2_f, next_f))
            chosen = 3;
          else if (diff_group(curr1_f, curr2_f))
            chosen = 1;
        }
        else if (diff_group(prev_f, curr1_f))
        {
          if (diff_group(curr2_f, next_f)) chosen = 3;
          else if (diff_group(curr1_f, curr2_f) && same_group(curr1_f - 1, curr1_f) &&
            same_group(curr2_f, next_f) && diff_group(next_f, next_f + 1) &&
            curr1_f - prev_f == 2 && diff_group(prev_f - 1, prev_f))
            chosen = 1;
        }
    } catch (const TIVTCError &e) {
        vsapi->setFilterError(e.what(), frameCtx);
        return nullptr;
    }

    if (chosen == 4) mode2_decA[n] = curr2_f + 1;
    else if (chosen >= 2) // either
    {
      if ((chosen == 2 && rup) || (chosen == 3 &&
        ((metricsOutArray[curr2_f << 1] * 2 > metricsOutArray[curr1_f << 1] * 3) ||
          (rup && metricsOutArray[curr2_f << 1] * 3 >= metricsOutArray[curr1_f << 1] * 2))))
        mode2_decA[n] = curr2_f;
      else mode2_decA[n] = curr1_f;
    }
    else if (chosen == 0) mode2_decA[n] = curr1_f;
    else mode2_decA[n] = curr2_f;
  }

  int ret = mode2_decA[n];
  if (ret < 0 || ret > nfrms) {
      vsapi->setFilterError("TDecimate:  mode 7 internal error! Tried to request a frame that doesn't exist.", frameCtx);
      return nullptr;
  }

//  if (debug)
//  {
//    sprintf(buf, "TDecimate:  ------------------------------------------\n");
//    OutputDebugString(buf);
//    sprintf(buf, "TDecimate:  inframe = %d  useframe = %d  chosen = %d\n", n, ret, chosen);
//    OutputDebugString(buf);
//    sprintf(buf, "TDecimate:  prev = %d  curr1 = %d  curr2 = %d  next = %d\n", prev_f,
//      curr1_f, curr2_f, next_f);
//    OutputDebugString(buf);
//    for (int i = std::max(0, ret - 3); i <= std::min(ret + 3, nfrms); ++i)
//    {
//      sprintf(buf, "TDecimate:  %d:  %3.2f  %" PRIu64 "%s%s\n", i, double(metricsOutArray[i << 1])*100.0 / double(MAX_DIFF),
//        metricsOutArray[i << 1], metricsOutArray[i << 1] < same_thresh ? "  (D)" :
//        metricsOutArray[i << 1] > diff_thresh ? "  (N)" :
//        aLUT[i] == 2 ? "  (N)" : aLUT[i] == 1 ? "  (S)" :
//        aLUT[i] == 0 ? "  (D)" : "", wasChosen(i, n) ? "  *" : "");
//      OutputDebugString(buf);
//    }
//  }

  enum {
      RetFrameIsReady = 69,
  };

  if (activationReason == arInitial || (activationReason == arAllFramesReady && (intptr_t)*frameData != RetFrameIsReady)) {
      vsapi->requestFrameFilter(ret, clip2, frameCtx);
      *frameData = (void *)RetFrameIsReady;
      return nullptr;
  }

  const VSFrameRef *src = vsapi->getFrameFilter(ret, clip2, frameCtx);

  if (display)
  {
    VSFrameRef *dst = vsapi->copyFrame(src, core);
    vsapi->freeFrame(src);

#define SZ 160
    char buf[SZ] = { 0 };

    std::string text = "TDecimate " VERSION " by tritical\n";

    snprintf(buf, SZ, "Mode: 7  Rate = %3.6f\n", rate);
    text += buf;
    snprintf(buf, SZ, "inframe = %d  useframe = %d  chosen = %d\n", n, ret, chosen);
    text += buf;
    snprintf(buf, SZ, "p = %d  c1 = %d  c2 = %d  n = %d\n", prev_f,
      curr1_f, curr2_f, next_f);
    text += buf;
    snprintf(buf, SZ, "dt = %3.2f  %" PRIu64 "  vt = %3.2f  %" PRIu64 "\n", dupThresh, same_thresh,
      vidThresh, diff_thresh);
    text += buf;

    for (int i = std::max(0, ret - 3); i <= std::min(ret + 3, nfrms); ++i)
    {
      snprintf(buf, SZ, "%d:  %3.2f  %" PRIu64 "%s%s\n", i, double(metricsOutArray[i << 1])*100.0 / double(MAX_DIFF),
        metricsOutArray[i << 1], metricsOutArray[i << 1] < same_thresh ? "  (D)" :
        metricsOutArray[i << 1] > diff_thresh ? "  (N)" :
        aLUT[i] == 2 ? "  (N)" : aLUT[i] == 1 ? "  (S)" :
        aLUT[i] == 0 ? "  (D)" : "", wasChosen(i, n) ? "  *" : "");
    text += buf;
    }
#undef SZ

    VSMap *props = vsapi->getFramePropsRW(dst);
    vsapi->propSetData(props, PROP_TDecimateDisplay, text.c_str(), text.size(), paReplace);

    return dst;
  }
  return src;
}

bool TDecimate::wasChosen(int i, int n)
{
  for (int p = std::max(n - 5, 0); p < std::min(n + 5, nfrmsN); ++p)
  {
    if (mode2_decA[p] == i) return true;
  }
  return false;
}

bool TDecimate::same_group(int f1, int f2)
{
  return diff_f(f1, f2) <= 0;
}

bool TDecimate::similar_group(int f1, int f2)
{
  return diff_f(f1, f2) <= 1;
}

bool TDecimate::diff_group(int f1, int f2)
{
  return diff_f(f1, f2) == 2;
}

int TDecimate::diff_f(int f1, int f2)
{
  int mx = 0;
  if (f2 < f1 || f2 < 0 || f1 > nfrms)
      throw TIVTCError("TDecimate:  mode 7 internal error (f2 < f1)!");
  if (f1 < 0) f1 = 0;
  if (f2 > nfrms) f2 = nfrms;
  if (f1 == f2)
  {
    if (aLUT[f2] == -20)
      aLUT[f2] = mode7_analysis(f2);
    return 0;
  }
  for (int i = f1 + 1; i <= f2; ++i)
  {
    if (aLUT[i] == -20) aLUT[i] = mode7_analysis(i);
    mx = std::max(mx, aLUT[i]);
  }
  return mx;
}

int TDecimate::mode7_analysis(int n) const
{
  uint64_t vals[3] = { UINT64_MAX, UINT64_MAX, UINT64_MAX };
  if (n == 0) return 2;
  vals[0] = metricsOutArray[(n - 1) << 1];
  vals[1] = metricsOutArray[n << 1];
  if (n != nfrms) vals[2] = metricsOutArray[(n + 1) << 1];
  if (n == nfrms)
  {
    if (vals[0] == UINT64_MAX || vals[1] == UINT64_MAX)
        throw TIVTCError("TDecimate:  mode 7 internal error (uncalculated metrics)!");
    if (vals[1] > diff_thresh || vals[1] * 2 > vals[0] * 3) return 2;
    else if (vals[1] < same_thresh || vals[1] * 4 < vals[0] ||
      (vals[1] * 2 < vals[0] && vals[0] > diff_thresh)) return 0;
    return 1;
  }
  if (vals[0] == UINT64_MAX || vals[1] == UINT64_MAX || vals[2] == UINT64_MAX)
      throw TIVTCError("TDecimate:  mode 7 internal error (uncalculated metrics)!");
  if (vals[1] > diff_thresh) return 2; // definitely different
  else if (vals[1] < same_thresh) return 0; // definitely the same
  else if (vals[1] < vals[0] && vals[1] < vals[2]) // local minimum difference
  {
    uint64_t minn = std::min(vals[0], vals[2]);
    if (vals[1] * 2 < minn && vals[0] > diff_thresh && vals[2] > diff_thresh) return 0;
    else if (vals[1] * 4 < minn) return 0;
  }
  else if (vals[1] > vals[0] && vals[1] > vals[2]) // local maximum difference
  {
    uint64_t maxn = std::max(vals[0], vals[2]);
    if (vals[1] * 2 > maxn * 3) return 2;
  }
  return 1;
}
