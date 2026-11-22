// Thermal States - a small HUD mod for CaramelClient / GlowberryClient.
//
// Shows the device's thermal state (Nominal / Fair / Serious / Critical / Shutdown) and, where the
// OS reports it, how much thermal headroom is left. The reading comes from Android's own NDK
// thermal API (AThermal_*), resolved at runtime with dlsym, so the mod loads on every supported
// device and says "unavailable" instead of guessing when the API is missing (Android 10 and older).
//
// The five levels mirror the names the launcher's GovernorAbi uses for BerryAPI's ThermalState, so a
// later version can read BerryAPI's governor directly (see README.md).

#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>

namespace {

constexpr const char *ModuleId = "thermal_states.hud";

// --- Thermal reading ------------------------------------------------------------------------

// Android's ATHERMAL_STATUS_* values (android/thermal.h, API 30).
constexpr int kStatusError = -1;
constexpr int kStatusNone = 0;
constexpr int kStatusLight = 1;
constexpr int kStatusModerate = 2;
constexpr int kStatusSevere = 3;
constexpr int kStatusCritical = 4;
constexpr int kStatusEmergency = 5;
constexpr int kStatusShutdown = 6;

enum class Level { Unknown, Nominal, Fair, Serious, Critical, Shutdown };

// Android has seven statuses; fold them onto the five-level scale BerryAPI's governor uses.
Level toLevel(int status) {
  switch (status) {
  case kStatusNone:
    return Level::Nominal;
  case kStatusLight:
  case kStatusModerate:
    return Level::Fair;
  case kStatusSevere:
    return Level::Serious;
  case kStatusCritical:
  case kStatusEmergency:
    return Level::Critical;
  case kStatusShutdown:
    return Level::Shutdown;
  default:
    return Level::Unknown;
  }
}

const char *levelName(Level level) {
  switch (level) {
  case Level::Nominal:
    return "Nominal";
  case Level::Fair:
    return "Fair";
  case Level::Serious:
    return "Serious";
  case Level::Critical:
    return "Critical";
  case Level::Shutdown:
    return "Shutdown";
  default:
    return "Unknown";
  }
}

// ARGB. Green -> yellow -> orange -> red as pressure rises; grey when there is no reading.
int levelColor(Level level) {
  switch (level) {
  case Level::Nominal:
    return static_cast<int>(0xFF4CAF50);
  case Level::Fair:
    return static_cast<int>(0xFFFFC107);
  case Level::Serious:
    return static_cast<int>(0xFFFF9800);
  case Level::Critical:
  case Level::Shutdown:
    return static_cast<int>(0xFFF44336);
  default:
    return static_cast<int>(0xFF9E9E9E);
  }
}

struct ThermalManager;
using AcquireFn = ThermalManager *(*)();
using ReleaseFn = void (*)(ThermalManager *);
using StatusFn = int (*)(ThermalManager *);
using HeadroomFn = float (*)(ThermalManager *, int);

class ThermalReader {
public:
  ThermalReader() {
    mLib = dlopen("libandroid.so", RTLD_NOW);
    if (!mLib) return;
    auto acquire = reinterpret_cast<AcquireFn>(dlsym(mLib, "AThermal_acquireManager"));
    mRelease = reinterpret_cast<ReleaseFn>(dlsym(mLib, "AThermal_releaseManager"));
    mStatus = reinterpret_cast<StatusFn>(dlsym(mLib, "AThermal_getCurrentThermalStatus"));
    mHeadroom = reinterpret_cast<HeadroomFn>(dlsym(mLib, "AThermal_getThermalHeadroom"));
    if (acquire && mStatus) mManager = acquire();
  }

  ~ThermalReader() {
    if (mManager && mRelease) mRelease(mManager);
    if (mLib) dlclose(mLib);
  }

  ThermalReader(const ThermalReader &) = delete;
  ThermalReader &operator=(const ThermalReader &) = delete;

  bool available() const { return mManager != nullptr; }

  Level level() const {
    if (!mManager) return Level::Unknown;
    int status = mStatus(mManager);
    return status == kStatusError ? Level::Unknown : toLevel(status);
  }

