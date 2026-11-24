#pragma once

// Apple Notification Center Service (ANCS): the iPhone's notifications on the display. The iPhone
// is the GATT server; the display subscribes to its Notification Source and Data Source and asks
// for the details of each notification over the Control Point. This file is the wire format only
// (portable, tested on the host); device/ancs.cpp does the Bluetooth.
//
// https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleNotificationCenterServiceSpecification/

#include <cstddef>
#include <cstdint>

namespace monav::ancs {

constexpr const char* kService = "7905F431-B5CE-4E99-A40F-4B1E122D00D0";
constexpr const char* kNotificationSource = "9FBF120D-6301-42D9-8C58-25E699A21DBD";
constexpr const char* kControlPoint = "69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9";
constexpr const char* kDataSource = "22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB";

enum class Category : uint8_t {
  Other = 0,
  IncomingCall = 1,
  MissedCall = 2,
  Voicemail = 3,
  Social = 4,
  Schedule = 5,
  Email = 6,
  News = 7,
  HealthAndFitness = 8,
  BusinessAndFinance = 9,
  Location = 10,
  Entertainment = 11,
};

enum class EventId : uint8_t { Added = 0, Modified = 1, Removed = 2 };

namespace flag {
constexpr uint8_t kSilent = 1 << 0;
constexpr uint8_t kImportant = 1 << 1;
constexpr uint8_t kPreExisting = 1 << 2;
constexpr uint8_t kPositiveAction = 1 << 3;
constexpr uint8_t kNegativeAction = 1 << 4;
}  // namespace flag

/** One 8-byte packet of the Notification Source. */
struct SourceEvent {
  EventId event = EventId::Added;
  uint8_t flags = 0;
  Category category = Category::Other;
  uint8_t count = 0;
  uint32_t uid = 0;
};

bool parseSource(const uint8_t* data, size_t len, SourceEvent& out);

/** A notification as the display shows it: what the iPhone sent, cleaned for the fonts. */
struct Notice {
  uint32_t uid = 0;
  Category category = Category::Other;
  uint8_t flags = 0;
  char app[64] = {0};      // bundle identifier, e.g. net.whatsapp.WhatsApp
  char appName[32] = {0};  // the app's display name (GetAppAttributes), may stay empty
  char title[64] = {0};    // mostly the sender
  char subtitle[48] = {0};
  char message[192] = {0};
  char positive[24] = {0};  // action labels, e.g. "Annehmen" / "Ablehnen"
  char negative[24] = {0};

  bool isCall() const { return category == Category::IncomingCall; }
};

// --- Control Point commands ------------------------------------------------------------------

/** Lengths the display asks for; the iPhone truncates longer values itself. */
constexpr uint16_t kTitleMax = 60;
constexpr uint16_t kSubtitleMax = 44;
constexpr uint16_t kMessageMax = 180;

/** GetNotificationAttributes for everything a Notice holds. Returns the bytes written (≤ 32). */
size_t buildGetNotification(uint8_t* out, size_t cap, uint32_t uid);
/** GetAppAttributes: the display name of a bundle identifier. */
size_t buildGetApp(uint8_t* out, size_t cap, const char* app);
/** PerformNotificationAction: positive (accept a call) or negative (decline, dismiss). */
size_t buildAction(uint8_t* out, size_t cap, uint32_t uid, bool positive);

/**
 * Collects a Data Source response, which may come in several notifications. Begin it with the
 * command just written; feed every Data Source packet. Done once every attribute asked for is in.
 */
class Response {
 public:
  enum class Result : uint8_t { More, Done, Error };

  void beginNotification(uint32_t uid);
  void beginApp(const char* app);
  void reset() { active_ = false; }
  bool active() const { return active_; }
  bool forApp() const { return command_ == 1; }

  Result feed(const uint8_t* data, size_t len);

  /** The notification's attributes (after beginNotification). */
  const Notice& notice() const { return notice_; }
  /** The app's display name (after beginApp). */
  const char* appName() const { return appName_; }
  const char* app() const { return appId_; }

 private:
  bool parse();
  void store(uint8_t attr, const uint8_t* value, size_t len);

  bool active_ = false;
  uint8_t command_ = 0;
  uint32_t uid_ = 0;
  char appId_[64] = {0};
  uint8_t buf_[600];
  size_t len_ = 0;
  Notice notice_;
  char appName_[32] = {0};
};

/**
 * Copies UTF-8 text into `out` so the display's fonts can draw it: typographic quotes and dashes
 * become their plain forms, emoji and everything outside the Basic Multilingual Plane are dropped,
 * control characters and runs of white space become one space, and the end never cuts a character.
 */
void cleanText(const char* in, size_t len, char* out, size_t cap);

}  // namespace monav::ancs
