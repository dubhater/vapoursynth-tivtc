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

#include <string>
#include <vector>
#include <math.h>
#include <VapourSynth.h>
#include "cpufeatures.h"
#ifdef VERSION
#undef VERSION
#endif
#define VERSION "v1.0.3"

template<typename pixel_t>
void maskClip2_C(const uint8_t* srcp, const uint8_t* dntp,
  const uint8_t* maskp, uint8_t* dstp, int src_pitch, int dnt_pitch,
  int msk_pitch, int dst_pitch, int width, int height);

void maskClip2_SSE2(const uint8_t* srcp, const uint8_t* dntp,
  const uint8_t* maskp, uint8_t* dstp, int src_pitch, int dnt_pitch,
  int msk_pitch, int dst_pitch, int width, int height);

template<typename pixel_t>
#if defined(GCC) || defined(CLANG)
__attribute__((__target__("sse4.1")))
#endif 
void maskClip2_SSE4(const uint8_t* srcp, const uint8_t* dntp,
  const uint8_t* maskp, uint8_t* dstp, int src_pitch, int dnt_pitch,
  int msk_pitch, int dst_pitch, int width, int height);

template<bool with_mask>
void blendDeintMask_SSE2(const uint8_t* srcp, uint8_t* dstp,
  const uint8_t* maskp, int src_pitch, int dst_pitch, int msk_pitch,
  int width, int height);

template<typename pixel_t, bool with_mask>
void blendDeintMask_C(const pixel_t* srcp, pixel_t* dstp,
  const uint8_t* maskp, int src_pitch, int dst_pitch, int msk_pitch,
  int width, int height);

template<bool with_mask>
void cubicDeintMask_SSE2(const uint8_t* srcp, uint8_t* dstp,
  const uint8_t* maskp, int src_pitch, int dst_pitch, int msk_pitch,
  int width, int height);

template<typename pixel_t, int bits_per_pixel, bool with_mask>
void cubicDeintMask_C(const pixel_t* srcp, pixel_t* dstp,
  const uint8_t* maskp, int src_pitch, int dst_pitch, int msk_pitch,
  int width, int height);

class TFMPP
{
private:
    const VSAPI *vsapi;
    VSNodeRef *child;

  CPUFeatures cpuFlags;

  int PP, mthresh;
  std::string ovr;
  bool display;
  VSNodeRef *clip2;
  bool usehints;
  int opt;
  bool uC2; // use clip2
  int PP_origSaved;
  int mthresh_origSaved;
  int nfrms;
  std::vector<int> setArray;
  VSFrameRef *mmask;

  void buildMotionMask(const VSFrameRef *prv, const VSFrameRef *src, const VSFrameRef *nxt,
    VSFrameRef *mask, int use) const;
  template<typename pixel_t>
  void buildMotionMask_core(const VSFrameRef *prv, const VSFrameRef *src, const VSFrameRef *nxt,
    VSFrameRef* mask, int use) const;
  void maskClip2(const VSFrameRef *src, const VSFrameRef *deint, const VSFrameRef *mask,
    VSFrameRef *dst) const;

//  void putHint(VSFrameRef *dst, int field, unsigned int hint);
//  template<typename pixel_t>
//  void putHint_core(VSFrameRef *dst, int field, unsigned int hint);
  void getProperties(const VSFrameRef *src, int& field, bool& combed) const;
//  template<typename pixel_t>
//  bool getHint_core(const VSFrameRef *src, int& field, bool& combed, unsigned int& hint);

  void getSetOvr(int n);

//  void denoiseYUY2(VSFrameRef *mask);
  void denoisePlanar(VSFrameRef *mask) const;

//  void linkYUY2(VSFrameRef *mask);
  template<int planarType>
  void linkPlanar(VSFrameRef *mask) const;

//  void destroyHint(VSFrameRef *dst, unsigned int hint);
//  template<typename pixel_t>
//  void destroyHint_core(VSFrameRef *dst, unsigned int hint);

  void BlendDeint(const VSFrameRef *src, const VSFrameRef *mask, VSFrameRef *dst,
    bool nomask) const;
  template<typename pixel_t>
  void BlendDeint_core(const VSFrameRef *src, const VSFrameRef* mask, VSFrameRef *dst,
    bool nomask) const;

  void CubicDeint(const VSFrameRef *src, const VSFrameRef *mask, VSFrameRef *dst, bool nomask,
    int field) const;
  template<typename pixel_t, int bits_per_pixel>
  void CubicDeint_core(const VSFrameRef *src, const VSFrameRef* mask, VSFrameRef *dst, bool nomask,
    int field) const;

  void elaDeint(VSFrameRef *dst, const VSFrameRef *mask, const VSFrameRef *src, bool nomask, int field) const;
  // not the same as in tdeinterlace.
  template<typename pixel_t, int bits_per_pixel>
  void elaDeintPlanar(VSFrameRef *dst, const VSFrameRef *mask, const VSFrameRef *src, bool nomask, int field) const;
//  void elaDeintYUY2(VSFrameRef *dst, const VSFrameRef *mask, const VSFrameRef *src, bool nomask, int field);

  void copyField(VSFrameRef *dst, const VSFrameRef *src, int field) const;
  void buildMotionMask1_SSE2(const uint8_t *srcp1, const uint8_t *srcp2,
    uint8_t *dstp, int s1_pitch, int s2_pitch, int dst_pitch, int width, int height, const CPUFeatures *cpu) const;
  void buildMotionMask2_SSE2(const uint8_t *srcp1, const uint8_t *srcp2,
    const uint8_t *srcp3, uint8_t *dstp, int s1_pitch, int s2_pitch,
    int s3_pitch, int dst_pitch, int width, int height, const CPUFeatures *cpu) const;

  void writeDisplay(VSFrameRef *dst, int n, int field) const;

public:
  const VSVideoInfo *vi;

  const VSFrameRef *GetFrame(int n, int activationReason, VSFrameContext *frameCtx, VSCore *core);
  TFMPP(VSNodeRef *_child, int _PP, int _mthresh, const char* _ovr, bool _display, VSNodeRef *_clip2,
    bool _usehints, int _opt, const VSAPI *_vsapi, VSCore *core);
  ~TFMPP();
};
