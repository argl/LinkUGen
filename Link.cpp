
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


// ========================================================================================================
//
// Host time source
//
// ========================================================================================================
//
// SuperCollider allocates Unit structs as raw memory and never runs C++ constructors on
// them — the `*_Ctor` functions are plain init callbacks, not real constructors. So any
// non-trivial member of a Unit (like HostTimeFilter, which owns a std::vector) starts life
// as uninitialised memory. On Linux that memory happens to come back zeroed, which is a
// valid empty vector, and the filter's linear regression works. On macOS it is garbage:
// `sampleTimeToHostTime` returns a nonsense host time, so `Link.kr` reports a constant,
// wildly negative beat.
//
// That constant beat is why the whole Link layer looked dead on macOS: LinkTrig is
// `Changed.kr(LinkCount...)`, and a beat that never changes produces *no triggers at all* —
// the sequencer's `/step` replies never fire, and LinkGrid never advances.
//
// On macOS, therefore, skip the filter entirely and take the host time straight from Link's
// own clock. The cost is the sub-block sample offset: timing accuracy drops from
// sample-accurate to block-rate (~1.3 ms at 64 samples), which is irrelevant for the kr-rate
// beat/count/trigger UGens built on top of this.
#if defined(LINK_PLATFORM_MACOSX)
  #define LINKUGEN_NO_HOST_TIME_FILTER 1
#endif


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
#ifndef LINKUGEN_NO_HOST_TIME_FILTER
  ableton::link::HostTimeFilter<ableton::link::platform::Clock> mHostTimeFilter;
#endif
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
  // Print("======= Hello from LinkUgen!\n");
  unit->mLastBeat = 0.0;
  SETCALC(Link_next);
}

