#include <Adafruit_Fingerprint.h>

// Temporary diagnostic switch:
//   1 = compile only the exact standalone Fingerprint.ino test below.
//   0 = compile/run the complete Smart Office Access System.
#define FINGERPRINT_ONLY_DEBUG 1

// Servo isolation uses the working servo_test.ino attach/write sequence, but
// keeps GPIO13 because GPIO18 is already the RC522 SCK pin in the full project.
#define SERVO_ONLY_DEBUG 0

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
  Serial.println("=== AS608 Fingerprint Test ===");

  fingerprintSerial.begin(57600, SERIAL_8N1, RX_PIN, TX_PIN);

  Serial.println("Checking fingerprint sensor...");

  finger.begin(57600);

  if (finger.verifyPassword()) {
    Serial.println("SUCCESS: AS608 detected!");
  } else {
    Serial.println("ERROR: AS608 not detected!");
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

#if FINGERPRINT_ONLY_DEBUG && SERVO_ONLY_DEBUG
#error "Enable only one hardware isolation mode at a time."
#endif

/*
  POWER WARNING:
  - Power the ESP32 from USB during development.
  - Never power the servo or other high-current loads from ESP32 3.3V.
  - Use a regulated external 5V supply for the servo and join its GND to
    ESP32 GND. While USB is connected, do not join external +5V to the
    ESP32 5V/VIN pin unless the exact board schematic confirms safe isolation.
  - A missing common ground or a weak 5V rail can cause fingerprint failures,
    servo jitter, LCD flicker, unstable readings, and ESP32 brownout resets.
*/

// If the LCD does not work with 0x27, try 0x3F.
#define LCD_ADDRESS 0x27
#define LCD_COLUMNS 20
#define LCD_ROWS 4
#define RED_LED_PIN 25
#define BUZZER_PIN 26
#define GREEN_LED_PIN 32
#define SERVO_PIN 13
#define ULTRASONIC_TRIG_PIN 33
#define ULTRASONIC_ECHO_PIN 34

/*
  HC-SR04 WARNING:
  ECHO is a 5V signal. Never connect it directly to ESP32 GPIO34.
  Use a voltage divider or a 3.3V-safe level shifter.
*/

// Dedicated pin map: every GPIO below has one component role only.
// GPIO 34 is input-only and is used only for the HC-SR04 Echo signal.
// GPIO 21/22 are LCD I2C, GPIO 16/17 are fingerprint UART2, and GPIO 13
// is the servo PWM signal. validatePinAssignments() checks for duplicates.
// RFID RC522 pins (ESP32 VSPI)
// POWER WARNING: RC522 VCC is 3.3V only; never connect it to 5V.
constexpr uint8_t RFID_SS_PIN = 5;
constexpr uint8_t RFID_SCK_PIN = 18;
constexpr uint8_t RFID_MOSI_PIN = 23;
constexpr uint8_t RFID_MISO_PIN = 19;
constexpr uint8_t RFID_RST_PIN = 27;

constexpr uint8_t LCD_SDA_PIN = 21;
constexpr uint8_t LCD_SCL_PIN = 22;

/*
  LCD I2C VOLTAGE NOTE:
  Many 5V backpacks pull SDA/SCL up to their VCC. ESP32 GPIO is 3.3V logic.
  Measure both idle bus lines first; use a bidirectional I2C level shifter if
  either is pulled above 3.3V. A 3.3V backpack supply is only an option when
  the exact LCD/backpack is confirmed to operate correctly at that voltage.
*/

// Fingerprint sensor pins (ESP32 UART2)
// Sensor TX connects to ESP32 RX; sensor RX connects to ESP32 TX.
// AS608 supply and UART voltage levels vary by module. Follow its label/data
// sheet. If powered at 5V, verify TX is 3.3V-safe before connecting GPIO16.
#define FINGER_RX_PIN 16
#define FINGER_TX_PIN 17

// The working standalone Fingerprint.ino uses 57600. Other rates are tried
// only after the exact working-test initialization pattern fails at 57600.
constexpr uint32_t FINGERPRINT_PRIMARY_BAUD = 57600;
constexpr uint32_t FINGERPRINT_FALLBACK_BAUD_RATES[] = {9600, 19200, 38400};
constexpr uint32_t FINGERPRINT_CAPTURE_TIMEOUT_MS = 15000;

constexpr int FINGERPRINT_NOT_RECOGNIZED = -1;
constexpr int FINGERPRINT_CAPTURE_TIMEOUT = -2;
constexpr int FINGERPRINT_READ_ERROR = -3;

const String ADMIN_RFID_UID = "47469C2E";
const String ADMIN_RFID_PLACEHOLDER = "PUT_ADMIN_CARD_UID_HERE";

enum Area {
  AREA_NONE = 0,
  MAIN_ENTRANCE = 1,
  COMPANY_A = 2,
  COMPANY_B = 3,
  SERVER_ROOM = 4,
  MANAGEMENT_ADMIN = 5,
  COMPANY_C = 6,
  COMPANY_D = 7
};

enum AccessMode {
  MODE_ENTRY,
  MODE_EXIT
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
    {3, "IT Admin", "IT", "IT", true, false, false, true, false, false, false, 0},
    {4, "Manager", "Management", "Manager", true, true, true, true, true, true, true, 0},
    {5, "Employee C", "Company C", "Employee", true, false, false, false, false, true, false, 0},
    {6, "Employee D", "Company D", "Employee", true, false, false, false, false, false, true, 0}};

constexpr size_t USER_COUNT = sizeof(users) / sizeof(users[0]);

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
String lastLcdLine3 = "";
String lastLcdLine4 = "";

void setup();
void loop();
void setupFingerprintOnlyDebug();
void loopFingerprintOnlyDebug();
void setupServoOnlyDebug();
void loopServoOnlyDebug();

void printMenu();
void printSystemStatus();
void printHardwareSelfTest();
bool validatePinAssignments();
void runSoftwareValidation();

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
void closeDoor(String reason);
void updateDoorState();
bool shouldCloseDoor();
bool shouldCloseDoorForState(bool isOpen,
                             unsigned long openElapsedMs,
                             bool personNear);
float readDistanceCm();
bool isPersonNear();
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
void lcdShowMessage(String line1,
                    String line2 = "",
                    String line3 = "",
                    String line4 = "");
String getLCDLine(String text);
bool isLCDMessageUnchanged(const String& line1,
                           const String& line2,
                           const String& line3,
                           const String& line4);
String getLCDAreaName(Area area);

void setupRFID();
void checkRFID();
String readRFIDUID();

void setupFingerprint();
bool setupFingerprintWithBaudScan();
bool initializeFingerprintAtBaud(uint32_t baudRate);
void printFingerprintParameters();
void testSimpleFingerprintScan();
bool enrollFingerprint(int id);
int readFingerprintID();

void handleSerialCommand();
void handleAreaSelection(char command);
void processSerialInput(String input);

String getAreaName(Area area);

User* findUserByFingerprintID(int fingerprintID);
bool checkPermission(User* user, Area area);
int getAreaBit(Area area);
bool isUserInsideArea(User* user, Area area);
void markUserInsideArea(User* user, Area area);
void markUserOutsideArea(User* user, Area area);
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
  Serial.println("- I2C LCD 20x4");
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
  Serial.println("Check LCD I2C and AS608 TX logic levels before connection.");
  Serial.println("All GND must be common.");
  Serial.println("If F/U/D fail together, check 5V power and common GND first.");
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
  Serial.println("- All users start OUTSIDE after reset");
  Serial.println("Attendance tracking initialized.");
  Serial.println("All users are OUTSIDE.");
  Serial.println("Anti-Passback enabled.");
  printAllInsideStatus();
  Serial.println();
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
  Serial.println("=== AS608 Fingerprint Isolation Test ===");

  // Preserve the known-working standalone Fingerprint.ino initialization
  // order exactly: explicit UART2 pins, finger.begin(), then password check.
  fingerprintSerial.begin(57600, SERIAL_8N1, FINGER_RX_PIN, FINGER_TX_PIN);
  delay(100);

  Serial.println("Checking fingerprint sensor...");

  finger.begin(57600);
  delay(100);

  fingerprintReady = finger.verifyPassword();
  if (!fingerprintReady) {
    fingerprintWorkingBaud = 0;
    Serial.println("ERROR: AS608 not detected even in isolation mode.");
    Serial.println("This means the problem is not caused by the full project logic.");
    Serial.println("Check current board, power, wiring, TX/RX, or GPIO16/GPIO17 availability.");
    Serial.println("Check:");
    Serial.println("- VCC");
    Serial.println("- GND");
    Serial.println("- TX/RX wiring");
    Serial.println("- Baud rate");
    return;
  }

  fingerprintWorkingBaud = 57600;
  Serial.println("SUCCESS: AS608 detected!");
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
  lcdShowMessage("ALERT TEST", "Red LED + Buzzer", "Testing Outputs", "Please Wait");

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
  lcdShowMessage("GREEN LED TEST", "GPIO 32", "Testing Output", "Please Wait");
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

  lcdShowMessage("SYSTEM UNLOCKED",
                 "Admin Verified",
                 "Failed Reset: 0",
                 "System Active");
  triggerAccessGrantedFeedback();
}

