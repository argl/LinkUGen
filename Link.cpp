
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
// Custom HostTimeFilter with explicit Clock initialization for macOS
//
// ========================================================================================================

#ifdef LINK_PLATFORM_MACOSX
template<typename Clock>
class CustomHostTimeFilter
{
  using NumberType = std::uint64_t;
  using Points = std::vector<std::pair<NumberType, std::int64_t>>;
  using PointIt = typename Points::iterator;

  static const std::size_t kNumPoints = 512;

public:
  CustomHostTimeFilter()
    : mIndex(0)
    , mHostTimeSampler() // Explicit default construction
  {
    mPoints.reserve(kNumPoints);
    // Force Clock initialization
    mHostTimeSampler = Clock();
    // Verify initialization by calling micros() once
    auto testMicros = mHostTimeSampler.micros();
    Print("CustomHostTimeFilter: Clock initialized, test micros = %llu\n", testMicros.count());
  }

  ~CustomHostTimeFilter() = default;

  void reset()
  {
    mIndex = 0;
    mPoints.clear();
  }

  std::chrono::microseconds sampleTimeToHostTime(const NumberType sampleTime)
  {
    const auto micros = static_cast<std::int64_t>(mHostTimeSampler.micros().count());
    const auto point = std::make_pair(sampleTime, micros);

    if (mPoints.size() < kNumPoints)
    {
      mPoints.push_back(point);
    }
    else
    {
      mPoints[mIndex] = point;
    }
    mIndex = (mIndex + 1) % kNumPoints;

    const auto result = linearRegression(mPoints.begin(), mPoints.end());
    const auto hostTime = (result.first * sampleTime) + result.second;

    return std::chrono::microseconds(llround(hostTime));
  }

private:
  // Simple linear regression implementation
  std::pair<double, double> linearRegression(PointIt begin, PointIt end)
  {
    const auto numPoints = static_cast<double>(std::distance(begin, end));
    if (numPoints < 2)
    {
      return std::make_pair(1.0, 0.0); // Default slope=1, intercept=0
    }

    double sumX = 0.0, sumY = 0.0, sumXY = 0.0, sumXX = 0.0;

    for (auto it = begin; it != end; ++it)
    {
      const double x = static_cast<double>(it->first);
      const double y = static_cast<double>(it->second);
      sumX += x;
      sumY += y;
      sumXY += x * y;
      sumXX += x * x;
    }

    const double meanX = sumX / numPoints;
    const double meanY = sumY / numPoints;

    const double denominator = sumXX - numPoints * meanX * meanX;
    if (std::abs(denominator) < 1e-10)
    {
      return std::make_pair(1.0, meanY - meanX); // Fallback
    }

    const double slope = (sumXY - numPoints * meanX * meanY) / denominator;
    const double intercept = meanY - slope * meanX;

    return std::make_pair(slope, intercept);
  }

  std::size_t mIndex;
  Points mPoints;
  Clock mHostTimeSampler;
};
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
  ableton::link::HostTimeFilter<ableton::link::platform::Clock> mHostTimeFilter;
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
#ifdef USE_HOST_TIME_FILTER
    // use the sample time from supercollider and convert to host time
    uint64 sampleTime = (unit->mWorld->mBufCounter * unit->mWorld->mBufLength) + unit->mWorld->mSampleOffset;
    const auto hostTime = unit->mHostTimeFilter.sampleTimeToHostTime(sampleTime);
    auto timeline = gLink->captureAudioSessionState();
    const auto beats = timeline.beatAtTime(hostTime, 4);
    *output = static_cast<float>(beats);
    unit->mLastBeat = *output;
#else
    // Calculate sample-accurate host time using current clock time as reference
    // auto currentClockTime = gLink->clock().micros();

    // Convert sample offset to time offset in microseconds
    const double sampleRate = unit->mWorld->mSampleRate;
    const auto sampleOffset = static_cast<double>((unit->mWorld->mBufCounter * unit->mWorld->mBufLength) + unit->mWorld->mSampleOffset);
    const auto timeOffsetMicros = static_cast<long long>(std::round((sampleOffset / sampleRate) * 1000000.0));

    // world->mBufCounter = 0;
    // world->mBufLength = inOptions->mBufLength;
    // world->mSampleOffset = 0;

    // Use clock time with sample-accurate offset
    const auto time = std::chrono::microseconds(timeOffsetMicros);

    auto timeline = gLink->captureAudioSessionState();
    const auto beats = timeline.beatAtTime(time, 4);
    *output = static_cast<float>(beats);
    unit->mLastBeat = *output;
#endif
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

  // Trigger states
  bool mGridTrigger;
  bool mBeatTrigger;
  bool mSignalTrigger;
  bool mDoneTrigger;

  // Envelope states
  double mEnabledEnv;
  double mSignalEnv;

  // Timing
  double mSignalStartBeat;

  // Link beat tracking (like basic Link ugen)
  double mLastLinkBeat;

  #ifdef LINK_PLATFORM_MACOSX
    CustomHostTimeFilter<ableton::link::platform::Clock> mHostTimeFilter;
  #else
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

  // Initialize beat tracking
  unit->mBeatStartBeat = 0.0;   // track in Link beats
  unit->mBeatCounter = 0;

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

  // Initialize Clock explicitly - construct in place
  // unit->mClock = ableton::link::platform::Clock();
