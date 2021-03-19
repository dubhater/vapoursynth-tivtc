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

#ifndef TDECIMATE_H
#define TDECIMATE_H

#include <stdio.h>
#include <math.h>
#ifndef _WIN32
#include <limits.h>
#include <stdlib.h>
#include <strings.h>
#define _strnicmp strncasecmp
#define _fullpath(absolute, relative, max) realpath((relative), (absolute))
#define MAX_PATH PATH_MAX
#else
#include <windows.h>
#endif
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <VapourSynth.h>
#include <VSHelper.h>

#include "internal.h"
//#include "Font.h"
#include "Cycle.h"
#include "calcCRC.h"
//#include "profUtil.h"
//#include "Cache.h"
#include "cpufeatures.h"

enum {
    RetFrameIsReady = 69,
};

// All the rest of this code was just copied from tdecimate.cpp because I'm
// too lazy to make it work such that it could call that code.
// pinterf 2020: moved the three versions to common codebase again: CalcMetricsExtracted().
struct CalcMetricData {
  bool predenoise;
  VSVideoInfo vi;
  bool chroma;
  const CPUFeatures *cpuFlags;
  int blockx;
  int blockx_half;
  int blockx_shift;
  int blocky;
  int blocky_half;
  int blocky_shift;
  uint64_t* diff;
  int nt;
  bool ssd; // ssd or sad

  bool metricF_needed; // from TDecimate: true, from FrameDiff: false
  // TDecimate
  uint64_t* metricF; // out!
  bool scene;
};

void CalcMetricsExtracted(const VSFrameRef *prevt, const VSFrameRef *currt, CalcMetricData& d, VSCore *core, const VSAPI *vsapi);

void blurFrame(const VSFrameRef *src, VSFrameRef *dst, int iterations,
  bool bchroma, const CPUFeatures *cpuFlags, VSCore *core, const VSAPI *vsapi);

uint64_t calcLumaDiffYUY2_SSD(const uint8_t* prvp, const uint8_t* nxtp,
  int width, int height, int prv_pitch, int nxt_pitch, int nt, int cpuFlags);

uint64_t calcLumaDiffYUY2_SAD(const uint8_t* prvp, const uint8_t* nxtp,
  int width, int height, int prv_pitch, int nxt_pitch, int nt, int cpuFlags);

class TDecimate
{
private:
    const VSAPI *vsapi;
    VSNodeRef *child;
    const VSVideoInfo *vi_child;
    const VSVideoInfo *vi_clip2;

  CPUFeatures cpuFlags;

  int mode;
  int cycleR, cycle;
  double rate, dupThresh;
  int hybrid;
  double vidThresh;
  int conCycleTP;
  int vidDetect;
  double sceneThresh;
  int conCycle;
  std::string ovr;
  std::string input;
  int nt;
  std::string output;
  std::string mkvOut;
  std::string tfmIn;
  int blockx, blocky;
  int vfrDec;
  bool debug, display;
  bool batch;
  bool tcfv1;
  bool se;
  int maxndl;
  bool chroma;
  bool m2PA;
  bool exPP;
  bool noblend;
  bool predenoise;
  bool ssd; // sum of squared distances (false = SAD)
  int sdlim;
  int opt;
  VSNodeRef *clip2;
  std::string orgOut;
  Cycle prev, curr, next, nbuf;

  int nfrms, nfrmsN, linearCount;
  int blocky_shift, blockx_shift, blockx_half, blocky_half;
  int lastn;
  int lastFrame, lastCycle, lastGroup, lastType, retFrames;
  uint64_t MAX_DIFF, sceneThreshU, sceneDivU, diff_thresh, same_thresh;
  double fps, mkvfps, mkvfps2;
  bool useTFMPP, cve, ecf, fullInfo;
  bool usehints;
  std::unique_ptr<uint64_t, decltype (&vs_aligned_free)> diff;
  std::vector<uint64_t> metricsArray, metricsOutArray, mode2_metrics;
  std::vector<int> aLUT, mode2_decA, mode2_order;
  std::unordered_map<int, std::pair<int, int>> frame_duration_info;
  unsigned int outputCrc;
  std::vector<uint8_t> ovrArray;
  int mode2_num, mode2_den, mode2_numCycles, mode2_cfs[10];
  FILE *mkvOutF;
  char outputFull[MAX_PATH];

  void init_mode_5(VSCore *core);
  void rerunFromStart(const int s, VSFrameContext *frameCtx, VSCore *core);
  void checkVideoMetrics(Cycle &c, double thresh);
  void checkVideoMatches(Cycle &p, Cycle &c);
  bool checkMatchDup(int mp, int mc);
  void findDupStrings(Cycle &p, Cycle &c, Cycle &n);

  int getTFMFrameProperties(const VSFrameRef *src, int& d2vfilm) const;
//  template<typename pixel_t>
//  int getHint_core(const VSFrameRef *src, int &d2vfilm);

//  template<typename pixel_t>
//  void restoreHint(const VSFrameRef *dst);