void applyLockedState() {
  systemLocked = true;
  waitingForEnrollmentID = false;
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

  disableAdminMode();
  waitingForEnrollmentID = false;
  Serial.println();
  Serial.println("[ADMIN MODE EXPIRED]");
  Serial.println();
  lcdShowMessage("ADMIN MODE", "Expired", "Enrollment Blocked", "Scan Admin RFID");
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
    lcdShowMessage("ACCESS DENIED",
                   "Unknown RFID",
                   "Failed: " + String(failedAttempts) + "/" +
                       String(MAX_FAILED_ATTEMPTS),
                   "Door Closed");
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
   * afterward, the shared 5 V supply is probably dropping under servo load.
   * Do not power the servo from ESP32 3.3 V. Prefer a stable external 5 V
   * servo supply, connect its GND to ESP32 GND, and measure the voltage.
   * The AS608 may draw about 120 mA and a moving servo can draw much more.
   */
  ESP32PWM::allocateTimer(0);
  doorServo.setPeriodHertz(50);
  const int servoPwmChannel = doorServo.attach(SERVO_PIN, 500, 2400);
  // Channel 0 is valid. Use attached() instead of treating a zero-valued
  // channel number as failure.
  servoInitialized = doorServo.attached();

  if (!servoInitialized) {
    doorOpen = false;
    Serial.println("[SERVO] Failed to attach a 50 Hz PWM signal on GPIO 13.");
    Serial.println("This is a PWM setup failure, not software detection of the motor.");
    return;
  }

  doorServo.write(DOOR_CLOSED_ANGLE);
  doorOpen = false;
  doorOpenedAt = 0;
  delay(300);
  Serial.println("[SERVO] 50 Hz PWM attached on GPIO 13.");
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
  Serial.println(getAreaName(selectedArea));
  Serial.print("Mode: ");
  Serial.println(getAccessModeName(currentMode));
  Serial.print("Servo Angle: ");
  Serial.println(DOOR_OPEN_ANGLE);

  lcdShowDoorOpen(currentMode == MODE_EXIT);
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

  return shouldCloseDoorForState(doorOpen, openElapsedMs, isPersonNear());
}

