#pragma once

// Every text the display draws itself, German and English, in one table. `hello.lang` picks the
// language; camera labels and road names come ready-made from the phone.

#include "core/model.h"

namespace monav {

enum class Str : uint8_t {
  Waiting,
  ReadyToPair,
  PairHint,
  Paired,
  PairedSub,
  Denied1,
  Denied2,
  DeniedHint,
  ResetQuestion,
  ResetConfirm,
  ResetDone,
  Connected,
  ConnectedSub,
  LinkLost,
  Bye,
  Kmh,
  LightRed,
  LightGreen,
  LightUncertain,
  LightUnknown,
  UntilGreen,
  UntilRed,
  KeepSpeed,
  Slower,
  Faster,
  Coast,
  Section,
  CamFixed,
  CamMobile,
  CamRedLight,
  CamSection,
  CamDistance,
  CamOther,
  Destination,
  Roundabout,
  ExitFmt,  // "%d" = exit number
  ArrivalFmt,  // "%s" = time
  Brightness,
  MapLoading,
  Attribution,
  UpdateTitle,
  DoNotPowerOff,
  Verifying,
  Restarting,
  UpdateFailed,
  KeepsRunning,
  Count,
};

const char* tr(Lang lang, Str id);

}  // namespace monav