#ifdef LINK_PLATFORM_MACOSX
  unit->mHostTimeFilter = CustomHostTimeFilter<ableton::link::platform::Clock>();
#endif

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
  float *signalEnvOut = OUT(4);
  float *doneOut = OUT(5);
  float *stateOut = OUT(6);
  float *sigEnvLengthOut = OUT(7);  // Signal envelope length using tempo formula
  float *linkBeatOut = OUT(8);      // Current Link beat

  // Inputs
  bool enabled = *IN(0) > 0.5f;

  // Reset triggers at start of each sample
  unit->mGridTrigger = false;
  unit->mBeatTrigger = false;
  unit->mSignalTrigger = false;
  unit->mDoneTrigger = false;

  if (gLink)
  {

      // do not use host time filter on mac, but get the time by counting samples
// #ifdef LINK_PLATFORM_MACOSX
//     const double sampleRate = unit->mWorld->mSampleRate;
//     const auto sampleOffset = static_cast<double>((unit->mWorld->mBufCounter * unit->mWorld->mBufLength) + unit->mWorld->mSampleOffset);
//     const auto timeOffsetMicros = static_cast<long long>((sampleOffset / sampleRate) * 1000000.0);
//     const auto time = std::chrono::microseconds(timeOffsetMicros);
//     auto timeline = gLink->captureAudioSessionState();
//     const double currentBeat = timeline.beatAtTime(time, 4);
//     const double currentTempo = timeline.tempo();
//     static int debugCounter = 0;
//     if (++debugCounter >= 500) {
//       Print("Debug: clock=%llu, time=%llu\n", clock, time.count());
//       debugCounter = 0;
//     }
// #else
    uint64 sampleTime = (unit->mWorld->mBufCounter * unit->mWorld->mBufLength) + unit->mWorld->mSampleOffset;
    const auto time = unit->mHostTimeFilter.sampleTimeToHostTime(sampleTime) + gLatency;
    auto timeline = gLink->captureAudioSessionState();
    const double currentBeat = timeline.beatAtTime(time, 4);
    const double currentTempo = timeline.tempo();
    static int debugCounter = 0;
    if (++debugCounter >= 500) {
        Print("Debug: sampleTime=%llu, time=%llu\n", sampleTime, time.count());
        debugCounter = 0;
    }
// #endif
    // Update Link beat output (replicate basic Link ugen functionality)
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

          // Generate independent beat triggers using Link beats
          double beatsElapsed = currentBeat - unit->mBeatStartBeat;
          if (beatsElapsed < 0) {
            // Handle beat wraparound
            beatsElapsed = 0;
          }

          // Calculate how many beat intervals have passed
          int expectedBeatCount = static_cast<int>(beatsElapsed / unit->mBeatInterval);

          if (expectedBeatCount > unit->mBeatCounter) {
            unit->mBeatTrigger = true;
            unit->mBeatCounter = expectedBeatCount;
          }
        }
        break;

      case STOPPING:
        {
          // Continue generating triggers until we stop
          if (gridBoundaryHit) {
            unit->mGridTrigger = true;
          }

          // Continue beat triggers
          double beatsElapsed = currentBeat - unit->mBeatStartBeat;
          if (beatsElapsed < 0) {
            beatsElapsed = 0;
          }

          int expectedBeatCount = static_cast<int>(beatsElapsed / unit->mBeatInterval);

          if (expectedBeatCount > unit->mBeatCounter) {
            unit->mBeatTrigger = true;
            unit->mBeatCounter = expectedBeatCount;
          }
        }
        break;
    }


    // Update enabled envelope based on state
    switch (unit->mState) {
      case IDLE:
        unit->mEnabledEnv = 0.0;
        break;
      case WAITING_TO_START:
        unit->mEnabledEnv = 0.0;
        break;
      case RUNNING:
      case STOPPING:
        unit->mEnabledEnv = 1.0;
        break;
    }

    // Calculate signal envelope length using tempo formula: (1/tempo) * 60 * beats
    double sigEnvLength = 0.0;
    if (unit->mState == RUNNING || unit->mState == STOPPING) {
      double beatsElapsed = currentBeat - unit->mSignalStartBeat;
      if (beatsElapsed < 0) {
        // Handle beat wraparound (though this should be rare)
        beatsElapsed = 0;
      }
      // Formula: (1/tempo) * 60 * beats = seconds
      // Uses Link tempo automatically
      sigEnvLength = (1.0 / currentTempo) * 60.0 * beatsElapsed;
    }

    // Update state
    unit->mLastBeat = currentBeat;
    unit->mLastEnabled = enabled;

    // Output values
    *gridTrigOut = unit->mGridTrigger ? 1.0f : 0.0f;
    *beatTrigOut = unit->mBeatTrigger ? 1.0f : 0.0f;
    *enabledEnvOut = static_cast<float>(unit->mEnabledEnv);
    *signalTrigOut = unit->mSignalTrigger ? 1.0f : 0.0f;
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