bool shouldCloseDoorForState(bool isOpen,
                             unsigned long openElapsedMs,
                             bool personNear) {
  return isOpen && openElapsedMs >= DOOR_MIN_OPEN_TIME_MS && !personNear;
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

bool isDistanceNear(float distanceCm) {
  return distanceCm > 0.0 && distanceCm <= PRESENCE_DISTANCE_CM;
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
    lcdShowMessage("PERSON DETECTED",
                   "Place Finger",
                   "Press R to Scan",
                   "Door Closed");
    presencePromptVisible = true;
  } else if (!personNear && presencePromptVisible) {
    lcdShowMenu();
    presencePromptVisible = false;
  }
}

void testDoor() {
  Serial.println();
  Serial.println("[SERVO TEST]");
  Serial.print("Signal pin: GPIO ");
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

  lcdShowMessage("SERVO TEST", "Open 90 deg", "Close 0 deg", "Check Motor");
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
  lcdShowMessage("SERVO TEST", "Test Complete", "Door Command: 0", "Check Motor");
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
    Serial.println("Check TRIG GPIO 33, ECHO GPIO 34, 5 V, common GND, and the Echo voltage divider.");
    Serial.println("GPIO 34 is input-only, which is correct for HC-SR04 Echo.");
    lcdShowMessage("ULTRA ERROR", "No Echo Received", "Check Wiring", "See Serial");
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
    lcdShowMessage("ULTRA TEST",
                   "Dist: " + String(lastDistanceCm, 1) + " cm",
                   "Near: " + String(personNear ? "YES" : "NO"),
                   "See Serial");
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
  lcdShowMessage("Smart Access System",
                 "ESP32 Ready",
                 "RFID + Fingerprint",
                 "Choose Area 1-7");
}

void lcdShowMenu() {
  lcdShowMessage("SMART ACCESS SYSTEM",
                 "System Ready",
                 "Choose Area",
                 "1-7 Serial");
}