  void blendFrames(const VSFrameRef *src1, const VSFrameRef *src2, VSFrameRef *dst,
    double amount1);
  void calcBlendRatios(double &amount1, double &amount2, int &frame1, int &frame2, int n,
    int bframe, int cycleI);

  const VSFrameRef *GetFrameMode01(int n, int activationReason, void **frameData, VSFrameContext *frameCtx, VSCore *core);
  const VSFrameRef *GetFrameMode2(int n, int activationReason, void **frameData, VSFrameContext *frameCtx, VSCore *core);
  const VSFrameRef *GetFrameMode3(int n, int activationReason, void **frameData, VSFrameContext *frameCtx, VSCore *core);
  const VSFrameRef *GetFrameMode4(int n, int activationReason, VSFrameContext *frameCtx, VSCore *core);
  const VSFrameRef *GetFrameMode56(int n, int activationReason, VSFrameContext *frameCtx, VSCore *core);
  const VSFrameRef *GetFrameMode7(int n, int activationReason, void **frameData, VSFrameContext *frameCtx, VSCore *core);
  void getOvrFrame(int n, uint64_t &metricU, uint64_t &metricF) const;
  void getOvrCycle(Cycle &current, bool mode2);
  void displayOutput(VSFrameRef *dst, int n,
    int ret, bool film, double amount1, double amount2, int f1, int f2);
  void formatMetrics(Cycle &current);
  void formatDups(Cycle &current);
  void formatDecs(std::string &buf, Cycle &current);
  void formatMatches(Cycle &current);
  void formatMatches(Cycle &current, Cycle &previous);
  void debugOutput1(int n, bool film, int blend);
  void debugOutput2(int n, int ret, bool film, int f1, int f2, double amount1, double amount2);
  void addMetricCycle(const Cycle &j);
  bool checkForObviousDecFrame(Cycle &p, Cycle &c, Cycle &n);
  void mostSimilarDecDecision(Cycle &p, Cycle &c, Cycle &n);
  int checkForD2VDecFrame(Cycle &p, Cycle &c, Cycle &n);
  bool checkForTwoDropLongestString(Cycle &p, Cycle &c, Cycle &n);
  int getNonDecMode2(int n, int start, int stop) const;
  double buildDecStrategy();
  void mode2MarkDecFrames(int cycleF);
  void removeMinN(int m, int n, int start, int stop);
  void removeMinN(int m, int n, uint64_t *metricsT, int *orderT, int &ovrC);
  int findDivisor(double decRatio, int min_den) const;
  int findNumerator(double decRatio, int divisor) const;
  double findCorrectionFactors(double decRatio, int num, int den, int rc[10]) const;
  void sortMetrics(uint64_t *metrics, int *order, int length) const;
  //void SedgeSort(uint64_t *metrics, int *order, int length);
  //void pQuickerSort(uint64_t *metrics, int *order, int lower, int upper);
  void calcMetricCycle(Cycle &current, bool scene, bool hnt, VSCore *core, VSFrameContext *frameCtx=nullptr) const;
  uint64_t calcMetric(const VSFrameRef *prevt, const VSFrameRef *currt, const VSVideoInfo *vi, int &blockNI,
    int &xblocksI, uint64_t &metricF, bool scene, VSCore *core) const;


  void calcBlendRatios2(double &amount1, double &amount2, int &frame1,
    int &frame2, int tf, Cycle &p, Cycle &c, Cycle &n, int remove);
  bool similar_group(int f1, int f2);
  bool same_group(int f1, int f2);
  bool diff_group(int f1, int f2);
  int diff_f(int f1, int f2);
  int mode7_analysis(int n) const;

  bool wasChosen(int i, int n);
  void calcMetricPreBuf(int n1, int n2, int pos, const VSVideoInfo *vit, bool scene, bool gethint, VSFrameContext *frameCtx, VSCore *core);
public:
  VSVideoInfo vi;

  const VSFrameRef *GetFrame(int n, int activationReason, void **frameData, VSFrameContext *frameCtx, VSCore *core);
  TDecimate(VSNodeRef *_child, int _mode, int _cycleR, int _cycle, double _rate,
    double _dupThresh, double _vidThresh, double _sceneThresh, int _hybrid,
    int _vidDetect, int _conCycle, int _conCycleTP, const char* _ovr,
    const char* _output, const char* _input, const char* _tfmIn, const char* _mkvOut,
    int _nt, int _blockx, int _blocky, bool _debug, bool _display, int _vfrDec,
    bool _batch, bool _tcfv1, bool _se, bool _chroma, bool _exPP, int _maxndl,
    bool _m2PA, bool _predenoise, bool _noblend, bool _ssd, bool _usehints,
    VSNodeRef *_clip2, int _sdlim, int _opt, const char* _orgOut, const VSAPI *_vsapi, VSCore *core);
  ~TDecimate();

//  int __stdcall SetCacheHints(int cachehints, int frame_range) override {
//    return cachehints == CACHE_GET_MTMODE ? MT_SERIALIZED : 0;
//  }
};

#endif // TDECIMATE_H