  // 0 = cool, 1 = the throttling threshold, above 1 = already throttled. Returns a negative value
  // when the OS has no forecast (API 31+ only, and not every device supplies one).
  float headroom() const {
    if (!mManager || !mHeadroom) return -1.0f;
    float value = mHeadroom(mManager, 0);
    return std::isnan(value) ? -1.0f : value;
  }

private:
  void *mLib = nullptr;
  ThermalManager *mManager = nullptr;
  ReleaseFn mRelease = nullptr;
  StatusFn mStatus = nullptr;
  HeadroomFn mHeadroom = nullptr;
};

// --- HUD ----------------------------------------------------------------------------------

std::atomic<bool> gShown{true};

void onToggle(std::string_view moduleId, bool enabled) {
  (void)moduleId;
  gShown = enabled;
  if (!enabled) {
    // An empty list clears the overlay.
    pl::modmenu::submitDrawCommands(ModuleId, std::vector<pl::modmenu::DrawCommand>{});
  }
}

void submitOverlay(const ThermalReader &reader) {
  using pl::modmenu::DrawCommand;
  using pl::modmenu::DrawCommandType;

  constexpr float X = 16.0f;
  constexpr float Y = 16.0f;
  constexpr float W = 168.0f;

  std::vector<DrawCommand> commands;

  const Level level = reader.level();
  const int color = levelColor(level);

  commands.push_back({.type = DrawCommandType::RectFilled,
                      .x = X,
                      .y = Y,
                      .w = W,
                      .h = 58.0f,
                      .color = static_cast<int>(0x99000000)});

  commands.push_back({.type = DrawCommandType::Text,
                      .x = X + 8.0f,
                      .y = Y + 6.0f,
                      .color = static_cast<int>(0xFFFFFFFF),
                      .size = 12.0f,
                      .text = "Thermal"});

  if (!reader.available()) {
    commands.push_back({.type = DrawCommandType::Text,
                        .x = X + 8.0f,
                        .y = Y + 26.0f,
                        .color = color,
                        .size = 12.0f,
                        .text = "Unavailable (needs Android 11+)"});
    pl::modmenu::submitDrawCommands(ModuleId, commands);
    return;
  }

  commands.push_back({.type = DrawCommandType::Text,
                      .x = X + 8.0f,
                      .y = Y + 24.0f,
                      .color = color,
                      .size = 16.0f,
                      .text = levelName(level)});

  // Headroom bar: the track, then the fill clamped to the track width.
  const float headroom = reader.headroom();
  if (headroom >= 0.0f) {
    const float barW = W - 16.0f;
    const float fill = barW * std::fmin(headroom, 1.0f);
    commands.push_back({.type = DrawCommandType::RectFilled,
                        .x = X + 8.0f,
                        .y = Y + 46.0f,
                        .w = barW,
                        .h = 5.0f,
                        .color = static_cast<int>(0x55FFFFFF)});
    commands.push_back({.type = DrawCommandType::RectFilled,
                        .x = X + 8.0f,
                        .y = Y + 46.0f,
                        .w = fill,
                        .h = 5.0f,
                        .color = color});
  }

  pl::modmenu::submitDrawCommands(ModuleId, commands);
}

} // namespace

// --- Mod lifecycle ------------------------------------------------------------------------

class ThermalStatesMod {
public:
  static ThermalStatesMod &instance() {
    static ThermalStatesMod mod;
    return mod;
  }

  ThermalStatesMod() : mSelf(*ll::mod::NativeMod::current()) {}

  [[nodiscard]] ll::mod::NativeMod &getSelf() const { return mSelf; }

  bool load() {
    mReader = std::make_unique<ThermalReader>();
    getSelf().getLogger().info("Thermal API {}",
                               mReader->available() ? "available" : "not available");
    return true;
  }

  bool enable() {
    const bool ok = pl::modmenu::ModuleBuilder(ModuleId, "Thermal States")
                        .modId(getSelf().getId())
                        .description("Shows the device thermal state and remaining headroom.")
                        .defaultEnabled(true)
                        .onToggle(onToggle)
                        .registerModule();
    if (!ok) return false;

    mRunning = true;
    mWorker = std::thread([this] {
      while (mRunning) {
        if (gShown) submitOverlay(*mReader);
        // Thermal state changes over seconds, so 1 Hz is plenty.
        for (int i = 0; i < 10 && mRunning; ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
    });
    return true;
  }

  bool disable() {
    mRunning = false;
    if (mWorker.joinable()) mWorker.join();
    pl::modmenu::unregisterModule(ModuleId);
    return true;
  }

  bool unload() {
    mReader.reset();
    return true;
  }

private:
  ll::mod::NativeMod &mSelf;
  std::unique_ptr<ThermalReader> mReader;
  std::atomic<bool> mRunning{false};
  std::thread mWorker;
};

PL_REGISTER_MOD(ThermalStatesMod, ThermalStatesMod::instance())