void lcdShowSelectedArea(Area area) {
  lcdShowMessage("AREA SELECTED",
                 getLCDAreaName(area),
                 area == SERVER_ROOM ? "Security Area" : "Press R to Scan",
                 area == SERVER_ROOM ? "Press R to Scan" : "X for Exit Mode");
}

void lcdShowEnrollStart() {
  lcdShowMessage("ENROLL MODE",
                 "Enter ID in Serial",
                 "Valid Range: 1-127",
                 "Admin Mode Active");
}

void lcdShowEnrollStep(String line1, String line2) {
  lcdShowMessage("FINGERPRINT ENROLL", line1, line2, "Please Wait");
}

void lcdShowEnrollSuccess(int id) {
  lcdShowMessage("ENROLL SUCCESS",
                 "Fingerprint Saved",
                 "ID: " + String(id),
                 "Admin Mode Active");
}

void lcdShowEnrollFailed(String shortReason) {
  lcdShowMessage("ENROLL FAILED", shortReason, "Try Again", "Check Serial");
}

void lcdShowPlaceFinger() {
  lcdShowMessage("PLACE FINGER",
                 "Scanning...",
                 "Area: " + getLCDAreaName(selectedArea),
                 "Please Wait");
}

void lcdShowAccessGranted(User* user, Area area) {
  String userName = user == nullptr ? "Authorized User" : user->name;
  lcdShowMessage("ACCESS GRANTED",
                 "User: " + userName,
                 "Area: " + getLCDAreaName(area),
                 "Door Opening");
}

void lcdShowAccessDenied(String reason) {
  if (reason.indexOf("not recognized") >= 0) {
    lcdShowMessage("ACCESS DENIED",
                   "Unknown Finger",
                   "Try Again",
                   "Door Closed");
  } else if (reason.indexOf("not configured") >= 0) {
    lcdShowMessage("ACCESS DENIED",
                   "Unknown User",
                   "Check Configuration",
                   "Door Closed");
  } else if (reason.indexOf("not allowed") >= 0) {
    lcdShowMessage("ACCESS DENIED",
                   "No Permission",
                   "Area: " + getLCDAreaName(selectedArea),
                   "Door Closed");
  } else if (reason.indexOf("already inside") >= 0) {
    lcdShowMessage("ACCESS DENIED",
                   "Already Inside",
                   "Use Exit Mode",
                   "Door Closed");
  } else if (reason.indexOf("not inside") >= 0) {
    lcdShowMessage("EXIT DENIED",
                   "Not Inside",
                   "Use Entry First",
                   "Door Closed");
  } else if (reason.indexOf("Timed out") >= 0) {
    lcdShowMessage("ACCESS DENIED",
                   "Scan Timeout",
                   "Try Again",
                   "Door Closed");
  } else if (reason.indexOf("not detected") >= 0) {
    lcdShowMessage("FINGER SENSOR",
                   "Not Found",
                   "Check Wiring/UART",
                   "Access Blocked");
  } else {
    lcdShowMessage("ACCESS DENIED", "Try Again", "Check Serial", "Door Closed");
  }
}

void lcdShowRFIDStatus(String line1, String line2) {
  lcdShowMessage("RFID STATUS", line1, line2, "Check Serial");
}

void lcdShowError(String line1, String line2) {
  lcdShowMessage("ERROR", line1, line2, "Check Serial");
}

void lcdShowDoorOpen(bool exitMode) {
  lcdShowMessage("DOOR OPEN",
                 exitMode ? "Please Exit" : "Please Enter",
                 "Waiting Clear",
                 "Auto Close Soon");
}

void lcdShowDoorClosed() {
  lcdShowMessage("DOOR CLOSED", "System Ready", "Choose Area", "1-7 Serial");
}

void lcdShowSystemLocked() {
  lcdShowMessage("SYSTEM LOCKED",
                 "Failed: " + String(failedAttempts) + "/" +
                     String(MAX_FAILED_ATTEMPTS),
                 "Scan Admin RFID",
                 "Access Blocked");
}

void lcdShowAdminMode() {
  lcdShowMessage("ADMIN MODE", "Enabled", "Enroll Allowed", "60 sec Timeout");
}

