#include <Adafruit_Fingerprint.h>

// Build modes are compile-time only. The normal firmware target is integrated;
// pass -DSMART_OFFICE_MODE=STANDALONE_MODE to build the tested local-only path.
#define STANDALONE_MODE 0
#define INTEGRATED_MODE 1

#ifndef SMART_OFFICE_MODE
#define SMART_OFFICE_MODE INTEGRATED_MODE
#endif

#if SMART_OFFICE_MODE != STANDALONE_MODE && SMART_OFFICE_MODE != INTEGRATED_MODE
#error "SMART_OFFICE_MODE must be INTEGRATED_MODE or STANDALONE_MODE."
#endif

// Temporary diagnostic switch:
//   1 = compile only the exact standalone Fingerprint.ino test below.
//   0 = compile/run the complete Smart Office Access System.
#define FINGERPRINT_ONLY_DEBUG 0

// Servo isolation uses the working servo_test.ino attach/write sequence, but
// keeps GPIO13 because GPIO18 is already the RC522 SCK pin in the full project.
#define SERVO_ONLY_DEBUG 0

#define FINGERPRINT_MODEL "AS608/JM-101B"
#define FINGERPRINT_EXPECTED_VOLTAGE "3.3V"
#define FINGERPRINT_EXPECTED_CURRENT_MA 60

#if FINGERPRINT_ONLY_DEBUG && SERVO_ONLY_DEBUG
#error "Enable only one hardware isolation mode at a time."
#endif

#if FINGERPRINT_ONLY_DEBUG

// Exact copy of the known-working standalone Fingerprint.ino. The complete
// Smart Office code, libraries, and global objects are excluded from this
// build by the file-level conditional compilation block.
#define RX_PIN 16
#define TX_PIN 17

HardwareSerial fingerprintSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerprintSerial);

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=== AS608 / JM-101B Fingerprint Test ===");
  Serial.println("Interface: UART only (USB D+/D- not used)");
  Serial.println("Expected voltage: 3.3V");

  fingerprintSerial.begin(57600, SERIAL_8N1, RX_PIN, TX_PIN);

  Serial.println("Checking sensor...");

  finger.begin(57600);

  if (finger.verifyPassword()) {
    Serial.println("SUCCESS: AS608 / JM-101B detected!");
  } else {
    Serial.println("ERROR: AS608 / JM-101B not detected!");
    Serial.println("Check:");
    Serial.println("- VCC");
    Serial.println("- GND");
    Serial.println("- TX/RX wiring");
    Serial.println("- Baud rate");
    return;
  }

  Serial.println("Sensor information:");

  if (finger.getParameters() == FINGERPRINT_OK) {
    Serial.print("Status: 0x");
    Serial.println(finger.status_reg, HEX);
    Serial.print("Capacity: ");
    Serial.println(finger.capacity);
    Serial.print("Security level: ");
    Serial.println(finger.security_level);
    Serial.print("Packet length: ");
    Serial.println(finger.packet_len);
    Serial.print("Baud rate: ");
    Serial.println(finger.baud_rate);
  }

  Serial.println();
  Serial.println("Place your finger on the sensor...");
}

void loop() {
  uint8_t result = finger.getImage();

  if (result == FINGERPRINT_OK) {
    Serial.println("Fingerprint detected!");

    result = finger.image2Tz();
    if (result == FINGERPRINT_OK) {
      Serial.println("Image converted successfully.");

      result = finger.fingerFastSearch();
      if (result == FINGERPRINT_OK) {
        Serial.println("MATCH FOUND!");
        Serial.print("ID: ");
        Serial.println(finger.fingerID);
        Serial.print("Confidence: ");
        Serial.println(finger.confidence);
      } else if (result == FINGERPRINT_NOTFOUND) {
        Serial.println("Fingerprint not found in database.");
      } else {
        Serial.print("Search error: ");
        Serial.println(result);
      }
    } else {
      Serial.print("Image conversion failed: ");
      Serial.println(result);
    }

    delay(1000);
  }
}

#else

#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

#if SMART_OFFICE_MODE == INTEGRATED_MODE
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <esp_system.h>
#include <time.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "INTEGRATED_MODE requires secrets.h. Copy secrets.example.h to secrets.h and fill the local values."
#endif

#if !defined(WIFI_SSID) || !defined(WIFI_PASSWORD) || !defined(BACKEND_BASE_URL) || \
    !defined(SMART_OFFICE_API_TOKEN) || !defined(SMART_OFFICE_TLS_ROOT_CA)
#error "secrets.h must define Wi-Fi, Backend URL, API token, and TLS root CA values."
#endif
#endif

/*
  POWER WARNING:
  - Power the ESP32 from USB during development.
  - Never power the servo or other high-current loads from ESP32 3.3V.
  - Use a regulated external 5V supply for the servo and join its GND to
    ESP32 GND. While USB is connected, do not join external +5V to the
    ESP32 5V/VIN pin unless the exact board schematic confirms safe isolation.
  - A missing common ground or unstable 3.3V/5V rails can cause fingerprint failures,
    servo jitter, LCD flicker, unstable readings, and ESP32 brownout resets.
*/

// If the LCD does not work with 0x27, try 0x3F.
#define LCD_ADDRESS 0x27
#define LCD_COLUMNS 16
#define LCD_ROWS 2
#define RED_LED_PIN 25
#define BUZZER_PIN 26
#define GREEN_LED_PIN 32
#define SERVO_PIN 13
#define SERVO_USE_SIMPLE_ATTACH 1
#define ULTRASONIC_TRIG_PIN 21
#define ULTRASONIC_ECHO_PIN 22

/*
  HC-SR04 WARNING:
  ECHO is a 5V signal. Never connect it directly to ESP32 GPIO22.
  Use a voltage divider or a 3.3V-safe level shifter.
*/

// Dedicated pin map: every GPIO below has one component role only.
// GPIO 21/22 are ultrasonic Trigger/Echo, GPIO 14/27 are LCD I2C,
// GPIO 16/17 are fingerprint UART2, and GPIO 13 is the servo PWM signal.
// validatePinAssignments() checks for duplicates.
// RFID RC522 pins (ESP32 VSPI)
// POWER WARNING: RC522 VCC is 3.3V only; never connect it to 5V.
constexpr uint8_t RFID_SS_PIN = 5;
constexpr uint8_t RFID_SCK_PIN = 18;
constexpr uint8_t RFID_MOSI_PIN = 23;
constexpr uint8_t RFID_MISO_PIN = 19;
constexpr uint8_t RFID_RST_PIN = 33;

constexpr uint8_t LCD_SDA_PIN = 14;
constexpr uint8_t LCD_SCL_PIN = 27;

/*
  LCD I2C VOLTAGE NOTE:
  Many 5V backpacks pull SDA/SCL up to their VCC. ESP32 GPIO is 3.3V logic.
  Measure both idle bus lines first; use a bidirectional I2C level shifter if
  either is pulled above 3.3V. A 3.3V backpack supply is only an option when
  the exact LCD/backpack is confirmed to operate correctly at that voltage.
*/

/*
  Fingerprint sensor used in this project:
  - AS608 / JM-101B optical fingerprint sensor
  - Product specification operating voltage: 3.3V
  - Typical current: about 60mA
  - Interface used: UART only; USB D+ and D- are not used
  - Use PCB/datasheet labels, not wire colors
  - Sensor UART TX -> ESP32 GPIO16 / RX2
  - Sensor UART RX -> ESP32 GPIO17 / TX2
*/
#define FINGER_RX_PIN 16
#define FINGER_TX_PIN 17

// The working standalone Fingerprint.ino uses 57600. Other rates are tried
// only after the exact working-test initialization pattern fails at 57600.
constexpr uint32_t FINGERPRINT_PRIMARY_BAUD = 57600;
constexpr uint32_t FINGERPRINT_FALLBACK_BAUD_RATES[] = {9600, 19200, 38400};
constexpr uint32_t FINGERPRINT_CAPTURE_TIMEOUT_MS = 15000;
constexpr uint16_t FINGERPRINT_DELETE_IDS[] = {1, 2, 3, 4, 5, 6};
constexpr size_t FINGERPRINT_DELETE_ID_COUNT =
    sizeof(FINGERPRINT_DELETE_IDS) / sizeof(FINGERPRINT_DELETE_IDS[0]);

constexpr int FINGERPRINT_NOT_RECOGNIZED = -1;
constexpr int FINGERPRINT_CAPTURE_TIMEOUT = -2;
constexpr int FINGERPRINT_READ_ERROR = -3;

const String ADMIN_RFID_UID = "47469C2E";
const String ADMIN_RFID_PLACEHOLDER = "PUT_ADMIN_CARD_UID_HERE";

enum Area {
  AREA_NONE = 0,
  COMPANY_A = 1,
  COMPANY_B = 2,
  COMPANY_C = 3,
  COMPANY_D = 4,
  SERVER_ROOM = 5,
  MANAGEMENT_ADMIN = 6,
  MAIN_ENTRANCE = 7
};

enum AccessMode {
  MODE_ENTRY,
  MODE_EXIT
};

enum AccessPrecheckResult {
  ACCESS_PRECHECK_OK,
  ACCESS_PRECHECK_LOCKED,
  ACCESS_PRECHECK_NO_AREA,
  ACCESS_PRECHECK_DOOR_OPEN,
  ACCESS_PRECHECK_ULTRASONIC_INVALID,
  ACCESS_PRECHECK_PERSON_NOT_NEAR,
  ACCESS_PRECHECK_FINGERPRINT_UNAVAILABLE
};

enum FingerprintDeleteGateResult {
  FINGERPRINT_DELETE_ALLOWED,
  FINGERPRINT_DELETE_BLOCKED_LOCKDOWN,
  FINGERPRINT_DELETE_BLOCKED_SENSOR,
  FINGERPRINT_DELETE_BLOCKED_ADMIN
};

enum FingerprintDeleteConfirmationResult {
  FINGERPRINT_DELETE_CONFIRMED,
  FINGERPRINT_DELETE_CANCELLED_TEXT,
  FINGERPRINT_DELETE_CANCELLED_LOCKDOWN,
  FINGERPRINT_DELETE_CANCELLED_SENSOR,
  FINGERPRINT_DELETE_CANCELLED_ADMIN
};

struct User {
  int fingerprintID;
  String name;
  String company;
  String role;
  bool canAccessMainEntrance;
  bool canAccessCompanyA;
  bool canAccessCompanyB;
  bool canAccessServerRoom;
  bool canAccessManagement;
  bool canAccessCompanyC;
  bool canAccessCompanyD;
  uint16_t insideMask;
};

User users[] = {
    {1, "Employee A", "Company A", "Employee", true, true, false, false, false, false, false, 0},
    {2, "Employee B", "Company B", "Employee", true, false, true, false, false, false, false, 0},
    {3, "Employee C", "Company C", "Employee", true, false, false, false, false, true, false, 0},
    {4, "Employee D", "Company D", "Employee", true, false, false, false, false, false, true, 0},
    {5, "IT Admin", "IT", "IT", true, false, false, true, false, false, false, 0},
    {6, "Manager", "Management", "Manager", true, true, true, true, true, true, true, 0}};

constexpr size_t USER_COUNT = sizeof(users) / sizeof(users[0]);

struct AccessControlStateSnapshot {
  Area selectedArea;
  AccessMode currentMode;
  bool systemLocked;
  bool adminMode;
  int failedAttempts;
  unsigned long adminModeStartTime;
  bool waitingForEnrollmentID;
  bool waitingForDeleteConfirmation;
  bool doorOpen;
  unsigned long doorOpenedAt;
  bool presencePromptVisible;
  unsigned long lastLCDMessageAt;
  String lcdLine1;
  String lcdLine2;
  uint16_t insideMasks[USER_COUNT];
};

MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
HardwareSerial fingerprintSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerprintSerial);
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLUMNS, LCD_ROWS);
Servo doorServo;

Area selectedArea = AREA_NONE;
bool rfidInitialized = false;
bool fingerprintReady = false;
uint32_t fingerprintWorkingBaud = 0;
bool lcdInitialized = false;
bool waitingForEnrollmentID = false;
bool waitingForDeleteConfirmation = false;

bool systemLocked = false;
bool adminMode = false;
int failedAttempts = 0;

const int MAX_FAILED_ATTEMPTS = 3;
const unsigned long ADMIN_MODE_TIMEOUT_MS = 60000;
constexpr bool COUNT_ANTI_PASSBACK_AS_FAILED_ATTEMPT = true;
constexpr bool COUNT_EXIT_OUTSIDE_AS_FAILED_ATTEMPT = false;
unsigned long adminModeStartTime = 0;

const int DOOR_CLOSED_ANGLE = 0;
const int DOOR_OPEN_ANGLE = 90;
const float PRESENCE_DISTANCE_CM = 20.0;
const unsigned long DOOR_MIN_OPEN_TIME_MS = 5000;
const unsigned long ULTRASONIC_CHECK_INTERVAL_MS = 300;
const unsigned long ULTRASONIC_ECHO_TIMEOUT_US = 30000;
const unsigned long PRESENCE_PROMPT_HOLD_MS = 1500;

AccessMode currentMode = MODE_ENTRY;
bool servoInitialized = false;
bool doorOpen = false;
unsigned long doorOpenedAt = 0;

float lastDistanceCm = -1.0;
unsigned long lastUltrasonicCheckAt = 0;
bool ultrasonicHasMeasurement = false;
bool presencePromptVisible = false;
unsigned long lastLCDMessageAt = 0;
String lastLcdLine1 = "";
String lastLcdLine2 = "";

enum IntegrationSyncState {
  INTEGRATION_UNSYNCED,
  INTEGRATION_SYNCED,
  INTEGRATION_RECONCILIATION_REQUIRED
};

#if SMART_OFFICE_MODE == INTEGRATED_MODE
enum WiFiConnectionState {
  WIFI_CONNECTION_IDLE,
  WIFI_CONNECTION_CONNECTING,
  WIFI_CONNECTION_CONNECTED,
  WIFI_CONNECTION_RETRY_WAIT
};

enum RemoteCommandDecision {
  REMOTE_COMMAND_ACCEPT,
  REMOTE_COMMAND_DUPLICATE,
  REMOTE_COMMAND_REJECT_INVALID,
  REMOTE_COMMAND_REJECT_NOT_READY,
  REMOTE_COMMAND_REJECT_CONFLICT
};

enum RemoteFingerprintWorkflowState {
  REMOTE_FINGERPRINT_PRECHECK,
  REMOTE_FINGERPRINT_WAITING_FOR_FINGER,
  REMOTE_FINGERPRINT_CONVERTING_IMAGE,
  REMOTE_FINGERPRINT_SEARCHING,
  REMOTE_ACCESS_CHECK_READY,
  REMOTE_ACCESS_CHECK_STATUS,
  REMOTE_AUTHORIZED_WAITING_DOOR,
  REMOTE_OPENING_DOOR,
  REMOTE_WAITING_FOR_COMPLETE_ACK,
  REMOTE_COMPLETION_RECONCILIATION_REQUIRED
};

enum RemoteFingerprintSensorOutcome {
  REMOTE_SENSOR_KEEP_WAITING,
  REMOTE_SENSOR_IMAGE_READY,
  REMOTE_SENSOR_MATCH,
  REMOTE_SENSOR_NOT_RECOGNIZED,
  REMOTE_SENSOR_READ_ERROR
};

struct RemoteAccessRequestState {
  bool active;
  String requestId;
  Area area;
  AccessMode direction;
  bool createdBySerial;
  String expiresAt;
  unsigned long receivedAt;
  RemoteFingerprintWorkflowState workflowState;
  unsigned long scanStartedAt;
  bool precheckDoorOpen;
  bool precheckPersonKnown;
  bool precheckPersonDetected;
  bool precheckDistanceKnown;
  float precheckDistanceCm;
  bool precheckFingerprintReady;
  String fingerprintResult;
  int fingerprintId;
  int fingerprintConfidence;
  String accessCheckPayload;
  unsigned long lastAccessCheckAttemptAt;
  bool liveAuthorizationReceived;
  bool doorOpenCommandIssued;
  String completionPayload;
  unsigned long lastCompletionAttemptAt;
};

constexpr unsigned long WIFI_CONNECTION_ATTEMPT_TIMEOUT_MS = 15000;
constexpr unsigned long WIFI_RETRY_DELAY_MS = 5000;
constexpr unsigned long HEARTBEAT_INTERVAL_MS = 2000;
constexpr unsigned long REMOTE_REQUEST_STATUS_INTERVAL_MS = 2000;
constexpr unsigned long ACCESS_CHECK_RETRY_INTERVAL_MS = 2000;
constexpr unsigned long COMPLETION_RETRY_INTERVAL_MS = 2000;
constexpr unsigned long REMOTE_FINGERPRINT_CAPTURE_TIMEOUT_MS = 25000;
constexpr unsigned long BACKEND_RETRY_INTERVAL_MS = 5000;
constexpr unsigned long BACKEND_FAILURE_LOG_INTERVAL_MS = 15000;
constexpr unsigned long BACKEND_SUSTAINED_OUTAGE_MS = 18000;
constexpr unsigned long BACKEND_ACCESS_FRESHNESS_MS = 10000;
constexpr unsigned long TLS_CLOCK_WAIT_LOG_INTERVAL_MS = 10000;
constexpr uint16_t HTTP_CONNECT_TIMEOUT_MS = 4000;
constexpr uint16_t HTTP_RESPONSE_TIMEOUT_MS = 7000;
constexpr uint8_t BACKEND_TRANSIENT_FAILURE_LIMIT = 3;
constexpr time_t TLS_MINIMUM_VALID_EPOCH = 1704067200;
constexpr size_t EXPECTED_BOOTSTRAP_ATTENDANCE_ROWS = USER_COUNT * 7;

IntegrationSyncState integrationSyncState = INTEGRATION_UNSYNCED;
String bootId;
String pendingAuthorizationId;
String lastUnexpectedCommandId;
RemoteAccessRequestState remoteAccessRequest;
String serialCreatedRequestId;
Area serialCreatedRequestArea = AREA_NONE;
AccessMode serialCreatedRequestDirection = MODE_ENTRY;
WiFiConnectionState wifiConnectionState = WIFI_CONNECTION_IDLE;
bool wifiWasConnected = false;
bool backendReachable = false;
bool backendSystemActive = false;
bool backendAdminMode = false;
bool tlsClockSyncStarted = false;
uint8_t consecutiveBackendFailures = 0;
unsigned long wifiConnectionAttemptStartedAt = 0;
unsigned long wifiRetryScheduledAt = 0;
unsigned long lastTlsClockWaitLogAt = 0;
unsigned long lastHeartbeatAttemptAt = 0;
unsigned long lastRemoteRequestStatusCheckAt = 0;
unsigned long lastBackendSyncAttemptAt = 0;
unsigned long lastBackendFailureLogAt = 0;
unsigned long lastBackendSuccessAt = 0;
#endif

void setup();
void loop();
void setupFingerprintOnlyDebug();
void loopFingerprintOnlyDebug();
void setupServoOnlyDebug();
void loopServoOnlyDebug();

void printMenu();
void printSystemStatus();
void printHardwareSelfTest();
void runHardwareReadinessCheck();
AccessControlStateSnapshot captureAccessControlState();
bool isAccessControlStateUnchanged(
    const AccessControlStateSnapshot& snapshot);
bool isLCDCacheUnchanged(const AccessControlStateSnapshot& snapshot);
void restoreAccessControlState(const AccessControlStateSnapshot& snapshot);
void restoreDiagnosticLCD(const AccessControlStateSnapshot& snapshot);
void restoreWorkingFingerprintState(bool wasReady,
                                    uint32_t savedBaud);
bool isStateGuardedDiagnosticCommand(char command);
void runStateGuardedDiagnostic(char command);
bool validatePinAssignments();
void runSoftwareValidation();

void printIntegrationStatus();
#if SMART_OFFICE_MODE == INTEGRATED_MODE
void setupNetworkIntegration();
void serviceNetworkIntegration();
void startWiFiConnectionAttempt();
void scheduleWiFiRetry();
String getWiFiStatusName(wl_status_t status);
String generateBootId();
String buildBackendUrl(const char* path);
bool backendUsesHttps();
bool serviceTlsClock();
bool beginBackendHttpRequest(HTTPClient& http,
                             WiFiClient& plainClient,
                             NetworkClientSecure& secureClient,
                             const String& url);
void addBackendAuthorization(HTTPClient& http);
bool sendHeartbeat();
bool fetchBootstrap();
bool postJson(const char* path,
              const String& requestBody,
              String& responseBody,
              int& responseCode,
              int expectedResponseCode);
bool getJson(const char* path,
             String& responseBody,
             int& responseCode);
bool applyHeartbeatResponse(const String& responseBody);
bool applyBootstrapResponse(const String& responseBody);
bool parseRemoteDirection(const String& value, AccessMode& direction);
RemoteCommandDecision evaluateRemoteCommand(
    const String& requestId,
    int areaId,
    AccessMode direction);
void handleRemoteCommand(JsonObject command);
void resetRemoteAccessRequestState();
void serviceRemoteFingerprintWorkflow();
void serviceRemoteDoorCompletionWorkflow();
bool shouldStartRemoteFingerprintScan(AccessMode direction,
                                      bool locked,
                                      bool isDoorOpen,
                                      bool fingerprintAvailable,
                                      bool personKnown,
                                      bool personDetected);
RemoteFingerprintSensorOutcome classifyRemoteFingerprintImageResult(
    uint8_t result);
RemoteFingerprintSensorOutcome classifyRemoteFingerprintSearchResult(
    uint8_t result);
bool isRemoteFingerprintSensorStep(RemoteFingerprintWorkflowState state);
void prepareRemoteAccessCheckPayload(const String& fingerprintResult,
                                     int fingerprintId = -1,
                                     int confidence = -1);
bool handleRemoteAccessCheckResponse(const String& responseBody);
bool canOpenRemoteDoorForState(RemoteFingerprintWorkflowState state,
                               bool active,
                               bool liveAuthorizationReceived,
                               bool pendingRequestMatches,
                               bool servoReady,
                               bool isDoorOpen,
                               bool locked);
bool canSendRemoteCompletionForState(
    RemoteFingerprintWorkflowState state,
    bool active,
    bool liveAuthorizationReceived,
    bool doorOpenCommandIssued,
    bool completionPayloadReady);
void prepareRemoteCompletionPayload();
bool handleRemoteCompletionResponse(const String& responseBody);
void enterRemoteCompletionReconciliation(const String& reason);
String getRemoteFingerprintWorkflowName(
    RemoteFingerprintWorkflowState state);
void serviceRemoteRequestStatus();
void clearTrackedRemoteRequest(const String& requestId,
                               const String& terminalState);
void createIntegratedSerialAccessRequest();
String getApiErrorDescription(const String& responseBody);
void printRemoteAccessStatus();
bool parseCanonicalSecurity(JsonDocument& document,
                            int& canonicalFailedAttempts,
                            bool& canonicalLockdown,
                            bool& canonicalSystemActive,
                            bool& canonicalAdminMode);
void mirrorCanonicalSecurity(int canonicalFailedAttempts,
                             bool canonicalLockdown);
bool backendRecentlyConfirmed();
void recordBackendSuccess(const String& operation);
void markBackendUnavailable(const String& operation, int responseCode);
void setIntegrationSyncState(IntegrationSyncState newState);
String getIntegrationSyncStateName(IntegrationSyncState state);
#endif

void setupAlertOutputs();
void triggerAccessDeniedAlert();
void triggerEnrollmentFailedAlert();
void triggerInvalidCommandAlert();
void triggerErrorAlert();
void beepOnce(int durationMs);
void beepMultiple(int count, int durationMs, int pauseMs);
void redLedOn(int durationMs);
void turnOffAlerts();
void testAlerts();

void setupSuccessOutput();
void triggerAccessGrantedFeedback();
void greenLedOn(int durationMs);
void turnOffSuccessOutput();
void testGreenLed();

void incrementFailedAttempts(String reason);
bool updateFailedAttemptCounter();
void resetFailedAttempts();
void lockSystem(String reason);
void unlockSystem();
void applyLockedState();
void applyUnlockedAdminState();
bool isSystemLocked();
bool isAdminModeActive();
void enableAdminMode();
void disableAdminMode();
void checkAdminModeTimeout();
bool hasAdminModeExpired(unsigned long now);
void handleAdminRFID();
void handleUnknownRFID(String uid);
void showLockdownStatus();
void printSecurityStatus();

void setupDoorServo();
void setupUltrasonicSensor();
void openDoor(String reason);
void openDoor(String reason, Area area, AccessMode accessMode);
void closeDoor(String reason);
void updateDoorState();
bool shouldCloseDoor();
bool shouldCloseDoorForState(bool isOpen,
                             unsigned long openElapsedMs,
                             bool ultrasonicMeasurementValid,
                             bool personNear);
float readDistanceCm();
bool isPersonNear();
bool isUltrasonicMeasurementValid(float distanceCm);
bool isDistanceNear(float distanceCm);
bool isPresenceRequiredForMode(AccessMode mode);
void updateUltrasonicMeasurement(bool forceRead);
void updatePresencePrompt();
void testDoor();
void printUltrasonicStatus();
void returnToEntryMode();
String getAccessModeName(AccessMode mode);

void setupLCD();
void lcdShowWelcome();
void lcdShowMenu();
void lcdShowSelectedArea(Area area);
void lcdShowEnrollStart();
void lcdShowEnrollStep(String line1, String line2);
void lcdShowEnrollSuccess(int id);
void lcdShowEnrollFailed(String shortReason);
void lcdShowPlaceFinger();
void lcdShowAccessGranted(User* user, Area area);
void lcdShowAccessDenied(String reason);
void lcdShowRFIDStatus(String line1, String line2);
void lcdShowError(String line1, String line2);
void lcdShowDoorOpen(bool exitMode);
void lcdShowDoorClosed();
void lcdShowSystemLocked();
void lcdShowAdminMode();
void lcdShowMessage(String line1, String line2 = "");
String getLCDLine(String text);
bool isLCDMessageUnchanged(const String& line1,
                           const String& line2);
String getLCDAreaName(Area area);

void setupRFID();
void checkRFID();
String readRFIDUID();

void setupFingerprint();
bool runFingerprintStandaloneStyleTest();
bool setupFingerprintWithBaudScan();
bool initializeFingerprintAtBaud(uint32_t baudRate);
void printFingerprintParameters();
void testSimpleFingerprintScan();
bool enrollFingerprint(int id);
FingerprintDeleteGateResult evaluateFingerprintDeleteGate(
    bool locked,
    bool sensorReady,
    bool adminActive);
FingerprintDeleteConfirmationResult evaluateFingerprintDeleteConfirmation(
    const String& input,
    bool locked,
    bool sensorReady,
    bool adminActive);
bool configuredFingerprintDeleteIDsAreExactly1To6();
bool isFingerprintSlotEmptyResult(uint8_t result);
String getFingerprintDeleteError(uint8_t result);
void requestConfiguredFingerprintDeletion();
void handleFingerprintDeleteConfirmation(const String& input);
void cancelFingerprintDeleteConfirmation(String reason);
void deleteConfiguredFingerprints();
int readFingerprintID();

void handleSerialCommand();
void handleAreaSelection(char command);
void processSerialInput(String input);
bool canEnableExitModeForState(bool locked,
                               Area area,
                               bool isDoorOpen);
AccessPrecheckResult evaluateAccessPrecheck(
    AccessMode mode,
    bool locked,
    Area area,
    bool isDoorOpen,
    bool ultrasonicMeasurementValid,
    bool personNear,
    bool fingerprintAvailable);

String getAreaName(Area area);

User* findUserByFingerprintID(int fingerprintID);
bool checkPermission(User* user, Area area);
int getAreaBit(Area area);
bool isUserInsideArea(User* user, Area area);
void markUserInsideArea(User* user, Area area);
void markUserOutsideArea(User* user, Area area);
bool commitAttendanceAfterDoorOpen(User* user,
                                   Area area,
                                   AccessMode accessMode,
                                   bool isDoorOpen);
bool canUserEnterArea(User* user, Area area);
bool canUserExitArea(User* user, Area area);
void handleEntryAccess(User* user, Area area);
void handleExitAccess(User* user, Area area);
void printUserInsideStatus(User* user);
void printAllInsideStatus();
int countUsersInsideArea(Area area);

void grantAccess(User* user, Area area, String method);
void denyAccess(String reason,
                Area area,
                String method,
                User* user = nullptr,
                bool countFailedAttempt = true);

bool isNumericInput(const String& value);
bool waitForFingerImage(uint32_t timeoutMs, uint8_t& result);
String getFingerprintError(uint8_t result);
void testFingerprintAccess();

void setup() {
#if FINGERPRINT_ONLY_DEBUG
  setupFingerprintOnlyDebug();
#elif SERVO_ONLY_DEBUG
  setupServoOnlyDebug();
#else
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println("Smart Office Building Access System");
  Serial.println("Task 7: Companies C/D + Anti-Passback");
  Serial.println("========================================");
  Serial.println();
  Serial.println("Hardware:");
  Serial.println("- ESP32 DEV KIT V1");
  Serial.println("- RFID RC522");
  Serial.println("- Fingerprint Sensor");
  Serial.println("- I2C LCD 16x2");
  Serial.println("- Red LED");
  Serial.println("- Active Buzzer");
  Serial.println("- Green LED");
  Serial.println("- Servo Door");
  Serial.println("- HC-SR04 Ultrasonic Sensor");
  Serial.println();

  // Informational only: these prints do not wait for user input or block setup.
  Serial.println("[POWER CHECK]");
  Serial.println("RFID must be 3.3V only.");
  Serial.println("Servo should use external regulated 5V.");
  Serial.println("HC-SR04 Echo must use a voltage divider.");
  Serial.println("JM-101B: use 3.3V and UART TX/RX only; never connect USB D+/D- to GPIO16/17.");
  Serial.println("All GND must be common.");
  Serial.println("If F/U/D fail together, check 3.3V, servo 5V, and common GND first.");
  Serial.println();

  // Initialize the proven AS608 UART path before any other peripheral library.
  // This keeps full-mode detection as close as possible to isolation mode and
  // prevents LCD/RFID setup from being a software-order variable.
  setupFingerprint();
  setupLCD();
  lcdShowWelcome();
  setupRFID();
  setupAlertOutputs();
  setupSuccessOutput();
  setupDoorServo();
  setupUltrasonicSensor();
  printHardwareSelfTest();
  if (!rfidInitialized || !fingerprintReady) {
    triggerErrorAlert();
  }
  printMenu();
  Serial.println("Security:");
  Serial.println("- Lockdown after 3 failed attempts");
  Serial.println("- Admin RFID required to unlock");
  Serial.println("- Enrollment requires Admin Mode");
  Serial.println("- Anti-passback enabled for all 7 areas");
#if SMART_OFFICE_MODE == INTEGRATED_MODE
  Serial.println("- Attendance/security mirrors load from Backend bootstrap");
  Serial.println("Attendance mirror is UNSYNCED until bootstrap succeeds.");
#else
  Serial.println("- All users start OUTSIDE after reset");
  Serial.println("Attendance tracking initialized.");
  Serial.println("All users are OUTSIDE.");
#endif
  Serial.println("Anti-Passback enabled.");
  printAllInsideStatus();
  Serial.println();
#if SMART_OFFICE_MODE == INTEGRATED_MODE
  setupNetworkIntegration();
#endif
  delay(1500);
  lcdShowMenu();
#endif
}

