
#include <ableton/Link.hpp>
#include <ableton/link/HostTimeFilter.hpp>
#include <ableton/platforms/Config.hpp>
#include <chrono>
#include <iostream>
#include "SC_Unit.h"
#include "SC_PlugIn.h"

static InterfaceTable *ft;

static ableton::Link *gLink = nullptr;
static std::chrono::microseconds gLatency = std::chrono::microseconds(0);
static float gTempo = 60.0;

// Temporary debug flag - set to true to force clock timing
static bool gForceClockTiming = true;

// ========================================================================================================
//
// Link Interface for SuperCollider
//
// ========================================================================================================

struct LinkEnabler : public Unit
{
};

extern "C"
{
  void LinkEnabler_Ctor(LinkEnabler *unit);
  void LinkEnabler_next(LinkEnabler *unit, int inNumSamples);
}

void LinkEnabler_Ctor(LinkEnabler *unit)
{
  gLatency = std::chrono::microseconds(static_cast<long long>(*IN(1)));
  Print("Link latency set to %d\n", gLatency.count());
  if (!gLink)
  {
    gTempo = *IN(0);
    gLink = new ableton::Link(gTempo);
    gLink->enable(true);
  }
  else
  {
    // std::cout<<"Link already running"<<std::endl;
  }
  SETCALC(LinkEnabler_next);
}

void LinkEnabler_next(LinkEnabler *unit, int inNumSamples)
{
}

struct LinkDisabler : public Unit
{
};

extern "C"
{
  void LinkDisabler_Ctor(LinkDisabler *unit);
  void LinkDisabler_next(LinkDisabler *unit, int inNumSamples);
}

void LinkDisabler_Ctor(LinkDisabler *unit)
{
  if (gLink)
  {
    gLink->enable(false);
    delete gLink;
    gLink = nullptr;
  }
  else
  {
    // std::cout<<"Link not running"<<std::endl;
  }
  SETCALC(LinkDisabler_next);
}

void LinkDisabler_next(LinkDisabler *unit, int inNumSamples)
{
}

struct Link : public Unit
{
  float mLastBeat;
  ableton::link::HostTimeFilter<ableton::link::platform::Clock> mHostTimeFilter;
  bool mUseHostTimeFilter;
  bool mHostTimeFilterInitialized;
};

extern "C"
{
  void Link_Ctor(Link *unit);
  void Link_next(Link *unit, int inNumSamples);
}

void Link_Ctor(Link *unit)
{
  if (!gLink)
  {
    Print("warn: Link not enabled!\n");
  }

  unit->mLastBeat = 0.0;
  unit->mHostTimeFilterInitialized = false;
  unit->mUseHostTimeFilter = true; // Default to sample-accurate timing

  // Initialize HostTimeFilter
  try {
    unit->mHostTimeFilter.reset();
    unit->mHostTimeFilterInitialized = true;
    Print("Link: HostTimeFilter initialized\n");
  }
  catch (...) {
    Print("Link: Failed to initialize HostTimeFilter, falling back to clock timing\n");
    unit->mUseHostTimeFilter = false;
    unit->mHostTimeFilterInitialized = false;
  }

  SETCALC(Link_next);
}

void Link_next(Link *unit, int inNumSamples)
{
  float *output = OUT(0);

  if (gLink)
  {
    if (!gForceClockTiming && unit->mUseHostTimeFilter && unit->mHostTimeFilterInitialized) {
      try {
        // Calculate sample-accurate host time using current clock time as reference
        uint64 sampleTime = (unit->mWorld->mBufCounter * unit->mWorld->mBufLength) + unit->mWorld->mSampleOffset;
        auto currentClockTime = gLink->clock().micros();

        // Convert sample offset to time offset
        double sampleRate = unit->mWorld->mSampleRate;
        auto sampleOffset = static_cast<double>(unit->mWorld->mSampleOffset);
        auto timeOffsetMicros = static_cast<long long>((sampleOffset / sampleRate) * 1000000.0);

        // Use clock time with sample-accurate offset
        auto hostTime = currentClockTime + std::chrono::microseconds(timeOffsetMicros);
        auto timeline = gLink->captureAudioSessionState();
        const auto beats = timeline.beatAtTime(hostTime, 4);
        *output = static_cast<float>(beats);
        unit->mLastBeat = *output;

        // Debug output every 500 buffers to compare with clock timing
        static int debugCounter = 0;
        if (++debugCounter % 500 == 0) {
          auto clockTime = gLink->clock().micros();
          auto clockTimeline = gLink->captureAudioSessionState();
          auto clockBeats = clockTimeline.beatAtTime(clockTime, 4);

          Print("Link Sample-Accurate Debug:\n");
          Print("  SampleOffset=%d, timeOffsetMicros=%lld\n", unit->mWorld->mSampleOffset, timeOffsetMicros);
          Print("  hostTime=%llu, clockTime=%llu\n", hostTime.count(), clockTime.count());
          Print("  Sample-accurate beats=%f, clock beats=%f\n", *output, clockBeats);
        }
      }
      catch (...) {
        Print("Link: Sample-accurate timing failed, switching to clock timing\n");
        unit->mUseHostTimeFilter = false;
        // Fall through to clock timing
      }
    }

    if (gForceClockTiming || !unit->mUseHostTimeFilter || !unit->mHostTimeFilterInitialized) {
      // Fallback approach: Use clock time with latency compensation
      // This may have more jitter but is more compatible across platforms
      const auto time = gLink->clock().micros() + gLatency;
      auto timeline = gLink->captureAudioSessionState();
      const auto beats = timeline.beatAtTime(time, 4);
      *output = static_cast<float>(beats);
      unit->mLastBeat = *output;

      // Debug output for clock timing method
      static int clockDebugCounter = 0;
      if (++clockDebugCounter % 1000 == 0) {
        Print("Link Clock Debug: time=%llu, latency=%lld, beats=%f\n",
              time.count(), gLatency.count(), *output);
      }
    }
  }
  else
  {
    *output = unit->mLastBeat;
  }
}