void lcdShowMessage(String line1,
                    String line2,
                    String line3,
                    String line4) {
  if (!lcdInitialized) {
    return;
  }

  line1 = getLCDLine(line1);
  line2 = getLCDLine(line2);
  line3 = getLCDLine(line3);
  line4 = getLCDLine(line4);

  if (isLCDMessageUnchanged(line1, line2, line3, line4)) {
    return;
  }

  lastLcdLine1 = line1;
  lastLcdLine2 = line2;
  lastLcdLine3 = line3;
  lastLcdLine4 = line4;
  lastLCDMessageAt = millis();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
  lcd.setCursor(0, 2);
  lcd.print(line3);
  lcd.setCursor(0, 3);
  lcd.print(line4);
}

bool isLCDMessageUnchanged(const String& line1,
                           const String& line2,
                           const String& line3,
                           const String& line4) {
  return line1 == lastLcdLine1 && line2 == lastLcdLine2 &&
         line3 == lastLcdLine3 && line4 == lastLcdLine4;
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
  Serial.println("1 = Main Entrance");
  Serial.println("2 = Company A");
  Serial.println("3 = Company B");
  Serial.println("4 = Server Room");
  Serial.println("5 = Management / Admin");
  Serial.println("6 = Company C");
  Serial.println("7 = Company D");
  Serial.println("E = Enroll fingerprint (Admin Mode only)");
  Serial.println("R = Read/test fingerprint");
  Serial.println("F = Detect AS608 using working Fingerprint.ino method");
  Serial.println("P = Simple fingerprint scan/match test");
  Serial.println("V = Run full software validation tests");
  Serial.println("M = Print menu");
  Serial.println("S = Show system status");
  Serial.println("T = Test Red LED and buzzer");
  Serial.println("G = Test Green LED");
  Serial.println("X = Exit Mode");
  Serial.println("D = Test Servo Door");
  Serial.println("U = Test Ultrasonic Sensor (10 readings)");
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
  Serial.println(" (input-only pin: correct)");
  Serial.print("Pin Conflict Check: ");
  Serial.println(validatePinAssignments() ? "PASS" : "FAIL");
  Serial.println("Power Note: If servo and fingerprint fail together, check stable 5 V power and common GND.");
  Serial.println("====================================================");
  Serial.println();

  lcdShowMessage("HW SELF TEST",
                 rfidInitialized ? "RFID: OK" : "RFID: FAIL",
                 fingerprintReady ? "Finger: OK" : "Finger: FAIL",
                 lcdInitialized ? "LCD: OK" : "LCD: FAIL");
}

