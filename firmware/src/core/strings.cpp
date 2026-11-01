#include "core/strings.h"

namespace monav {
namespace {

struct Entry {
  const char* de;
  const char* en;
};

// Order must match enum class Str.
constexpr Entry kStrings[] = {
    /* Waiting          */ {"Warte auf Monav", "Waiting for Monav"},
    /* ReadyToPair      */ {"Bereit zum Koppeln", "Ready to pair"},
    /* PairHint         */ {"Monav › Display einrichten", "Monav › Set up display"},
    /* Paired           */ {"Gekoppelt", "Paired"},
    /* PairedSub        */ {"Monav ist verbunden", "Monav is connected"},
    /* Denied1          */ {"Gehört zu einem", "Belongs to"},
    /* Denied2          */ {"anderen iPhone", "another iPhone"},
    /* DeniedHint       */ {"3 s halten zum Zurücksetzen", "Hold 3 s to reset"},
    /* ResetQuestion    */ {"Kopplung zurücksetzen?", "Reset pairing?"},
    /* ResetConfirm     */ {"Tippen zum Bestätigen", "Tap to confirm"},
    /* ResetDone        */ {"Kopplung gelöscht", "Pairing removed"},
    /* Connected        */ {"Verbunden", "Connected"},
    /* ConnectedSub     */ {"Bereit für die Fahrt", "Ready to drive"},
    /* LinkLost         */ {"Verbindung unterbrochen", "Connection lost"},
    /* Bye              */ {"Bis bald", "See you soon"},
    /* Kmh              */ {"km/h", "km/h"},
    /* LightRed         */ {"Rot", "Red"},
    /* LightGreen       */ {"Grün", "Green"},
    /* LightUncertain   */ {"Unsicher", "Uncertain"},
    /* LightUnknown     */ {"Ampel", "Light"},
    /* UntilGreen       */ {"bis Grün", "until green"},
    /* UntilRed         */ {"bis Rot", "until red"},
    /* KeepSpeed        */ {"Tempo halten", "Keep speed"},
    /* Slower           */ {"Langsamer", "Slow down"},
    /* Faster           */ {"Schneller", "Speed up"},
    /* Coast            */ {"Ausrollen", "Coast"},
    /* Section          */ {"Streckenradar", "Section check"},
    /* CamFixed         */ {"Fester Blitzer", "Fixed camera"},
    /* CamMobile        */ {"Mobiler Blitzer", "Mobile camera"},
    /* CamRedLight      */ {"Rotlichtblitzer", "Red-light camera"},
    /* CamSection       */ {"Abschnittskontrolle", "Section control"},
    /* CamDistance      */ {"Abstandskontrolle", "Distance check"},
    /* CamOther         */ {"Blitzer", "Speed camera"},
    /* Destination      */ {"Ziel", "Destination"},
    /* Roundabout       */ {"Kreisverkehr", "Roundabout"},
    /* ExitFmt          */ {"%d. Ausfahrt", "Exit %d"},
    /* ArrivalFmt       */ {"Ankunft %s", "Arrival %s"},
    /* Brightness       */ {"Helligkeit", "Brightness"},
    /* MapLoading       */ {"Karte lädt …", "Loading map …"},
    /* Attribution      */ {"© Apple Karten", "© Apple Maps"},
    /* UpdateTitle      */ {"Update auf", "Updating to"},
    /* DoNotPowerOff    */ {"Nicht ausschalten", "Do not switch off"},
    /* Verifying        */ {"Wird geprüft …", "Verifying …"},
    /* Restarting       */ {"Neustart …", "Restarting …"},
    /* UpdateFailed     */ {"Update fehlgeschlagen", "Update failed"},
    /* KeepsRunning     */ {"Die bisherige Version läuft weiter", "The current version keeps running"},
};

static_assert(sizeof(kStrings) / sizeof(kStrings[0]) == static_cast<size_t>(Str::Count),
              "string table out of sync with enum class Str");

}  // namespace

const char* tr(Lang lang, Str id) {
  const Entry& e = kStrings[static_cast<size_t>(id)];
  return lang == Lang::En ? e.en : e.de;
}

}  // namespace monav