void Link_next(Link *unit, int inNumSamples)
{
  float *output = OUT(0);
  // static int sLastBufCounter = -1;
  // static double sLastBeat = 0.0;

  if (gLink)
  {
#ifdef LINKUGEN_NO_HOST_TIME_FILTER
    const auto time = gLink->clock().micros() + gLatency;
#else
    uint64 sampleTime = (unit->mWorld->mBufCounter * unit->mWorld->mBufLength) + unit->mWorld->mSampleOffset;
    const auto time = unit->mHostTimeFilter.sampleTimeToHostTime(sampleTime) + gLatency;
#endif
    auto timeline = gLink->captureAudioSessionState();
    const double currentBeat = timeline.beatAtTime(time, 4);
    *output = static_cast<float>(currentBeat);
    unit->mLastBeat = *output;


    // #ifdef USE_HOST_TIME_FILTER
    //     // use the sample time from supercollider and convert to host time
    //     uint64 sampleTime = (unit->mWorld->mBufCounter * unit->mWorld->mBufLength) + unit->mWorld->mSampleOffset;
    //     const auto hostTime = unit->mHostTimeFilter.sampleTimeToHostTime(sampleTime);
    //     auto timeline = gLink->captureAudioSessionState();
    //     const auto beats = timeline.beatAtTime(hostTime, 4);
    //     *output = static_cast<float>(beats);
    //     unit->mLastBeat = *output;
    // #else
    //     // Calculate sample-accurate host time using current clock time as reference
    //     // auto currentClockTime = gLink->clock().micros();

    //     // Convert sample offset to time offset in microseconds
    //     const double sampleRate = unit->mWorld->mSampleRate;
    //     const auto sampleOffset = static_cast<double>((unit->mWorld->mBufCounter * unit->mWorld->mBufLength) + unit->mWorld->mSampleOffset);
    //     const auto timeOffsetMicros = static_cast<long long>(std::round((sampleOffset / sampleRate) * 1000000.0));

    //     // world->mBufCounter = 0;
    //     // world->mBufLength = inOptions->mBufLength;
    //     // world->mSampleOffset = 0;

    //     // Use clock time with sample-accurate offset
    //     const auto time = std::chrono::microseconds(timeOffsetMicros);

    //     auto timeline = gLink->captureAudioSessionState();
    //     const auto beats = timeline.beatAtTime(time, 4);
    //     *output = static_cast<float>(beats);
    //     unit->mLastBeat = *output;
    // #endif
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
// LinkGrid - State machine based grid/beat locked operation with Link tempo
//
// ========================================================================================================

enum LinkGridState {
  IDLE = 0,           // Not enabled, not running
  WAITING_TO_START,   // Enabled, waiting for grid trigger to start
  RUNNING,           // Currently running and producing output
  STOPPING          // Disabled, waiting for grid trigger to stop cleanly
};

struct LinkGrid : public Unit
{
  // State machine
  LinkGridState mState;
  LinkGridState mLastState;

  // Input tracking
  bool mLastEnabled;

  // Grid tracking
  double mLastBeat;
  double mGridSize;
  double mBeats;      // beats in beat units
  int mGridCounter;

  // Independent beat tracking
  double mBeatStartBeat;  // track in Link beats
  int mBeatCounter;
  double mBeatInterval;   // Beat interval in Link beats
  double mLastBeatPosition; // For boundary detection

  // Trigger states
  bool mGridTrigger;
  bool mBeatTrigger;
  bool mSignalTrigger;
  bool mDoneTrigger;

  // Envelope states
  double mEnabledEnv;
  double mSignalEnv;

  double mPhase;

  // Timing
  double mSignalStartBeat;

  // Link beat tracking (like basic Link ugen)
  double mLastLinkBeat;

#ifndef LINKUGEN_NO_HOST_TIME_FILTER
  ableton::link::HostTimeFilter<ableton::link::platform::Clock> mHostTimeFilter;
#endif
};

extern "C"
{
  void LinkGrid_Ctor(LinkGrid *unit);
  void LinkGrid_next(LinkGrid *unit, int inNumSamples);
}

void LinkGrid_Ctor(LinkGrid *unit)
{
  if (!gLink)
  {
    Print("warn: Link not enabled for LinkGrid!\n");
  }

  // Initialize state machine
  unit->mState = IDLE;
  unit->mLastState = IDLE;

  // Initialize input tracking
  unit->mLastEnabled = false;

  // Initialize parameters from inputs
  unit->mGridSize = *IN(1);     // Grid size in beats
  unit->mBeats = *IN(2);        // beats in beat units

  // Calculate beat interval in Link beats
  unit->mBeatInterval = unit->mBeats;

  // Initialize tracking
  unit->mLastBeat = 0.0;
  unit->mGridCounter = 0;

  unit->mPhase = 0.0;

  // Initialize beat tracking
  unit->mBeatStartBeat = 0.0;   // track in Link beats
  unit->mBeatCounter = 0;
  unit->mLastBeatPosition = 0.0;

  // Initialize triggers
  unit->mGridTrigger = false;
  unit->mBeatTrigger = false;
  unit->mSignalTrigger = false;
  unit->mDoneTrigger = false;

  // Initialize envelopes
  unit->mEnabledEnv = 0.0;
  unit->mSignalEnv = 0.0;

  // Initialize timing
  unit->mSignalStartBeat = 0.0;

  // Initialize Link beat tracking
  unit->mLastLinkBeat = 0.0;

  Print("LinkGrid setup: %.3f %.3f\n", unit->mGridSize, unit->mBeats);

  SETCALC(LinkGrid_next);
}

void LinkGrid_next(LinkGrid *unit, int inNumSamples)
{
  // Outputs
  float *gridTrigOut = OUT(0);
  float *beatTrigOut = OUT(1);
  float *enabledEnvOut = OUT(2);
  float *signalTrigOut = OUT(3);
  float *phaseOut = OUT(4);
  float *signalEnvOut = OUT(5);
  float *doneOut = OUT(6);
  float *stateOut = OUT(7);
  float *sigEnvLengthOut = OUT(8);
  float *linkBeatOut = OUT(9);

  // Inputs
  bool enabled = *IN(0) > 0.5f;
  unit->mGridSize = *IN(1);     // Grid size in beats
  // unit->mBeats = *IN(2);        // beats in beat units
  // unit->mBeatInterval = unit->mBeats;




  // Reset triggers at start of each sample
  unit->mGridTrigger = false;
  unit->mBeatTrigger = false;
  unit->mSignalTrigger = false;
  unit->mDoneTrigger = false;

  if (gLink)
  {
#ifdef LINKUGEN_NO_HOST_TIME_FILTER
    const auto time = gLink->clock().micros() + gLatency;
#else
    uint64 sampleTime = (unit->mWorld->mBufCounter * unit->mWorld->mBufLength) + unit->mWorld->mSampleOffset;
    const auto time = unit->mHostTimeFilter.sampleTimeToHostTime(sampleTime) + gLatency;
#endif
    auto timeline = gLink->captureAudioSessionState();
    const double currentBeat = timeline.beatAtTime(time, 4);
    const double currentTempo = timeline.tempo();

    double newBeatInterval = *IN(2); // beats parameter
    bool intervalChanged = (fabs(newBeatInterval - unit->mBeats) > 1e-6);
    if (intervalChanged) {
      unit->mBeats = newBeatInterval;
      unit->mBeatInterval = unit->mBeats;

      // Reset beat position tracking for new interval
      if (unit->mState == RUNNING || unit->mState == STOPPING) {
          unit->mLastBeatPosition = 0.0;
          Print("Beat interval changed to %.2f, reset position tracking\n", unit->mBeatInterval);
      }
    }


    unit->mLastLinkBeat = currentBeat;

    // Detect grid trigger
    bool gridBoundaryHit = false;
    double gridPosition = fmod(currentBeat, unit->mGridSize);
    double lastGridPosition = fmod(unit->mLastBeat, unit->mGridSize);

    if (gridPosition < lastGridPosition ||
        (lastGridPosition < 0.01 && gridPosition > unit->mGridSize - 0.01))
    {
      gridBoundaryHit = true;
      unit->mGridCounter++;
    }

    // Store previous state for transition detection
    unit->mLastState = unit->mState;

    // STATE MACHINE LOGIC
    switch (unit->mState)
    {
      case IDLE:
        if (enabled && !unit->mLastEnabled) {
          // Enable signal received - transition to waiting
          unit->mState = WAITING_TO_START;
          Print("LinkGrid: IDLE -> WAITING_TO_START\n");
        }
        break;

      case WAITING_TO_START:
        if (!enabled) {
          // Disabled before we started - go back to idle
          unit->mState = IDLE;
          Print("LinkGrid: WAITING_TO_START -> IDLE (cancelled)\n");
        }
        else if (gridBoundaryHit) {
          // Grid trigger while waiting - start running
          unit->mState = RUNNING;
          unit->mBeatStartBeat = currentBeat;  // track in Link beats
          unit->mBeatCounter = 0;
          unit->mLastBeatPosition = 0.0;
          unit->mSignalStartBeat = currentBeat;  // Record signal start beat
          unit->mSignalTrigger = true;
          unit->mBeatTrigger = true;
          Print("LinkGrid: WAITING_TO_START -> RUNNING at beat %f\n", currentBeat);
        }
        break;

      case RUNNING:
        if (!enabled) {
          // Disable signal received - transition to stopping
          unit->mState = STOPPING;
          Print("LinkGrid: RUNNING -> STOPPING\n");
        }
        break;

      case STOPPING:
        if (enabled) {
          // Re-enabled while stopping - go back to running
          unit->mState = RUNNING;
          Print("LinkGrid: STOPPING -> RUNNING (re-enabled)\n");
        }
        else if (gridBoundaryHit) {
          // Grid trigger while stopping - stop cleanly
          unit->mState = IDLE;
          unit->mSignalEnv = 0.0; // Stop signal envelope on grid boundary
          unit->mDoneTrigger = true;
          Print("LinkGrid: STOPPING -> IDLE (done) at beat %f\n", currentBeat);
        }
        break;
    }

    // STATE-BASED OUTPUTS
    switch (unit->mState)
    {
      case IDLE:
        unit->mSignalEnv = 0.0;
        break;

      case WAITING_TO_START:
        unit->mSignalEnv = 0.0;
        break;

      case RUNNING:
        {
          unit->mSignalEnv = 1.0;

          // Generate grid triggers
          if (gridBoundaryHit) {
            unit->mGridTrigger = true;
          }

          // Generate beat triggers using boundary detection relative to start beat
          double beatPosition = fmod(currentBeat - unit->mBeatStartBeat, unit->mBeatInterval);

          // Detect beat boundary crossing
          if (beatPosition < unit->mLastBeatPosition ||
              (unit->mLastBeatPosition < 0.01 && beatPosition > unit->mBeatInterval - 0.01))
          {
            unit->mBeatTrigger = true;
            unit->mBeatCounter++;
          }

          unit->mLastBeatPosition = beatPosition;
        }
        break;

      case STOPPING:
        {
          // Continue generating triggers until we stop
          if (gridBoundaryHit) {
            unit->mGridTrigger = true;
          }

          // Generate beat triggers using boundary detection relative to start beat
          double beatPosition = fmod(currentBeat - unit->mBeatStartBeat, unit->mBeatInterval);

          // Detect beat boundary crossing
          if (beatPosition < unit->mLastBeatPosition ||
              (unit->mLastBeatPosition < 0.01 && beatPosition > unit->mBeatInterval - 0.01))
          {
            unit->mBeatTrigger = true;
            unit->mBeatCounter++;
          }

          unit->mLastBeatPosition = beatPosition;
        }
        break;
    }


    // Update enabled envelope based on state
    switch (unit->mState) {
      case IDLE:
        unit->mEnabledEnv = 0.0;
        break;
      case WAITING_TO_START:
      case RUNNING:
      case STOPPING:
        unit->mEnabledEnv = 1.0;
        break;
    }

    // sigEnvLength: duration of one beat block in seconds at the current tempo.
    // Used by consumer synthdefs as the timeScale of per-block EnvGens triggered
    // on btrig, so it must be the block size, not elapsed time. Sampling elapsed
    // time here would yield 0 at the first btrig (signal-start grid boundary),
    // collapsing the first block's envelope and producing a glitched first loop.
    double sigEnvLength = (60.0 / currentTempo) * unit->mBeatInterval;
    if (unit->mState == RUNNING || unit->mState == STOPPING) {
      double beatsElapsed = currentBeat - unit->mSignalStartBeat;
      if (beatsElapsed < 0) {
        // Handle beat wraparound (though this should be rare)
        beatsElapsed = 0;
      }
      unit->mPhase = fmod(beatsElapsed / unit->mBeatInterval, 1.0);
    } else {
      unit->mPhase = 0;
    }

    // Update state
    unit->mLastBeat = currentBeat;
    unit->mLastEnabled = enabled;

    // Output values
    *gridTrigOut = unit->mGridTrigger ? 1.0f : 0.0f;
    *beatTrigOut = unit->mBeatTrigger ? 1.0f : 0.0f;
    *enabledEnvOut = static_cast<float>(unit->mEnabledEnv);
    *signalTrigOut = unit->mSignalTrigger ? 1.0f : 0.0f;
    *phaseOut = static_cast<float>(unit->mPhase);
    *signalEnvOut = static_cast<float>(unit->mSignalEnv);
    *doneOut = unit->mDoneTrigger ? 1.0f : 0.0f;
    *stateOut = static_cast<float>(unit->mState);
    *sigEnvLengthOut = static_cast<float>(sigEnvLength);
    *linkBeatOut = static_cast<float>(unit->mLastLinkBeat);
  }
  else
  {
    // Link not available - output zeros
    *gridTrigOut = 0.0f;
    *beatTrigOut = 0.0f;
    *enabledEnvOut = 0.0f;
    *signalTrigOut = 0.0f;
    *phaseOut = 0.0f;
    *signalEnvOut = 0.0f;
    *doneOut = 0.0f;
    *stateOut = 0.0f;
    *sigEnvLengthOut = 0.0f;
    *linkBeatOut = 0.0f;
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
  DefineSimpleUnit(LinkGrid);
}