void runSoftwareValidation() {
  constexpr int MAX_STORED_VALIDATION_FAILURES = 70;
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
  AccessMode savedCurrentMode = currentMode;
  bool savedWaitingForEnrollmentID = waitingForEnrollmentID;
  bool savedPresencePromptVisible = presencePromptVisible;
  String savedLcdLine1 = lastLcdLine1;
  String savedLcdLine2 = lastLcdLine2;
  String savedLcdLine3 = lastLcdLine3;
  String savedLcdLine4 = lastLcdLine4;

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
  User* itAdmin = findUserByFingerprintID(3);
  User* manager = findUserByFingerprintID(4);
  User* employeeC = findUserByFingerprintID(5);
  User* employeeD = findUserByFingerprintID(6);

  validate("All six configured fingerprint IDs resolve",
           employeeA != nullptr && employeeB != nullptr && itAdmin != nullptr &&
               manager != nullptr && employeeC != nullptr && employeeD != nullptr,
           "IDs 1-6 mapped to local users",
           "One or more IDs missing");
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

  const Area allAreas[] = {MAIN_ENTRANCE,
                           COMPANY_A,
                           COMPANY_B,
                           SERVER_ROOM,
                           MANAGEMENT_ADMIN,
                           COMPANY_C,
                           COMPANY_D};
  constexpr size_t allAreaCount = sizeof(allAreas) / sizeof(allAreas[0]);
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
  validate("Anti-passback rejects duplicate Company A entry",
           !canUserEnterArea(employeeA, COMPANY_A),
           "entry denied",
           "entry allowed");
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
  validate("Entry is allowed again after exit",
           canUserEnterArea(employeeA, COMPANY_A),
           "entry allowed",
           "entry denied");
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

  validate("LCD dimensions are 20x4",
           LCD_COLUMNS == 20 && LCD_ROWS == 4,
           "20 columns, 4 rows",
           String(LCD_COLUMNS) + "x" + String(LCD_ROWS));
  String trimmedLCDLine = getLCDLine("1234567890123456789012345");
  validate("LCD long lines are trimmed to 20 characters",
           trimmedLCDLine.length() == LCD_COLUMNS,
           "20 characters",
           String(trimmedLCDLine.length()) + " characters");
  validate("LCD short lines remain unchanged",
           getLCDLine("System Ready") == "System Ready",
           "System Ready",
           getLCDLine("System Ready"));
  lastLcdLine1 = "A";
  lastLcdLine2 = "B";
  lastLcdLine3 = "C";
  lastLcdLine4 = "D";
  validate("LCD duplicate screen detection prevents refresh",
           isLCDMessageUnchanged("A", "B", "C", "D"),
           "unchanged",
           "changed");
  validate("LCD changed screen requests refresh",
           !isLCDMessageUnchanged("A", "B", "C", "Different"),
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
           !shouldCloseDoorForState(true, DOOR_MIN_OPEN_TIME_MS - 1, false),
           "stay open",
           "close");
  validate("Door closes after minimum time when area is clear",
           shouldCloseDoorForState(true, DOOR_MIN_OPEN_TIME_MS, false),
           "close",
           "stay open");
  validate("Door stays open while a person remains near",
           !shouldCloseDoorForState(true, DOOR_MIN_OPEN_TIME_MS, true),
           "stay open",
           "close");
  validate("Closed door never requests another close",
           !shouldCloseDoorForState(false, DOOR_MIN_OPEN_TIME_MS, false),
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
  validate("Ultrasonic pulse timeout is finite",
           ULTRASONIC_ECHO_TIMEOUT_US == 30000,
           "30000 us",
           String(ULTRASONIC_ECHO_TIMEOUT_US) + " us");

  const char serialCommands[] = {'E', 'R', 'P', 'X', 'M', 'S',
                                 'T', 'G', 'D', 'U', 'F', 'V'};
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
  bool areaCommandMappingValid = true;
  for (int areaNumber = 1; areaNumber <= 7; ++areaNumber) {
    Area area = static_cast<Area>(areaNumber);
    areaCommandMappingValid &= getAreaName(area) != "None";
  }
  validate("Serial area commands 1-7 map to valid areas",
           areaCommandMappingValid,
           "all valid",
           "one or more invalid");
  validate("Component GPIO assignments have no conflicts",
           validatePinAssignments(),
           "no conflicts",
           "conflict found");

  for (size_t i = 0; i < USER_COUNT; ++i) {
    users[i].insideMask = savedInsideMasks[i];
  }
  systemLocked = savedSystemLocked;
  adminMode = savedAdminMode;
  failedAttempts = savedFailedAttempts;
  adminModeStartTime = savedAdminModeStartTime;
  currentMode = savedCurrentMode;
  waitingForEnrollmentID = savedWaitingForEnrollmentID;
  presencePromptVisible = savedPresencePromptVisible;
  lastLcdLine1 = savedLcdLine1;
  lastLcdLine2 = savedLcdLine2;
  lastLcdLine3 = savedLcdLine3;
  lastLcdLine4 = savedLcdLine4;

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

void printSystemStatus() {
  Serial.println();
  Serial.println("[SYSTEM STATUS]");
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
  Serial.println("Power Note: If servo and fingerprint fail together, check stable 5 V power and common GND.");
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
                 "Check Serial",
                 "Users + Occupancy",
                 systemLocked ? "System Locked" : "System Active");
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
    Serial.println("Check 3.3V power, SPI wiring, and GPIO 27 reset wiring.");
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

bool setupFingerprintWithBaudScan() {
  fingerprintReady = false;
  fingerprintWorkingBaud = 0;
  lcdShowMessage("FINGER TEST",
                 "Working Test Setup",
                 "UART2 GPIO 16/17",
                 "Baud: 57600");

  Serial.println();
  Serial.println("=== AS608 Fingerprint Test ===");
  Serial.println("Using working Fingerprint.ino pattern");
  Serial.println("UART: HardwareSerial(2)");
  Serial.print("RX: GPIO");
  Serial.println(FINGER_RX_PIN);
  Serial.print("TX: GPIO");
  Serial.println(FINGER_TX_PIN);
  Serial.println("Format: SERIAL_8N1");
  Serial.print("Baud: ");
  Serial.println(FINGERPRINT_PRIMARY_BAUD);
  Serial.println();

  Serial.println("Trying AS608 at 57600...");
  if (initializeFingerprintAtBaud(FINGERPRINT_PRIMARY_BAUD)) {
    Serial.println("[FINGERPRINT] SUCCESS: AS608 detected at 57600");
    printFingerprintParameters();
    lcdShowMessage("FINGERPRINT OK",
                   "AS608 Detected",
                   "Baud: 57600",
                   "Ready");
    Serial.println();
    return true;
  }

  Serial.println("[FINGERPRINT] ERROR: AS608 not detected using the working-test initialization pattern");
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
      lcdShowMessage("FINGERPRINT OK",
                     "AS608 Detected",
                     "Baud: " + String(baudRate),
                     "Ready");
      Serial.println();
      return true;
    }
  }

  Serial.println("[FINGERPRINT ERROR]");
  Serial.println("ERROR: AS608 not detected!");
  Serial.println("Check:");
  Serial.println("- VCC and supported module voltage");
  Serial.println("- Common GND");
  Serial.println("- Sensor TX -> ESP32 RX2 GPIO16");
  Serial.println("- Sensor RX -> ESP32 TX2 GPIO17");
  Serial.println("- Baud rate and default password");
  Serial.println();
  lcdShowMessage("FINGER ERROR",
                 "AS608 Missing",
                 "Check Wiring",
                 "See Serial");
  return false;
}