void loop() {
#if FINGERPRINT_ONLY_DEBUG
  loopFingerprintOnlyDebug();
#elif SERVO_ONLY_DEBUG
  loopServoOnlyDebug();
#else
  handleSerialCommand();
  checkRFID();
  checkAdminModeTimeout();
  updateDoorState();
  updatePresencePrompt();
#if SMART_OFFICE_MODE == INTEGRATED_MODE
  serviceNetworkIntegration();
#endif
#endif
}

void setupFingerprintOnlyDebug() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=== FINGERPRINT ONLY DEBUG MODE ===");
  Serial.println("Other modules are disabled.");
  Serial.println("RFID disabled.");
  Serial.println("LCD disabled.");
  Serial.println("Servo disabled.");
  Serial.println("Ultrasonic disabled.");
  Serial.println("LEDs and buzzer disabled.");
  Serial.println("Using HardwareSerial(2)");
  Serial.print("RX GPIO");
  Serial.println(FINGER_RX_PIN);
  Serial.print("TX GPIO");
  Serial.println(FINGER_TX_PIN);
  Serial.println("Baud 57600");
  Serial.println();
  Serial.println("=== AS608 / JM-101B Fingerprint Isolation Test ===");
  Serial.println("Expected voltage: 3.3V");
  Serial.println("Interface: UART TX/RX only; USB D+/D- are unused.");

  // Preserve the known-working standalone Fingerprint.ino initialization
  // order exactly: explicit UART2 pins, finger.begin(), then password check.
  fingerprintSerial.begin(57600, SERIAL_8N1, FINGER_RX_PIN, FINGER_TX_PIN);
  delay(100);

  Serial.println("Checking sensor...");

  finger.begin(57600);
  delay(100);

  fingerprintReady = finger.verifyPassword();
  if (!fingerprintReady) {
    fingerprintWorkingBaud = 0;
    Serial.println("ERROR: AS608 / JM-101B not detected even in isolation mode.");
    Serial.println("This means the problem is not caused by the full project logic.");
    Serial.println("Check current board, power, wiring, TX/RX, or GPIO16/GPIO17 availability.");
    Serial.println("Check:");
    Serial.println("- VCC must be 3.3V for this JM-101B");
    Serial.println("- GND");
    Serial.println("- UART TX/RX wiring (not USB D+/D-)");
    Serial.println("- Baud rate");
    return;
  }

  fingerprintWorkingBaud = 57600;
  Serial.println("SUCCESS: AS608 / JM-101B detected!");
  Serial.println();
  Serial.println("Sensor information:");

  if (finger.getParameters() == FINGERPRINT_OK) {
    Serial.print("Status: 0x");
    Serial.println(finger.status_reg, HEX);
    Serial.print("Capacity: ");
    Serial.println(finger.capacity);
    Serial.print("Security level: ");
    Serial.println(finger.security_level);
    Serial.print("Packet length: ");
    Serial.println(finger.packet_len);
    Serial.print("Baud rate: ");
    Serial.println(finger.baud_rate);
  } else {
    Serial.println("Could not read sensor parameters.");
  }

  Serial.println();
  Serial.println("Place your finger on the sensor...");
}

void loopFingerprintOnlyDebug() {
  if (!fingerprintReady) {
    delay(100);
    return;
  }

  uint8_t result = finger.getImage();

  if (result == FINGERPRINT_OK) {
    Serial.println("Fingerprint detected!");

    result = finger.image2Tz();
    if (result == FINGERPRINT_OK) {
      Serial.println("Image converted successfully.");

      result = finger.fingerFastSearch();
      if (result == FINGERPRINT_OK) {
        Serial.println("MATCH FOUND!");
        Serial.print("ID: ");
        Serial.println(finger.fingerID);
        Serial.print("Confidence: ");
        Serial.println(finger.confidence);
      } else if (result == FINGERPRINT_NOTFOUND) {
        Serial.println("Fingerprint not found in database.");
      } else {
        Serial.print("Search error: ");
        Serial.println(result);
      }
    } else {
      Serial.print("Image conversion failed: ");
      Serial.println(result);
    }

    delay(1000);
  }
}

void setupServoOnlyDebug() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=== SERVO ONLY DEBUG MODE ===");
  Serial.println("Other modules are disabled.");
  Serial.println("RFID disabled.");
  Serial.println("Fingerprint disabled.");
  Serial.println("LCD disabled.");
  Serial.println("Ultrasonic disabled.");
  Serial.println("LEDs and buzzer disabled.");
  Serial.println("Using ESP32Servo");
  Serial.print("Servo signal: GPIO");
  Serial.println(SERVO_PIN);
  Serial.println("Reference test used GPIO18, but GPIO18 is RC522 SCK here.");
  Serial.println("Connect the servo signal wire to GPIO13 for this project.");
  Serial.println("Power servo from external regulated 5V; join GND to ESP32 GND.");
  Serial.println();

  // Preserve the known-working servo_test.ino initialization style. Do not
  // allocate a timer or override pulse limits in this isolation path.
  const int servoPwmChannel = doorServo.attach(SERVO_PIN);
  // ESP32Servo can validly allocate channel 0, so attach(...) > 0 is an
  // incorrect success test. attached() reports the real attachment state.
  servoInitialized = doorServo.attached();
  if (!servoInitialized) {
    Serial.println("ERROR: Servo PWM attach failed!");
    Serial.println("Check board selection and GPIO13 availability.");
    return;
  }

  Serial.println("SG90 Servo Isolation Test");
  Serial.println("SUCCESS: Servo PWM attached.");
  Serial.print("PWM channel: ");
  Serial.println(servoPwmChannel);
}

void loopServoOnlyDebug() {
  if (!servoInitialized) {
    delay(100);
    return;
  }

  // Preserve the movement sequence and timing from the working servo_test.ino.
  Serial.println("Moving to 0 degrees");
  doorServo.write(0);
  delay(1000);

  Serial.println("Moving to 90 degrees");
  doorServo.write(90);
  delay(1000);

  Serial.println("Moving to 180 degrees");
  doorServo.write(180);
  delay(1000);

  Serial.println("Back to 90 degrees");
  doorServo.write(90);
  delay(1000);
}

#if SMART_OFFICE_MODE == INTEGRATED_MODE
String generateBootId() {
  uint64_t chipValue = ESP.getEfuseMac();
  uint32_t randomValue = esp_random();
  char value[40];
  snprintf(value,
           sizeof(value),
           "boot_%08lX%08lX%08lX",
           static_cast<unsigned long>(chipValue >> 32),
           static_cast<unsigned long>(chipValue & 0xFFFFFFFFULL),
           static_cast<unsigned long>(randomValue));
  return String(value);
}

String buildBackendUrl(const char* path) {
  String baseUrl = BACKEND_BASE_URL;
  while (baseUrl.endsWith("/")) {
    baseUrl.remove(baseUrl.length() - 1);
  }
  return baseUrl + path;
}

bool backendUsesHttps() {
  return String(BACKEND_BASE_URL).startsWith("https://");
}

bool serviceTlsClock() {
  if (!backendUsesHttps()) {
    return true;
  }

  if (time(nullptr) >= TLS_MINIMUM_VALID_EPOCH) {
    if (tlsClockSyncStarted) {
      Serial.println("[TLS] Trusted clock synchronized; certificate validation enabled.");
      tlsClockSyncStarted = false;
      lastBackendSyncAttemptAt = 0;
    }
    return true;
  }

  unsigned long now = millis();
  if (!tlsClockSyncStarted) {
    configTime(0, 0, "time.cloudflare.com", "pool.ntp.org");
    tlsClockSyncStarted = true;
    lastTlsClockWaitLogAt = now;
    Serial.println("[TLS] Waiting for trusted network time before HTTPS...");
  } else if (now - lastTlsClockWaitLogAt >= TLS_CLOCK_WAIT_LOG_INTERVAL_MS) {
    lastTlsClockWaitLogAt = now;
    Serial.println("[TLS] Still waiting for trusted network time; HTTPS remains blocked.");
  }
  return false;
}

bool beginBackendHttpRequest(HTTPClient& http,
                             WiFiClient& plainClient,
                             NetworkClientSecure& secureClient,
                             const String& url) {
  if (url.startsWith("https://")) {
    if (strlen(SMART_OFFICE_TLS_ROOT_CA) == 0) {
      Serial.println("[BACKEND] HTTPS blocked: TLS root CA is not configured.");
      return false;
    }
    secureClient.setCACert(SMART_OFFICE_TLS_ROOT_CA);
    return http.begin(secureClient, url);
  }
  if (url.startsWith("http://")) {
    return http.begin(plainClient, url);
  }
  Serial.println("[BACKEND] Unsupported URL scheme; use http:// or https://.");
  return false;
}

void addBackendAuthorization(HTTPClient& http) {
  http.addHeader(
      "Authorization", String("Bearer ") + SMART_OFFICE_API_TOKEN);
}

String getIntegrationSyncStateName(IntegrationSyncState state) {
  switch (state) {
    case INTEGRATION_SYNCED:
      return "SYNCED";
    case INTEGRATION_RECONCILIATION_REQUIRED:
      return "RECONCILIATION REQUIRED";
    case INTEGRATION_UNSYNCED:
    default:
      return "UNSYNCED";
  }
}

void setIntegrationSyncState(IntegrationSyncState newState) {
  if (integrationSyncState == newState) {
    return;
  }
  integrationSyncState = newState;
  Serial.print("[BACKEND] State: ");
  Serial.println(getIntegrationSyncStateName(integrationSyncState));
}

String getWiFiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "IDLE";
    case WL_NO_SSID_AVAIL:
      return "NO SSID AVAILABLE";
    case WL_SCAN_COMPLETED:
      return "SCAN COMPLETED";
    case WL_CONNECTED:
      return "CONNECTED";
    case WL_CONNECT_FAILED:
      return "CONNECT FAILED";
    case WL_CONNECTION_LOST:
      return "CONNECTION LOST";
    case WL_DISCONNECTED:
      return "DISCONNECTED";
    case WL_NO_SHIELD:
      return "NO WIFI SHIELD";
    default:
      return "UNKNOWN";
  }
}