struct LinkTempo : public Unit
{
  double mCurTempo;
  double mTempoCalc;
};

extern "C"
{
  void LinkTempo_Ctor(LinkTempo *unit);
  void LinkTempo_next(LinkTempo *unit, int inNumSamples);
}

void LinkTempo_Ctor(LinkTempo *unit)
{

  if (!gLink)
  {
    Print("Link not enabled! can't set Link Tempo!\n");
  }
  else
  {
    const auto timeline = gLink->captureAudioSessionState();
    unit->mCurTempo = timeline.tempo();
    unit->mTempoCalc = unit->mCurTempo - *IN(0);
  }

  SETCALC(LinkTempo_next);
}

void LinkTempo_next(LinkTempo *unit, int inNumSamples)
{
  if (gLink)
  {
    auto timeline = gLink->captureAudioSessionState();
    timeline.setTempo(unit->mCurTempo - (*IN(1) * unit->mTempoCalc), gLink->clock().micros());
    gLink->commitAudioSessionState(timeline);
  }
}

// tempo as a ugen

struct LinkTempoGen : public Unit
{
  // double mCurTempo;
  // double mTempoCalc;
};

extern "C"
{
  void LinkTempoGen_Ctor(LinkTempoGen *unit);
  void LinkTempoGen_next(LinkTempoGen *unit, int inNumSamples);
}



void LinkTempoGen_Ctor(LinkTempoGen *unit)
{

  if (!gLink)
  {
    Print("Link not enabled! can't create LinkTempoGen\n");
  }
  else
  {
    // const auto timeline = gLink->captureAudioSessionState();
    // unit->mCurTempo = timeline.tempo();
    // unit->mTempoCalc = unit->mCurTempo - *IN(0);
  }

  SETCALC(LinkTempoGen_next);
}

void LinkTempoGen_next(LinkTempoGen *unit, int inNumSamples)
{
  float *output = OUT(0);
  if (gLink)
  {
    auto timeline = gLink->captureAudioSessionState();
    *output = static_cast<float>(timeline.tempo());
  }
  else
  {
    *output = 120.0;
  }
}



// ========================================================================================================
//
// Application Interface
//
// ========================================================================================================

extern "C"
{
  double GetLinkBeat();
  double GetLinkBeatToTime(double beat);
  float GetLinkTempo();
  void SyncUnixTimeWithLink();
}

static long gDiff = 0;

void SyncUnixTimeWithLink()
{
  if (gLink)
  {
    unsigned long diff = 0;
    for (int i = 0; i < 10; i++)
    {
      const auto time = gLink->clock().micros();
      unsigned long since_epoch = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count() - 0;
      diff += since_epoch - time.count();
    }
    gDiff = diff / 10;
  }
}

double GetLinkBeat()
{
  double output = 0.0;
  if (gLink)
  {
    const auto time = gLink->clock().micros();
    auto timeline = gLink->captureAppSessionState();
    const auto beats = timeline.beatAtTime(time, 4);
    output = beats;
  }
  return output;
}

double GetLinkBeatToTime(double beat)
{
  double output = 0.0;
  if (gLink)
  {
    auto timeline = gLink->captureAppSessionState();
    auto time = timeline.timeAtBeat(beat, 4);
    output = (time.count() + gDiff) * 1e-6;
  }
  return output;
}

float GetLinkTempo()
{
  float output = 0.0;
  if (gLink)
  {
    auto timeline = gLink->captureAppSessionState();
    output = static_cast<float>(timeline.tempo());
  }
  return output;
}

// ========================================================================================================

PluginLoad(Link)
{
  ft = inTable;
  DefineSimpleUnit(LinkEnabler);
  DefineSimpleUnit(LinkDisabler);
  DefineSimpleUnit(Link);
  DefineSimpleUnit(LinkTempo);
  DefineSimpleUnit(LinkTempoGen);

}