bool initializeFingerprintAtBaud(uint32_t baudRate) {
  // Match the known-working Fingerprint.ino begin order exactly. This helper is
  // called only at startup or by command F, never repeatedly from loop().
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
  Serial.println("=== AS608 Simple Scan Test ===");

  if (!fingerprintReady) {
    Serial.println("ERROR: AS608 is not initialized.");
    Serial.println("Run F to repeat the working-test detection method.");
    lcdShowMessage("FINGER ERROR",
                   "AS608 Missing",
                   "Press F to Test",
                   "See Serial");
    return;
  }

  Serial.println("Place a finger on the sensor (15 second timeout)...");
  lcdShowMessage("PLACE FINGER", "Scanning...", "Please Wait", "Test Mode");

  uint8_t result = FINGERPRINT_NOFINGER;
  if (!waitForFingerImage(FINGERPRINT_CAPTURE_TIMEOUT_MS, result)) {
    Serial.println("Scan timeout: no finger detected.");
    lcdShowMessage("FINGER TEST", "Scan Timeout", "No Finger", "Try Again");
    return;
  }

  if (result != FINGERPRINT_OK) {
    Serial.print("Image capture error: ");
    Serial.println(getFingerprintError(result));
    lcdShowMessage("FINGER ERROR", "Capture Failed", "Check Serial", "Try Again");
    return;
  }

  Serial.println("Fingerprint detected!");
  result = finger.image2Tz();
  if (result != FINGERPRINT_OK) {
    Serial.print("Image conversion error: ");
    Serial.println(getFingerprintError(result));
    lcdShowMessage("FINGER ERROR", "Bad Image", "Try Again", "See Serial");
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
                   "ID: " + String(finger.fingerID),
                   "Confidence: " + String(finger.confidence),
                   "Scan Test Only");
    return;
  }

  if (result == FINGERPRINT_NOTFOUND) {
    Serial.println("Fingerprint not found in database.");
    lcdShowMessage("NO FINGER MATCH", "Not In Database", "Test Complete", "Try Another");
    return;
  }

  Serial.print("Search error: ");
  Serial.println(result);
  lcdShowMessage("FINGER ERROR", "Search Failed", "Check Serial", "Try Again");
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
    lcdShowMessage("ADMIN REQUIRED",
                   "Scan Admin RFID",
                   "Enrollment Blocked",
                   "Access Denied");
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

