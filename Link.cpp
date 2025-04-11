
#include <ableton/Link.hpp>
#include <chrono>
#include <iostream>
#include "SC_Unit.h"
#include "SC_PlugIn.h"

static InterfaceTable *ft;

static ableton::Link *gLink = nullptr;
static std::chrono::microseconds gLatency = std::chrono::microseconds(0);
static float gTempo = 60.0;

// ========================================================================================================
//
// Link Interface for SuperCollider
//
// ========================================================================================================

// --- LinkEnabler ---
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
  Print("Link latency set to %lld us\n", gLatency.count()); // Use %lld for long long
  if (!gLink)
  {
    gTempo = *IN(0);
    gLink = new ableton::Link(gTempo);
    gLink->enable(true);
  }
  else
  {
    Print("Link already running\n");
  }
  SETCALC(LinkEnabler_next);
  // No output needed, make sure it's zeroed
  ZOUT0(0);
}
void LinkEnabler_next(LinkEnabler *unit, int inNumSamples)
{
  // This UGen doesn't produce output, it's just for setup.
}

// --- LinkDisabler ---
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
  }
  SETCALC(LinkDisabler_next);
  // No output needed, make sure it's zeroed
  ZOUT0(0);
}

void LinkDisabler_next(LinkDisabler *unit, int inNumSamples)
{
  // This UGen doesn't produce output, it's just for teardown.
}

// --- Link (Beat) ---
struct Link : public Unit
{
  float mLastBeat;
  double mSampleDur;                      // Store sample duration (1.0 / sampleRate)
  std::chrono::microseconds mStartMicros; // Store start time for the block
};

extern "C"
{
  void Link_Ctor(Link *unit);
  void Link_next_k(Link *unit, int inNumSamples); // Renamed for clarity
  void Link_next_a(Link *unit, int inNumSamples); // Audio rate version
}

void Link_Ctor(Link *unit)
{
  if (!gLink)
  {
    Print("warn: Link not enabled! Link UGen will output 0.\n");
    unit->mLastBeat = 0.0;
  }
  else
  {
    // Initialize with current beat
    const auto time = gLink->clock().micros() + gLatency;
    auto timeline = gLink->captureAudioSessionState();
    unit->mLastBeat = static_cast<float>(timeline.beatAtTime(time, 4));
  }

  unit->mSampleDur = 1.0 / unit->mWorld->mSampleRate; // Store 1/sr

  // Determine calculation function based on rate
  if (INRATE(0) == calc_FullRate)
  {
    SETCALC(Link_next_a);
    // Initialize start time for the first block
    unit->mStartMicros = gLink->clock().micros() + gLatency;
    Link_next_a(unit, 1); // Calculate initial value
  }
  else
  {
    SETCALC(Link_next_k);
    Link_next_k(unit, 1); // Calculate initial value
  }
}

void Link_next_k(Link *unit, int inNumSamples)
{
  float *output = OUT(0);
  if (gLink)
  {
    const auto time = gLink->clock().micros() + gLatency;
    auto timeline = gLink->captureAudioSessionState();
    const auto beats = timeline.beatAtTime(time, 4);
    *output = static_cast<float>(beats);
    unit->mLastBeat = *output;
  }
  else
  {
    *output = unit->mLastBeat; // Output last known beat or 0 if never enabled
  }
}

// Audio Rate Calculation
void Link_next_a(Link *unit, int inNumSamples)
{
  float *output = OUT(0);
  if (gLink)
  {
    auto timeline = gLink->captureAudioSessionState();
    // Use the correctly stored sample duration
    double sampleDurMicros = unit->mSampleDur * 1e6; // Sample duration in microseconds

    for (int i = 0; i < inNumSamples; ++i)
    {
      // Calculate time for the current sample
      const auto sampleTime = unit->mStartMicros + std::chrono::microseconds(static_cast<long long>(i * sampleDurMicros));
      const auto beats = timeline.beatAtTime(sampleTime, 4);
      output[i] = static_cast<float>(beats);
    }
    // Update start time for the next block
    unit->mStartMicros += std::chrono::microseconds(static_cast<long long>(inNumSamples * sampleDurMicros));
    unit->mLastBeat = output[inNumSamples - 1]; // Store last beat of the block
  }
  else
  {
    // Fill with last known beat or 0 if never enabled
    for (int i = 0; i < inNumSamples; ++i)
    {
      output[i] = unit->mLastBeat;
    }
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
