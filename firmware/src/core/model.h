#pragma once

// What the phone tells the display (PROTOCOL.md), as plain structs. No Arduino, no BLE: this
// header is shared by the firmware and the host preview build.

#include <cstddef>
#include <cstdint>

namespace monav {

enum class Lang : uint8_t { De, En };

enum class LightState : uint8_t { Red, Green, Uncertain, Unknown };
/** The lane a traffic light is for (`light.turn`); All when left out. */
enum class Lane : uint8_t { All, Left, Straight, Right };
enum class CamKind : uint8_t { Fixed, Mobile, RedLight, Section, Distance, Other };
enum class TurnKind : uint8_t {
  Straight,
  SlightLeft,
  Left,
  SharpLeft,
  SlightRight,
  Right,
  SharpRight,
  UTurn,
  KeepLeft,
  KeepRight,
  Roundabout,
  Arrive,
  Other,
};

struct Light {
  bool present = false;
  LightState st = LightState::Unknown;
  int64_t from = 0;  // phone ms; 0 = unknown
  int64_t to = 0;    // phone ms; 0 = not within the forecast
  float d = 0;       // metres at Scene::at
  int adv = -1;      // km/h that catches green; 0 = stop; -1 = current speed is right
  Lane lane = Lane::All;
};

struct Camera {
  bool present = false;
  CamKind kind = CamKind::Other;
  float d = 0;
  int vmax = 0;  // 0 = unknown
  char label[48] = {0};
};

struct Section {
  bool present = false;
  int vmax = 0;
  float avg = 0;
};

struct Turn {
  bool present = false;
  TurnKind kind = TurnKind::Other;
  float d = 0;
  char road[72] = {0};
  int exit = 0;     // roundabout exit, 0 = not given
  int64_t eta = 0;  // phone ms; 0 = unknown
};

struct Scene {
  int64_t at = 0;
  Light light;
  Camera cam;
  Section sec;
  Turn turn;
};

/** A charging station near the car with live availability (`chg`, from a charger plugin). */
struct Charger {
  double lat = 0;
  double lon = 0;
  int16_t free = 0;    // free charge points
  int16_t total = -1;  // all charge points; −1 when the phone leaves it out (free places only)
};

struct Chargers {
  static constexpr int kMax = 16;
  Charger st[kMax];
  int count = 0;
};

enum class MsgType : uint8_t { Unknown, Hello, Vehicle, Scene, Config, Bye, Ota, Map, Route, Dev, Chargers };

/** What a tap on the glass picked (reported as `layout` in welcome and status). */
enum class Layout : uint8_t { Hud, Focus, Map };

/** `op` of the ota / map / route messages. */
enum class TransferOp : uint8_t { None, Begin, End, Abort, Clear };

/** One parsed phone → display message. Only the fields of its type are meaningful. */
struct Message {
  MsgType type = MsgType::Unknown;

  // hello
  int proto = 0;
  char token[68] = {0};
  bool hasLang = false;
  Lang lang = Lang::De;

  // hello, v: phone clock
  bool hasNow = false;
  int64_t now = 0;

  // hello, cfg: optional UTC offset of the phone in minutes (extension, see PROTOCOL.md)
  bool hasTz = false;
  int tzMin = 0;

  // v
  float kmh = 0;
  int lim = 0;  // 0 = no limit known
  bool hasPos = false;  // v (0.2.0): the car's place on the matched road
  double lat = 0;       // v, map, route
  double lon = 0;
  bool hasHdg = false;
  float hdg = 0;  // degrees from north, clockwise

  // s
  Scene scene;

  // chg
  Chargers chargers;

  // cfg
  int bright = -1;  // -1 = not in the message
  bool hasTilt = false;
  bool tilt = true;
  int rot = -1;  // degrees clockwise, 0..359; -1 = not in the message

  // ota / map / route
  TransferOp op = TransferOp::None;
  int id = 0;
  uint32_t size = 0;
  char sha256[65] = {0};  // ota: hex
  char version[24] = {0};  // ota
  int w = 0, h = 0;        // map: pixels
  float mpp = 0;           // map: metres per pixel
  char attr[40] = {0};     // map: whom the picture is credited to

  // dev (only honoured by builds with MONAV_DEV_TOKEN)
  bool hasLayout = false;
  Layout layout = Layout::Hud;
};

}  // namespace monav