void processSerialInput(String input) {
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
      lcdShowMessage("ADMIN REQUIRED",
                     "Scan Admin RFID",
                     "Enrollment Blocked",
                     "Access Denied");
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
        lcdShowMessage("ADMIN REQUIRED",
                       "Scan Admin RFID",
                       "Enrollment Blocked",
                       "Access Denied");
        triggerErrorAlert();
        return;
      }
      if (!fingerprintReady) {
        Serial.println("[FINGERPRINT ERROR]");
        Serial.println("Cannot enroll. Sensor not detected.");
        Serial.println("Run command F for baud-rate, wiring, and power diagnostics.");
        lcdShowMessage("FINGER ERROR",
                       "Sensor Missing",
                       "Cannot Enroll",
                       "Press F to Test");
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
      testFingerprintAccess();
      break;

    case 'F':
      setupFingerprintWithBaudScan();
      break;

    case 'P':
      testSimpleFingerprintScan();
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
      if (isSystemLocked()) {
        Serial.println("[SYSTEM LOCKED]");
        Serial.println("Exit Mode is blocked. Scan Admin RFID card to unlock.");
        showLockdownStatus();
        triggerErrorAlert();
        return;
      }
      if (selectedArea == AREA_NONE) {
        Serial.println("[EXIT MODE ERROR]");
        Serial.println("Please select an area first using 1-7.");
        lcdShowError("Select Area", "Before Exit");
        triggerErrorAlert();
        return;
      }
      if (doorOpen) {
        Serial.println("[EXIT MODE ERROR]");
        Serial.println("Door is already open.");
        lcdShowError("Door Is Open", "Wait To Close");
        triggerErrorAlert();
        return;
      }
      currentMode = MODE_EXIT;
      presencePromptVisible = false;
      Serial.println("[EXIT MODE]");
      Serial.print("Selected Area: ");
      Serial.println(getAreaName(selectedArea));
      Serial.println("Enter R and scan a permitted fingerprint.");
      lcdShowMessage("EXIT MODE",
                     "Area: " + getLCDAreaName(selectedArea),
                     "Press R to Scan",
                     "No Presence Needed");
      break;

    case 'D':
      testDoor();
      break;

    case 'U':
      printUltrasonicStatus();
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
    case MAIN_ENTRANCE:
      return "Main Entrance";
    case COMPANY_A:
      return "Company A";
    case COMPANY_B:
      return "Company B";
    case SERVER_ROOM:
      return "Server Room";
    case MANAGEMENT_ADMIN:
      return "Management / Admin";
    case COMPANY_C:
      return "Company C";
    case COMPANY_D:
      return "Company D";
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
  if (areaNumber < static_cast<int>(MAIN_ENTRANCE) ||
      areaNumber > static_cast<int>(COMPANY_D)) {
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

  const Area trackedAreas[] = {MAIN_ENTRANCE,
                               COMPANY_A,
                               COMPANY_B,
                               COMPANY_C,
                               COMPANY_D,
                               SERVER_ROOM,
                               MANAGEMENT_ADMIN};
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
  const Area trackedAreas[] = {MAIN_ENTRANCE,
                               COMPANY_A,
                               COMPANY_B,
                               COMPANY_C,
                               COMPANY_D,
                               SERVER_ROOM,
                               MANAGEMENT_ADMIN};
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

  if (grantedMode == MODE_EXIT) {
    markUserOutsideArea(user, area);
  } else {
    markUserInsideArea(user, area);
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
    lcdShowMessage(
        "EXIT GRANTED",
        "User: " +
            (user == nullptr ? String("Authorized User") : user->name),
        "Area: " + getLCDAreaName(area),
        "Door Opening");
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
  if (isSystemLocked()) {
    Serial.println("[SYSTEM LOCKED]");
    Serial.println("Fingerprint access is blocked.");
    Serial.println("Scan Admin RFID card to unlock.");
    incrementFailedAttempts("Fingerprint access attempted during lockdown");
    showLockdownStatus();
    triggerAccessDeniedAlert();
    return;
  }

  if (selectedArea == AREA_NONE) {
    Serial.println("[ERROR]");
    Serial.println("Please select an area first using 1-7");
    lcdShowError("Select Area", "First");
    triggerErrorAlert();
    return;
  }

  if (doorOpen) {
    Serial.println("[ACCESS BLOCKED]");
    Serial.println("Door is already open. Wait for it to close.");
    lcdShowError("Door Is Open", "Wait To Close");
    triggerErrorAlert();
    returnToEntryMode();
    return;
  }

  if (isPresenceRequiredForMode(currentMode) && !isPersonNear()) {
    Serial.println("[ENTRY BLOCKED]");
    Serial.println("No person detected near door.");
    if (lastDistanceCm <= 0.0) {
      Serial.println("Ultrasonic measurement unavailable or timed out.");
    } else {
      Serial.print("Current Distance: ");
      Serial.print(lastDistanceCm, 1);
      Serial.println(" cm");
    }
    lcdShowMessage("COME CLOSER",
                   "No Person Near",
                   "Scan Blocked",
                   "Door Closed");
    return;
  }

  if (!fingerprintReady) {
    Serial.println("[FINGERPRINT ERROR]");
    Serial.println("Cannot scan. Sensor not detected.");
    Serial.println("Run command F for baud-rate, wiring, and power diagnostics.");
    lcdShowMessage("FINGER ERROR",
                   "Sensor Missing",
                   "Cannot Scan",
                   "Press F to Test");
    triggerErrorAlert();
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
                 "ID: " + String(fingerprintID),
                 "Confidence: " + String(finger.confidence),
                 "Checking Access");

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