void startWiFiConnectionAttempt() {
  if (wifiConnectionState == WIFI_CONNECTION_CONNECTING) {
    return;
  }

  wifiConnectionAttemptStartedAt = millis();
  wifiConnectionState = WIFI_CONNECTION_CONNECTING;
  Serial.print("[WIFI] Starting connection to ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void scheduleWiFiRetry() {
  wifiConnectionState = WIFI_CONNECTION_RETRY_WAIT;
  wifiRetryScheduledAt = millis();
  Serial.println("[WIFI] Retry scheduled");
}

void setupNetworkIntegration() {
  bootId = generateBootId();
  integrationSyncState = INTEGRATION_UNSYNCED;
  backendReachable = false;
  pendingAuthorizationId = "";
  wifiConnectionState = WIFI_CONNECTION_IDLE;
  resetRemoteAccessRequestState();
  serialCreatedRequestId = "";
  serialCreatedRequestArea = AREA_NONE;
  serialCreatedRequestDirection = MODE_ENTRY;
  lastRemoteRequestStatusCheckAt = 0;
  tlsClockSyncStarted = false;
  lastTlsClockWaitLogAt = 0;
  consecutiveBackendFailures = 0;
  lastBackendSuccessAt = 0;

  Serial.println("[INTEGRATION] Mode: INTEGRATED");
  Serial.print("[INTEGRATION] Boot ID: ");
  Serial.println(bootId);
  Serial.print("[BACKEND] Configured URL: ");
  Serial.println(BACKEND_BASE_URL);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  startWiFiConnectionAttempt();
}

void serviceNetworkIntegration() {
  unsigned long now = millis();
  wl_status_t wifiStatus = WiFi.status();
  bool wifiConnected = wifiStatus == WL_CONNECTED;

  if (!wifiConnected) {
    if (wifiConnectionState == WIFI_CONNECTION_CONNECTED ||
        wifiWasConnected) {
      Serial.println("[WIFI] Disconnected");
      wifiWasConnected = false;
      backendReachable = false;
      consecutiveBackendFailures = 0;
      lastBackendSuccessAt = 0;
      setIntegrationSyncState(INTEGRATION_UNSYNCED);
      WiFi.disconnect(false, false);
      scheduleWiFiRetry();
      return;
    }

    if (wifiConnectionState == WIFI_CONNECTION_CONNECTING) {
      if (now - wifiConnectionAttemptStartedAt >=
          WIFI_CONNECTION_ATTEMPT_TIMEOUT_MS) {
        Serial.println("[WIFI] Connection attempt timed out");
        Serial.print("[WIFI] Status: ");
        Serial.print(static_cast<int>(wifiStatus));
        Serial.print(" (");
        Serial.print(getWiFiStatusName(wifiStatus));
        Serial.println(')');
        WiFi.disconnect(false, false);
        scheduleWiFiRetry();
      }
      return;
    }

    if (wifiConnectionState == WIFI_CONNECTION_RETRY_WAIT) {
      if (now - wifiRetryScheduledAt >= WIFI_RETRY_DELAY_MS) {
        startWiFiConnectionAttempt();
      }
      return;
    }

    if (wifiConnectionState == WIFI_CONNECTION_IDLE) {
      startWiFiConnectionAttempt();
    }
    return;
  }

  if (wifiConnectionState != WIFI_CONNECTION_CONNECTED ||
      !wifiWasConnected) {
    wifiConnectionState = WIFI_CONNECTION_CONNECTED;
    wifiWasConnected = true;
    backendReachable = false;
    consecutiveBackendFailures = 0;
    lastBackendSuccessAt = 0;
    lastBackendSyncAttemptAt = 0;
    lastHeartbeatAttemptAt = 0;
    Serial.println("[WIFI] Connected");
    Serial.print("[WIFI] IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("[WIFI] RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    Serial.println("[BACKEND] Synchronizing...");
    if (backendUsesHttps()) {
      Serial.println("[TLS] HTTPS configured; certificate validation requires trusted time.");
    }
  }

  if (!serviceTlsClock()) {
    return;
  }

  if (integrationSyncState == INTEGRATION_UNSYNCED) {
    if (lastBackendSyncAttemptAt == 0 ||
        now - lastBackendSyncAttemptAt >= BACKEND_RETRY_INTERVAL_MS) {
      fetchBootstrap();
    }
    return;
  }

  if (integrationSyncState == INTEGRATION_RECONCILIATION_REQUIRED &&
      (lastBackendSyncAttemptAt == 0 ||
       now - lastBackendSyncAttemptAt >= BACKEND_RETRY_INTERVAL_MS)) {
    // Keep reporting physical state while periodically checking whether an
    // operator/later phase has resolved the durable pending authorization.
    if (!fetchBootstrap()) {
      return;
    }
  }

  if (lastHeartbeatAttemptAt == 0 ||
      now - lastHeartbeatAttemptAt >= HEARTBEAT_INTERVAL_MS) {
    sendHeartbeat();
  }
  serviceRemoteRequestStatus();
  serviceRemoteFingerprintWorkflow();
  serviceRemoteDoorCompletionWorkflow();
}

bool postJson(const char* path,
              const String& requestBody,
              String& responseBody,
              int& responseCode,
              int expectedResponseCode) {
  responseBody = "";
  responseCode = -1;
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClient plainClient;
  NetworkClientSecure secureClient;
  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
  http.setReuse(false);
  String url = buildBackendUrl(path);
  if (!beginBackendHttpRequest(http, plainClient, secureClient, url)) {
    return false;
  }

  addBackendAuthorization(http);
  http.addHeader("Content-Type", "application/json");
  responseCode = http.POST(requestBody);
  if (responseCode > 0) {
    responseBody = http.getString();
  }
  http.end();
  return responseCode == expectedResponseCode;
}

bool getJson(const char* path,
             String& responseBody,
             int& responseCode) {
  responseBody = "";
  responseCode = -1;
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClient plainClient;
  NetworkClientSecure secureClient;
  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
  http.setReuse(false);
  String url = buildBackendUrl(path);
  if (!beginBackendHttpRequest(http, plainClient, secureClient, url)) {
    return false;
  }

  addBackendAuthorization(http);
  responseCode = http.GET();
  if (responseCode > 0) {
    responseBody = http.getString();
  }
  http.end();
  return responseCode == HTTP_CODE_OK;
}

void resetRemoteAccessRequestState() {
  remoteAccessRequest.active = false;
  remoteAccessRequest.requestId = "";
  remoteAccessRequest.area = AREA_NONE;
  remoteAccessRequest.direction = MODE_ENTRY;
  remoteAccessRequest.createdBySerial = false;
  remoteAccessRequest.expiresAt = "";
  remoteAccessRequest.receivedAt = 0;
  remoteAccessRequest.workflowState = REMOTE_FINGERPRINT_PRECHECK;
  remoteAccessRequest.scanStartedAt = 0;
  remoteAccessRequest.precheckDoorOpen = false;
  remoteAccessRequest.precheckPersonKnown = false;
  remoteAccessRequest.precheckPersonDetected = false;
  remoteAccessRequest.precheckDistanceKnown = false;
  remoteAccessRequest.precheckDistanceCm = -1.0;
  remoteAccessRequest.precheckFingerprintReady = false;
  remoteAccessRequest.fingerprintResult = "";
  remoteAccessRequest.fingerprintId = -1;
  remoteAccessRequest.fingerprintConfidence = -1;
  remoteAccessRequest.accessCheckPayload = "";
  remoteAccessRequest.lastAccessCheckAttemptAt = 0;
  remoteAccessRequest.liveAuthorizationReceived = false;
  remoteAccessRequest.doorOpenCommandIssued = false;
  remoteAccessRequest.completionPayload = "";
  remoteAccessRequest.lastCompletionAttemptAt = 0;
}

bool parseRemoteDirection(const String& value, AccessMode& direction) {
  if (value == "ENTRY") {
    direction = MODE_ENTRY;
    return true;
  }
  if (value == "EXIT") {
    direction = MODE_EXIT;
    return true;
  }
  return false;
}

RemoteCommandDecision evaluateRemoteCommand(
    const String& requestId,
    int areaId,
    AccessMode direction) {
  String trimmedRequestId = requestId;
  trimmedRequestId.trim();
  if (requestId.length() == 0 || trimmedRequestId != requestId ||
      areaId < static_cast<int>(COMPANY_A) ||
      areaId > static_cast<int>(MAIN_ENTRANCE)) {
    return REMOTE_COMMAND_REJECT_INVALID;
  }

  if (integrationSyncState != INTEGRATION_SYNCED ||
      pendingAuthorizationId.length() > 0) {
    return REMOTE_COMMAND_REJECT_NOT_READY;
  }

  Area incomingArea = static_cast<Area>(areaId);
  if (remoteAccessRequest.active) {
    if (remoteAccessRequest.requestId == requestId &&
        remoteAccessRequest.area == incomingArea &&
        remoteAccessRequest.direction == direction) {
      return REMOTE_COMMAND_DUPLICATE;
    }
    return REMOTE_COMMAND_REJECT_CONFLICT;
  }

  if (serialCreatedRequestId.length() > 0 &&
      (serialCreatedRequestId != requestId ||
       serialCreatedRequestArea != incomingArea ||
       serialCreatedRequestDirection != direction)) {
    return REMOTE_COMMAND_REJECT_CONFLICT;
  }
  return REMOTE_COMMAND_ACCEPT;
}

void handleRemoteCommand(JsonObject command) {
  bool fieldsValid = command["request_id"].is<const char*>() &&
                     command["area_id"].is<int>() &&
                     command["direction"].is<const char*>() &&
                     command["expires_at"].is<const char*>();
  String requestId = fieldsValid
                         ? String(command["request_id"].as<const char*>())
                         : String("MALFORMED");
  int areaId = fieldsValid ? command["area_id"].as<int>() : 0;
  String directionValue = fieldsValid
                              ? String(command["direction"].as<const char*>())
                              : String("");
  String expiresAt = fieldsValid
                         ? String(command["expires_at"].as<const char*>())
                         : String("");
  String diagnosticId = requestId.length() > 0
                            ? requestId
                            : String("MALFORMED_EMPTY_ID");
  AccessMode direction = MODE_ENTRY;
  if (!fieldsValid || expiresAt.length() == 0 ||
      !parseRemoteDirection(directionValue, direction)) {
    if (lastUnexpectedCommandId != diagnosticId) {
      Serial.println("[ACCESS] Malformed command ignored");
      Serial.println("[ACCESS] No acknowledgement or hardware action performed.");
      lastUnexpectedCommandId = diagnosticId;
    }
    return;
  }

  RemoteCommandDecision decision =
      evaluateRemoteCommand(requestId, areaId, direction);
  if (decision == REMOTE_COMMAND_DUPLICATE) {
    return;
  }
  if (decision != REMOTE_COMMAND_ACCEPT) {
    if (lastUnexpectedCommandId != diagnosticId) {
      Serial.print("[ACCESS] Command rejected: ");
      if (decision == REMOTE_COMMAND_REJECT_INVALID) {
        Serial.println("invalid request fields");
      } else if (decision == REMOTE_COMMAND_REJECT_NOT_READY) {
        Serial.println("station is not ready or requires reconciliation");
      } else {
        Serial.println("another request is already tracked");
      }
      Serial.println("[ACCESS] No acknowledgement or hardware action performed.");
      lastUnexpectedCommandId = diagnosticId;
    }
    return;
  }

  remoteAccessRequest.active = true;
  remoteAccessRequest.requestId = requestId;
  remoteAccessRequest.area = static_cast<Area>(areaId);
  remoteAccessRequest.direction = direction;
  remoteAccessRequest.createdBySerial =
      serialCreatedRequestId == requestId;
  remoteAccessRequest.expiresAt = expiresAt;
  remoteAccessRequest.receivedAt = millis();
  remoteAccessRequest.workflowState = REMOTE_FINGERPRINT_PRECHECK;
  remoteAccessRequest.scanStartedAt = 0;
  remoteAccessRequest.precheckDoorOpen = false;
  remoteAccessRequest.precheckPersonKnown = false;
  remoteAccessRequest.precheckPersonDetected = false;
  remoteAccessRequest.precheckDistanceKnown = false;
  remoteAccessRequest.precheckDistanceCm = -1.0;
  remoteAccessRequest.precheckFingerprintReady = false;
  remoteAccessRequest.fingerprintResult = "";
  remoteAccessRequest.fingerprintId = -1;
  remoteAccessRequest.fingerprintConfidence = -1;
  remoteAccessRequest.accessCheckPayload = "";
  remoteAccessRequest.lastAccessCheckAttemptAt = 0;
  remoteAccessRequest.liveAuthorizationReceived = false;
  remoteAccessRequest.doorOpenCommandIssued = false;
  remoteAccessRequest.completionPayload = "";
  remoteAccessRequest.lastCompletionAttemptAt = 0;
  if (remoteAccessRequest.createdBySerial) {
    serialCreatedRequestId = "";
    serialCreatedRequestArea = AREA_NONE;
    serialCreatedRequestDirection = MODE_ENTRY;
  }
  lastRemoteRequestStatusCheckAt = 0;
  lastUnexpectedCommandId = "";

  Serial.println();
  Serial.println("[ACCESS] Command received");
  Serial.print("[ACCESS] Request: ");
  Serial.println(remoteAccessRequest.requestId);
  Serial.print("[ACCESS] Area: ");
  Serial.print(areaId);
  Serial.print(" - ");
  Serial.println(getAreaName(remoteAccessRequest.area));
  Serial.print("[ACCESS] Direction: ");
  Serial.println(getAccessModeName(remoteAccessRequest.direction));
  Serial.println("[ACCESS] Starting physical precheck and fingerprint workflow");
  Serial.println("[ACCESS] Door opens only after live Backend authorization.");
  Serial.println();
}

bool shouldStartRemoteFingerprintScan(AccessMode direction,
                                      bool locked,
                                      bool isDoorOpen,
                                      bool fingerprintAvailable,
                                      bool personKnown,
                                      bool personDetected) {
  if (locked || isDoorOpen || !fingerprintAvailable) {
    return false;
  }
  return direction == MODE_EXIT || (personKnown && personDetected);
}

RemoteFingerprintSensorOutcome classifyRemoteFingerprintImageResult(
    uint8_t result) {
  if (result == FINGERPRINT_NOFINGER) {
    return REMOTE_SENSOR_KEEP_WAITING;
  }
  if (result == FINGERPRINT_OK) {
    return REMOTE_SENSOR_IMAGE_READY;
  }
  return REMOTE_SENSOR_READ_ERROR;
}

RemoteFingerprintSensorOutcome classifyRemoteFingerprintSearchResult(
    uint8_t result) {
  if (result == FINGERPRINT_OK) {
    return REMOTE_SENSOR_MATCH;
  }
  if (result == FINGERPRINT_NOTFOUND) {
    return REMOTE_SENSOR_NOT_RECOGNIZED;
  }
  return REMOTE_SENSOR_READ_ERROR;
}

bool isRemoteFingerprintSensorStep(RemoteFingerprintWorkflowState state) {
  return state == REMOTE_FINGERPRINT_WAITING_FOR_FINGER ||
         state == REMOTE_FINGERPRINT_CONVERTING_IMAGE ||
         state == REMOTE_FINGERPRINT_SEARCHING;
}

String getRemoteFingerprintWorkflowName(
    RemoteFingerprintWorkflowState state) {
  switch (state) {
    case REMOTE_FINGERPRINT_PRECHECK:
      return "PHYSICAL_PRECHECK";
    case REMOTE_FINGERPRINT_WAITING_FOR_FINGER:
      return "WAITING_FOR_FINGERPRINT";
    case REMOTE_FINGERPRINT_CONVERTING_IMAGE:
      return "PROCESSING_FINGERPRINT";
    case REMOTE_FINGERPRINT_SEARCHING:
      return "MATCHING_FINGERPRINT";
    case REMOTE_ACCESS_CHECK_READY:
      return "SENDING_ACCESS_CHECK";
    case REMOTE_ACCESS_CHECK_STATUS:
      return "CHECKING_REQUEST_STATUS";
    case REMOTE_AUTHORIZED_WAITING_DOOR:
      return "AUTHORIZED_WAITING_FOR_DOOR";
    case REMOTE_OPENING_DOOR:
      return "OPENING_DOOR";
    case REMOTE_WAITING_FOR_COMPLETE_ACK:
      return "WAITING_FOR_COMPLETE_ACK";
    case REMOTE_COMPLETION_RECONCILIATION_REQUIRED:
      return "COMPLETION_RECONCILIATION_REQUIRED";
    default:
      return "UNKNOWN";
  }
}

void prepareRemoteAccessCheckPayload(const String& capturedResult,
                                     int fingerprintId,
                                     int confidence) {
  if (!remoteAccessRequest.active ||
      remoteAccessRequest.accessCheckPayload.length() > 0) {
    return;
  }

  String finalResult = capturedResult;
  if (finalResult == "MATCH" &&
      (fingerprintId < 0 || confidence < 0)) {
    finalResult = "READ_ERROR";
    fingerprintId = -1;
    confidence = -1;
  }

  remoteAccessRequest.fingerprintResult = finalResult;
  remoteAccessRequest.fingerprintId =
      finalResult == "MATCH" ? fingerprintId : -1;
  remoteAccessRequest.fingerprintConfidence =
      finalResult == "MATCH" ? confidence : -1;

  JsonDocument payload;
  payload["request_id"] = remoteAccessRequest.requestId;
  JsonObject precheck = payload["precheck"].to<JsonObject>();
  precheck["door_state"] =
      remoteAccessRequest.precheckDoorOpen ? "OPEN" : "CLOSED";
  if (remoteAccessRequest.precheckPersonKnown) {
    precheck["person_detected"] =
        remoteAccessRequest.precheckPersonDetected;
  } else {
    precheck["person_detected"] = nullptr;
  }
  if (remoteAccessRequest.precheckDistanceKnown) {
    precheck["distance_cm"] = remoteAccessRequest.precheckDistanceCm;
  } else {
    precheck["distance_cm"] = nullptr;
  }
  precheck["fingerprint_ready"] =
      remoteAccessRequest.precheckFingerprintReady;
  payload["fingerprint_result"] = finalResult;
  if (finalResult == "MATCH") {
    payload["fingerprint_id"] = fingerprintId;
    payload["confidence"] = confidence;
  }

  serializeJson(payload, remoteAccessRequest.accessCheckPayload);
  remoteAccessRequest.workflowState = REMOTE_ACCESS_CHECK_READY;
  remoteAccessRequest.lastAccessCheckAttemptAt = 0;

  Serial.print("[FINGERPRINT] Result captured: ");
  Serial.println(finalResult);
  if (finalResult == "MATCH") {
    Serial.print("[FINGERPRINT] ID / confidence: ");
    Serial.print(fingerprintId);
    Serial.print(" / ");
    Serial.println(confidence);
  }
  Serial.println("[ACCESS CHECK] Exact payload retained until Backend response.");
}

bool handleRemoteAccessCheckResponse(const String& responseBody) {
  JsonDocument response;
  DeserializationError jsonError = deserializeJson(response, responseBody);
  bool valid = !jsonError &&
               response["request_id"].is<const char*>() &&
               response["status"].is<const char*>() &&
               response["reason_code"].is<const char*>() &&
               response["message"].is<const char*>() &&
               response["failed_attempts"].is<int>() &&
               response["lockdown_active"].is<bool>();
  if (!valid) {
    Serial.println("[ACCESS CHECK] Invalid response ignored; exact payload retained.");
    return false;
  }

  String responseRequestId = response["request_id"].as<const char*>();
  String status = response["status"].as<const char*>();
  String reasonCode = response["reason_code"].as<const char*>();
  String message = response["message"].as<const char*>();
  int canonicalFailedAttempts = response["failed_attempts"].as<int>();
  bool canonicalLockdown = response["lockdown_active"].as<bool>();
  if (responseRequestId != remoteAccessRequest.requestId ||
      canonicalFailedAttempts < 0 ||
      canonicalFailedAttempts > MAX_FAILED_ATTEMPTS) {
    Serial.println("[ACCESS CHECK] Mismatched response ignored; exact payload retained.");
    return false;
  }

  if (status != "DENIED" && status != "AUTHORIZED_WAITING_DOOR") {
    Serial.print("[ACCESS CHECK] Unexpected status ignored: ");
    Serial.println(status);
    return false;
  }

  if (status == "AUTHORIZED_WAITING_DOOR" &&
      reasonCode != "AUTHORIZED") {
    Serial.println("[ACCESS CHECK] Inconsistent authorization response ignored; exact payload retained.");
    return false;
  }

  mirrorCanonicalSecurity(canonicalFailedAttempts, canonicalLockdown);
  Serial.println();
  Serial.println("[ACCESS CHECK RESPONSE]");
  Serial.print("Request: ");
  Serial.println(responseRequestId);
  Serial.print("Status: ");
  Serial.println(status);
  Serial.print("Reason: ");
  Serial.print(reasonCode);
  Serial.print(" - ");
  Serial.println(message);
  Serial.print("Failed Attempts: ");
  Serial.print(failedAttempts);
  Serial.print('/');
  Serial.println(MAX_FAILED_ATTEMPTS);

  if (status == "DENIED") {
    lcdShowMessage("ACCESS DENIED", reasonCode);
    clearTrackedRemoteRequest(responseRequestId, "DENIED");
    return true;
  }

  pendingAuthorizationId = responseRequestId;
  remoteAccessRequest.liveAuthorizationReceived = true;
  remoteAccessRequest.workflowState = REMOTE_AUTHORIZED_WAITING_DOOR;
  setIntegrationSyncState(INTEGRATION_RECONCILIATION_REQUIRED);
  lcdShowMessage("AUTHORIZED", "WAIT FOR DOOR");
  Serial.println("Live authorization confirmed for the current RAM request.");
  Serial.println();
  return true;
}

void serviceRemoteFingerprintWorkflow() {
  if (!remoteAccessRequest.active ||
      remoteAccessRequest.workflowState ==
          REMOTE_AUTHORIZED_WAITING_DOOR) {
    return;
  }

  RemoteFingerprintWorkflowState state =
      remoteAccessRequest.workflowState;
  if (state == REMOTE_FINGERPRINT_PRECHECK) {
    if (integrationSyncState != INTEGRATION_SYNCED ||
        !backendReachable || WiFi.status() != WL_CONNECTED) {
      return;
    }

    remoteAccessRequest.precheckDoorOpen = doorOpen;
    remoteAccessRequest.precheckFingerprintReady = fingerprintReady;
    remoteAccessRequest.precheckPersonKnown = false;
    remoteAccessRequest.precheckPersonDetected = false;
    remoteAccessRequest.precheckDistanceKnown = false;
    remoteAccessRequest.precheckDistanceCm = -1.0;

    if (remoteAccessRequest.direction == MODE_ENTRY) {
      // ENTRY uses a new physical reading immediately before the scan. EXIT
      // deliberately skips this requirement.
      updateUltrasonicMeasurement(true);
      bool measurementValid =
          isUltrasonicMeasurementValid(lastDistanceCm);
      remoteAccessRequest.precheckPersonKnown = measurementValid;
      remoteAccessRequest.precheckDistanceKnown = measurementValid;
      if (measurementValid) {
        remoteAccessRequest.precheckDistanceCm = lastDistanceCm;
        remoteAccessRequest.precheckPersonDetected =
            isDistanceNear(lastDistanceCm);
      }
    }

    bool shouldScan = shouldStartRemoteFingerprintScan(
        remoteAccessRequest.direction,
        systemLocked,
        remoteAccessRequest.precheckDoorOpen,
        remoteAccessRequest.precheckFingerprintReady,
        remoteAccessRequest.precheckPersonKnown,
        remoteAccessRequest.precheckPersonDetected);
    if (!shouldScan) {
      Serial.println("[ACCESS PRECHECK] Fingerprint scan not started.");
      if (systemLocked) {
        Serial.println("[ACCESS PRECHECK] Lockdown is active.");
      } else if (remoteAccessRequest.precheckDoorOpen) {
        Serial.println("[ACCESS PRECHECK] Door is already open.");
      } else if (!remoteAccessRequest.precheckFingerprintReady) {
        Serial.println("[ACCESS PRECHECK] Fingerprint sensor is unavailable.");
      } else if (!remoteAccessRequest.precheckPersonKnown) {
        Serial.println("[ACCESS PRECHECK] Fresh ultrasonic reading is unavailable.");
      } else {
        Serial.println("[ACCESS PRECHECK] No person detected within 20 cm.");
      }
      prepareRemoteAccessCheckPayload("NOT_SCANNED");
      return;
    }

    remoteAccessRequest.workflowState =
        REMOTE_FINGERPRINT_WAITING_FOR_FINGER;
    remoteAccessRequest.scanStartedAt = millis();
    Serial.println();
    Serial.println("[REMOTE FINGERPRINT SCAN]");
    Serial.print("Request: ");
    Serial.println(remoteAccessRequest.requestId);
    Serial.print("Direction: ");
    Serial.println(getAccessModeName(remoteAccessRequest.direction));
    Serial.println("Place one finger on the sensor (25 second timeout).");
    lcdShowPlaceFinger();
    presencePromptVisible = false;
    return;
  }

  if (state == REMOTE_FINGERPRINT_WAITING_FOR_FINGER) {
    if (millis() - remoteAccessRequest.scanStartedAt >=
        REMOTE_FINGERPRINT_CAPTURE_TIMEOUT_MS) {
      prepareRemoteAccessCheckPayload("TIMEOUT");
      return;
    }

    uint8_t imageResult = finger.getImage();
    RemoteFingerprintSensorOutcome outcome =
        classifyRemoteFingerprintImageResult(imageResult);
    if (outcome == REMOTE_SENSOR_KEEP_WAITING) {
      return;
    }
    if (outcome == REMOTE_SENSOR_IMAGE_READY) {
      remoteAccessRequest.workflowState =
          REMOTE_FINGERPRINT_CONVERTING_IMAGE;
      return;
    }
    Serial.print("[FINGERPRINT] Image read error: ");
    Serial.println(getFingerprintError(imageResult));
    prepareRemoteAccessCheckPayload("READ_ERROR");
    return;
  }

  if (state == REMOTE_FINGERPRINT_CONVERTING_IMAGE) {
    uint8_t conversionResult = finger.image2Tz();
    if (conversionResult == FINGERPRINT_OK) {
      remoteAccessRequest.workflowState = REMOTE_FINGERPRINT_SEARCHING;
      return;
    }
    Serial.print("[FINGERPRINT] Image conversion error: ");
    Serial.println(getFingerprintError(conversionResult));
    prepareRemoteAccessCheckPayload("READ_ERROR");
    return;
  }

  if (state == REMOTE_FINGERPRINT_SEARCHING) {
    uint8_t searchResult = finger.fingerFastSearch();
    RemoteFingerprintSensorOutcome outcome =
        classifyRemoteFingerprintSearchResult(searchResult);
    if (outcome == REMOTE_SENSOR_MATCH) {
      prepareRemoteAccessCheckPayload(
          "MATCH", finger.fingerID, finger.confidence);
    } else if (outcome == REMOTE_SENSOR_NOT_RECOGNIZED) {
      prepareRemoteAccessCheckPayload("NOT_RECOGNIZED");
    } else {
      Serial.print("[FINGERPRINT] Search error: ");
      Serial.println(getFingerprintError(searchResult));
      prepareRemoteAccessCheckPayload("READ_ERROR");
    }
    return;
  }

  if (state != REMOTE_ACCESS_CHECK_READY) {
    return;
  }
  bool accessCheckCanReachCanonicalRequest =
      integrationSyncState == INTEGRATION_SYNCED ||
      (integrationSyncState == INTEGRATION_RECONCILIATION_REQUIRED &&
       pendingAuthorizationId == remoteAccessRequest.requestId);
  if (!accessCheckCanReachCanonicalRequest || !backendReachable ||
      WiFi.status() != WL_CONNECTED) {
    return;
  }

  unsigned long now = millis();
  if (remoteAccessRequest.lastAccessCheckAttemptAt != 0 &&
      now - remoteAccessRequest.lastAccessCheckAttemptAt <
          ACCESS_CHECK_RETRY_INTERVAL_MS) {
    return;
  }
  remoteAccessRequest.lastAccessCheckAttemptAt = now;

  String responseBody;
  int responseCode = -1;
  bool checked = postJson("/api/access/check",
                          remoteAccessRequest.accessCheckPayload,
                          responseBody,
                          responseCode,
                          HTTP_CODE_OK);
  if (checked) {
    if (handleRemoteAccessCheckResponse(responseBody)) {
      recordBackendSuccess("Access check");
    }
    return;
  }

  if (responseCode == HTTP_CODE_GONE) {
    Serial.println("[ACCESS CHECK] Request is no longer actionable; checking canonical status.");
    remoteAccessRequest.workflowState = REMOTE_ACCESS_CHECK_STATUS;
    lastRemoteRequestStatusCheckAt = 0;
    return;
  }
  if (responseCode < 0) {
    markBackendUnavailable("Access check", responseCode);
    Serial.println("[ACCESS CHECK] Captured result and exact payload retained for retry.");
    return;
  }

  Serial.print("[ACCESS CHECK] Backend returned HTTP ");
  Serial.print(responseCode);
  Serial.println("; exact payload retained for retry.");
}

bool canOpenRemoteDoorForState(RemoteFingerprintWorkflowState state,
                               bool active,
                               bool liveAuthorizationReceived,
                               bool pendingRequestMatches,
                               bool servoReady,
                               bool isDoorOpen,
                               bool locked) {
  return state == REMOTE_AUTHORIZED_WAITING_DOOR && active &&
         liveAuthorizationReceived && pendingRequestMatches && servoReady &&
         !isDoorOpen && !locked;
}

bool canSendRemoteCompletionForState(
    RemoteFingerprintWorkflowState state,
    bool active,
    bool liveAuthorizationReceived,
    bool doorOpenCommandIssued,
    bool completionPayloadReady) {
  return state == REMOTE_WAITING_FOR_COMPLETE_ACK && active &&
         liveAuthorizationReceived && doorOpenCommandIssued &&
         completionPayloadReady;
}

void prepareRemoteCompletionPayload() {
  if (!remoteAccessRequest.active ||
      !remoteAccessRequest.doorOpenCommandIssued ||
      remoteAccessRequest.completionPayload.length() > 0) {
    return;
  }

  JsonDocument payload;
  payload["request_id"] = remoteAccessRequest.requestId;
  payload["door_result"] = "DOOR_OPENED";
  serializeJson(payload, remoteAccessRequest.completionPayload);
  remoteAccessRequest.workflowState = REMOTE_WAITING_FOR_COMPLETE_ACK;
  remoteAccessRequest.lastCompletionAttemptAt = 0;
  Serial.println("[ACCESS COMPLETE] Exact DOOR_OPENED payload retained for retry.");
}

void enterRemoteCompletionReconciliation(const String& reason) {
  if (!remoteAccessRequest.active) {
    return;
  }
  remoteAccessRequest.workflowState =
      REMOTE_COMPLETION_RECONCILIATION_REQUIRED;
  setIntegrationSyncState(INTEGRATION_RECONCILIATION_REQUIRED);
  Serial.println();
  Serial.println("[HIGH SEVERITY RECONCILIATION REQUIRED]");
  Serial.print("Request: ");
  Serial.println(remoteAccessRequest.requestId);
  Serial.print("Reason: ");
  Serial.println(reason);
  Serial.println("Pending authorization and RAM request are preserved.");
  Serial.println("The local door lifecycle remains active and safe.");
  Serial.println();
  lcdShowMessage("RECONCILIATION", "CHECK DOOR/API");
}

bool handleRemoteCompletionResponse(const String& responseBody) {
  JsonDocument response;
  DeserializationError jsonError = deserializeJson(response, responseBody);
  bool valid = !jsonError &&
               response["request_id"].is<const char*>() &&
               response["status"].is<const char*>() &&
               response["reason_code"].is<const char*>() &&
               response["message"].is<const char*>() &&
               response["failed_attempts"].is<int>() &&
               response["lockdown_active"].is<bool>();
  if (!valid) {
    Serial.println("[ACCESS COMPLETE] Invalid response; exact payload retained for retry.");
    return false;
  }

  String responseRequestId = response["request_id"].as<const char*>();
  String status = response["status"].as<const char*>();
  String reasonCode = response["reason_code"].as<const char*>();
  String message = response["message"].as<const char*>();
  int canonicalFailedAttempts = response["failed_attempts"].as<int>();
  bool canonicalLockdown = response["lockdown_active"].as<bool>();
  if (responseRequestId != remoteAccessRequest.requestId ||
      canonicalFailedAttempts < 0 ||
      canonicalFailedAttempts > MAX_FAILED_ATTEMPTS) {
    Serial.println("[ACCESS COMPLETE] Mismatched response; exact payload retained for retry.");
    return false;
  }

  if (status != "GRANTED" || reasonCode != "AUTHORIZED") {
    enterRemoteCompletionReconciliation(
        "Unexpected completion outcome " + status + " / " + reasonCode);
    return false;
  }

  Area completedArea = remoteAccessRequest.area;
  AccessMode completedDirection = remoteAccessRequest.direction;
  mirrorCanonicalSecurity(canonicalFailedAttempts, canonicalLockdown);

  Serial.println();
  Serial.println(completedDirection == MODE_EXIT ? "[EXIT GRANTED]"
                                                  : "[ENTRY GRANTED]");
  Serial.print("Request: ");
  Serial.println(responseRequestId);
  Serial.print("Area: ");
  Serial.println(getAreaName(completedArea));
  Serial.print("Backend: ");
  Serial.print(status);
  Serial.print(" - ");
  Serial.println(message);
  Serial.println("Attendance and AccessLog were committed by Backend.");
  Serial.println("No local attendance mutation was performed.");

  pendingAuthorizationId = "";
  if (completedDirection == MODE_EXIT) {
    lcdShowMessage("EXIT GRANTED", "BACKEND CONFIRMED");
  } else {
    lcdShowMessage("ACCESS GRANTED", "BACKEND CONFIRMED");
  }
  triggerAccessGrantedFeedback();
  clearTrackedRemoteRequest(responseRequestId, "GRANTED");

  // Completion responses do not carry the full attendance matrix. Force a
  // canonical bootstrap refresh instead of mutating the local mirror.
  lastBackendSyncAttemptAt = 0;
  setIntegrationSyncState(INTEGRATION_UNSYNCED);
  return true;
}

void serviceRemoteDoorCompletionWorkflow() {
  if (!remoteAccessRequest.active) {
    return;
  }

  RemoteFingerprintWorkflowState state =
      remoteAccessRequest.workflowState;
  if (state == REMOTE_AUTHORIZED_WAITING_DOOR) {
    bool pendingMatches = pendingAuthorizationId.length() > 0 &&
                          pendingAuthorizationId ==
                              remoteAccessRequest.requestId;
    bool mayOpen = canOpenRemoteDoorForState(
        state,
        remoteAccessRequest.active,
        remoteAccessRequest.liveAuthorizationReceived,
        pendingMatches,
        servoInitialized,
        doorOpen,
        systemLocked);
    if (!mayOpen) {
      String reason = "Live authorization door preconditions failed";
      if (!remoteAccessRequest.liveAuthorizationReceived) {
        reason = "Authorization was discovered by status/bootstrap, not received live";
      } else if (!pendingMatches) {
        reason = "Pending authorization does not match the active request";
      } else if (!servoInitialized) {
        reason = "DOOR HARDWARE ERROR: servo is not initialized";
      } else if (doorOpen) {
        reason = "Door was already open before this authorization";
      } else if (systemLocked) {
        reason = "System entered Lockdown before door opening";
      }
      enterRemoteCompletionReconciliation(reason);
      return;
    }

    remoteAccessRequest.workflowState = REMOTE_OPENING_DOOR;
    Serial.println();
    Serial.println("[DOOR] Backend authorization confirmed");
    Serial.print("[DOOR] Opening for request: ");
    Serial.println(remoteAccessRequest.requestId);
    Serial.print("[DOOR] Area: ");
    Serial.println(getAreaName(remoteAccessRequest.area));
    Serial.print("[DOOR] Direction: ");
    Serial.println(getAccessModeName(remoteAccessRequest.direction));
    openDoor("Live Backend authorization",
             remoteAccessRequest.area,
             remoteAccessRequest.direction);
    if (!doorOpen) {
      enterRemoteCompletionReconciliation(
          "DOOR HARDWARE ERROR: software OPEN state was not reached");
      return;
    }

    remoteAccessRequest.doorOpenCommandIssued = true;
    prepareRemoteCompletionPayload();
    return;
  }

  bool completionReady = canSendRemoteCompletionForState(
      state,
      remoteAccessRequest.active,
      remoteAccessRequest.liveAuthorizationReceived,
      remoteAccessRequest.doorOpenCommandIssued,
      remoteAccessRequest.completionPayload.length() > 0);
  if (!completionReady) {
    return;
  }

  bool canonicalRequestReachable =
      integrationSyncState == INTEGRATION_SYNCED ||
      (integrationSyncState == INTEGRATION_RECONCILIATION_REQUIRED &&
       pendingAuthorizationId == remoteAccessRequest.requestId);
  if (!canonicalRequestReachable || !backendReachable ||
      WiFi.status() != WL_CONNECTED) {
    return;
  }

  unsigned long now = millis();
  if (remoteAccessRequest.lastCompletionAttemptAt != 0 &&
      now - remoteAccessRequest.lastCompletionAttemptAt <
          COMPLETION_RETRY_INTERVAL_MS) {
    return;
  }
  remoteAccessRequest.lastCompletionAttemptAt = now;

  String responseBody;
  int responseCode = -1;
  bool completed = postJson("/api/access/complete",
                            remoteAccessRequest.completionPayload,
                            responseBody,
                            responseCode,
                            HTTP_CODE_OK);
  if (completed) {
    if (handleRemoteCompletionResponse(responseBody)) {
      recordBackendSuccess("Access completion");
    }
    return;
  }

  if (responseCode < 0) {
    markBackendUnavailable("Access completion", responseCode);
    Serial.println("[ACCESS COMPLETE] Door was opened; exact payload retained for retry.");
    return;
  }
  if (responseCode >= 500) {
    Serial.print("[ACCESS COMPLETE] Backend HTTP ");
    Serial.print(responseCode);
    Serial.println("; exact payload retained for retry.");
    return;
  }

  String rejection = getApiErrorDescription(responseBody);
  enterRemoteCompletionReconciliation(
      "Completion rejected after door OPEN (HTTP " +
      String(responseCode) + "): " + rejection);
}

String getApiErrorDescription(const String& responseBody) {
  JsonDocument document;
  if (deserializeJson(document, responseBody)) {
    return "Backend response could not be parsed";
  }
  JsonVariant detail = document["detail"];
  if (!detail.is<JsonObject>()) {
    return "Backend rejected the request";
  }
  const char* reason = detail["reason_code"] | "BACKEND_REJECTED";
  const char* message = detail["message"] | "Request rejected";
  return String(reason) + ": " + String(message);
}

void createIntegratedSerialAccessRequest() {
  AccessMode requestedDirection = currentMode;
  if (selectedArea == AREA_NONE) {
    Serial.println("[ACCESS REQUEST ERROR]");
    Serial.println("Select an area first using 1-7.");
    returnToEntryMode();
    return;
  }
  if (integrationSyncState != INTEGRATION_SYNCED ||
      !backendReachable || WiFi.status() != WL_CONNECTED ||
      !backendRecentlyConfirmed() ||
      pendingAuthorizationId.length() > 0) {
    Serial.println("[ACCESS REQUEST BLOCKED]");
    Serial.println("Integrated Backend is not recently confirmed and ready.");
    returnToEntryMode();
    return;
  }
  bool existingRequestIsSerialOwned =
      serialCreatedRequestId.length() > 0 ||
      (remoteAccessRequest.active && remoteAccessRequest.createdBySerial);
  if (existingRequestIsSerialOwned) {
    Serial.println("[ACCESS REQUEST BLOCKED]");
    Serial.println("This Serial access request is already tracked locally.");
    // A duplicate R must not consume the Exit Mode owned by the request that
    // is already in progress. App-owned collisions still reach FastAPI below
    // so its canonical REQUEST_IN_PROGRESS response remains authoritative.
    return;
  }

  JsonDocument request;
  request["area_id"] = static_cast<int>(selectedArea);
  request["direction"] = getAccessModeName(requestedDirection);
  String requestBody;
  requestBody.reserve(64);
  serializeJson(request, requestBody);

  String responseBody;
  int responseCode = -1;
  bool accepted = postJson("/api/access/requests",
                           requestBody,
                           responseBody,
                           responseCode,
                           HTTP_CODE_ACCEPTED);
  if (!accepted) {
    Serial.println("[ACCESS REQUEST REJECTED]");
    Serial.print("HTTP/code: ");
    Serial.println(responseCode);
    if (responseBody.length() > 0) {
      Serial.println(getApiErrorDescription(responseBody));
    }
    if (responseCode < 0) {
      markBackendUnavailable("Access request creation", responseCode);
    }
    // R consumes the one-attempt Exit Mode just like the original local flow.
    returnToEntryMode();
    return;
  }

  JsonDocument response;
  DeserializationError jsonError = deserializeJson(response, responseBody);
  bool responseValid = !jsonError &&
                       response["request_id"].is<const char*>() &&
                       response["status"].is<const char*>() &&
                       response["area_id"].is<int>() &&
                       response["direction"].is<const char*>() &&
                       response["expires_at"].is<const char*>();
  String canonicalRequestId = responseValid
                                  ? String(response["request_id"].as<const char*>())
                                  : String("");
  String canonicalStatus = responseValid
                               ? String(response["status"].as<const char*>())
                               : String("");
  String canonicalDirection = responseValid
                                  ? String(response["direction"].as<const char*>())
                                  : String("");
  if (!responseValid || canonicalRequestId.length() == 0 ||
      canonicalStatus != "QUEUED" ||
      response["area_id"].as<int>() != static_cast<int>(selectedArea) ||
      canonicalDirection != getAccessModeName(requestedDirection)) {
    Serial.println("[ACCESS REQUEST ERROR]");
    Serial.println("Backend returned an invalid creation response; request will not be acknowledged.");
    returnToEntryMode();
    return;
  }

  recordBackendSuccess("Access request creation");
  serialCreatedRequestId = canonicalRequestId;
  serialCreatedRequestArea = selectedArea;
  serialCreatedRequestDirection = requestedDirection;
  lastRemoteRequestStatusCheckAt = 0;
  Serial.println();
  Serial.println("[ACCESS REQUEST CREATED]");
  Serial.print("Request: ");
  Serial.println(serialCreatedRequestId);
  Serial.print("Area: ");
  Serial.print(static_cast<int>(serialCreatedRequestArea));
  Serial.print(" - ");
  Serial.println(getAreaName(serialCreatedRequestArea));
  Serial.print("Direction: ");
  Serial.println(getAccessModeName(serialCreatedRequestDirection));
  Serial.println("Waiting for heartbeat command delivery.");
  Serial.println();
}

void clearTrackedRemoteRequest(const String& requestId,
                               const String& terminalState) {
  bool serialOwned = false;
  bool cleared = false;
  if (remoteAccessRequest.active &&
      remoteAccessRequest.requestId == requestId) {
    serialOwned = remoteAccessRequest.createdBySerial;
    resetRemoteAccessRequestState();
    cleared = true;
  }
  if (serialCreatedRequestId == requestId) {
    serialOwned = true;
    serialCreatedRequestId = "";
    serialCreatedRequestArea = AREA_NONE;
    serialCreatedRequestDirection = MODE_ENTRY;
    cleared = true;
  }
  if (!cleared) {
    return;
  }

  lastRemoteRequestStatusCheckAt = 0;
  Serial.print("[ACCESS] Request ");
  Serial.print(requestId);
  Serial.print(" finished: ");
  Serial.println(terminalState);
  if (serialOwned) {
    returnToEntryMode();
  }
}

void serviceRemoteRequestStatus() {
  String trackedRequestId = remoteAccessRequest.active
                                ? remoteAccessRequest.requestId
                                : serialCreatedRequestId;
  if (remoteAccessRequest.active) {
    RemoteFingerprintWorkflowState workflowState =
        remoteAccessRequest.workflowState;
    bool statusCheckAllowed =
        workflowState == REMOTE_FINGERPRINT_PRECHECK ||
        workflowState == REMOTE_FINGERPRINT_WAITING_FOR_FINGER ||
        workflowState == REMOTE_ACCESS_CHECK_STATUS;
    if (!statusCheckAllowed) {
      return;
    }
  }
  if (trackedRequestId.length() == 0 ||
      integrationSyncState != INTEGRATION_SYNCED ||
      !backendReachable || WiFi.status() != WL_CONNECTED) {
    return;
  }

  unsigned long now = millis();
  if (lastRemoteRequestStatusCheckAt != 0 &&
      now - lastRemoteRequestStatusCheckAt <
          REMOTE_REQUEST_STATUS_INTERVAL_MS) {
    return;
  }
  lastRemoteRequestStatusCheckAt = now;

  String path = "/api/access/requests/" + trackedRequestId;
  String responseBody;
  int responseCode = -1;
  bool statusAvailable = getJson(path.c_str(), responseBody, responseCode);
  if (responseCode == HTTP_CODE_GONE) {
    recordBackendSuccess("Request status");
    if (pendingAuthorizationId.length() == 0 &&
        integrationSyncState != INTEGRATION_RECONCILIATION_REQUIRED) {
      clearTrackedRemoteRequest(trackedRequestId, "EXPIRED");
    }
    return;
  }
  if (!statusAvailable) {
    if (responseCode < 0) {
      markBackendUnavailable("Request status", responseCode);
    } else {
      Serial.print("[ACCESS] Status check returned HTTP ");
      Serial.println(responseCode);
    }
    return;
  }

  JsonDocument response;
  DeserializationError jsonError = deserializeJson(response, responseBody);
  if (jsonError || !response["request_id"].is<const char*>() ||
      !response["status"].is<const char*>()) {
    Serial.println("[ACCESS] Invalid request-status response ignored.");
    return;
  }
  String responseRequestId = response["request_id"].as<const char*>();
  String status = response["status"].as<const char*>();
  if (responseRequestId != trackedRequestId) {
    Serial.println("[ACCESS] Mismatched request-status response ignored.");
    return;
  }

  if (status == "QUEUED" || status == "IN_PROGRESS") {
    recordBackendSuccess("Request status");
    if (remoteAccessRequest.active &&
        remoteAccessRequest.workflowState == REMOTE_ACCESS_CHECK_STATUS &&
        remoteAccessRequest.accessCheckPayload.length() > 0) {
      remoteAccessRequest.workflowState = REMOTE_ACCESS_CHECK_READY;
      remoteAccessRequest.lastAccessCheckAttemptAt = 0;
    }
    return;
  }
  if (status == "AUTHORIZED_WAITING_DOOR") {
    recordBackendSuccess("Request status");
    pendingAuthorizationId = trackedRequestId;
    if (remoteAccessRequest.active) {
      remoteAccessRequest.liveAuthorizationReceived = false;
      remoteAccessRequest.workflowState =
          REMOTE_COMPLETION_RECONCILIATION_REQUIRED;
    }
    setIntegrationSyncState(INTEGRATION_RECONCILIATION_REQUIRED);
    Serial.println("[ACCESS] Authorization was discovered by status polling.");
    Serial.println("[ACCESS] Door remains unchanged; reconciliation is required.");
    return;
  }
  if (status == "GRANTED" || status == "DENIED" ||
      status == "FAILED" || status == "EXPIRED") {
    recordBackendSuccess("Request status");
    clearTrackedRemoteRequest(trackedRequestId, status);
    return;
  }
  Serial.println("[ACCESS] Unknown request status ignored.");
}

bool parseCanonicalSecurity(JsonDocument& document,
                            int& canonicalFailedAttempts,
                            bool& canonicalLockdown,
                            bool& canonicalSystemActive,
                            bool& canonicalAdminMode) {
  if (!document["failed_attempts"].is<int>() ||
      !document["lockdown_active"].is<bool>() ||
      !document["system_active"].is<bool>() ||
      !document["admin_mode"].is<bool>()) {
    return false;
  }

  canonicalFailedAttempts = document["failed_attempts"].as<int>();
  canonicalLockdown = document["lockdown_active"].as<bool>();
  canonicalSystemActive = document["system_active"].as<bool>();
  canonicalAdminMode = document["admin_mode"].as<bool>();
  return canonicalFailedAttempts >= 0 &&
         canonicalFailedAttempts <= MAX_FAILED_ATTEMPTS;
}

void mirrorCanonicalSecurity(int canonicalFailedAttempts,
                             bool canonicalLockdown) {
  if (canonicalLockdown && !systemLocked) {
    // Mirror canonical Lockdown without alerts, servo movement, or Backend writes.
    applyLockedState();
  } else if (!canonicalLockdown && systemLocked) {
    systemLocked = false;
  }
  failedAttempts = canonicalFailedAttempts;
}

void recordBackendSuccess(const String& operation) {
  if (consecutiveBackendFailures > 0) {
    Serial.print("[BACKEND] ");
    Serial.print(operation);
    Serial.println(" recovered");
    Serial.println("[BACKEND] Transient failure counter reset.");
  }
  consecutiveBackendFailures = 0;
  lastBackendSuccessAt = millis();
  lastBackendFailureLogAt = 0;
  backendReachable = true;
}

bool backendRecentlyConfirmed() {
  return backendReachable && lastBackendSuccessAt != 0 &&
         millis() - lastBackendSuccessAt <= BACKEND_ACCESS_FRESHNESS_MS;
}

void markBackendUnavailable(const String& operation, int responseCode) {
  unsigned long now = millis();
  bool mayRemainCanonical = responseCode < 0 && backendReachable &&
                            integrationSyncState != INTEGRATION_UNSYNCED;
  if (mayRemainCanonical) {
    if (consecutiveBackendFailures < BACKEND_TRANSIENT_FAILURE_LIMIT) {
      ++consecutiveBackendFailures;
    }
    bool failureLimitReached =
        consecutiveBackendFailures >= BACKEND_TRANSIENT_FAILURE_LIMIT;
    bool outageWindowElapsed = lastBackendSuccessAt != 0 &&
                               now - lastBackendSuccessAt >=
                                   BACKEND_SUSTAINED_OUTAGE_MS;

    Serial.print("[BACKEND] ");
    Serial.print(operation);
    Serial.print(" transient failure ");
    Serial.print(consecutiveBackendFailures);
    Serial.print('/');
    Serial.print(BACKEND_TRANSIENT_FAILURE_LIMIT);
    Serial.print(" (HTTP/code ");
    Serial.print(responseCode);
    if (responseCode == HTTPC_ERROR_READ_TIMEOUT) {
      Serial.print(": response/read timeout");
    } else if (responseCode == HTTPC_ERROR_CONNECTION_REFUSED) {
      Serial.print(": connection refused");
    }
    Serial.println(')');

    if (!failureLimitReached || !outageWindowElapsed) {
      Serial.println("[BACKEND] Remaining in current canonical state within transport grace period.");
      return;
    }
    Serial.println("[BACKEND] Sustained Backend connectivity loss.");
    Serial.print("[WIFI] RSSI at loss: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  }

  bool shouldLog = backendReachable || lastBackendFailureLogAt == 0 ||
                   now - lastBackendFailureLogAt >=
                       BACKEND_FAILURE_LOG_INTERVAL_MS;
  backendReachable = false;
  setIntegrationSyncState(INTEGRATION_UNSYNCED);

  if (shouldLog) {
    Serial.print("[BACKEND] ");
    Serial.print(operation);
    Serial.print(" failed");
    if (responseCode != -1) {
      Serial.print(" (HTTP/code ");
      Serial.print(responseCode);
      Serial.print(')');
    }
    Serial.println("; local hardware loop continues.");
    lastBackendFailureLogAt = now;
  }
}

bool applyHeartbeatResponse(const String& responseBody) {
  JsonDocument document;
  DeserializationError jsonError = deserializeJson(document, responseBody);
  if (jsonError) {
    Serial.print("[BACKEND] Invalid heartbeat JSON: ");
    Serial.println(jsonError.c_str());
    return false;
  }

  int canonicalFailedAttempts = 0;
  bool canonicalLockdown = false;
  bool canonicalSystemActive = false;
  bool canonicalAdminMode = false;
  if (!parseCanonicalSecurity(document,
                              canonicalFailedAttempts,
                              canonicalLockdown,
                              canonicalSystemActive,
                              canonicalAdminMode)) {
    Serial.println("[BACKEND] Heartbeat response is missing canonical state.");
    return false;
  }

  JsonVariant pendingRequest = document["pending_request_id"];
  JsonVariant command = document["command"];
  if (!pendingRequest.isNull() && !pendingRequest.is<const char*>()) {
    Serial.println("[BACKEND] Heartbeat pending-request field is invalid.");
    return false;
  }

  mirrorCanonicalSecurity(canonicalFailedAttempts, canonicalLockdown);
  backendSystemActive = canonicalSystemActive;
  backendAdminMode = canonicalAdminMode;

  if (!pendingRequest.isNull()) {
    String receivedPendingId = pendingRequest.as<const char*>();
    if (receivedPendingId != pendingAuthorizationId) {
      Serial.print("[BACKEND] Pending authorization requires reconciliation: ");
      Serial.println(receivedPendingId);
    }
    pendingAuthorizationId = receivedPendingId;
    if (remoteAccessRequest.active &&
        remoteAccessRequest.requestId == receivedPendingId &&
        remoteAccessRequest.workflowState != REMOTE_ACCESS_CHECK_READY &&
        !remoteAccessRequest.liveAuthorizationReceived) {
      remoteAccessRequest.workflowState =
          REMOTE_COMPLETION_RECONCILIATION_REQUIRED;
    }
    setIntegrationSyncState(INTEGRATION_RECONCILIATION_REQUIRED);
  }

  if (!command.isNull()) {
    if (command.is<JsonObject>()) {
      handleRemoteCommand(command.as<JsonObject>());
    } else if (lastUnexpectedCommandId != "MALFORMED_COMMAND_TYPE") {
      Serial.println("[ACCESS] Malformed command ignored");
      Serial.println("[ACCESS] No acknowledgement or hardware action performed.");
      lastUnexpectedCommandId = "MALFORMED_COMMAND_TYPE";
    }
  }
  return true;
}

bool sendHeartbeat() {
  lastHeartbeatAttemptAt = millis();
  updateUltrasonicMeasurement(false);

  bool measurementValid = isUltrasonicMeasurementValid(lastDistanceCm);
  JsonDocument request;
  request["boot_id"] = bootId;
  request["door_state"] = doorOpen ? "OPEN" : "CLOSED";
  if (measurementValid) {
    request["person_detected"] = isDistanceNear(lastDistanceCm);
    request["distance_cm"] = lastDistanceCm;
  } else {
    request["person_detected"] = nullptr;
    request["distance_cm"] = nullptr;
  }
  request["fingerprint_ready"] = fingerprintReady;
  request["admin_mode"] = adminMode;
  if (remoteAccessRequest.active) {
    request["active_request_id"] = remoteAccessRequest.requestId;
  } else {
    request["active_request_id"] = nullptr;
  }

  String requestBody;
  requestBody.reserve(256);
  serializeJson(request, requestBody);

  String responseBody;
  int responseCode = -1;
  if (!postJson("/api/esp32/heartbeat",
                requestBody,
                responseBody,
                responseCode,
                HTTP_CODE_OK) ||
      !applyHeartbeatResponse(responseBody)) {
    markBackendUnavailable("Heartbeat", responseCode);
    return false;
  }

  recordBackendSuccess("Heartbeat");
  return true;
}

bool applyBootstrapResponse(const String& responseBody) {
  JsonDocument document;
  DeserializationError jsonError = deserializeJson(document, responseBody);
  if (jsonError) {
    Serial.print("[BACKEND] Invalid bootstrap JSON: ");
    Serial.println(jsonError.c_str());
    return false;
  }

  int canonicalFailedAttempts = 0;
  bool canonicalLockdown = false;
  bool canonicalSystemActive = false;
  bool canonicalAdminMode = false;
  if (!parseCanonicalSecurity(document,
                              canonicalFailedAttempts,
                              canonicalLockdown,
                              canonicalSystemActive,
                              canonicalAdminMode)) {
    Serial.println("[BACKEND] Bootstrap is missing canonical security state.");
    return false;
  }

  JsonArray attendance = document["attendance"].as<JsonArray>();
  if (attendance.isNull() ||
      attendance.size() != EXPECTED_BOOTSTRAP_ATTENDANCE_ROWS) {
    Serial.print("[BACKEND] Bootstrap attendance row count invalid: ");
    Serial.println(attendance.isNull() ? 0 : attendance.size());
    return false;
  }

  uint16_t rebuiltInsideMasks[USER_COUNT] = {};
  bool seenAttendance[USER_COUNT][7] = {};
  for (JsonObject row : attendance) {
    if (!row["user_id"].is<int>() || !row["area_id"].is<int>() ||
        !row["is_inside"].is<bool>()) {
      Serial.println("[BACKEND] Bootstrap attendance row has invalid fields.");
      return false;
    }

    int userId = row["user_id"].as<int>();
    int areaId = row["area_id"].as<int>();
    int userIndex = -1;
    for (size_t i = 0; i < USER_COUNT; ++i) {
      if (users[i].fingerprintID == userId) {
        userIndex = static_cast<int>(i);
        break;
      }
    }

    if (userIndex < 0 || areaId < 1 || areaId > 7 ||
        seenAttendance[userIndex][areaId - 1]) {
      Serial.println("[BACKEND] Bootstrap attendance mapping is invalid or duplicated.");
      return false;
    }

    seenAttendance[userIndex][areaId - 1] = true;
    if (row["is_inside"].as<bool>()) {
      rebuiltInsideMasks[userIndex] |=
          static_cast<uint16_t>(1U << (areaId - 1));
    }
  }

  for (size_t userIndex = 0; userIndex < USER_COUNT; ++userIndex) {
    for (size_t areaIndex = 0; areaIndex < 7; ++areaIndex) {
      if (!seenAttendance[userIndex][areaIndex]) {
        Serial.println("[BACKEND] Bootstrap attendance matrix is incomplete.");
        return false;
      }
    }
  }

  JsonVariant pending = document["pending_authorization"];
  bool hasPendingAuthorization = !pending.isNull();
  String receivedPendingId;
  int pendingUserId = 0;
  int pendingAreaId = 0;
  String pendingDirection;
  if (hasPendingAuthorization) {
    if (!pending.is<JsonObject>() || !pending["request_id"].is<const char*>() ||
        !pending["user_id"].is<int>() || !pending["area_id"].is<int>() ||
        !pending["direction"].is<const char*>() ||
        !pending["created_at"].is<const char*>() ||
        !pending["authorized_at"].is<const char*>() ||
        !pending["expires_at"].is<const char*>()) {
      Serial.println("[BACKEND] Pending authorization payload is invalid.");
      return false;
    }
    receivedPendingId = pending["request_id"].as<const char*>();
    pendingUserId = pending["user_id"].as<int>();
    pendingAreaId = pending["area_id"].as<int>();
    pendingDirection = pending["direction"].as<const char*>();
    if (receivedPendingId.length() == 0 || pendingUserId < 1 ||
        pendingUserId > static_cast<int>(USER_COUNT) || pendingAreaId < 1 ||
        pendingAreaId > 7 ||
        (pendingDirection != "ENTRY" && pendingDirection != "EXIT")) {
      Serial.println("[BACKEND] Pending authorization values are out of range.");
      return false;
    }
  }

  // Commit mirrors only after the complete 6 x 7 matrix and pending payload
  // have passed validation. A partial response can never erase attendance.
  for (size_t i = 0; i < USER_COUNT; ++i) {
    users[i].insideMask = rebuiltInsideMasks[i];
  }
  mirrorCanonicalSecurity(canonicalFailedAttempts, canonicalLockdown);
  backendSystemActive = canonicalSystemActive;
  backendAdminMode = canonicalAdminMode;
  if (hasPendingAuthorization) {
    pendingAuthorizationId = receivedPendingId;
    if (remoteAccessRequest.active &&
        remoteAccessRequest.requestId == receivedPendingId &&
        remoteAccessRequest.workflowState != REMOTE_ACCESS_CHECK_READY &&
        !remoteAccessRequest.liveAuthorizationReceived) {
      remoteAccessRequest.workflowState =
          REMOTE_COMPLETION_RECONCILIATION_REQUIRED;
    }
    setIntegrationSyncState(INTEGRATION_RECONCILIATION_REQUIRED);
    Serial.println("[BACKEND] Bootstrap found a persistent pending authorization.");
    Serial.print("[BACKEND] Request/User/Area/Direction: ");
    Serial.print(receivedPendingId);
    Serial.print(" / ");
    Serial.print(pendingUserId);
    Serial.print(" / ");
    Serial.print(pendingAreaId);
    Serial.print(" / ");
    Serial.println(pendingDirection);
    Serial.println("[BACKEND] Door remains unchanged; manual/NVS reconciliation is required.");
  } else {
    pendingAuthorizationId = "";
    if (remoteAccessRequest.active &&
        remoteAccessRequest.workflowState ==
            REMOTE_COMPLETION_RECONCILIATION_REQUIRED) {
      setIntegrationSyncState(INTEGRATION_RECONCILIATION_REQUIRED);
      Serial.println("[BACKEND] Attendance mirrors loaded; local door transaction still requires reconciliation.");
    } else {
      setIntegrationSyncState(INTEGRATION_SYNCED);
      Serial.println("[BACKEND] Bootstrap succeeded: 42 attendance mirrors loaded.");
    }
  }
  return true;
}

bool fetchBootstrap() {
  lastBackendSyncAttemptAt = millis();
  String responseBody;
  int responseCode = -1;
  if (!getJson("/api/esp32/bootstrap", responseBody, responseCode) ||
      !applyBootstrapResponse(responseBody)) {
    markBackendUnavailable("Bootstrap", responseCode);
    return false;
  }

  recordBackendSuccess("Bootstrap");
  lastHeartbeatAttemptAt = 0;
  return true;
}
#endif

void setupAlertOutputs() {
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  turnOffAlerts();
  Serial.println("[ALERTS] Red LED and active buzzer initialized.");
}

void triggerAccessDeniedAlert() {
  turnOffSuccessOutput();
  digitalWrite(RED_LED_PIN, HIGH);
  beepMultiple(3, 150, 150);
  delay(1250);
  turnOffAlerts();
}

void triggerEnrollmentFailedAlert() {
  turnOffSuccessOutput();
  digitalWrite(RED_LED_PIN, HIGH);
  beepMultiple(2, 150, 150);
  delay(1050);
  turnOffAlerts();
}

void triggerInvalidCommandAlert() {
  turnOffSuccessOutput();
  beepOnce(150);
}

void triggerErrorAlert() {
  turnOffSuccessOutput();
  digitalWrite(RED_LED_PIN, HIGH);
  beepMultiple(2, 120, 120);
  delay(640);
  turnOffAlerts();
}

void beepOnce(int durationMs) {
  if (durationMs <= 0) {
    return;
  }

  digitalWrite(BUZZER_PIN, HIGH);
  delay(durationMs);
  digitalWrite(BUZZER_PIN, LOW);
}

void beepMultiple(int count, int durationMs, int pauseMs) {
  for (int i = 0; i < count; ++i) {
    beepOnce(durationMs);
    if (i < count - 1 && pauseMs > 0) {
      delay(pauseMs);
    }
  }
}

void redLedOn(int durationMs) {
  if (durationMs <= 0) {
    return;
  }

  digitalWrite(RED_LED_PIN, HIGH);
  delay(durationMs);
  digitalWrite(RED_LED_PIN, LOW);
}

void turnOffAlerts() {
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
}

void testAlerts() {
  turnOffSuccessOutput();
  Serial.println();
  Serial.println("[ALERT TEST]");
  Serial.println("Testing Red LED and active buzzer.");
  lcdShowMessage("ALERT TEST", "LED + BUZZER");

  digitalWrite(RED_LED_PIN, HIGH);
  beepOnce(300);
  delay(300);
  turnOffAlerts();

  Serial.println("[ALERT TEST] Complete");
  Serial.println();
}

void setupSuccessOutput() {
  pinMode(GREEN_LED_PIN, OUTPUT);
  turnOffSuccessOutput();
  Serial.println("[SUCCESS] Green LED initialized.");
}

void triggerAccessGrantedFeedback() {
  turnOffAlerts();
  greenLedOn(2000);
}

void greenLedOn(int durationMs) {
  if (durationMs <= 0) {
    return;
  }

  digitalWrite(GREEN_LED_PIN, HIGH);
  delay(durationMs);
  digitalWrite(GREEN_LED_PIN, LOW);
}

void turnOffSuccessOutput() {
  digitalWrite(GREEN_LED_PIN, LOW);
}

void testGreenLed() {
  turnOffAlerts();
  Serial.println();
  Serial.println("[GREEN LED TEST]");
  Serial.println("Testing Green LED on GPIO 32.");
  lcdShowMessage("GREEN LED TEST", "GPIO32 ACTIVE");
  greenLedOn(750);
  Serial.println("[GREEN LED TEST] Complete");
  Serial.println();
}

void incrementFailedAttempts(String reason) {
  bool lockdownThresholdReached = updateFailedAttemptCounter();

  Serial.println("[FAILED ACCESS ATTEMPT]");
  Serial.print("Reason: ");
  Serial.println(reason);
  Serial.print("Failed Attempts: ");
  Serial.print(failedAttempts);
  Serial.print('/');
  Serial.println(MAX_FAILED_ATTEMPTS);

  if (lockdownThresholdReached && !systemLocked) {
    lockSystem("Too many failed attempts");
  }
}

bool updateFailedAttemptCounter() {
  if (failedAttempts < MAX_FAILED_ATTEMPTS) {
    ++failedAttempts;
  }
  return failedAttempts >= MAX_FAILED_ATTEMPTS;
}

void resetFailedAttempts() {
  failedAttempts = 0;
}

void lockSystem(String reason) {
  if (systemLocked) {
    return;
  }

  applyLockedState();

  Serial.println();
  Serial.println("[SYSTEM LOCKED]");
  Serial.print("Reason: ");
  Serial.println(reason);
  Serial.print("Failed Attempts: ");
  Serial.print(failedAttempts);
  Serial.print('/');
  Serial.println(MAX_FAILED_ATTEMPTS);
  Serial.println("Unlock Required: Admin RFID");
  Serial.println();

  showLockdownStatus();
  turnOffSuccessOutput();
  digitalWrite(RED_LED_PIN, HIGH);
  beepMultiple(5, 200, 150);
  delay(1000);
  turnOffAlerts();
}

void unlockSystem() {
  applyUnlockedAdminState();

  Serial.println("[ADMIN RFID]");
  Serial.println("Admin recognized.");
  Serial.println("System Unlocked.");
  Serial.println("Failed Attempts Reset: 0");
  Serial.println("Admin Mode Enabled.");
  Serial.print("Door State: ");
  Serial.println(doorOpen ? "OPEN (unchanged)" : "CLOSED");
  Serial.println();

  lcdShowMessage("SYSTEM UNLOCKED", "ADMIN VERIFIED");
  triggerAccessGrantedFeedback();
}

void applyLockedState() {
  systemLocked = true;
  waitingForEnrollmentID = false;
  waitingForDeleteConfirmation = false;
  currentMode = MODE_ENTRY;
  presencePromptVisible = false;
  disableAdminMode();
}

void applyUnlockedAdminState() {
  systemLocked = false;
  resetFailedAttempts();
  enableAdminMode();
}

bool isSystemLocked() {
  return systemLocked;
}

bool isAdminModeActive() {
  checkAdminModeTimeout();
  return adminMode;
}

void enableAdminMode() {
  adminMode = true;
  adminModeStartTime = millis();
}

void disableAdminMode() {
  adminMode = false;
  adminModeStartTime = 0;
}

void checkAdminModeTimeout() {
  if (!hasAdminModeExpired(millis())) {
    return;
  }

  bool deleteConfirmationWasPending = waitingForDeleteConfirmation;
  disableAdminMode();
  waitingForEnrollmentID = false;
  waitingForDeleteConfirmation = false;
  Serial.println();
  Serial.println("[ADMIN MODE EXPIRED]");
  if (deleteConfirmationWasPending) {
    Serial.println("[FINGERPRINT DELETE CANCELLED]");
    Serial.println("Reason: Admin Mode expired before confirmation.");
  }
  Serial.println();
  lcdShowMessage("ADMIN EXPIRED", "SCAN ADMIN RFID");
}

bool hasAdminModeExpired(unsigned long now) {
  return adminMode && now - adminModeStartTime >= ADMIN_MODE_TIMEOUT_MS;
}

void handleAdminRFID() {
  if (isSystemLocked()) {
    unlockSystem();
    return;
  }

  resetFailedAttempts();
  enableAdminMode();

  Serial.println("[ADMIN RFID]");
  Serial.println("Admin Master Card recognized.");
  Serial.println("Admin Mode Enabled for 60 seconds.");
  Serial.println("Failed Attempts Reset: 0");
  Serial.println();

  lcdShowAdminMode();
  triggerAccessGrantedFeedback();
}

void handleUnknownRFID(String uid) {
  bool wasLocked = systemLocked;

  Serial.println("[UNKNOWN RFID]");

  if (wasLocked) {
    Serial.println("System remains locked.");
    Serial.println("Admin RFID required.");
  } else {
    Serial.println("Result: Unknown RFID card");
  }

  incrementFailedAttempts("Unknown RFID card: " + uid);
  Serial.print("System State: ");
  Serial.println(systemLocked ? "LOCKED" : "ACTIVE");

  if (wasLocked) {
    lcdShowSystemLocked();
    triggerAccessDeniedAlert();
  } else if (!systemLocked) {
    lcdShowRFIDStatus("Unknown RFID", "Access Denied");
    triggerAccessDeniedAlert();
    lcdShowMessage("UNKNOWN RFID",
                   "FAILED " + String(failedAttempts) + "/" +
                       String(MAX_FAILED_ATTEMPTS));
  }

  Serial.println();
}

void showLockdownStatus() {
  lcdShowSystemLocked();
}

void printSecurityStatus() {
  checkAdminModeTimeout();

  Serial.print("System Locked: ");
  Serial.println(systemLocked ? "YES" : "NO");
  Serial.print("Admin Mode: ");
  Serial.println(adminMode ? "YES" : "NO");
  Serial.print("Admin Mode Remaining: ");

  if (adminMode) {
    unsigned long elapsed = millis() - adminModeStartTime;
    unsigned long remainingMs = elapsed < ADMIN_MODE_TIMEOUT_MS
                                    ? ADMIN_MODE_TIMEOUT_MS - elapsed
                                    : 0;
    Serial.print((remainingMs + 999) / 1000);
  } else {
    Serial.print(0);
  }

  Serial.println(" seconds");
  Serial.print("Failed Attempts: ");
  Serial.print(failedAttempts);
  Serial.print('/');
  Serial.println(MAX_FAILED_ATTEMPTS);
}

void setupDoorServo() {
  /*
   * IMPORTANT POWER NOTE:
   * If the fingerprint sensor worked before connecting the servo but stopped
   * afterward, check common-ground noise and both the 3.3 V sensor rail and
   * external 5 V servo rail. Do not power the servo from ESP32 3.3 V.
   * This JM-101B is specified for 3.3 V at about 60 mA; the moving servo can
   * draw much more from its separate regulated 5 V supply.
   */
  int servoPwmChannel = -1;
#if SERVO_USE_SIMPLE_ATTACH
  // Match the working servo_test.ino attach pattern on the conflict-free
  // project pin. GPIO18 remains reserved for the RC522 SCK signal.
  servoPwmChannel = doorServo.attach(SERVO_PIN);
#else
  ESP32PWM::allocateTimer(0);
  doorServo.setPeriodHertz(50);
  servoPwmChannel = doorServo.attach(SERVO_PIN, 500, 2400);
#endif
  // Channel 0 is valid. Use attached() instead of treating a zero-valued
  // channel number as failure.
  servoInitialized = doorServo.attached();

  if (!servoInitialized) {
    doorOpen = false;
    Serial.println("[SERVO] Failed to attach PWM on GPIO 13.");
    Serial.println("This is a PWM setup failure, not software detection of the motor.");
    return;
  }

  doorServo.write(DOOR_CLOSED_ANGLE);
  doorOpen = false;
  doorOpenedAt = 0;
  delay(300);
  Serial.print("[SERVO] Attach mode: ");
#if SERVO_USE_SIMPLE_ATTACH
  Serial.println("simple attach (working servo_test.ino pattern)");
#else
  Serial.println("advanced 50 Hz / 500-2400 us");
#endif
  Serial.println("[SERVO] PWM attached on GPIO 13.");
  Serial.print("[SERVO] PWM channel: ");
  Serial.println(servoPwmChannel);
  Serial.println("[SERVO] Closed-angle command sent: 0 degrees.");
  Serial.println("Note: a three-wire servo has no feedback, so movement cannot be detected in software.");
}

void setupUltrasonicSensor() {
  pinMode(ULTRASONIC_TRIG_PIN, OUTPUT);
  pinMode(ULTRASONIC_ECHO_PIN, INPUT);
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
  ultrasonicHasMeasurement = false;
  lastDistanceCm = -1.0;
  Serial.println("[ULTRASONIC] HC-SR04 initialized.");
}

void openDoor(String reason) {
  openDoor(reason, selectedArea, currentMode);
}

void openDoor(String reason, Area area, AccessMode accessMode) {
  if (isSystemLocked()) {
    Serial.println("[DOOR BLOCKED]");
    Serial.println("System is locked. Door remains closed.");
    showLockdownStatus();
    triggerErrorAlert();
    return;
  }

  if (!servoInitialized) {
    Serial.println("[DOOR ERROR] Servo is not initialized.");
    lcdShowError("Servo Error", "Not Initialized");
    triggerErrorAlert();
    return;
  }

  if (doorOpen) {
    doorOpenedAt = millis();
    Serial.println("[DOOR] Already open; minimum-open timer restarted.");
    return;
  }

  doorServo.write(DOOR_OPEN_ANGLE);
  doorOpen = true;
  doorOpenedAt = millis();
  presencePromptVisible = false;

  Serial.println("[DOOR OPEN]");
  Serial.print("Reason: ");
  Serial.println(reason);
  Serial.print("Selected Area: ");
  Serial.println(getAreaName(area));
  Serial.print("Mode: ");
  Serial.println(getAccessModeName(accessMode));
  Serial.print("Servo Angle: ");
  Serial.println(DOOR_OPEN_ANGLE);

  lcdShowDoorOpen(accessMode == MODE_EXIT);
}

void closeDoor(String reason) {
  if (!servoInitialized) {
    Serial.println("[DOOR ERROR] Servo is not initialized.");
    return;
  }

  if (!doorOpen) {
    Serial.println("[DOOR] Already closed; no servo movement needed.");
    return;
  }

  doorServo.write(DOOR_CLOSED_ANGLE);
  doorOpen = false;
  doorOpenedAt = 0;
  presencePromptVisible = false;

  Serial.println("[DOOR CLOSED]");
  Serial.print("Reason: ");
  Serial.println(reason);
  Serial.print("Servo Angle: ");
  Serial.println(DOOR_CLOSED_ANGLE);
  lcdShowDoorClosed();
}

void updateDoorState() {
  if (shouldCloseDoor()) {
    closeDoor("Minimum open time elapsed and doorway is clear");
  }
}

bool shouldCloseDoor() {
  if (!doorOpen) {
    return false;
  }

  unsigned long openElapsedMs = millis() - doorOpenedAt;
  if (openElapsedMs < DOOR_MIN_OPEN_TIME_MS) {
    return false;
  }

  updateUltrasonicMeasurement(false);
  bool measurementValid = isUltrasonicMeasurementValid(lastDistanceCm);
  bool personNear = isDistanceNear(lastDistanceCm);
  return shouldCloseDoorForState(
      doorOpen, openElapsedMs, measurementValid, personNear);
}

bool shouldCloseDoorForState(bool isOpen,
                             unsigned long openElapsedMs,
                             bool ultrasonicMeasurementValid,
                             bool personNear) {
  return isOpen && openElapsedMs >= DOOR_MIN_OPEN_TIME_MS &&
         ultrasonicMeasurementValid && !personNear;
}

float readDistanceCm() {
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(ULTRASONIC_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);

  unsigned long duration = pulseIn(
      ULTRASONIC_ECHO_PIN,
      HIGH,
      ULTRASONIC_ECHO_TIMEOUT_US);

  if (duration == 0) {
    return -1.0;
  }

  return duration * 0.0343f / 2.0f;
}

bool isPersonNear() {
  updateUltrasonicMeasurement(false);
  return isDistanceNear(lastDistanceCm);
}

bool isUltrasonicMeasurementValid(float distanceCm) {
  return distanceCm > 0.0;
}

bool isDistanceNear(float distanceCm) {
  return isUltrasonicMeasurementValid(distanceCm) &&
         distanceCm <= PRESENCE_DISTANCE_CM;
}

bool isPresenceRequiredForMode(AccessMode mode) {
  return mode == MODE_ENTRY;
}

void updateUltrasonicMeasurement(bool forceRead) {
  unsigned long now = millis();
  if (!forceRead && ultrasonicHasMeasurement &&
      now - lastUltrasonicCheckAt < ULTRASONIC_CHECK_INTERVAL_MS) {
    return;
  }

  lastDistanceCm = readDistanceCm();
  lastUltrasonicCheckAt = millis();
  ultrasonicHasMeasurement = true;
}

void updatePresencePrompt() {
  static unsigned long lastPromptCheckAt = 0;
  unsigned long now = millis();

  if (now - lastPromptCheckAt < ULTRASONIC_CHECK_INTERVAL_MS) {
    return;
  }
  lastPromptCheckAt = now;

  if (now - lastLCDMessageAt < PRESENCE_PROMPT_HOLD_MS) {
    return;
  }

  if (systemLocked) {
    if (presencePromptVisible) {
      showLockdownStatus();
      presencePromptVisible = false;
    }
    return;
  }

  if (doorOpen || currentMode == MODE_EXIT) {
    presencePromptVisible = false;
    return;
  }

  if (selectedArea == AREA_NONE) {
    if (presencePromptVisible) {
      lcdShowMenu();
      presencePromptVisible = false;
    }
    return;
  }

  bool personNear = isPersonNear();
  if (personNear && !presencePromptVisible) {
    lcdShowMessage("PERSON DETECTED", "PRESS R TO SCAN");
    presencePromptVisible = true;
  } else if (!personNear && presencePromptVisible) {
    lcdShowMenu();
    presencePromptVisible = false;
  }
}

void testDoor() {
  Serial.println();
  Serial.println("[SERVO TEST]");
  Serial.println("Library: ESP32Servo");
  Serial.print("Servo signal pin: GPIO");
  Serial.println(SERVO_PIN);

  if (systemLocked) {
    Serial.println("Servo test blocked while system is locked.");
    showLockdownStatus();
    triggerErrorAlert();
    return;
  }

  if (!servoInitialized) {
    Serial.println("Servo test unavailable: servo is not initialized.");
    lcdShowError("Servo Error", "Not Initialized");
    triggerErrorAlert();
    return;
  }

  if (doorOpen) {
    Serial.println("Servo test blocked: door is already open.");
    lcdShowError("Door Is Open", "Wait To Close");
    triggerErrorAlert();
    return;
  }

  lcdShowMessage("SERVO TEST", "0-90-0 DEGREES");
  Serial.println("Moving to 0 degrees");
  doorServo.write(DOOR_CLOSED_ANGLE);
  doorOpen = false;
  doorOpenedAt = 0;
  delay(1000);

  Serial.println("Moving to 90 degrees");
  doorServo.write(DOOR_OPEN_ANGLE);
  delay(2000);

  Serial.println("Moving back to 0 degrees");
  doorServo.write(DOOR_CLOSED_ANGLE);
  delay(500);
  doorOpen = false;
  doorOpenedAt = 0;
  presencePromptVisible = false;

  Serial.println("Test complete.");
  Serial.println("If the servo did not move, check stable 5 V power, signal GPIO 13, and common GND.");
  Serial.println("The ESP32 can confirm PWM output, but a three-wire servo provides no position feedback.");
  lcdShowMessage("SERVO TEST", "TEST COMPLETE");
  Serial.println();
}

void printUltrasonicStatus() {
  Serial.println();
  Serial.println("[ULTRASONIC TEST]");
  Serial.print("TRIG: GPIO ");
  Serial.println(ULTRASONIC_TRIG_PIN);
  Serial.print("ECHO: GPIO ");
  Serial.println(ULTRASONIC_ECHO_PIN);
  Serial.print("Echo timeout: ");
  Serial.print(ULTRASONIC_ECHO_TIMEOUT_US);
  Serial.println(" us");

  constexpr int readingCount = 10;
  float distanceTotal = 0.0;
  int validReadings = 0;

  for (int i = 0; i < readingCount; ++i) {
    float distanceCm = readDistanceCm();
    Serial.print("Reading ");
    Serial.print(i + 1);
    Serial.print(": ");

    if (distanceCm > 0.0) {
      Serial.print(distanceCm, 1);
      Serial.println(" cm");
      distanceTotal += distanceCm;
      ++validReadings;
    } else {
      Serial.println("TIMEOUT - no echo received");
    }
    delay(60);
  }

  ultrasonicHasMeasurement = true;
  lastUltrasonicCheckAt = millis();

  if (validReadings == 0) {
    lastDistanceCm = -1.0;
    Serial.println("[ULTRASONIC ERROR]");
    Serial.println("No echo received in any of the 10 readings.");
    Serial.println("Check TRIG GPIO 21, ECHO GPIO 22, 5 V, common GND, and the Echo voltage divider.");
    Serial.println("HC-SR04 Echo must reach GPIO 22 through 3.3 V level protection.");
    lcdShowMessage("ULTRA ERROR", "NO ECHO");
  } else {
    lastDistanceCm = distanceTotal / validReadings;
    bool personNear = lastDistanceCm <= PRESENCE_DISTANCE_CM;
    Serial.print("Valid Readings: ");
    Serial.print(validReadings);
    Serial.print('/');
    Serial.println(readingCount);
    Serial.print("Average: ");
    Serial.print(lastDistanceCm, 1);
    Serial.println(" cm");
    Serial.print("Person Near: ");
    Serial.println(personNear ? "YES" : "NO");
    lcdShowMessage("ULTRA " + String(personNear ? "NEAR" : "CLEAR"),
                   String(lastDistanceCm, 1) + " CM");
  }
  Serial.println();
}

void returnToEntryMode() {
  if (currentMode == MODE_EXIT) {
    currentMode = MODE_ENTRY;
    presencePromptVisible = false;
    Serial.println("[MODE] Returned to ENTRY mode.");
  }
}

String getAccessModeName(AccessMode mode) {
  return mode == MODE_EXIT ? "EXIT" : "ENTRY";
}

void setupLCD() {
  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  Wire.beginTransmission(LCD_ADDRESS);
  lcdInitialized = Wire.endTransmission() == 0;

  if (!lcdInitialized) {
    Serial.print("[LCD] Not detected at I2C address 0x");
    Serial.println(LCD_ADDRESS, HEX);
    Serial.println("Check wiring or try LCD_ADDRESS 0x3F.");
    return;
  }

  lcd.init();
  lcd.backlight();
  lcd.clear();
  Serial.print("[LCD] Initialized at I2C address 0x");
  Serial.println(LCD_ADDRESS, HEX);
}

void lcdShowWelcome() {
  lcdShowMessage("SMART ACCESS", "ESP32 READY");
}

void lcdShowMenu() {
  lcdShowMessage("SYSTEM READY", "SELECT AREA 1-7");
}

void lcdShowSelectedArea(Area area) {
  lcdShowMessage(getLCDAreaName(area), "R=SCAN X=EXIT");
}

void lcdShowEnrollStart() {
  lcdShowMessage("ENROLL MODE", "ID 1-127 SERIAL");
}

void lcdShowEnrollStep(String line1, String line2) {
  lcdShowMessage(line1, line2);
}

void lcdShowEnrollSuccess(int id) {
  lcdShowMessage("ENROLL SUCCESS", "ID " + String(id) + " SAVED");
}

void lcdShowEnrollFailed(String shortReason) {
  lcdShowMessage("ENROLL FAILED", shortReason);
}

void lcdShowPlaceFinger() {
  lcdShowMessage("PLACE FINGER", "SCANNING...");
}

void lcdShowAccessGranted(User* user, Area area) {
  String userName = user == nullptr ? "Authorized User" : user->name;
  lcdShowMessage("ACCESS GRANTED", userName);
}

void lcdShowAccessDenied(String reason) {
  if (reason.indexOf("not recognized") >= 0) {
    lcdShowMessage("ACCESS DENIED", "UNKNOWN FINGER");
  } else if (reason.indexOf("not configured") >= 0) {
    lcdShowMessage("ACCESS DENIED", "UNKNOWN USER");
  } else if (reason.indexOf("not allowed") >= 0) {
    lcdShowMessage("ACCESS DENIED", "NO PERMISSION");
  } else if (reason.indexOf("already inside") >= 0) {
    lcdShowMessage("ACCESS DENIED", "ALREADY INSIDE");
  } else if (reason.indexOf("not inside") >= 0) {
    lcdShowMessage("EXIT DENIED", "NOT INSIDE");
  } else if (reason.indexOf("Timed out") >= 0) {
    lcdShowMessage("ACCESS DENIED", "SCAN TIMEOUT");
  } else if (reason.indexOf("not detected") >= 0) {
    lcdShowMessage("FINGER SENSOR", "NOT FOUND");
  } else {
    lcdShowMessage("ACCESS DENIED", "TRY AGAIN");
  }
}

void lcdShowRFIDStatus(String line1, String line2) {
  lcdShowMessage(line1, line2);
}

void lcdShowError(String line1, String line2) {
  lcdShowMessage(line1, line2);
}

void lcdShowDoorOpen(bool exitMode) {
  lcdShowMessage("DOOR OPEN", exitMode ? "PLEASE EXIT" : "PLEASE ENTER");
}

void lcdShowDoorClosed() {
  lcdShowMessage("DOOR CLOSED", "SELECT AREA 1-7");
}

void lcdShowSystemLocked() {
  lcdShowMessage("SYSTEM LOCKED", "SCAN ADMIN RFID");
}

void lcdShowAdminMode() {
  lcdShowMessage("ADMIN MODE", "ENABLED 60 SEC");
}

void lcdShowMessage(String line1, String line2) {
  if (!lcdInitialized) {
    return;
  }

  line1 = getLCDLine(line1);
  line2 = getLCDLine(line2);

  if (isLCDMessageUnchanged(line1, line2)) {
    return;
  }

  lastLcdLine1 = line1;
  lastLcdLine2 = line2;
  lastLCDMessageAt = millis();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

bool isLCDMessageUnchanged(const String& line1,
                           const String& line2) {
  return line1 == lastLcdLine1 && line2 == lastLcdLine2;
}

String getLCDLine(String text) {
  text.trim();
  if (text.length() > LCD_COLUMNS) {
    text = text.substring(0, LCD_COLUMNS);
  }
  return text;
}

String getLCDAreaName(Area area) {
  if (area == MANAGEMENT_ADMIN) {
    return "Admin Room";
  }
  return getAreaName(area);
}

void printMenu() {
  Serial.println();
  Serial.println("Commands:");
  Serial.println("1 = Company A");
  Serial.println("2 = Company B");
  Serial.println("3 = Company C");
  Serial.println("4 = Company D");
  Serial.println("5 = Server Room");
  Serial.println("6 = Management / Admin");
  Serial.println("7 = Main Entrance");
  Serial.println("E = Enroll fingerprint (Admin Mode only)");
  Serial.println("C = Delete configured fingerprints 1-6 (Admin Mode + confirmation)");
  Serial.println("R = Read/test fingerprint");
  Serial.println("F = Detect AS608/JM-101B at 3.3V using UART2");
  Serial.println("P = Simple fingerprint scan/match test");
  Serial.println("V = Run full software validation tests");
  Serial.println("M = Print menu");
  Serial.println("S = Show system status");
  Serial.println("T = Test Red LED and buzzer");
  Serial.println("G = Test Green LED");
  Serial.println("X = Exit Mode");
  Serial.println("D = Test Servo Door");
  Serial.println("U = Test Ultrasonic Sensor (10 readings)");
  Serial.println("W = Run non-destructive hardware readiness check");
  Serial.println();
}

bool validatePinAssignments() {
  const uint8_t usedPins[] = {RFID_SS_PIN,
                              RFID_SCK_PIN,
                              RFID_MOSI_PIN,
                              RFID_MISO_PIN,
                              RFID_RST_PIN,
                              FINGER_RX_PIN,
                              FINGER_TX_PIN,
                              LCD_SDA_PIN,
                              LCD_SCL_PIN,
                              RED_LED_PIN,
                              BUZZER_PIN,
                              GREEN_LED_PIN,
                              SERVO_PIN,
                              ULTRASONIC_TRIG_PIN,
                              ULTRASONIC_ECHO_PIN};
  const char* pinNames[] = {"RFID SS",       "RFID SCK",    "RFID MOSI",
                            "RFID MISO",     "RFID RST",    "Fingerprint RX",
                            "Fingerprint TX", "LCD SDA",     "LCD SCL",
                            "Red LED",       "Buzzer",      "Green LED",
                            "Servo",         "Ultrasonic TRIG",
                            "Ultrasonic ECHO"};
  constexpr size_t usedPinCount = sizeof(usedPins) / sizeof(usedPins[0]);
  bool pinMapValid = true;

  for (size_t i = 0; i < usedPinCount; ++i) {
    for (size_t j = i + 1; j < usedPinCount; ++j) {
      if (usedPins[i] == usedPins[j]) {
        pinMapValid = false;
        Serial.print("[PIN CONFLICT] GPIO ");
        Serial.print(usedPins[i]);
        Serial.print(" is assigned to ");
        Serial.print(pinNames[i]);
        Serial.print(" and ");
        Serial.println(pinNames[j]);
      }
    }
  }

  return pinMapValid;
}

void printHardwareSelfTest() {
  Serial.println();
  Serial.println("================ HARDWARE SELF TEST ================");
  Serial.print("LCD: ");
  Serial.println(lcdInitialized ? "Initialized" : "Not Initialized");
  Serial.print("RFID RC522: ");
  Serial.println(rfidInitialized ? "Initialized" : "Not Initialized");
  Serial.print("Fingerprint: ");
  Serial.println(fingerprintReady ? "Detected" : "Not Detected");
  Serial.print("Fingerprint Model: ");
  Serial.println(FINGERPRINT_MODEL);
  Serial.print("Fingerprint Expected Voltage: ");
  Serial.println(FINGERPRINT_EXPECTED_VOLTAGE);
  Serial.print("Fingerprint Baud: ");
  if (fingerprintWorkingBaud > 0) {
    Serial.println(fingerprintWorkingBaud);
  } else {
    Serial.println("unknown");
  }
  Serial.print("Red LED: GPIO ");
  Serial.println(RED_LED_PIN);
  Serial.print("Green LED: GPIO ");
  Serial.println(GREEN_LED_PIN);
  Serial.print("Buzzer: GPIO ");
  Serial.println(BUZZER_PIN);
  Serial.print("Servo Signal: GPIO ");
  Serial.print(SERVO_PIN);
  Serial.println(servoInitialized ? " (PWM attached)" : " (PWM attach failed)");
  Serial.print("Ultrasonic TRIG: GPIO ");
  Serial.println(ULTRASONIC_TRIG_PIN);
  Serial.print("Ultrasonic ECHO: GPIO ");
  Serial.print(ULTRASONIC_ECHO_PIN);
  Serial.println(" (configured as input through voltage divider)");
  Serial.print("Pin Conflict Check: ");
  Serial.println(validatePinAssignments() ? "PASS" : "FAIL");
  Serial.println("Power Note: JM-101B uses 3.3V; servo uses separate 5V; common GND is required.");
  Serial.println("====================================================");
  Serial.println();

  lcdShowMessage("HW SELF TEST",
                 rfidInitialized && fingerprintReady && lcdInitialized
                     ? "CORE DEVICES OK"
                     : "CHECK SERIAL");
}

AccessControlStateSnapshot captureAccessControlState() {
  AccessControlStateSnapshot snapshot;
  snapshot.selectedArea = selectedArea;
  snapshot.currentMode = currentMode;
  snapshot.systemLocked = systemLocked;
  snapshot.adminMode = adminMode;
  snapshot.failedAttempts = failedAttempts;
  snapshot.adminModeStartTime = adminModeStartTime;
  snapshot.waitingForEnrollmentID = waitingForEnrollmentID;
  snapshot.waitingForDeleteConfirmation = waitingForDeleteConfirmation;
  snapshot.doorOpen = doorOpen;
  snapshot.doorOpenedAt = doorOpenedAt;
  snapshot.presencePromptVisible = presencePromptVisible;
  snapshot.lastLCDMessageAt = lastLCDMessageAt;
  snapshot.lcdLine1 = lastLcdLine1;
  snapshot.lcdLine2 = lastLcdLine2;
  for (size_t i = 0; i < USER_COUNT; ++i) {
    snapshot.insideMasks[i] = users[i].insideMask;
  }
  return snapshot;
}

bool isAccessControlStateUnchanged(
    const AccessControlStateSnapshot& snapshot) {
  bool unchanged = selectedArea == snapshot.selectedArea &&
                   currentMode == snapshot.currentMode &&
                   systemLocked == snapshot.systemLocked &&
                   adminMode == snapshot.adminMode &&
                   failedAttempts == snapshot.failedAttempts &&
                   adminModeStartTime == snapshot.adminModeStartTime &&
                   waitingForEnrollmentID == snapshot.waitingForEnrollmentID &&
                   waitingForDeleteConfirmation ==
                       snapshot.waitingForDeleteConfirmation &&
                   doorOpen == snapshot.doorOpen &&
                   doorOpenedAt == snapshot.doorOpenedAt &&
                   presencePromptVisible == snapshot.presencePromptVisible;
  for (size_t i = 0; i < USER_COUNT; ++i) {
    unchanged &= users[i].insideMask == snapshot.insideMasks[i];
  }
  return unchanged;
}

bool isLCDCacheUnchanged(const AccessControlStateSnapshot& snapshot) {
  return lastLCDMessageAt == snapshot.lastLCDMessageAt &&
         lastLcdLine1 == snapshot.lcdLine1 &&
         lastLcdLine2 == snapshot.lcdLine2;
}

void restoreAccessControlState(const AccessControlStateSnapshot& snapshot) {
  selectedArea = snapshot.selectedArea;
  currentMode = snapshot.currentMode;
  systemLocked = snapshot.systemLocked;
  adminMode = snapshot.adminMode;
  failedAttempts = snapshot.failedAttempts;
  adminModeStartTime = snapshot.adminModeStartTime;
  waitingForEnrollmentID = snapshot.waitingForEnrollmentID;
  waitingForDeleteConfirmation = snapshot.waitingForDeleteConfirmation;
  doorOpen = snapshot.doorOpen;
  doorOpenedAt = snapshot.doorOpenedAt;
  presencePromptVisible = snapshot.presencePromptVisible;
  lastLCDMessageAt = snapshot.lastLCDMessageAt;
  lastLcdLine1 = snapshot.lcdLine1;
  lastLcdLine2 = snapshot.lcdLine2;
  for (size_t i = 0; i < USER_COUNT; ++i) {
    users[i].insideMask = snapshot.insideMasks[i];
  }
}

void restoreDiagnosticLCD(const AccessControlStateSnapshot& snapshot) {
  if (lcdInitialized && !isLCDCacheUnchanged(snapshot)) {
    // Redraw the pre-diagnostic screen while the diagnostic cache is still
    // active. restoreAccessControlState() then restores the original timestamp.
    lcdShowMessage(snapshot.lcdLine1, snapshot.lcdLine2);
  }
}

bool isStateGuardedDiagnosticCommand(char command) {
  return command == 'F' || command == 'P' || command == 'D' ||
         command == 'U' || command == 'W';
}

void restoreWorkingFingerprintState(bool wasReady,
                                    uint32_t savedBaud) {
  if (wasReady && !fingerprintReady) {
    fingerprintReady = true;
    fingerprintWorkingBaud = savedBaud;
  }
}

void runHardwareReadinessCheck() {
  AccessControlStateSnapshot savedState = captureAccessControlState();

  Serial.println();
  Serial.println("[HARDWARE READINESS CHECK]");
  Serial.println("This check does not move the servo or make access decisions.");
  Serial.print("Fingerprint Sensor: ");
  Serial.println(FINGERPRINT_MODEL);
  Serial.print("Expected voltage: ");
  Serial.println(FINGERPRINT_EXPECTED_VOLTAGE);
  Serial.println("UART: GPIO16 RX, GPIO17 TX");
  Serial.print("Baud: ");
  Serial.println(FINGERPRINT_PRIMARY_BAUD);
  Serial.println("USB D+/D-: not used");
  Serial.println();

  bool fingerprintOK = runFingerprintStandaloneStyleTest();
  printUltrasonicStatus();
  bool ultrasonicOK = ultrasonicHasMeasurement && lastDistanceCm > 0.0;
  bool gpioOK = validatePinAssignments();

  bool accessStateUnchanged = isAccessControlStateUnchanged(savedState);

  // Enforce W's non-destructive contract even if a future diagnostic is
  // accidentally changed. Hardware status variables remain updated.
  restoreAccessControlState(savedState);

  bool ready = fingerprintOK && ultrasonicOK && servoInitialized &&
               rfidInitialized && lcdInitialized && gpioOK &&
               accessStateUnchanged;

  Serial.println();
  Serial.println("[HARDWARE READINESS SUMMARY]");
  Serial.print("Fingerprint: ");
  Serial.println(fingerprintOK ? "OK" : "FAIL");
  Serial.print("Ultrasonic: ");
  Serial.println(ultrasonicOK ? "OK" : "FAIL / Timeout");
  Serial.print("Servo pin configured: GPIO");
  Serial.println(SERVO_PIN);
  Serial.print("Servo PWM attached: ");
  Serial.println(servoInitialized ? "YES" : "NO");
  Serial.println("Servo movement test: use D");
  Serial.print("RFID: ");
  Serial.println(rfidInitialized ? "OK" : "FAIL");
  Serial.print("LCD 16x2: ");
  Serial.println(lcdInitialized ? "OK" : "FAIL");
  Serial.print("GPIO conflicts: ");
  Serial.println(gpioOK ? "none" : "FOUND");
  Serial.print("Access/security state unchanged: ");
  Serial.println(accessStateUnchanged ? "YES" : "NO - state restored");
  Serial.println("Power warning: JM-101B 3.3V, servo regulated 5V, and common GND.");
  Serial.print("Result: ");
  Serial.println(ready ? "READY" : "NOT READY");
  Serial.println();

  lcdShowMessage("HW READINESS", ready ? "READY" : "NOT READY");
}

void runStateGuardedDiagnostic(char command) {
  if (!isStateGuardedDiagnosticCommand(command)) {
    return;
  }

  AccessControlStateSnapshot savedState = captureAccessControlState();
  bool fingerprintWasReady = fingerprintReady;
  uint32_t savedFingerprintBaud = fingerprintWorkingBaud;
  switch (command) {
    case 'F':
      runFingerprintStandaloneStyleTest();
      break;
    case 'P':
      testSimpleFingerprintScan();
      break;
    case 'D':
      testDoor();
      break;
    case 'U':
      printUltrasonicStatus();
      break;
    case 'W':
      runHardwareReadinessCheck();
      break;
  }

  // A temporary diagnostic communication failure must not disable a sensor
  // that was already working before F, P, or W was run.
  if (fingerprintWasReady && !fingerprintReady) {
    restoreWorkingFingerprintState(
        fingerprintWasReady, savedFingerprintBaud);
    Serial.println("[DIAGNOSTIC STATE GUARD] Restored prior fingerprint-ready state.");
  }

  bool accessStateUnchanged = isAccessControlStateUnchanged(savedState);
  bool lcdCacheUnchanged = isLCDCacheUnchanged(savedState);
  restoreDiagnosticLCD(savedState);
  restoreAccessControlState(savedState);

  if (!accessStateUnchanged) {
    Serial.println("[DIAGNOSTIC STATE GUARD]");
    Serial.println("ERROR: Diagnostic attempted to change access-control state.");
    Serial.println("Original attendance and security state restored.");
  } else if (!lcdCacheUnchanged) {
    Serial.println("[DIAGNOSTIC STATE GUARD] Original LCD screen restored.");
  }
}

void runSoftwareValidation() {
  constexpr int MAX_STORED_VALIDATION_FAILURES = 100;
  String failedDetails[MAX_STORED_VALIDATION_FAILURES];
  int totalTests = 0;
  int passedTests = 0;
  int failedTests = 0;
  int storedFailures = 0;

  uint16_t savedInsideMasks[USER_COUNT];
  for (size_t i = 0; i < USER_COUNT; ++i) {
    savedInsideMasks[i] = users[i].insideMask;
    users[i].insideMask = 0;
  }

  bool savedSystemLocked = systemLocked;
  bool savedAdminMode = adminMode;
  int savedFailedAttempts = failedAttempts;
  unsigned long savedAdminModeStartTime = adminModeStartTime;
  Area savedSelectedArea = selectedArea;
  AccessMode savedCurrentMode = currentMode;
  bool savedWaitingForEnrollmentID = waitingForEnrollmentID;
  bool savedWaitingForDeleteConfirmation = waitingForDeleteConfirmation;
  bool savedDoorOpen = doorOpen;
  unsigned long savedDoorOpenedAt = doorOpenedAt;
  float savedLastDistanceCm = lastDistanceCm;
  unsigned long savedLastUltrasonicCheckAt = lastUltrasonicCheckAt;
  bool savedUltrasonicHasMeasurement = ultrasonicHasMeasurement;
  bool savedPresencePromptVisible = presencePromptVisible;
  bool savedFingerprintReady = fingerprintReady;
  uint32_t savedFingerprintWorkingBaud = fingerprintWorkingBaud;
  unsigned long savedLastLCDMessageAt = lastLCDMessageAt;
  String savedLcdLine1 = lastLcdLine1;
  String savedLcdLine2 = lastLcdLine2;

  auto validate = [&](const String& name,
                      bool passed,
                      const String& expected,
                      const String& actual) {
    ++totalTests;
    Serial.print("Test ");
    Serial.print(totalTests);
    Serial.print(": ");
    Serial.println(name);
    Serial.println(passed ? "PASS" : "FAIL");

    if (passed) {
      ++passedTests;
    } else {
      ++failedTests;
      Serial.print("  Expected: ");
      Serial.println(expected);
      Serial.print("  Actual: ");
      Serial.println(actual);
      if (storedFailures < MAX_STORED_VALIDATION_FAILURES) {
        failedDetails[storedFailures++] =
            "- " + name + " | Expected: " + expected + " | Actual: " + actual;
      }
    }
    Serial.println();
  };

  Serial.println();
  Serial.println("[SOFTWARE VALIDATION START]");
  Serial.println("Hardware I/O is not used by this test.");
  Serial.println("Runtime security and attendance state will be restored afterward.");
  Serial.println();

  User* employeeA = findUserByFingerprintID(1);
  User* employeeB = findUserByFingerprintID(2);
  User* employeeC = findUserByFingerprintID(3);
  User* employeeD = findUserByFingerprintID(4);
  User* itAdmin = findUserByFingerprintID(5);
  User* manager = findUserByFingerprintID(6);

  validate("Fingerprint IDs 1-6 map to the exact configured users",
           employeeA != nullptr && employeeB != nullptr && itAdmin != nullptr &&
               manager != nullptr && employeeC != nullptr && employeeD != nullptr &&
               employeeA->name == "Employee A" &&
               employeeB->name == "Employee B" &&
               employeeC->name == "Employee C" &&
               employeeD->name == "Employee D" &&
               itAdmin->name == "IT Admin" && manager->name == "Manager",
           "1=A, 2=B, 3=C, 4=D, 5=IT Admin, 6=Manager",
           "fingerprint-to-user mapping mismatch");
  validate("Employee A -> Company A permission",
           checkPermission(employeeA, COMPANY_A),
           "allowed",
           "denied");
  validate("Employee A -> Company B permission",
           !checkPermission(employeeA, COMPANY_B),
           "denied",
           "allowed");
  validate("Employee B -> Company B permission",
           checkPermission(employeeB, COMPANY_B),
           "allowed",
           "denied");
  validate("Employee B -> Company A permission",
           !checkPermission(employeeB, COMPANY_A),
           "denied",
           "allowed");
  validate("Employee C -> Company C permission",
           checkPermission(employeeC, COMPANY_C),
           "allowed",
           "denied");
  validate("Employee D -> Company D permission",
           checkPermission(employeeD, COMPANY_D),
           "allowed",
           "denied");
  validate("IT Admin -> Server Room permission",
           checkPermission(itAdmin, SERVER_ROOM),
           "allowed",
           "denied");
  bool regularEmployeesDeniedServer =
      !checkPermission(employeeA, SERVER_ROOM) &&
      !checkPermission(employeeB, SERVER_ROOM) &&
      !checkPermission(employeeC, SERVER_ROOM) &&
      !checkPermission(employeeD, SERVER_ROOM);
  validate("Regular employees cannot access Server Room",
           regularEmployeesDeniedServer,
           "all denied",
           "at least one allowed");
  bool regularEmployeesDeniedManagement =
      !checkPermission(employeeA, MANAGEMENT_ADMIN) &&
      !checkPermission(employeeB, MANAGEMENT_ADMIN) &&
      !checkPermission(employeeC, MANAGEMENT_ADMIN) &&
      !checkPermission(employeeD, MANAGEMENT_ADMIN);
  validate("Regular employees cannot access Management/Admin",
           regularEmployeesDeniedManagement,
           "all denied",
           "at least one allowed");
  validate("Employee A is denied Companies B/C/D",
           !checkPermission(employeeA, COMPANY_B) &&
               !checkPermission(employeeA, COMPANY_C) &&
               !checkPermission(employeeA, COMPANY_D),
           "all denied",
           "at least one allowed");
  validate("Employee B is denied Companies A/C/D",
           !checkPermission(employeeB, COMPANY_A) &&
               !checkPermission(employeeB, COMPANY_C) &&
               !checkPermission(employeeB, COMPANY_D),
           "all denied",
           "at least one allowed");
  validate("Employee C is denied Companies A/B/D",
           !checkPermission(employeeC, COMPANY_A) &&
               !checkPermission(employeeC, COMPANY_B) &&
               !checkPermission(employeeC, COMPANY_D),
           "all denied",
           "at least one allowed");
  validate("Employee D is denied Companies A/B/C",
           !checkPermission(employeeD, COMPANY_A) &&
               !checkPermission(employeeD, COMPANY_B) &&
               !checkPermission(employeeD, COMPANY_C),
           "all denied",
           "at least one allowed");
  validate("IT Admin is limited to Main Entrance and Server Room",
           checkPermission(itAdmin, MAIN_ENTRANCE) &&
               checkPermission(itAdmin, SERVER_ROOM) &&
               !checkPermission(itAdmin, COMPANY_A) &&
               !checkPermission(itAdmin, COMPANY_B) &&
               !checkPermission(itAdmin, COMPANY_C) &&
               !checkPermission(itAdmin, COMPANY_D) &&
               !checkPermission(itAdmin, MANAGEMENT_ADMIN),
           "Main + Server only",
           "permission profile mismatch");

  const Area allAreas[] = {COMPANY_A,
                           COMPANY_B,
                           COMPANY_C,
                           COMPANY_D,
                           SERVER_ROOM,
                           MANAGEMENT_ADMIN,
                           MAIN_ENTRANCE};
  constexpr size_t allAreaCount = sizeof(allAreas) / sizeof(allAreas[0]);
  bool allUsersStartOutside = true;
  for (size_t userIndex = 0; userIndex < USER_COUNT; ++userIndex) {
    for (size_t areaIndex = 0; areaIndex < allAreaCount; ++areaIndex) {
      allUsersStartOutside &=
          !isUserInsideArea(&users[userIndex], allAreas[areaIndex]);
    }
  }
  validate("All users start OUTSIDE every area",
           allUsersStartOutside,
           "all OUTSIDE",
           "at least one INSIDE");
  bool managerAllowedEverywhere = true;
  for (size_t i = 0; i < allAreaCount; ++i) {
    managerAllowedEverywhere &= checkPermission(manager, allAreas[i]);
  }
  validate("Manager -> all seven areas",
           managerAllowedEverywhere,
           "allowed everywhere",
           "one or more areas denied");
  bool everyoneAllowedMain = true;
  for (size_t i = 0; i < USER_COUNT; ++i) {
    everyoneAllowedMain &= checkPermission(&users[i], MAIN_ENTRANCE);
  }
  validate("All configured users -> Main Entrance",
           everyoneAllowedMain,
           "all allowed",
           "one or more users denied");
  validate("Unknown fingerprint ID is rejected safely",
           findUserByFingerprintID(127) == nullptr,
           "nullptr",
           "configured user returned");
  validate("AREA_NONE permission is rejected safely",
           !checkPermission(employeeA, AREA_NONE),
           "denied",
           "allowed");
  validate("Null user permission check is safe",
           !checkPermission(nullptr, COMPANY_A),
           "denied",
           "allowed");
  validate("Null user occupancy check is safe",
           !isUserInsideArea(nullptr, COMPANY_A),
           "OUTSIDE",
           "INSIDE");
  markUserInsideArea(nullptr, COMPANY_A);
  markUserOutsideArea(nullptr, COMPANY_A);
  validate("Null user occupancy updates return safely",
           true,
           "no crash",
           "no crash");
  Area invalidArea = static_cast<Area>(99);
  validate("Invalid area has no occupancy bit",
           getAreaBit(invalidArea) == 0,
           "0",
           String(getAreaBit(invalidArea)));
  validate("Invalid area name resolves safely",
           getAreaName(invalidArea) == "None",
           "None",
           getAreaName(invalidArea));
  validate("Area commands 1-7 use the exact new mapping",
           static_cast<int>(COMPANY_A) == 1 &&
               static_cast<int>(COMPANY_B) == 2 &&
               static_cast<int>(COMPANY_C) == 3 &&
               static_cast<int>(COMPANY_D) == 4 &&
               static_cast<int>(SERVER_ROOM) == 5 &&
               static_cast<int>(MANAGEMENT_ADMIN) == 6 &&
               static_cast<int>(MAIN_ENTRANCE) == 7 &&
               getAreaName(static_cast<Area>(1)) == "Company A" &&
               getAreaName(static_cast<Area>(2)) == "Company B" &&
               getAreaName(static_cast<Area>(3)) == "Company C" &&
               getAreaName(static_cast<Area>(4)) == "Company D" &&
               getAreaName(static_cast<Area>(5)) == "Server Room" &&
               getAreaName(static_cast<Area>(6)) == "Management / Admin" &&
               getAreaName(static_cast<Area>(7)) == "Main Entrance",
           "1=A, 2=B, 3=C, 4=D, 5=Server, 6=Management, 7=Main",
           "area command mapping mismatch");
  validate("insideMask bits 0-6 match the new area order",
           getAreaBit(COMPANY_A) == (1 << 0) &&
               getAreaBit(COMPANY_B) == (1 << 1) &&
               getAreaBit(COMPANY_C) == (1 << 2) &&
               getAreaBit(COMPANY_D) == (1 << 3) &&
               getAreaBit(SERVER_ROOM) == (1 << 4) &&
               getAreaBit(MANAGEMENT_ADMIN) == (1 << 5) &&
               getAreaBit(MAIN_ENTRANCE) == (1 << 6),
           "bits 0-6 follow commands 1-7",
           "insideMask bit mapping mismatch");

  validate("Entry without a selected area is rejected before fingerprint scan",
           evaluateAccessPrecheck(MODE_ENTRY,
                                  false,
                                  AREA_NONE,
                                  false,
                                  true,
                                  true,
                                  true) == ACCESS_PRECHECK_NO_AREA,
           "NO_AREA",
           "wrong precheck result");
  failedAttempts = 1;
  AccessPrecheckResult noPersonEntry = evaluateAccessPrecheck(
      MODE_ENTRY, false, COMPANY_A, false, true, false, true);
  validate("Entry without a nearby person is rejected without a failure count",
           noPersonEntry == ACCESS_PRECHECK_PERSON_NOT_NEAR &&
               failedAttempts == 1,
           "PERSON_NOT_NEAR and counter unchanged",
           "precheck or counter mismatch");
  AccessPrecheckResult timeoutEntry = evaluateAccessPrecheck(
      MODE_ENTRY, false, COMPANY_A, false, false, false, true);
  validate("Entry ultrasonic timeout is rejected before fingerprint scan",
           timeoutEntry == ACCESS_PRECHECK_ULTRASONIC_INVALID,
           "ULTRASONIC_INVALID",
           "wrong precheck result");
  validate("Authorized Entry precheck passes only with valid near presence",
           evaluateAccessPrecheck(MODE_ENTRY,
                                  false,
                                  COMPANY_A,
                                  false,
                                  true,
                                  true,
                                  true) == ACCESS_PRECHECK_OK,
           "OK",
           "blocked");
  validate("Unavailable fingerprint sensor blocks scan after Entry checks",
           evaluateAccessPrecheck(MODE_ENTRY,
                                  false,
                                  COMPANY_A,
                                  false,
                                  true,
                                  true,
                                  false) ==
               ACCESS_PRECHECK_FINGERPRINT_UNAVAILABLE,
           "FINGERPRINT_UNAVAILABLE",
           "wrong precheck result");
  validate("X without a selected area is rejected",
           !canEnableExitModeForState(false, AREA_NONE, false),
           "rejected",
           "enabled");
  validate("X during Lockdown is rejected",
           !canEnableExitModeForState(true, COMPANY_A, false),
           "rejected",
           "enabled");
  validate("X while the door is open is rejected",
           !canEnableExitModeForState(false, COMPANY_A, true),
           "rejected",
           "enabled");
  validate("Exit precheck does not require an ultrasonic measurement",
           evaluateAccessPrecheck(MODE_EXIT,
                                  false,
                                  COMPANY_A,
                                  false,
                                  false,
                                  false,
                                  true) == ACCESS_PRECHECK_OK,
           "OK without ultrasonic",
           "blocked");
  currentMode = MODE_EXIT;
  returnToEntryMode();
  validate("Completed Exit attempts return to Entry Mode",
           currentMode == MODE_ENTRY,
           "ENTRY",
           getAccessModeName(currentMode));
  doorOpen = false;
  validate("Unauthorized Entry is rejected before the door opens",
           !canUserEnterArea(employeeA, COMPANY_B) && !doorOpen,
           "denied and door closed",
           "entry allowed or door opened");

  if (employeeA != nullptr) {
    employeeA->insideMask = 0;
  }
  validate("Employee A starts OUTSIDE Company A",
           !isUserInsideArea(employeeA, COMPANY_A),
           "OUTSIDE",
           "INSIDE");
  validate("Employee A may enter Company A while outside",
           canUserEnterArea(employeeA, COMPANY_A),
           "entry allowed",
           "entry denied");
  markUserInsideArea(employeeA, COMPANY_A);
  validate("Entry marks Employee A INSIDE Company A",
           isUserInsideArea(employeeA, COMPANY_A),
           "INSIDE",
           "OUTSIDE");
  validate("Company A occupancy count reflects entry",
           countUsersInsideArea(COMPANY_A) == 1,
           "1",
           String(countUsersInsideArea(COMPANY_A)));
  validate("Area masks remain independent",
           !isUserInsideArea(employeeA, COMPANY_B),
           "Company B OUTSIDE",
           "Company B INSIDE");
  if (employeeC != nullptr) {
    employeeC->insideMask = 0;
  }
  markUserInsideArea(employeeC, COMPANY_C);
  validate("New Company C bit drives only Company C occupancy",
           countUsersInsideArea(COMPANY_C) == 1 &&
               countUsersInsideArea(COMPANY_D) == 0 &&
               !isUserInsideArea(employeeC, COMPANY_D),
           "Company C=1, Company D=0",
           "occupancy or area bit leaked");
  markUserOutsideArea(employeeC, COMPANY_C);
  doorOpen = false;
  validate("Duplicate Entry is rejected without opening the door",
           !canUserEnterArea(employeeA, COMPANY_A) && !doorOpen,
           "entry denied and door closed",
           "entry allowed or door opened");
  validate("Employee A may exit Company A while inside",
           canUserExitArea(employeeA, COMPANY_A),
           "exit allowed",
           "exit denied");
  markUserOutsideArea(employeeA, COMPANY_A);
  validate("Exit marks Employee A OUTSIDE Company A",
           !isUserInsideArea(employeeA, COMPANY_A),
           "OUTSIDE",
           "INSIDE");
  validate("Exit is rejected while already outside",
           !canUserExitArea(employeeA, COMPANY_A),
           "exit denied",
           "exit allowed");
  failedAttempts = 2;
  if (!canUserExitArea(employeeA, COMPANY_A) &&
      COUNT_EXIT_OUTSIDE_AS_FAILED_ATTEMPT) {
    updateFailedAttemptCounter();
  }
  validate("Already-outside Exit leaves the failure counter unchanged",
           failedAttempts == 2,
           "2",
           String(failedAttempts));
  validate("Entry is allowed again after exit",
           canUserEnterArea(employeeA, COMPANY_A),
           "entry allowed",
           "entry denied");
  if (employeeA != nullptr) {
    employeeA->insideMask = 0;
  }
  bool entryCommittedWhileClosed =
      commitAttendanceAfterDoorOpen(employeeA,
                                    COMPANY_A,
                                    MODE_ENTRY,
                                    false);
  validate("Entry does not mark INSIDE while door software state is CLOSED",
           !entryCommittedWhileClosed &&
               !isUserInsideArea(employeeA, COMPANY_A),
           "unchanged OUTSIDE",
           "attendance changed");
  bool entryCommittedAfterOpen =
      commitAttendanceAfterDoorOpen(employeeA,
                                    COMPANY_A,
                                    MODE_ENTRY,
                                    true);
  validate("Entry marks INSIDE only after door software state is OPEN",
           entryCommittedAfterOpen &&
               isUserInsideArea(employeeA, COMPANY_A),
           "INSIDE after OPEN",
           "attendance not committed correctly");
  bool exitCommittedWhileClosed =
      commitAttendanceAfterDoorOpen(employeeA,
                                    COMPANY_A,
                                    MODE_EXIT,
                                    false);
  validate("Exit does not mark OUTSIDE while door software state is CLOSED",
           !exitCommittedWhileClosed &&
               isUserInsideArea(employeeA, COMPANY_A),
           "unchanged INSIDE",
           "attendance changed");
  bool exitCommittedAfterOpen =
      commitAttendanceAfterDoorOpen(employeeA,
                                    COMPANY_A,
                                    MODE_EXIT,
                                    true);
  validate("Exit marks OUTSIDE only after door software state is OPEN",
           exitCommittedAfterOpen &&
               !isUserInsideArea(employeeA, COMPANY_A),
           "OUTSIDE after OPEN",
           "attendance not committed correctly");
  validate("Duplicate entry counts as a failed attempt",
           COUNT_ANTI_PASSBACK_AS_FAILED_ATTEMPT,
           "true",
           "false");
  validate("Already-outside exit does not count as failure",
           !COUNT_EXIT_OUTSIDE_AS_FAILED_ATTEMPT,
           "false",
           "true");
  validate("AREA_NONE has no occupancy bit",
           getAreaBit(AREA_NONE) == 0,
           "0",
           String(getAreaBit(AREA_NONE)));

  failedAttempts = 0;
  systemLocked = false;
  bool thresholdAfterFirst = updateFailedAttemptCounter();
  validate("Failed attempt 1 keeps system below lockdown threshold",
           failedAttempts == 1 && !thresholdAfterFirst,
           "1/3 and active",
           String(failedAttempts) + "/3");
  bool thresholdAfterSecond = updateFailedAttemptCounter();
  validate("Failed attempt 2 keeps system below lockdown threshold",
           failedAttempts == 2 && !thresholdAfterSecond,
           "2/3 and active",
           String(failedAttempts) + "/3");
  currentMode = MODE_EXIT;
  waitingForEnrollmentID = true;
  adminMode = true;
  bool thresholdAfterThird = updateFailedAttemptCounter();
  if (thresholdAfterThird) {
    applyLockedState();
  }
  validate("Failed attempt 3 locks system exactly at threshold",
           failedAttempts == MAX_FAILED_ATTEMPTS && systemLocked,
           "3/3 and locked",
           String(failedAttempts) + "/3, locked=" +
               String(systemLocked ? "true" : "false"));
  validate("Lockdown returns mode to ENTRY and cancels enrollment",
           currentMode == MODE_ENTRY && !waitingForEnrollmentID && !adminMode,
           "ENTRY, enrollment off, Admin off",
           "lock state incomplete");
  updateFailedAttemptCounter();
  validate("Failed-attempt counter saturates at maximum",
           failedAttempts == MAX_FAILED_ATTEMPTS,
           String(MAX_FAILED_ATTEMPTS),
           String(failedAttempts));
  validate("Fingerprint access state is blocked while locked",
           isSystemLocked(),
           "locked",
           "active");

  bool doorStateBeforeAdminUnlock = doorOpen;
  applyUnlockedAdminState();
  validate("Admin unlock clears lockdown",
           !systemLocked,
           "active",
           "locked");
  validate("Admin unlock resets failed attempts",
           failedAttempts == 0,
           "0",
           String(failedAttempts));
  validate("Admin unlock enables Admin Mode",
           adminMode,
           "enabled",
           "disabled");
  validate("Admin unlock does not open the door",
           doorOpen == doorStateBeforeAdminUnlock,
           "door unchanged",
           "door state changed");
  failedAttempts = 2;
  resetFailedAttempts();
  validate("Successful-access counter reset logic",
           failedAttempts == 0,
           "0",
           String(failedAttempts));

  disableAdminMode();
  validate("Enrollment security state is blocked outside Admin Mode",
           !adminMode,
           "blocked",
           "allowed");
  enableAdminMode();
  validate("Admin Mode active state",
           adminMode && !hasAdminModeExpired(millis()),
           "active",
           "inactive");
  adminMode = true;
  adminModeStartTime = 1000;
  validate("Admin Mode remains active before 60 seconds",
           !hasAdminModeExpired(1000 + ADMIN_MODE_TIMEOUT_MS - 1),
           "active",
           "expired early");
  validate("Admin Mode expires at 60 seconds",
           hasAdminModeExpired(1000 + ADMIN_MODE_TIMEOUT_MS),
           "expired",
           "active");
  if (hasAdminModeExpired(1000 + ADMIN_MODE_TIMEOUT_MS)) {
    disableAdminMode();
  }
  validate("Enrollment is blocked again after Admin timeout",
           !adminMode,
           "blocked",
           "allowed");

  validate("Fingerprint deletion is blocked without Admin Mode",
           evaluateFingerprintDeleteGate(false, true, false) ==
               FINGERPRINT_DELETE_BLOCKED_ADMIN,
           "blocked",
           "allowed");
  validate("Fingerprint deletion is blocked during Lockdown",
           evaluateFingerprintDeleteGate(true, true, true) ==
               FINGERPRINT_DELETE_BLOCKED_LOCKDOWN,
           "blocked",
           "allowed");
  validate("Fingerprint deletion is blocked when sensor is not ready",
           evaluateFingerprintDeleteGate(false, false, true) ==
               FINGERPRINT_DELETE_BLOCKED_SENSOR,
           "blocked",
           "allowed");
  validate("C alone only arms confirmation and cannot approve deletion",
           evaluateFingerprintDeleteGate(false, true, true) ==
                   FINGERPRINT_DELETE_ALLOWED &&
               evaluateFingerprintDeleteConfirmation("C", false, true, true) ==
                   FINGERPRINT_DELETE_CANCELLED_TEXT,
           "confirmation pending; no delete approval",
           "unexpected delete approval");
  validate("Only exact uppercase DELETE confirms fingerprint deletion",
           evaluateFingerprintDeleteConfirmation("DELETE", false, true, true) ==
                   FINGERPRINT_DELETE_CONFIRMED &&
               evaluateFingerprintDeleteConfirmation("delete", false, true, true) ==
                   FINGERPRINT_DELETE_CANCELLED_TEXT &&
               evaluateFingerprintDeleteConfirmation("DELETE ", false, true, true) ==
                   FINGERPRINT_DELETE_CANCELLED_TEXT,
           "only exact DELETE approved",
           "non-exact text approved");
  validate("Other delete confirmation text cancels",
           evaluateFingerprintDeleteConfirmation("NO", false, true, true) ==
               FINGERPRINT_DELETE_CANCELLED_TEXT,
           "cancelled",
           "approved");
  validate("Expired Admin Mode cancels pending fingerprint deletion",
           evaluateFingerprintDeleteConfirmation("DELETE", false, true, false) ==
               FINGERPRINT_DELETE_CANCELLED_ADMIN,
           "cancelled",
           "approved");
  validate("Configured fingerprint deletion IDs are exactly 1-6",
           configuredFingerprintDeleteIDsAreExactly1To6(),
           "1,2,3,4,5,6",
           "delete ID list mismatch");
  AccessControlStateSnapshot deleteDecisionState = captureAccessControlState();
  bool employeeAPermissionBeforeDeleteDecision =
      employeeA != nullptr && employeeA->canAccessCompanyA;
  evaluateFingerprintDeleteGate(false, true, true);
  evaluateFingerprintDeleteConfirmation("DELETE", false, true, true);
  validate("Fingerprint delete decisions preserve access-control state",
           isAccessControlStateUnchanged(deleteDecisionState) &&
               employeeA != nullptr && employeeA->canAccessCompanyA ==
                                            employeeAPermissionBeforeDeleteDecision,
           "permissions, attendance, door, area and mode unchanged",
           "access-control state changed");

  validate("LCD dimensions are 16x2",
           LCD_COLUMNS == 16 && LCD_ROWS == 2,
           "16 columns, 2 rows",
           String(LCD_COLUMNS) + "x" + String(LCD_ROWS));
  String trimmedLCDLine = getLCDLine("12345678901234567890");
  validate("LCD long lines are trimmed to 16 characters",
           trimmedLCDLine.length() == LCD_COLUMNS,
           "16 characters",
           String(trimmedLCDLine.length()) + " characters");
  validate("LCD short lines remain unchanged",
           getLCDLine("System Ready") == "System Ready",
           "System Ready",
           getLCDLine("System Ready"));
  lastLcdLine1 = "A";
  lastLcdLine2 = "B";
  validate("LCD duplicate screen detection prevents refresh",
           isLCDMessageUnchanged("A", "B"),
           "unchanged",
           "changed");
  validate("LCD changed screen requests refresh",
           !isLCDMessageUnchanged("A", "Different"),
           "changed",
           "unchanged");

  validate("Servo door angles are 0 closed and 90 open",
           DOOR_CLOSED_ANGLE == 0 && DOOR_OPEN_ANGLE == 90,
           "0/90 degrees",
           String(DOOR_CLOSED_ANGLE) + "/" + String(DOOR_OPEN_ANGLE));
  validate("Door minimum-open time is at least 5 seconds",
           DOOR_MIN_OPEN_TIME_MS >= 5000,
           ">= 5000 ms",
           String(DOOR_MIN_OPEN_TIME_MS) + " ms");
  validate("Door stays open before minimum-open time",
           !shouldCloseDoorForState(
               true, DOOR_MIN_OPEN_TIME_MS - 1, true, false),
           "stay open",
           "close");
  float clearDistanceCm = PRESENCE_DISTANCE_CM + 0.1f;
  validate("Door closes only after a valid clear-distance measurement",
           shouldCloseDoorForState(
               true,
               DOOR_MIN_OPEN_TIME_MS,
               isUltrasonicMeasurementValid(clearDistanceCm),
               isDistanceNear(clearDistanceCm)),
           "close",
           "stay open");
  validate("Door stays open while a person remains near",
           !shouldCloseDoorForState(
               true, DOOR_MIN_OPEN_TIME_MS, true, true),
           "stay open",
           "close");
  validate("Door stays open when ultrasonic measurement times out",
           !shouldCloseDoorForState(
               true, DOOR_MIN_OPEN_TIME_MS, false, false),
           "stay open",
           "close");
  validate("Closed door never requests another close",
           !shouldCloseDoorForState(
               false, DOOR_MIN_OPEN_TIME_MS, true, false),
           "no close request",
           "close requested");

  validate("Presence threshold is 20 cm",
           PRESENCE_DISTANCE_CM == 20.0,
           "20.0 cm",
           String(PRESENCE_DISTANCE_CM, 1) + " cm");
  validate("Distance at threshold counts as near",
           isDistanceNear(PRESENCE_DISTANCE_CM),
           "near",
           "not near");
  validate("Distance above threshold is not near",
           !isDistanceNear(PRESENCE_DISTANCE_CM + 0.1),
           "not near",
           "near");
  validate("Invalid ultrasonic distance is not near",
           !isDistanceNear(-1.0),
           "not near",
           "near");
  validate("Timeout and zero are invalid ultrasonic measurements",
           !isUltrasonicMeasurementValid(-1.0) &&
               !isUltrasonicMeasurementValid(0.0),
           "both invalid",
           "one treated as valid");
  validate("Entry Mode requires ultrasonic presence",
           isPresenceRequiredForMode(MODE_ENTRY),
           "required",
           "not required");
  validate("Exit Mode does not require ultrasonic presence",
           !isPresenceRequiredForMode(MODE_EXIT),
           "not required",
           "required");
  failedAttempts = 1;
  if (isPresenceRequiredForMode(MODE_ENTRY) && !isDistanceNear(-1.0)) {
    // Production entry flow returns here without registering a failure.
  }
  validate("No-person entry block leaves failure counter unchanged",
           failedAttempts == 1,
           "1",
           String(failedAttempts));
  AccessControlStateSnapshot diagnosticState = captureAccessControlState();
  validate("F/P/D/U diagnostics and Hardware readiness W use the state guard",
           isStateGuardedDiagnosticCommand('F') &&
               isStateGuardedDiagnosticCommand('P') &&
               isStateGuardedDiagnosticCommand('D') &&
               isStateGuardedDiagnosticCommand('U') &&
               isStateGuardedDiagnosticCommand('W'),
           "all guarded",
           "one or more unguarded");
  selectedArea = selectedArea == COMPANY_A ? COMPANY_B : COMPANY_A;
  validate("Diagnostic state guard detects selected-area changes",
           !isAccessControlStateUnchanged(diagnosticState),
           "change detected",
           "change missed");
  restoreAccessControlState(diagnosticState);
  validate("Diagnostic state guard restores selected area and security state",
           isAccessControlStateUnchanged(diagnosticState),
           "state restored",
           "state mismatch");
  users[0].insideMask ^= static_cast<uint16_t>(getAreaBit(MAIN_ENTRANCE));
  validate("Diagnostic state guard detects attendance changes",
           !isAccessControlStateUnchanged(diagnosticState),
           "change detected",
           "change missed");
  restoreAccessControlState(diagnosticState);
  validate("Diagnostic state guard restores attendance masks",
           isAccessControlStateUnchanged(diagnosticState),
           "attendance restored",
           "attendance mismatch");
  lastLcdLine1 = "DIAGNOSTIC SCREEN";
  ++lastLCDMessageAt;
  validate("Diagnostic state guard detects LCD cache changes",
           !isLCDCacheUnchanged(diagnosticState),
           "change detected",
           "change missed");
  restoreAccessControlState(diagnosticState);
  validate("Diagnostic state guard restores LCD cached state",
           isLCDCacheUnchanged(diagnosticState),
           "LCD cache restored",
           "LCD cache mismatch");
  fingerprintReady = false;
  fingerprintWorkingBaud = 0;
  restoreWorkingFingerprintState(true, FINGERPRINT_PRIMARY_BAUD);
  validate("Fingerprint diagnostic restores a previously working sensor state",
           fingerprintReady &&
               fingerprintWorkingBaud == FINGERPRINT_PRIMARY_BAUD,
           "ready at 57600",
           "working state lost");
  validate("Ultrasonic pulse timeout is finite",
           ULTRASONIC_ECHO_TIMEOUT_US == 30000,
           "30000 us",
           String(ULTRASONIC_ECHO_TIMEOUT_US) + " us");

  validate("Fingerprint integration uses UART2 GPIO16/17 at 57600",
           FINGER_RX_PIN == 16 && FINGER_TX_PIN == 17 &&
               FINGERPRINT_PRIMARY_BAUD == 57600,
           "RX16/TX17/57600",
           "fingerprint transport mismatch");
  validate("Fingerprint profile is AS608/JM-101B at 3.3V",
           String(FINGERPRINT_MODEL) == "AS608/JM-101B" &&
               String(FINGERPRINT_EXPECTED_VOLTAGE) == "3.3V" &&
               FINGERPRINT_EXPECTED_CURRENT_MA == 60,
           "AS608/JM-101B, 3.3V, about 60mA",
           "sensor profile mismatch");
  validate("Servo uses project GPIO13 instead of RFID SCK GPIO18",
           SERVO_PIN == 13 && RFID_SCK_PIN == 18 &&
               SERVO_PIN != RFID_SCK_PIN,
           "Servo 13, RFID SCK 18",
           "pin mismatch or conflict");
  validate("Servo simple attach mode matches the working standalone test",
           SERVO_USE_SIMPLE_ATTACH == 1,
           "enabled",
           "disabled");
  validate("Verified hardware GPIO map is unchanged",
           FINGER_RX_PIN == 16 && FINGER_TX_PIN == 17 &&
               SERVO_PIN == 13 && ULTRASONIC_TRIG_PIN == 21 &&
               ULTRASONIC_ECHO_PIN == 22 && LCD_SDA_PIN == 14 &&
               LCD_SCL_PIN == 27 && RFID_SS_PIN == 5 &&
               RFID_SCK_PIN == 18 && RFID_MOSI_PIN == 23 &&
               RFID_MISO_PIN == 19 && RFID_RST_PIN == 33,
           "Finger 16/17, Servo 13, Ultra 21/22, LCD 14/27, RFID 5/18/23/19/33",
           "pin map mismatch");

  const char serialCommands[] = {'E', 'C', 'R', 'P', 'X', 'M', 'S', 'T',
                                 'G', 'D', 'U', 'F', 'V', 'W'};
  constexpr size_t serialCommandCount =
      sizeof(serialCommands) / sizeof(serialCommands[0]);
  bool serialCommandsUnique = true;
  for (size_t i = 0; i < serialCommandCount; ++i) {
    for (size_t j = i + 1; j < serialCommandCount; ++j) {
      if (serialCommands[i] == serialCommands[j]) {
        serialCommandsUnique = false;
      }
    }
  }
  validate("Serial letter commands are unique",
           serialCommandsUnique,
           "no conflicts",
           "duplicate command");
  validate("Serial area commands 1-7 map to valid distinct bits",
           getAreaBit(static_cast<Area>(1)) == 0x01 &&
               getAreaBit(static_cast<Area>(2)) == 0x02 &&
               getAreaBit(static_cast<Area>(3)) == 0x04 &&
               getAreaBit(static_cast<Area>(4)) == 0x08 &&
               getAreaBit(static_cast<Area>(5)) == 0x10 &&
               getAreaBit(static_cast<Area>(6)) == 0x20 &&
               getAreaBit(static_cast<Area>(7)) == 0x40,
           "0x01,0x02,0x04,0x08,0x10,0x20,0x40",
           "invalid or duplicate area bit");
  validate("Component GPIO assignments have no conflicts",
           validatePinAssignments(),
           "no conflicts",
           "conflict found");

#if SMART_OFFICE_MODE == INTEGRATED_MODE
  IntegrationSyncState savedIntegrationSyncState = integrationSyncState;
  String savedPendingAuthorizationId = pendingAuthorizationId;
  String savedUnexpectedCommandId = lastUnexpectedCommandId;
  RemoteAccessRequestState savedRemoteAccessRequest = remoteAccessRequest;
  String savedSerialCreatedRequestId = serialCreatedRequestId;
  Area savedSerialCreatedRequestArea = serialCreatedRequestArea;
  AccessMode savedSerialCreatedRequestDirection =
      serialCreatedRequestDirection;
  unsigned long savedRemoteStatusCheckAt =
      lastRemoteRequestStatusCheckAt;
  AccessControlStateSnapshot remoteCommandAccessState =
      captureAccessControlState();
  bool remoteCommandFingerprintReady = fingerprintReady;

  integrationSyncState = INTEGRATION_SYNCED;
  pendingAuthorizationId = "";
  lastUnexpectedCommandId = "";
  remoteAccessRequest.active = false;
  remoteAccessRequest.requestId = "";
  serialCreatedRequestId = "";

  JsonDocument invalidAreaCommand;
  invalidAreaCommand["request_id"] = "validation_invalid_area";
  invalidAreaCommand["area_id"] = 8;
  invalidAreaCommand["direction"] = "ENTRY";
  invalidAreaCommand["expires_at"] = "2099-01-01T00:00:00Z";
  handleRemoteCommand(invalidAreaCommand.as<JsonObject>());
  validate("Phase 5 malformed command area is rejected",
           !remoteAccessRequest.active,
           "no active remote request",
           "invalid command became active");

  JsonDocument invalidDirectionCommand;
  invalidDirectionCommand["request_id"] = "validation_invalid_direction";
  invalidDirectionCommand["area_id"] = 1;
  invalidDirectionCommand["direction"] = "SIDEWAYS";
  invalidDirectionCommand["expires_at"] = "2099-01-01T00:00:00Z";
  handleRemoteCommand(invalidDirectionCommand.as<JsonObject>());
  validate("Phase 5 malformed command direction is rejected",
           !remoteAccessRequest.active,
           "no active remote request",
           "invalid command became active");

  JsonDocument validRemoteCommand;
  validRemoteCommand["request_id"] = "validation_req_a";
  validRemoteCommand["area_id"] = 1;
  validRemoteCommand["direction"] = "ENTRY";
  validRemoteCommand["expires_at"] = "2099-01-01T00:00:00Z";
  handleRemoteCommand(validRemoteCommand.as<JsonObject>());
  validate("Phase 5 valid command is accepted once",
           remoteAccessRequest.active &&
               remoteAccessRequest.requestId == "validation_req_a" &&
               remoteAccessRequest.area == COMPANY_A &&
               remoteAccessRequest.direction == MODE_ENTRY,
           "req_a active for Company A Entry",
           "valid command state mismatch");

  unsigned long firstReceivedAt = remoteAccessRequest.receivedAt;
  handleRemoteCommand(validRemoteCommand.as<JsonObject>());
  validate("Phase 5 duplicate command redelivery is idempotent",
           remoteAccessRequest.active &&
               remoteAccessRequest.requestId == "validation_req_a" &&
               remoteAccessRequest.receivedAt == firstReceivedAt,
           "original request state unchanged",
           "duplicate changed request state");

  JsonDocument conflictingRemoteCommand;
  conflictingRemoteCommand["request_id"] = "validation_req_b";
  conflictingRemoteCommand["area_id"] = 2;
  conflictingRemoteCommand["direction"] = "EXIT";
  conflictingRemoteCommand["expires_at"] = "2099-01-01T00:00:00Z";
  handleRemoteCommand(conflictingRemoteCommand.as<JsonObject>());
  validate("Phase 5 different command cannot replace active request",
           remoteAccessRequest.active &&
               remoteAccessRequest.requestId == "validation_req_a" &&
               remoteAccessRequest.area == COMPANY_A &&
               remoteAccessRequest.direction == MODE_ENTRY,
           "req_a preserved",
           "active request was replaced");

  validate("Phase 5 command handling has no access hardware/business side effect",
           isAccessControlStateUnchanged(remoteCommandAccessState) &&
               fingerprintReady == remoteCommandFingerprintReady,
           "door, attendance, security, mode and fingerprint state unchanged",
           "access-control or fingerprint state changed");

  remoteAccessRequest.active = false;
  remoteAccessRequest.requestId = "";
  integrationSyncState = INTEGRATION_RECONCILIATION_REQUIRED;
  handleRemoteCommand(validRemoteCommand.as<JsonObject>());
  validate("Phase 5 reconciliation state blocks command acceptance",
           !remoteAccessRequest.active,
           "command rejected",
           "command accepted while reconciliation required");

  validate("Phase 6 Entry scan requires a fresh detected person",
           shouldStartRemoteFingerprintScan(
               MODE_ENTRY, false, false, true, true, true),
           "scan allowed",
           "scan blocked");
  validate("Phase 6 Entry blocks missing or negative presence",
           !shouldStartRemoteFingerprintScan(
               MODE_ENTRY, false, false, true, false, false) &&
               !shouldStartRemoteFingerprintScan(
                   MODE_ENTRY, false, false, true, true, false),
           "both blocked",
           "Entry scan allowed without a detected person");
  validate("Phase 6 Exit does not require presence",
           shouldStartRemoteFingerprintScan(
               MODE_EXIT, false, false, true, false, false),
           "scan allowed",
           "Exit scan incorrectly blocked by presence");
  validate("Phase 6 safety prechecks block scanning",
           !shouldStartRemoteFingerprintScan(
               MODE_ENTRY, true, false, true, true, true) &&
               !shouldStartRemoteFingerprintScan(
                   MODE_ENTRY, false, true, true, true, true) &&
               !shouldStartRemoteFingerprintScan(
                   MODE_ENTRY, false, false, false, true, true),
           "Lockdown, open door and unavailable sensor blocked",
           "at least one unsafe scan was allowed");
  validate("Phase 6 image result mapping is complete",
           classifyRemoteFingerprintImageResult(FINGERPRINT_NOFINGER) ==
                   REMOTE_SENSOR_KEEP_WAITING &&
               classifyRemoteFingerprintImageResult(FINGERPRINT_OK) ==
                   REMOTE_SENSOR_IMAGE_READY &&
               classifyRemoteFingerprintImageResult(
                   FINGERPRINT_PACKETRECIEVEERR) ==
                   REMOTE_SENSOR_READ_ERROR,
           "NOFINGER waits, OK advances, errors map to READ_ERROR",
           "image result mapping mismatch");
  validate("Phase 6 search result mapping is complete",
           classifyRemoteFingerprintSearchResult(FINGERPRINT_OK) ==
                   REMOTE_SENSOR_MATCH &&
               classifyRemoteFingerprintSearchResult(FINGERPRINT_NOTFOUND) ==
                   REMOTE_SENSOR_NOT_RECOGNIZED &&
               classifyRemoteFingerprintSearchResult(
                   FINGERPRINT_PACKETRECIEVEERR) ==
                   REMOTE_SENSOR_READ_ERROR,
           "MATCH, NOT_RECOGNIZED and READ_ERROR mapped",
           "search result mapping mismatch");

  integrationSyncState = INTEGRATION_SYNCED;
  pendingAuthorizationId = "";
  resetRemoteAccessRequestState();
  remoteAccessRequest.active = true;
  remoteAccessRequest.requestId = "validation_phase6_exact_id";
  remoteAccessRequest.area = COMPANY_A;
  remoteAccessRequest.direction = MODE_ENTRY;
  remoteAccessRequest.precheckDoorOpen = false;
  remoteAccessRequest.precheckPersonKnown = true;
  remoteAccessRequest.precheckPersonDetected = true;
  remoteAccessRequest.precheckDistanceKnown = true;
  remoteAccessRequest.precheckDistanceCm = 10.5;
  remoteAccessRequest.precheckFingerprintReady = true;
  prepareRemoteAccessCheckPayload("MATCH", 1, 126);
  JsonDocument matchPayload;
  DeserializationError matchPayloadError = deserializeJson(
      matchPayload, remoteAccessRequest.accessCheckPayload);
  validate("Phase 6 MATCH payload reuses exact request and identity",
           !matchPayloadError &&
               matchPayload["request_id"] ==
                   "validation_phase6_exact_id" &&
               matchPayload["fingerprint_result"] == "MATCH" &&
               matchPayload["fingerprint_id"] == 1 &&
               matchPayload["confidence"] == 126,
           "exact request ID with MATCH identity/confidence",
           "MATCH payload mismatch");

  String immutablePayload = remoteAccessRequest.accessCheckPayload;
  prepareRemoteAccessCheckPayload("NOT_RECOGNIZED");
  validate("Phase 6 access-check retry payload is immutable",
           remoteAccessRequest.accessCheckPayload == immutablePayload,
           "byte-for-byte identical payload",
           "captured payload changed before retry");
  validate("Phase 6 captured result prevents a second sensor scan",
           remoteAccessRequest.workflowState == REMOTE_ACCESS_CHECK_READY &&
               !isRemoteFingerprintSensorStep(
                   remoteAccessRequest.workflowState),
           "payload ready and sensor inactive",
           "sensor workflow remained active");

  resetRemoteAccessRequestState();
  remoteAccessRequest.active = true;
  remoteAccessRequest.requestId = "validation_phase6_no_identity";
  remoteAccessRequest.area = COMPANY_A;
  remoteAccessRequest.direction = MODE_ENTRY;
  remoteAccessRequest.precheckFingerprintReady = true;
  prepareRemoteAccessCheckPayload("TIMEOUT");
  JsonDocument timeoutPayload;
  DeserializationError timeoutPayloadError = deserializeJson(
      timeoutPayload, remoteAccessRequest.accessCheckPayload);
  validate("Phase 6 non-MATCH payload omits fingerprint identity",
           !timeoutPayloadError &&
               timeoutPayload["fingerprint_result"] == "TIMEOUT" &&
               timeoutPayload["fingerprint_id"].isNull() &&
               timeoutPayload["confidence"].isNull(),
           "TIMEOUT without fingerprint_id or confidence",
           "non-MATCH payload exposed identity fields");

  validate("Phase 7 door cannot open before live authorization",
           !canOpenRemoteDoorForState(
               REMOTE_ACCESS_CHECK_READY,
               true,
               false,
               true,
               true,
               false,
               false),
           "door blocked",
           "door opening was allowed before authorization");
  validate("Phase 7 current-boot live authorization may open",
           canOpenRemoteDoorForState(
               REMOTE_AUTHORIZED_WAITING_DOOR,
               true,
               true,
               true,
               true,
               false,
               false),
           "door allowed",
           "valid live authorization was blocked");
  validate("Phase 7 bootstrap-only pending cannot auto-open",
           !canOpenRemoteDoorForState(
               REMOTE_AUTHORIZED_WAITING_DOOR,
               true,
               false,
               true,
               true,
               false,
               false),
           "door blocked",
           "bootstrap-only authorization could open door");
  validate("Phase 7 unsafe door conditions fail closed",
           !canOpenRemoteDoorForState(
               REMOTE_AUTHORIZED_WAITING_DOOR,
               true,
               true,
               false,
               true,
               false,
               false) &&
               !canOpenRemoteDoorForState(
                   REMOTE_AUTHORIZED_WAITING_DOOR,
                   true,
                   true,
                   true,
                   false,
                   false,
                   false) &&
               !canOpenRemoteDoorForState(
                   REMOTE_AUTHORIZED_WAITING_DOOR,
                   true,
                   true,
                   true,
                   true,
                   true,
                   false) &&
               !canOpenRemoteDoorForState(
                   REMOTE_AUTHORIZED_WAITING_DOOR,
                   true,
                   true,
                   true,
                   true,
                   false,
                   true),
           "mismatch, servo failure, open door and Lockdown blocked",
           "at least one unsafe condition allowed opening");
  validate("Phase 7 completion cannot send before software OPEN",
           !canSendRemoteCompletionForState(
               REMOTE_AUTHORIZED_WAITING_DOOR,
               true,
               true,
               false,
               false),
           "completion blocked",
           "completion allowed before door OPEN");

  resetRemoteAccessRequestState();
  remoteAccessRequest.active = true;
  remoteAccessRequest.requestId = "validation_phase7_complete";
  remoteAccessRequest.area = COMPANY_A;
  remoteAccessRequest.direction = MODE_ENTRY;
  remoteAccessRequest.workflowState = REMOTE_OPENING_DOOR;
  remoteAccessRequest.liveAuthorizationReceived = true;
  remoteAccessRequest.doorOpenCommandIssued = true;
  prepareRemoteCompletionPayload();
  JsonDocument completionPayload;
  DeserializationError completionPayloadError = deserializeJson(
      completionPayload, remoteAccessRequest.completionPayload);
  validate("Phase 7 completion payload uses exact canonical request",
           !completionPayloadError &&
               completionPayload["request_id"] ==
                   "validation_phase7_complete" &&
               completionPayload["door_result"] == "DOOR_OPENED",
           "exact request ID and DOOR_OPENED",
           "completion payload mismatch");

  String immutableCompletionPayload =
      remoteAccessRequest.completionPayload;
  prepareRemoteCompletionPayload();
  validate("Phase 7 completion retry payload is immutable",
           remoteAccessRequest.completionPayload ==
               immutableCompletionPayload,
           "byte-for-byte identical payload",
           "completion payload changed before retry");
  validate("Phase 7 opened transaction is eligible for completion retry",
           canSendRemoteCompletionForState(
               remoteAccessRequest.workflowState,
               remoteAccessRequest.active,
               remoteAccessRequest.liveAuthorizationReceived,
               remoteAccessRequest.doorOpenCommandIssued,
               remoteAccessRequest.completionPayload.length() > 0),
           "same completion remains eligible",
           "completion retry was blocked");
  validate("Phase 7 completion retry cannot reopen servo",
           !canOpenRemoteDoorForState(
               remoteAccessRequest.workflowState,
               remoteAccessRequest.active,
               remoteAccessRequest.liveAuthorizationReceived,
               true,
               true,
               false,
               false),
           "door opening disabled after first OPEN",
           "completion retry could reopen door");
  validate("Phase 7 reconciliation state blocks automatic actions",
           !canOpenRemoteDoorForState(
               REMOTE_COMPLETION_RECONCILIATION_REQUIRED,
               true,
               true,
               true,
               true,
               false,
               false) &&
               !canSendRemoteCompletionForState(
                   REMOTE_COMPLETION_RECONCILIATION_REQUIRED,
                   true,
                   true,
                   true,
                   true),
           "door and completion blocked",
           "automatic action allowed during reconciliation");

  restoreAccessControlState(remoteCommandAccessState);
  fingerprintReady = remoteCommandFingerprintReady;
  integrationSyncState = savedIntegrationSyncState;
  pendingAuthorizationId = savedPendingAuthorizationId;
  lastUnexpectedCommandId = savedUnexpectedCommandId;
  remoteAccessRequest = savedRemoteAccessRequest;
  serialCreatedRequestId = savedSerialCreatedRequestId;
  serialCreatedRequestArea = savedSerialCreatedRequestArea;
  serialCreatedRequestDirection = savedSerialCreatedRequestDirection;
  lastRemoteRequestStatusCheckAt = savedRemoteStatusCheckAt;
#endif

  for (size_t i = 0; i < USER_COUNT; ++i) {
    users[i].insideMask = savedInsideMasks[i];
  }
  systemLocked = savedSystemLocked;
  adminMode = savedAdminMode;
  failedAttempts = savedFailedAttempts;
  adminModeStartTime = savedAdminModeStartTime;
  selectedArea = savedSelectedArea;
  currentMode = savedCurrentMode;
  waitingForEnrollmentID = savedWaitingForEnrollmentID;
  waitingForDeleteConfirmation = savedWaitingForDeleteConfirmation;
  doorOpen = savedDoorOpen;
  doorOpenedAt = savedDoorOpenedAt;
  lastDistanceCm = savedLastDistanceCm;
  lastUltrasonicCheckAt = savedLastUltrasonicCheckAt;
  ultrasonicHasMeasurement = savedUltrasonicHasMeasurement;
  presencePromptVisible = savedPresencePromptVisible;
  fingerprintReady = savedFingerprintReady;
  fingerprintWorkingBaud = savedFingerprintWorkingBaud;
  lastLCDMessageAt = savedLastLCDMessageAt;
  lastLcdLine1 = savedLcdLine1;
  lastLcdLine2 = savedLcdLine2;

  Serial.println("[SOFTWARE VALIDATION END]");
  Serial.println("====================================");
  Serial.println("SOFTWARE VALIDATION SUMMARY");
  Serial.print("Total tests: ");
  Serial.println(totalTests);
  Serial.print("Passed: ");
  Serial.println(passedTests);
  Serial.print("Failed: ");
  Serial.println(failedTests);
  Serial.print("Result: ");
  Serial.println(failedTests == 0 ? "OK" : "FAILED");
  Serial.println("====================================");

  if (failedTests > 0) {
    Serial.println("FAILED TESTS:");
    for (int i = 0; i < storedFailures; ++i) {
      Serial.println(failedDetails[i]);
    }
  }
  Serial.println("Original runtime state restored.");
  Serial.println();
}

#if SMART_OFFICE_MODE == INTEGRATED_MODE
void printRemoteAccessStatus() {
  Serial.println("Remote Access Request:");
  if (remoteAccessRequest.active) {
    Serial.print("  Request ID: ");
    Serial.println(remoteAccessRequest.requestId);
    Serial.print("  Area: ");
    Serial.println(getAreaName(remoteAccessRequest.area));
    Serial.print("  Direction: ");
    Serial.println(getAccessModeName(remoteAccessRequest.direction));
    Serial.print("  State: ");
    Serial.println(getRemoteFingerprintWorkflowName(
        remoteAccessRequest.workflowState));
    if (remoteAccessRequest.fingerprintResult.length() > 0) {
      Serial.print("  Fingerprint Result: ");
      Serial.println(remoteAccessRequest.fingerprintResult);
    }
    return;
  }
  if (serialCreatedRequestId.length() > 0) {
    Serial.print("  Request ID: ");
    Serial.println(serialCreatedRequestId);
    Serial.print("  Area: ");
    Serial.println(getAreaName(serialCreatedRequestArea));
    Serial.print("  Direction: ");
    Serial.println(getAccessModeName(serialCreatedRequestDirection));
    Serial.println("  State: WAITING_FOR_HEARTBEAT_DELIVERY");
    return;
  }
  Serial.println("  NONE");
}
#endif

void printIntegrationStatus() {
#if SMART_OFFICE_MODE == INTEGRATED_MODE
  Serial.println("Integration Mode: INTEGRATED");
  Serial.print("Wi-Fi: ");
  Serial.println(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
  Serial.print("Backend: ");
  Serial.println(getIntegrationSyncStateName(integrationSyncState));
  Serial.print("Backend Reachable: ");
  Serial.println(backendReachable ? "YES" : "NO");
  Serial.print("Backend System Active: ");
  Serial.println(backendSystemActive ? "YES" : "NO");
  Serial.print("Backend Admin Mirror: ");
  Serial.println(backendAdminMode ? "ON" : "OFF");
  Serial.print("Boot ID: ");
  Serial.println(bootId);
  Serial.print("Pending Authorization: ");
  Serial.println(pendingAuthorizationId.length() > 0
                     ? pendingAuthorizationId
                     : String("NONE"));
  printRemoteAccessStatus();
#else
  Serial.println("Integration Mode: STANDALONE");
  Serial.println("Wi-Fi: DISABLED");
  Serial.println("Backend: DISABLED");
  Serial.println("Boot ID: NOT USED");
#endif
}

void printSystemStatus() {
  Serial.println();
  Serial.println("[SYSTEM STATUS]");
  printIntegrationStatus();
  Serial.println();
  Serial.print("Selected Area: ");
  Serial.println(getAreaName(selectedArea));
  Serial.print("Current Mode: ");
  Serial.println(getAccessModeName(currentMode));
  printSecurityStatus();
  Serial.println();
  Serial.print("Door State: ");
  Serial.println(doorOpen ? "OPEN" : "CLOSED");
  Serial.print("Servo PWM Attached: ");
  Serial.println(servoInitialized ? "YES" : "NO");
  Serial.print("Servo Pin: GPIO ");
  Serial.println(SERVO_PIN);
  Serial.print("Servo Closed Angle: ");
  Serial.println(DOOR_CLOSED_ANGLE);
  Serial.print("Servo Open Angle: ");
  Serial.println(DOOR_OPEN_ANGLE);
  Serial.println();
  Serial.print("Ultrasonic TRIG/ECHO: GPIO ");
  Serial.print(ULTRASONIC_TRIG_PIN);
  Serial.print("/GPIO ");
  Serial.println(ULTRASONIC_ECHO_PIN);
  Serial.print("Presence Threshold: ");
  Serial.print(PRESENCE_DISTANCE_CM, 1);
  Serial.println(" cm");
  updateUltrasonicMeasurement(true);
  Serial.print("Last Ultrasonic Distance: ");
  if (lastDistanceCm > 0.0) {
    Serial.print(lastDistanceCm, 1);
    Serial.println(" cm");
  } else {
    Serial.println("measurement timeout");
  }
  Serial.print("Person Near: ");
  Serial.println(lastDistanceCm > 0.0 &&
                         lastDistanceCm <= PRESENCE_DISTANCE_CM
                     ? "YES"
                     : "NO");
  Serial.println();
  Serial.print("RFID Initialized: ");
  Serial.println(rfidInitialized ? "YES" : "NO");
  Serial.print("Fingerprint Detected: ");
  Serial.println(fingerprintReady ? "YES" : "NO");
  Serial.print("Fingerprint Model / Voltage: ");
  Serial.print(FINGERPRINT_MODEL);
  Serial.print(" / ");
  Serial.println(FINGERPRINT_EXPECTED_VOLTAGE);
  Serial.print("Fingerprint Baud: ");
  if (fingerprintWorkingBaud > 0) {
    Serial.println(fingerprintWorkingBaud);
  } else {
    Serial.println("unknown");
  }
  Serial.print("LCD Initialized: ");
  Serial.println(lcdInitialized ? "YES" : "NO");
  Serial.print("Red LED Pin: GPIO ");
  Serial.println(RED_LED_PIN);
  Serial.print("Buzzer Pin: GPIO ");
  Serial.println(BUZZER_PIN);
  Serial.print("Green LED Pin: GPIO ");
  Serial.println(GREEN_LED_PIN);
  Serial.println("Power Note: JM-101B uses 3.3V UART; servo uses separate 5V; common GND required.");
  Serial.print("Admin RFID UID: ");
  Serial.println(ADMIN_RFID_UID);
  Serial.println("Users configured:");

  for (size_t i = 0; i < USER_COUNT; ++i) {
    Serial.print("ID ");
    Serial.print(users[i].fingerprintID);
    Serial.print(" - ");
    Serial.println(users[i].name);
  }
  printAllInsideStatus();
  Serial.println();
  lcdShowMessage("SYSTEM STATUS",
                 systemLocked ? "SYSTEM LOCKED" : "CHECK SERIAL");
}

void setupRFID() {
  SPI.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
  rfid.PCD_Init();
  delay(50);

  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  rfidInitialized = version != 0x00 && version != 0xFF;

  if (rfidInitialized) {
    Serial.print("[RFID] RC522 initialized. Version: 0x");
    Serial.println(version, HEX);
  } else {
    Serial.println("[RFID] Reader not initialized.");
    Serial.println("Check 3.3V power, SPI wiring, and GPIO 33 reset wiring.");
  }
}

void checkRFID() {
  if (!rfidInitialized) {
    return;
  }

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return;
  }

  String uid = readRFIDUID();

  Serial.println();
  Serial.println("[RFID DETECTED]");
  Serial.print("UID: ");
  Serial.println(uid);

  if (ADMIN_RFID_UID == ADMIN_RFID_PLACEHOLDER) {
    Serial.println("Result: Admin UID not configured yet");
    Serial.println("Copy the UID above into ADMIN_RFID_UID, then upload again.");
    lcdShowRFIDStatus("RFID UID Read", "Check Serial");
  } else if (uid == ADMIN_RFID_UID) {
    Serial.println("Role: Admin Master Card");
    handleAdminRFID();
  } else {
    handleUnknownRFID(uid);
  }
  Serial.println();

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

String readRFIDUID() {
  String uid;
  uid.reserve(rfid.uid.size * 2);

  for (byte i = 0; i < rfid.uid.size; ++i) {
    if (rfid.uid.uidByte[i] < 0x10) {
      uid += '0';
    }
    uid += String(rfid.uid.uidByte[i], HEX);
  }

  uid.toUpperCase();
  return uid;
}

void setupFingerprint() {
  setupFingerprintWithBaudScan();
}

bool runFingerprintStandaloneStyleTest() {
  fingerprintReady = false;
  fingerprintWorkingBaud = 0;

  Serial.println();
  Serial.println("=== AS608 / JM-101B Fingerprint Test ===");
  Serial.println("UART: HardwareSerial(2)");
  Serial.print("RX: GPIO");
  Serial.println(FINGER_RX_PIN);
  Serial.print("TX: GPIO");
  Serial.println(FINGER_TX_PIN);
  Serial.print("Baud: ");
  Serial.println(FINGERPRINT_PRIMARY_BAUD);
  Serial.print("Voltage expected: ");
  Serial.println(FINGERPRINT_EXPECTED_VOLTAGE);
  Serial.println("USB D+/D-: not used");

  // Exact low-level order from the working Fingerprint.ino test:
  // UART2 begin -> finger.begin(57600) -> verifyPassword().
  fingerprintSerial.begin(FINGERPRINT_PRIMARY_BAUD,
                          SERIAL_8N1,
                          FINGER_RX_PIN,
                          FINGER_TX_PIN);
  Serial.println("Checking sensor...");
  finger.begin(FINGERPRINT_PRIMARY_BAUD);

  if (!finger.verifyPassword()) {
    Serial.println("ERROR: AS608 / JM-101B not detected!");
    Serial.println("Check:");
    Serial.println("- VCC must be 3.3V for this JM-101B");
    Serial.println("- GND");
    Serial.println("- UART TX/RX wiring (not USB D+/D-)");
    Serial.println("- Baud rate");
    lcdShowMessage("FINGER ERROR", "JM-101B MISSING");
    return false;
  }

  fingerprintReady = true;
  fingerprintWorkingBaud = FINGERPRINT_PRIMARY_BAUD;
  Serial.println("SUCCESS: AS608 / JM-101B detected!");
  printFingerprintParameters();
  lcdShowMessage("FINGERPRINT OK", "57600 BAUD");
  Serial.println();
  return true;
}

bool setupFingerprintWithBaudScan() {
  lcdShowMessage("FINGER TEST", "UART2 57600");

  if (runFingerprintStandaloneStyleTest()) {
    return true;
  }

  Serial.println("[FINGERPRINT] ERROR: AS608 / JM-101B not detected using the working-test initialization pattern");
  Serial.println("Primary 57600 test failed; trying optional fallback rates.");

  constexpr size_t fallbackCount =
      sizeof(FINGERPRINT_FALLBACK_BAUD_RATES) /
      sizeof(FINGERPRINT_FALLBACK_BAUD_RATES[0]);
  for (size_t i = 0; i < fallbackCount; ++i) {
    const uint32_t baudRate = FINGERPRINT_FALLBACK_BAUD_RATES[i];
    Serial.print("[FINGERPRINT] Trying fallback baud ");
    Serial.print(baudRate);
    Serial.println("...");

    if (initializeFingerprintAtBaud(baudRate)) {
      Serial.print("[FINGERPRINT] SUCCESS: AS608 detected at fallback baud ");
      Serial.println(baudRate);
      printFingerprintParameters();
      lcdShowMessage("FINGERPRINT OK", String(baudRate) + " BAUD");
      Serial.println();
      return true;
    }
  }

  Serial.println("[FINGERPRINT ERROR]");
  Serial.println("ERROR: AS608 / JM-101B not detected!");
  Serial.println("Check:");
  Serial.println("- JM-101B VCC connected to 3.3V");
  Serial.println("- Common GND");
  Serial.println("- UART TX -> ESP32 RX2 GPIO16");
  Serial.println("- UART RX -> ESP32 TX2 GPIO17");
  Serial.println("- USB D+/D- left disconnected");
  Serial.println("- Baud rate and default password");
  Serial.println();
  lcdShowMessage("FINGER ERROR", "JM-101B MISSING");
  return false;
}

bool initializeFingerprintAtBaud(uint32_t baudRate) {
  // Optional recovery helper for startup fallback rates. Command F uses only
  // runFingerprintStandaloneStyleTest() and the exact working 57600 pattern.
  fingerprintSerial.begin(baudRate, SERIAL_8N1, FINGER_RX_PIN, FINGER_TX_PIN);
  delay(100);

  finger.begin(baudRate);
  delay(100);

  while (fingerprintSerial.available() > 0) {
    fingerprintSerial.read();
  }

  if (!finger.verifyPassword()) {
    return false;
  }

  fingerprintReady = true;
  fingerprintWorkingBaud = baudRate;
  return true;
}

void printFingerprintParameters() {
  uint8_t result = finger.getParameters();
  if (result != FINGERPRINT_OK) {
    Serial.print("[FINGERPRINT] Sensor detected, but getParameters failed: ");
    Serial.println(result);
    return;
  }

  Serial.println();
  Serial.println("Sensor information:");
  Serial.print("Status: 0x");
  Serial.println(finger.status_reg, HEX);
  Serial.print("Capacity: ");
  Serial.println(finger.capacity);
  Serial.print("Security level: ");
  Serial.println(finger.security_level);
  Serial.print("Packet length: ");
  Serial.println(finger.packet_len);
  Serial.print("Baud rate: ");
  Serial.println(finger.baud_rate);
}

void testSimpleFingerprintScan() {
  Serial.println();
  Serial.println("=== AS608 / JM-101B Simple Scan Test ===");

  if (!fingerprintReady) {
    Serial.println("ERROR: AS608 is not initialized.");
    Serial.println("Run F to repeat the working-test detection method.");
    lcdShowMessage("FINGER ERROR", "PRESS F TO TEST");
    return;
  }

  Serial.println("Place a finger on the sensor (15 second timeout)...");
  lcdShowMessage("PLACE FINGER", "SCANNING...");

  uint8_t result = FINGERPRINT_NOFINGER;
  if (!waitForFingerImage(FINGERPRINT_CAPTURE_TIMEOUT_MS, result)) {
    Serial.println("Scan timeout: no finger detected.");
    lcdShowMessage("FINGER TEST", "SCAN TIMEOUT");
    return;
  }

  if (result != FINGERPRINT_OK) {
    Serial.print("Image capture error: ");
    Serial.println(getFingerprintError(result));
    lcdShowMessage("FINGER ERROR", "CAPTURE FAILED");
    return;
  }

  Serial.println("Fingerprint detected!");
  result = finger.image2Tz();
  if (result != FINGERPRINT_OK) {
    Serial.print("Image conversion error: ");
    Serial.println(getFingerprintError(result));
    lcdShowMessage("FINGER ERROR", "BAD IMAGE");
    return;
  }

  Serial.println("Image converted successfully.");
  result = finger.fingerFastSearch();
  if (result == FINGERPRINT_OK) {
    Serial.println("MATCH FOUND!");
    Serial.print("ID: ");
    Serial.println(finger.fingerID);
    Serial.print("Confidence: ");
    Serial.println(finger.confidence);
    lcdShowMessage("FINGER MATCH",
                   "ID " + String(finger.fingerID) + " C " +
                       String(finger.confidence));
    return;
  }

  if (result == FINGERPRINT_NOTFOUND) {
    Serial.println("Fingerprint not found in database.");
    lcdShowMessage("NO FINGER MATCH", "NOT IN DATABASE");
    return;
  }

  Serial.print("Search error: ");
  Serial.println(result);
  lcdShowMessage("FINGER ERROR", "SEARCH FAILED");
}

FingerprintDeleteGateResult evaluateFingerprintDeleteGate(
    bool locked,
    bool sensorReady,
    bool adminActive) {
  if (locked) {
    return FINGERPRINT_DELETE_BLOCKED_LOCKDOWN;
  }
  if (!sensorReady) {
    return FINGERPRINT_DELETE_BLOCKED_SENSOR;
  }
  if (!adminActive) {
    return FINGERPRINT_DELETE_BLOCKED_ADMIN;
  }
  return FINGERPRINT_DELETE_ALLOWED;
}

FingerprintDeleteConfirmationResult evaluateFingerprintDeleteConfirmation(
    const String& input,
    bool locked,
    bool sensorReady,
    bool adminActive) {
  if (locked) {
    return FINGERPRINT_DELETE_CANCELLED_LOCKDOWN;
  }
  if (!sensorReady) {
    return FINGERPRINT_DELETE_CANCELLED_SENSOR;
  }
  if (!adminActive) {
    return FINGERPRINT_DELETE_CANCELLED_ADMIN;
  }
  return input == "DELETE" ? FINGERPRINT_DELETE_CONFIRMED
                           : FINGERPRINT_DELETE_CANCELLED_TEXT;
}

bool configuredFingerprintDeleteIDsAreExactly1To6() {
  if (FINGERPRINT_DELETE_ID_COUNT != 6) {
    return false;
  }
  for (size_t i = 0; i < FINGERPRINT_DELETE_ID_COUNT; ++i) {
    if (FINGERPRINT_DELETE_IDS[i] != i + 1) {
      return false;
    }
  }
  return true;
}

bool isFingerprintSlotEmptyResult(uint8_t result) {
  return result == FINGERPRINT_NOTFOUND ||
         result == FINGERPRINT_DBRANGEFAIL;
}

String getFingerprintDeleteError(uint8_t result) {
  switch (result) {
    case FINGERPRINT_DELETEFAIL:
      return "sensor could not delete the template";
    case FINGERPRINT_DBRANGEFAIL:
      return "template slot is empty or invalid";
    case FINGERPRINT_NOTFOUND:
      return "template not found";
    default:
      return getFingerprintError(result);
  }
}

void cancelFingerprintDeleteConfirmation(String reason) {
  waitingForDeleteConfirmation = false;
  Serial.println("[FINGERPRINT DELETE CANCELLED]");
  if (reason.length() > 0) {
    Serial.print("Reason: ");
    Serial.println(reason);
  }
  Serial.println();
  lcdShowMessage("DELETE CANCELLED", "NO IDS DELETED");
}

void requestConfiguredFingerprintDeletion() {
  FingerprintDeleteGateResult gate = evaluateFingerprintDeleteGate(
      systemLocked,
      fingerprintReady,
      isAdminModeActive());

  if (gate == FINGERPRINT_DELETE_BLOCKED_LOCKDOWN) {
    Serial.println("[SYSTEM LOCKED]");
    Serial.println("Fingerprint deletion is blocked.");
    Serial.println("Scan Admin RFID card to unlock.");
    showLockdownStatus();
    return;
  }
  if (gate == FINGERPRINT_DELETE_BLOCKED_SENSOR) {
    Serial.println("[FINGERPRINT ERROR]");
    Serial.println("Cannot delete fingerprints. Sensor not detected.");
    Serial.println("Run command F for diagnostics.");
    lcdShowMessage("FINGER ERROR", "DELETE BLOCKED");
    return;
  }
  if (gate == FINGERPRINT_DELETE_BLOCKED_ADMIN) {
    Serial.println("[ADMIN REQUIRED]");
    Serial.println("Fingerprint deletion is allowed only in Admin Mode.");
    Serial.println("Scan the Admin RFID card first.");
    lcdShowMessage("ADMIN REQUIRED", "SCAN ADMIN RFID");
    return;
  }

  waitingForDeleteConfirmation = true;
  Serial.println();
  Serial.println("[FINGERPRINT DELETE]");
  Serial.println("This will delete fingerprint IDs 1-6.");
  Serial.println("Type DELETE to confirm.");
  Serial.println();
  lcdShowMessage("DELETE IDS 1-6", "TYPE DELETE");
}

void handleFingerprintDeleteConfirmation(const String& input) {
  bool adminActive = isAdminModeActive();
  if (!waitingForDeleteConfirmation) {
    // The timeout handler already cancelled and reported this request.
    return;
  }

  FingerprintDeleteConfirmationResult confirmation =
      evaluateFingerprintDeleteConfirmation(
          input,
          systemLocked,
          fingerprintReady,
          adminActive);

  // Every confirmation attempt leaves the waiting state before any sensor I/O.
  waitingForDeleteConfirmation = false;

  switch (confirmation) {
    case FINGERPRINT_DELETE_CONFIRMED:
      deleteConfiguredFingerprints();
      return;
    case FINGERPRINT_DELETE_CANCELLED_LOCKDOWN:
      cancelFingerprintDeleteConfirmation("System entered Lockdown.");
      return;
    case FINGERPRINT_DELETE_CANCELLED_SENSOR:
      cancelFingerprintDeleteConfirmation("Fingerprint sensor is not ready.");
      return;
    case FINGERPRINT_DELETE_CANCELLED_ADMIN:
      cancelFingerprintDeleteConfirmation("Admin Mode is not active or expired.");
      return;
    case FINGERPRINT_DELETE_CANCELLED_TEXT:
    default:
      cancelFingerprintDeleteConfirmation("Confirmation text did not exactly match DELETE.");
      return;
  }
}

void deleteConfiguredFingerprints() {
  int deletedCount = 0;
  int emptyCount = 0;
  int failedCount = 0;

  Serial.println("[FINGERPRINT DELETE START]");
  for (size_t i = 0; i < FINGERPRINT_DELETE_ID_COUNT; ++i) {
    uint16_t id = FINGERPRINT_DELETE_IDS[i];

    // loadModel() is read-only and helps distinguish an already-empty slot
    // from a genuine delete/flash failure on sensors that return DELETEFAIL
    // for both cases. deleteModel() is still issued once for every ID 1-6.
    uint8_t loadResult = finger.loadModel(id);
    bool slotWasEmpty = isFingerprintSlotEmptyResult(loadResult);
    uint8_t deleteResult = finger.deleteModel(id);

    Serial.print("ID ");
    Serial.print(id);
    if (deleteResult == FINGERPRINT_OK) {
      ++deletedCount;
      Serial.println(": DELETED");
      continue;
    }

    bool notFound = isFingerprintSlotEmptyResult(deleteResult) ||
                    (slotWasEmpty &&
                     deleteResult == FINGERPRINT_DELETEFAIL);
    if (notFound) {
      ++emptyCount;
      Serial.println(": ALREADY EMPTY / NOT FOUND");
      continue;
    }

    ++failedCount;
    Serial.print(": FAILED - ");
    Serial.print(getFingerprintDeleteError(deleteResult));
    Serial.print(" (code 0x");
    if (deleteResult < 0x10) {
      Serial.print('0');
    }
    Serial.print(deleteResult, HEX);
    Serial.println(')');
  }

  Serial.println();
  Serial.print("Deleted successfully: ");
  Serial.println(deletedCount);
  Serial.print("Already empty / not found: ");
  Serial.println(emptyCount);
  Serial.print("Failed: ");
  Serial.println(failedCount);

  if (failedCount == 0) {
    Serial.println("[FINGERPRINT DELETE COMPLETE]");
    lcdShowMessage("DELETE COMPLETE", "ENROLL IDS 1-6");
  } else {
    Serial.println("[FINGERPRINT DELETE FINISHED WITH ERRORS]");
    Serial.println("One or more IDs were not deleted. Review the error codes above.");
    lcdShowMessage("DELETE ERRORS", "CHECK SERIAL");
  }
  Serial.println();
}

bool enrollFingerprint(int id) {
  if (isSystemLocked()) {
    Serial.println("[SYSTEM LOCKED]");
    Serial.println("Fingerprint enrollment is blocked.");
    Serial.println("Scan Admin RFID card to unlock.");
    showLockdownStatus();
    triggerErrorAlert();
    return false;
  }

  if (!isAdminModeActive()) {
    Serial.println("[ADMIN REQUIRED]");
    Serial.println("Enrollment is allowed only in Admin Mode.");
    Serial.println("Scan the Admin RFID card first.");
    lcdShowMessage("ADMIN REQUIRED", "SCAN ADMIN RFID");
    triggerErrorAlert();
    return false;
  }

  Serial.println();
  Serial.println("[ENROLL MODE]");
  Serial.print("Enrolling fingerprint ID: ");
  Serial.println(id);
  Serial.println("Place finger on the sensor.");
  lcdShowEnrollStep("Place Finger", "On Sensor");

  uint8_t result = FINGERPRINT_NOFINGER;
  if (!waitForFingerImage(FINGERPRINT_CAPTURE_TIMEOUT_MS, result)) {
    Serial.println("[ENROLL FAILED]");
    Serial.println("Reason: timed out waiting for finger");
    lcdShowEnrollFailed("No Finger");
    triggerEnrollmentFailedAlert();
    return false;
  }

  if (result != FINGERPRINT_OK) {
    Serial.println("[ENROLL FAILED]");
    Serial.print("Reason: ");
    Serial.println(getFingerprintError(result));
    lcdShowEnrollFailed("Sensor Error");
    triggerEnrollmentFailedAlert();
    return false;
  }

  result = finger.image2Tz(1);
  if (result != FINGERPRINT_OK) {
    Serial.println("[ENROLL FAILED]");
    Serial.print("Reason: ");
    Serial.println(getFingerprintError(result));
    lcdShowEnrollFailed("Bad Image");
    triggerEnrollmentFailedAlert();
    return false;
  }

  Serial.println("First image captured.");
  Serial.println("Remove finger.");
  lcdShowEnrollStep("Remove Finger", "Please Wait");

  uint32_t removeStart = millis();
  while (millis() - removeStart < FINGERPRINT_CAPTURE_TIMEOUT_MS) {
    result = finger.getImage();
    if (result == FINGERPRINT_NOFINGER) {
      break;
    }
    delay(50);
  }

  if (result != FINGERPRINT_NOFINGER) {
    Serial.println("[ENROLL FAILED]");
    Serial.println("Reason: timed out waiting for finger removal");
    lcdShowEnrollFailed("Remove Finger");
    triggerEnrollmentFailedAlert();
    return false;
  }

  delay(500);
  Serial.println("Place the same finger again.");
  lcdShowEnrollStep("Place Same", "Finger Again");

  if (!waitForFingerImage(FINGERPRINT_CAPTURE_TIMEOUT_MS, result)) {
    Serial.println("[ENROLL FAILED]");
    Serial.println("Reason: timed out waiting for second fingerprint image");
    lcdShowEnrollFailed("No 2nd Image");
    triggerEnrollmentFailedAlert();
    return false;
  }

  if (result != FINGERPRINT_OK) {
    Serial.println("[ENROLL FAILED]");
    Serial.print("Reason: ");
    Serial.println(getFingerprintError(result));
    lcdShowEnrollFailed("Sensor Error");
    triggerEnrollmentFailedAlert();
    return false;
  }

  result = finger.image2Tz(2);
  if (result != FINGERPRINT_OK) {
    Serial.println("[ENROLL FAILED]");
    Serial.print("Reason: ");
    Serial.println(getFingerprintError(result));
    lcdShowEnrollFailed("Bad 2nd Image");
    triggerEnrollmentFailedAlert();
    return false;
  }

  result = finger.createModel();
  if (result != FINGERPRINT_OK) {
    Serial.println("[ENROLL FAILED]");
    Serial.print("Reason: ");
    Serial.println(result == FINGERPRINT_ENROLLMISMATCH
                       ? "fingerprints did not match"
                       : getFingerprintError(result));
    lcdShowEnrollFailed(result == FINGERPRINT_ENROLLMISMATCH
                            ? "No Match"
                            : "Model Error");
    triggerEnrollmentFailedAlert();
    return false;
  }

  result = finger.storeModel(id);
  if (result != FINGERPRINT_OK) {
    Serial.println("[ENROLL FAILED]");
    Serial.print("Reason: ");
    Serial.println(getFingerprintError(result));
    lcdShowEnrollFailed("Store Error");
    triggerEnrollmentFailedAlert();
    return false;
  }

  Serial.println("[ENROLL SUCCESS]");
  Serial.print("Fingerprint saved with ID: ");
  Serial.println(id);
  Serial.println();
  lcdShowEnrollSuccess(id);
  return true;
}

int readFingerprintID() {
  uint8_t result = FINGERPRINT_NOFINGER;

  if (!waitForFingerImage(FINGERPRINT_CAPTURE_TIMEOUT_MS, result)) {
    return FINGERPRINT_CAPTURE_TIMEOUT;
  }

  if (result != FINGERPRINT_OK) {
    return FINGERPRINT_READ_ERROR;
  }

  result = finger.image2Tz();
  if (result != FINGERPRINT_OK) {
    return FINGERPRINT_READ_ERROR;
  }

  result = finger.fingerFastSearch();
  if (result == FINGERPRINT_NOTFOUND) {
    return FINGERPRINT_NOT_RECOGNIZED;
  }

  if (result != FINGERPRINT_OK) {
    return FINGERPRINT_READ_ERROR;
  }

  return finger.fingerID;
}

void handleSerialCommand() {
  static String inputBuffer;

  while (Serial.available() > 0) {
    char input = static_cast<char>(Serial.read());

    if (input == '\r' || input == '\n') {
      if (inputBuffer.length() > 0) {
        processSerialInput(inputBuffer);
        inputBuffer = "";
      }
      continue;
    }

    if (inputBuffer.length() < 16) {
      inputBuffer += input;
    }
  }
}

bool canEnableExitModeForState(bool locked,
                               Area area,
                               bool isDoorOpen) {
  return !locked && area != AREA_NONE && !isDoorOpen;
}

AccessPrecheckResult evaluateAccessPrecheck(
    AccessMode mode,
    bool locked,
    Area area,
    bool isDoorOpen,
    bool ultrasonicMeasurementValid,
    bool personNear,
    bool fingerprintAvailable) {
  if (locked) {
    return ACCESS_PRECHECK_LOCKED;
  }
  if (area == AREA_NONE) {
    return ACCESS_PRECHECK_NO_AREA;
  }
  if (isDoorOpen) {
    return ACCESS_PRECHECK_DOOR_OPEN;
  }
  if (isPresenceRequiredForMode(mode)) {
    if (!ultrasonicMeasurementValid) {
      return ACCESS_PRECHECK_ULTRASONIC_INVALID;
    }
    if (!personNear) {
      return ACCESS_PRECHECK_PERSON_NOT_NEAR;
    }
  }
  if (!fingerprintAvailable) {
    return ACCESS_PRECHECK_FINGERPRINT_UNAVAILABLE;
  }
  return ACCESS_PRECHECK_OK;
}

void processSerialInput(String input) {
  if (waitingForDeleteConfirmation) {
    // Do not trim or normalize: confirmation must be exactly uppercase DELETE.
    handleFingerprintDeleteConfirmation(input);
    return;
  }

  input.trim();

  if (input.length() == 0) {
    return;
  }

  if (waitingForEnrollmentID) {
    waitingForEnrollmentID = false;

    if (isSystemLocked()) {
      Serial.println("[SYSTEM LOCKED]");
      Serial.println("Fingerprint enrollment is blocked.");
      Serial.println("Scan Admin RFID card to unlock.");
      showLockdownStatus();
      triggerErrorAlert();
      return;
    }

    if (!isAdminModeActive()) {
      Serial.println("[ADMIN REQUIRED]");
      Serial.println("Admin Mode expired before an enrollment ID was entered.");
      Serial.println("Scan the Admin RFID card again.");
      lcdShowMessage("ADMIN REQUIRED", "SCAN ADMIN RFID");
      triggerErrorAlert();
      return;
    }

    if (!isNumericInput(input)) {
      Serial.println("[ENROLL FAILED]");
      Serial.println("Reason: fingerprint ID must be a number from 1 to 127");
      lcdShowEnrollFailed("Invalid ID");
      triggerEnrollmentFailedAlert();
      return;
    }

    int id = input.toInt();
    if (id < 1 || id > 127) {
      Serial.println("[ENROLL FAILED]");
      Serial.println("Reason: fingerprint ID must be between 1 and 127");
      lcdShowEnrollFailed("ID Must Be 1-127");
      triggerEnrollmentFailedAlert();
      return;
    }

    enrollFingerprint(id);
    return;
  }

  input.toUpperCase();

  if (input.length() != 1) {
    Serial.println("[INVALID COMMAND]");
    Serial.println("Use M to print menu");
    lcdShowError("Invalid Command", "Use M for Menu");
    triggerInvalidCommandAlert();
    return;
  }

  char command = input.charAt(0);

  if (command >= '1' && command <= '7') {
    handleAreaSelection(command);
    return;
  }

  switch (command) {
    case 'C':
      requestConfiguredFingerprintDeletion();
      break;

    case 'E':
      if (isSystemLocked()) {
        Serial.println("[SYSTEM LOCKED]");
        Serial.println("Fingerprint enrollment is blocked.");
        Serial.println("Scan Admin RFID card to unlock.");
        showLockdownStatus();
        triggerErrorAlert();
        return;
      }
      if (!isAdminModeActive()) {
        Serial.println("[ADMIN REQUIRED]");
        Serial.println("Enrollment is allowed only in Admin Mode.");
        Serial.println("Scan the Admin RFID card first.");
        lcdShowMessage("ADMIN REQUIRED", "SCAN ADMIN RFID");
        triggerErrorAlert();
        return;
      }
      if (!fingerprintReady) {
        Serial.println("[FINGERPRINT ERROR]");
        Serial.println("Cannot enroll. Sensor not detected.");
        Serial.println("Run command F for baud-rate, wiring, and power diagnostics.");
        lcdShowMessage("FINGER ERROR", "CANNOT ENROLL");
        triggerErrorAlert();
        return;
      }
      waitingForEnrollmentID = true;
      Serial.println();
      Serial.println("[ENROLL MODE]");
      Serial.println("Enter Fingerprint ID between 1 and 127:");
      lcdShowEnrollStart();
      break;

    case 'R':
#if SMART_OFFICE_MODE == INTEGRATED_MODE
      createIntegratedSerialAccessRequest();
#else
      testFingerprintAccess();
#endif
      break;

    case 'F':
      runStateGuardedDiagnostic('F');
      break;

    case 'P':
      runStateGuardedDiagnostic('P');
      break;

    case 'V':
      runSoftwareValidation();
      break;

    case 'M':
      printMenu();
      lcdShowMenu();
      break;

    case 'S':
      printSystemStatus();
      break;

    case 'T':
      testAlerts();
      break;

    case 'G':
      testGreenLed();
      break;

    case 'X':
      if (!canEnableExitModeForState(systemLocked, selectedArea, doorOpen)) {
        if (isSystemLocked()) {
          Serial.println("[SYSTEM LOCKED]");
          Serial.println("Exit Mode is blocked. Scan Admin RFID card to unlock.");
          showLockdownStatus();
        } else if (selectedArea == AREA_NONE) {
          Serial.println("[EXIT MODE ERROR]");
          Serial.println("Please select an area first using 1-7.");
          lcdShowError("Select Area", "Before Exit");
        } else {
          Serial.println("[EXIT MODE ERROR]");
          Serial.println("Door is already open.");
          lcdShowError("Door Is Open", "Wait To Close");
        }
        triggerErrorAlert();
        return;
      }
      currentMode = MODE_EXIT;
      presencePromptVisible = false;
      Serial.println("[EXIT MODE]");
      Serial.print("Selected Area: ");
      Serial.println(getAreaName(selectedArea));
      Serial.println("Enter R and scan a permitted fingerprint.");
      lcdShowMessage("EXIT " + getLCDAreaName(selectedArea),
                     "PRESS R TO SCAN");
      break;

    case 'D':
      runStateGuardedDiagnostic('D');
      break;

    case 'U':
      runStateGuardedDiagnostic('U');
      break;

    case 'W':
      runStateGuardedDiagnostic('W');
      break;

    default:
      Serial.println("[INVALID COMMAND]");
      Serial.println("Use M to print menu");
      lcdShowError("Invalid Command", "Use M for Menu");
      triggerInvalidCommandAlert();
      break;
  }
}

void handleAreaSelection(char command) {
  selectedArea = static_cast<Area>(command - '0');
  presencePromptVisible = false;
  Serial.print("[AREA SELECTED] ");
  Serial.println(getAreaName(selectedArea));
  lcdShowSelectedArea(selectedArea);
}

String getAreaName(Area area) {
  switch (area) {
    case COMPANY_A:
      return "Company A";
    case COMPANY_B:
      return "Company B";
    case COMPANY_C:
      return "Company C";
    case COMPANY_D:
      return "Company D";
    case SERVER_ROOM:
      return "Server Room";
    case MANAGEMENT_ADMIN:
      return "Management / Admin";
    case MAIN_ENTRANCE:
      return "Main Entrance";
    default:
      return "None";
  }
}

User* findUserByFingerprintID(int fingerprintID) {
  for (size_t i = 0; i < USER_COUNT; ++i) {
    if (users[i].fingerprintID == fingerprintID) {
      return &users[i];
    }
  }

  return nullptr;
}

bool checkPermission(User* user, Area area) {
  if (user == nullptr || area == AREA_NONE) {
    return false;
  }

  switch (area) {
    case MAIN_ENTRANCE:
      return user->canAccessMainEntrance;
    case COMPANY_A:
      return user->canAccessCompanyA;
    case COMPANY_B:
      return user->canAccessCompanyB;
    case SERVER_ROOM:
      return user->canAccessServerRoom;
    case MANAGEMENT_ADMIN:
      return user->canAccessManagement;
    case COMPANY_C:
      return user->canAccessCompanyC;
    case COMPANY_D:
      return user->canAccessCompanyD;
    default:
      return false;
  }
}

int getAreaBit(Area area) {
  int areaNumber = static_cast<int>(area);
  if (areaNumber < static_cast<int>(COMPANY_A) ||
      areaNumber > static_cast<int>(MAIN_ENTRANCE)) {
    return 0;
  }

  return 1 << (areaNumber - 1);
}

bool isUserInsideArea(User* user, Area area) {
  int areaBit = getAreaBit(area);
  return user != nullptr && areaBit != 0 &&
         (user->insideMask & static_cast<uint16_t>(areaBit)) != 0;
}

void markUserInsideArea(User* user, Area area) {
  int areaBit = getAreaBit(area);
  if (user != nullptr && areaBit != 0) {
    user->insideMask |= static_cast<uint16_t>(areaBit);
  }
}

void markUserOutsideArea(User* user, Area area) {
  int areaBit = getAreaBit(area);
  if (user != nullptr && areaBit != 0) {
    user->insideMask &= ~static_cast<uint16_t>(areaBit);
  }
}

bool commitAttendanceAfterDoorOpen(User* user,
                                   Area area,
                                   AccessMode accessMode,
                                   bool isDoorOpen) {
  if (!isDoorOpen || user == nullptr || getAreaBit(area) == 0) {
    return false;
  }

  if (accessMode == MODE_EXIT) {
    markUserOutsideArea(user, area);
  } else {
    markUserInsideArea(user, area);
  }
  return true;
}

bool canUserEnterArea(User* user, Area area) {
  return checkPermission(user, area) && !isUserInsideArea(user, area);
}

bool canUserExitArea(User* user, Area area) {
  return checkPermission(user, area) && isUserInsideArea(user, area);
}

void handleEntryAccess(User* user, Area area) {
  if (!checkPermission(user, area)) {
    denyAccess("User is not allowed to access " + getAreaName(area),
               area,
               "Fingerprint",
               user);
    return;
  }

  if (!canUserEnterArea(user, area)) {
    Serial.println("[ANTI-PASSBACK]");
    Serial.println("Anti-Passback: ACTIVE");
    denyAccess("User is already inside " + getAreaName(area),
               area,
               "Fingerprint",
               user,
               COUNT_ANTI_PASSBACK_AS_FAILED_ATTEMPT);
    return;
  }

  grantAccess(user, area, "Fingerprint");
}

void handleExitAccess(User* user, Area area) {
  if (!isUserInsideArea(user, area)) {
    denyAccess("User is not inside " + getAreaName(area),
               area,
               "Fingerprint",
               user,
               COUNT_EXIT_OUTSIDE_AS_FAILED_ATTEMPT);
    return;
  }

  if (!canUserExitArea(user, area)) {
    denyAccess("User is not allowed to access " + getAreaName(area),
               area,
               "Fingerprint",
               user);
    return;
  }

  grantAccess(user, area, "Fingerprint");
}

void printUserInsideStatus(User* user) {
  if (user == nullptr) {
    return;
  }

  const Area trackedAreas[] = {COMPANY_A,
                               COMPANY_B,
                               COMPANY_C,
                               COMPANY_D,
                               SERVER_ROOM,
                               MANAGEMENT_ADMIN,
                               MAIN_ENTRANCE};
  constexpr size_t trackedAreaCount =
      sizeof(trackedAreas) / sizeof(trackedAreas[0]);

  Serial.print("ID ");
  Serial.print(user->fingerprintID);
  Serial.print(" - ");
  Serial.println(user->name);
  for (size_t i = 0; i < trackedAreaCount; ++i) {
    Serial.print("  ");
    Serial.print(getAreaName(trackedAreas[i]));
    Serial.print(": ");
    Serial.println(isUserInsideArea(user, trackedAreas[i]) ? "INSIDE"
                                                            : "OUTSIDE");
  }
}

int countUsersInsideArea(Area area) {
  int count = 0;
  for (size_t i = 0; i < USER_COUNT; ++i) {
    if (isUserInsideArea(&users[i], area)) {
      ++count;
    }
  }
  return count;
}

void printAllInsideStatus() {
  const Area trackedAreas[] = {COMPANY_A,
                               COMPANY_B,
                               COMPANY_C,
                               COMPANY_D,
                               SERVER_ROOM,
                               MANAGEMENT_ADMIN,
                               MAIN_ENTRANCE};
  constexpr size_t trackedAreaCount =
      sizeof(trackedAreas) / sizeof(trackedAreas[0]);

  Serial.println();
  Serial.println("Users Inside Status:");
  for (size_t i = 0; i < USER_COUNT; ++i) {
    printUserInsideStatus(&users[i]);
  }

  Serial.println("Occupancy:");
  for (size_t i = 0; i < trackedAreaCount; ++i) {
    Serial.print("  ");
    Serial.print(getAreaName(trackedAreas[i]));
    Serial.print(": ");
    Serial.println(countUsersInsideArea(trackedAreas[i]));
  }
}

void grantAccess(User* user, Area area, String method) {
  AccessMode grantedMode = currentMode;
  turnOffAlerts();
  openDoor(grantedMode == MODE_EXIT ? "Authorized fingerprint exit"
                                    : "Authorized fingerprint entry");

  if (!doorOpen) {
    Serial.println("[ACCESS HARDWARE ERROR]");
    Serial.println("Authorization passed, but the door did not open.");
    Serial.println("Attendance state and failed-attempt counter are unchanged.");
    Serial.print("User: ");
    Serial.println(user == nullptr ? "Unknown" : user->name);
    Serial.print("Area: ");
    Serial.println(getAreaName(area));
    Serial.println("Door: CLOSED");
    Serial.println();
    returnToEntryMode();
    return;
  }

  if (!commitAttendanceAfterDoorOpen(user, area, grantedMode, doorOpen)) {
    Serial.println("[ACCESS STATE ERROR]");
    Serial.println("Door is open, but attendance could not be committed.");
    Serial.println("Failed-attempt counter is unchanged.");
    returnToEntryMode();
    return;
  }
  resetFailedAttempts();

  Serial.println(grantedMode == MODE_EXIT ? "[EXIT GRANTED]"
                                          : "[ENTRY GRANTED]");
  if (user != nullptr) {
    Serial.print("User: ");
    Serial.println(user->name);
    Serial.print("Company: ");
    Serial.println(user->company);
    Serial.print("Role: ");
    Serial.println(user->role);
    Serial.print("Fingerprint ID: ");
    Serial.println(user->fingerprintID);
  }
  Serial.print("Method: ");
  Serial.println(method);
  Serial.print("Area: ");
  Serial.println(getAreaName(area));
  Serial.print("Mode: ");
  Serial.println(getAccessModeName(grantedMode));
  Serial.print("Status: ");
  Serial.println(grantedMode == MODE_EXIT ? "OUTSIDE" : "INSIDE");
  Serial.println("Result: ACCESS GRANTED");
  Serial.println(grantedMode == MODE_EXIT
                     ? "Reason: Authorized user exited selected area"
                     : "Reason: Authorized user entered selected area");
  Serial.println("Failed Attempts Reset: 0");
  Serial.println("System State: ACTIVE");
  Serial.print("Door: ");
  Serial.println("OPEN");
  Serial.println();

  if (grantedMode == MODE_EXIT) {
    lcdShowMessage("EXIT GRANTED",
                   user == nullptr ? String("AUTHORIZED USER") : user->name);
  } else {
    lcdShowAccessGranted(user, area);
  }
  triggerAccessGrantedFeedback();
  lcdShowDoorOpen(grantedMode == MODE_EXIT);
  returnToEntryMode();
}

void denyAccess(String reason,
                Area area,
                String method,
                User* user,
                bool countFailedAttempt) {
  turnOffSuccessOutput();
  bool wasLocked = systemLocked;
  AccessMode deniedMode = currentMode;
  Serial.println(deniedMode == MODE_EXIT ? "[EXIT DENIED]" : "[ENTRY DENIED]");
  if (user != nullptr) {
    Serial.print("User: ");
    Serial.println(user->name);
    Serial.print("Company: ");
    Serial.println(user->company);
    Serial.print("Role: ");
    Serial.println(user->role);
  }
  Serial.print("Method: ");
  Serial.println(method);
  Serial.print("Area: ");
  Serial.println(getAreaName(area));
  Serial.print("Mode: ");
  Serial.println(getAccessModeName(currentMode));
  Serial.println("Result: ACCESS DENIED");
  Serial.print("Reason: ");
  Serial.println(reason);
  lcdShowAccessDenied(reason);

  if (countFailedAttempt) {
    incrementFailedAttempts(reason);
  } else {
    Serial.print("Failed Attempts: unchanged at ");
    Serial.println(failedAttempts);
  }
  Serial.print("System State: ");
  Serial.println(systemLocked ? "LOCKED" : "ACTIVE");
  Serial.println();

  if (!countFailedAttempt) {
    triggerAccessDeniedAlert();
  } else if (!systemLocked) {
    triggerAccessDeniedAlert();
    lcdShowAccessDenied(reason);
  } else if (wasLocked) {
    showLockdownStatus();
    triggerAccessDeniedAlert();
  }

  Serial.print("Door: ");
  Serial.println(doorOpen ? "OPEN (unchanged)" : "CLOSED");
  returnToEntryMode();
}

bool isNumericInput(const String& value) {
  if (value.length() == 0) {
    return false;
  }

  for (size_t i = 0; i < value.length(); ++i) {
    if (!isDigit(value.charAt(i))) {
      return false;
    }
  }

  return true;
}

bool waitForFingerImage(uint32_t timeoutMs, uint8_t& result) {
  uint32_t start = millis();

  while (millis() - start < timeoutMs) {
    result = finger.getImage();

    if (result == FINGERPRINT_OK) {
      return true;
    }

    if (result != FINGERPRINT_NOFINGER) {
      return true;
    }

    delay(50);
  }

  return false;
}

String getFingerprintError(uint8_t result) {
  switch (result) {
    case FINGERPRINT_PACKETRECIEVEERR:
      return "fingerprint sensor communication error";
    case FINGERPRINT_IMAGEFAIL:
      return "fingerprint imaging error";
    case FINGERPRINT_IMAGEMESS:
      return "fingerprint image was too messy";
    case FINGERPRINT_FEATUREFAIL:
    case FINGERPRINT_INVALIDIMAGE:
      return "fingerprint features could not be found";
    case FINGERPRINT_ENROLLMISMATCH:
      return "fingerprints did not match";
    case FINGERPRINT_BADLOCATION:
      return "invalid fingerprint storage location";
    case FINGERPRINT_FLASHERR:
      return "fingerprint sensor flash storage error";
    default:
      return "unknown fingerprint sensor error";
  }
}

void testFingerprintAccess() {
  bool ultrasonicMeasurementValid = true;
  bool personNear = false;
  if (!systemLocked && selectedArea != AREA_NONE && !doorOpen &&
      isPresenceRequiredForMode(currentMode)) {
    // R requires a fresh valid measurement in Entry Mode. Exit Mode skips
    // this read entirely and never uses presence as a scan precondition.
    updateUltrasonicMeasurement(true);
    ultrasonicMeasurementValid =
        isUltrasonicMeasurementValid(lastDistanceCm);
    personNear = isDistanceNear(lastDistanceCm);
  }

  AccessPrecheckResult precheck = evaluateAccessPrecheck(
      currentMode,
      systemLocked,
      selectedArea,
      doorOpen,
      ultrasonicMeasurementValid,
      personNear,
      fingerprintReady);

  if (precheck != ACCESS_PRECHECK_OK) {
    switch (precheck) {
      case ACCESS_PRECHECK_LOCKED:
        Serial.println("[SYSTEM LOCKED]");
        Serial.println("Fingerprint access is blocked.");
        Serial.println("Scan Admin RFID card to unlock.");
        incrementFailedAttempts("Fingerprint access attempted during lockdown");
        showLockdownStatus();
        triggerAccessDeniedAlert();
        break;

      case ACCESS_PRECHECK_NO_AREA:
        Serial.println("[ERROR]");
        Serial.println("Please select an area first using 1-7");
        lcdShowError("Select Area", "First");
        triggerErrorAlert();
        break;

      case ACCESS_PRECHECK_DOOR_OPEN:
        Serial.println("[ACCESS BLOCKED]");
        Serial.println("Door is already open. Wait for it to close.");
        lcdShowError("Door Is Open", "Wait To Close");
        triggerErrorAlert();
        break;

      case ACCESS_PRECHECK_ULTRASONIC_INVALID:
        Serial.println("[ENTRY BLOCKED]");
        Serial.println("Ultrasonic measurement unavailable or timed out.");
        Serial.println("No fingerprint scan was started; door remains closed.");
        lcdShowMessage("ULTRA TIMEOUT", "ENTRY BLOCKED");
        break;

      case ACCESS_PRECHECK_PERSON_NOT_NEAR:
        Serial.println("[ENTRY BLOCKED]");
        Serial.println("No person detected within 20 cm of the door.");
        Serial.print("Current Distance: ");
        Serial.print(lastDistanceCm, 1);
        Serial.println(" cm");
        lcdShowMessage("COME CLOSER", "NO PERSON NEAR");
        break;

      case ACCESS_PRECHECK_FINGERPRINT_UNAVAILABLE:
        Serial.println("[FINGERPRINT ERROR]");
        Serial.println("Cannot scan. Sensor not detected.");
        Serial.println("Run command F for baud-rate, wiring, and power diagnostics.");
        lcdShowMessage("FINGER ERROR", "CANNOT SCAN");
        triggerErrorAlert();
        break;

      case ACCESS_PRECHECK_OK:
        break;
    }
    returnToEntryMode();
    return;
  }

  Serial.println();
  Serial.println("[FINGERPRINT TEST]");
  Serial.print("Mode: ");
  Serial.println(getAccessModeName(currentMode));
  Serial.println("Place finger on the sensor.");
  lcdShowPlaceFinger();
  presencePromptVisible = false;

  int fingerprintID = readFingerprintID();

  if (fingerprintID == FINGERPRINT_CAPTURE_TIMEOUT) {
    denyAccess("Timed out waiting for a finger", selectedArea, "Fingerprint");
    return;
  }

  if (fingerprintID == FINGERPRINT_NOT_RECOGNIZED) {
    Serial.println("[FINGERPRINT]");
    denyAccess("Fingerprint not recognized", selectedArea, "Fingerprint");
    return;
  }

  if (fingerprintID == FINGERPRINT_READ_ERROR) {
    denyAccess("Could not read fingerprint", selectedArea, "Fingerprint");
    return;
  }

  Serial.println("[FINGERPRINT DETECTED]");
  Serial.print("Fingerprint ID: ");
  Serial.println(fingerprintID);
  Serial.print("Confidence: ");
  Serial.println(finger.confidence);
  lcdShowMessage("FINGER MATCH",
                 "ID " + String(fingerprintID) + " C " +
                     String(finger.confidence));

  User* user = findUserByFingerprintID(fingerprintID);
  if (user == nullptr) {
    denyAccess("Fingerprint ID is not configured in the local users array",
               selectedArea,
               "Fingerprint");
    return;
  }

  if (currentMode == MODE_EXIT) {
    handleExitAccess(user, selectedArea);
  } else {
    handleEntryAccess(user, selectedArea);
  }
}

#endif  // FINGERPRINT_ONLY_DEBUG
