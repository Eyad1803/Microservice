# Smart Office Building Access System — Task 7

This Arduino project continues the Task 6 hardware and security system, adds Company C and Company D, and tracks every configured user's inside/outside state independently for all seven areas. Anti-passback prevents duplicate entry until that user completes a valid exit from the same area.

Task 7 is a software-only update. It does not add Wi-Fi, an app, database, real push buttons, parking logic, a relay, multiple servos, sensors, or persistent storage. All users start outside after an ESP32 reset.

## Standalone project components

- ESP32 development board
- AS608 / JM-101B 3.3 V UART fingerprint sensor (about 60 mA)
- RC522 RFID reader used only for the Admin Master Card
- 20x4 I2C LCD
- SG90-compatible servo door actuator
- HC-SR04 ultrasonic presence sensor
- Red LED, green LED, and active buzzer
- External regulated 5 V supply and required voltage-level protection

## Implemented Tasks 1–7

- **Task 1:** Serial area selection, fingerprint recognition/enrollment, local users and permissions, and Admin RFID.
- **Task 2:** Four-line 20x4 LCD status and error messages with trimmed rows and duplicate-screen suppression.
- **Task 3:** Red LED and active-buzzer feedback for denials and errors.
- **Task 4:** Green LED feedback for granted entry, granted exit, and recognized Admin RFID.
- **Task 5:** Three-attempt Lockdown Mode, Admin unlock, 60-second Admin Mode, and protected enrollment.
- **Task 6:** GPIO13 servo door plus GPIO33/34 ultrasonic presence and safe automatic closing.
- **Task 7:** Company C/D, users 5/6, per-user/per-area anti-passback, attendance masks, and occupancy counts.

## Project Logic Contract

- Fingerprint identifies the user but does not open the door directly; permissions and anti-passback decide access.
- The servo opens only after Access Granted or Exit Granted. Command `D` is the isolated movement-test exception.
- Ultrasonic detects presence only and never opens the door.
- RFID is Admin-only and never opens the door directly.
- Attendance changes only after the door software state becomes `OPEN`.
- Diagnostic commands do not change attendance or security state.
- Enrollment is allowed only while Admin Mode is active.
- Lockdown is cleared only by the configured Admin RFID card.

## Power safety — read before connecting USB

The proposed power plan is safe **only with the protections below in place**. Do not power the complete assembled circuit until the LCD I2C voltage, JM-101B 3.3 V supply, HC-SR04 Echo divider, and external 5 V output have been checked with a multimeter.

Recommended development power arrangement:

- Power the ESP32 from the computer USB connector.
- Power the RC522 only from the ESP32 `3V3` pin.
- Power this project's AS608 / JM-101B from ESP32 `3V3`, as specified for the selected module.
- Use a regulated external 5 V rail for the servo and, with the required level protection, the LCD and HC-SR04.
- Connect the external supply GND directly to ESP32 GND so every signal has the same reference.
- While USB is connected, **do not connect the external supply +5 V to the ESP32 `5V`/`VIN` pin** unless the exact DevKit schematic confirms that its USB and VIN paths are safely isolated. Clone boards differ, and joining two 5 V sources can back-feed a computer USB port or a regulator.
- Never connect external 5 V to `3V3`, and never allow a powered 5 V peripheral output to drive an unpowered ESP32 GPIO.

Use a star-like ground arrangement: return the servo's VCC/GND current directly to the external supply, then join that supply GND to the ESP32 GND. Do not route servo current through the ESP32 board or thin breadboard tracks. Confirm polarity before applying power.

## Wiring

### RC522 to ESP32

| RC522 | ESP32 |
| --- | --- |
| SDA / SS | GPIO 5 |
| SCK | GPIO 18 |
| MOSI | GPIO 23 |
| MISO | GPIO 19 |
| RST | GPIO 27 |
| 3.3V | 3V3 |
| GND | GND |

> Power the RC522 from **3.3 V only**.

### AS608 / JM-101B fingerprint sensor to ESP32 UART2

| JM-101B signal | ESP32 |
| --- | --- |
| VCC | `3V3` — this selected JM-101B is specified for 3.3 V |
| GND | Common GND |
| UART TX | GPIO 16 / RX2 |
| UART RX | GPIO 17 / TX2 |
| USB D+ | Not connected |
| USB D- | Not connected |

The selected sensor is the AS608 / JM-101B profile documented by the product specification as 3.3 V, about 60 mA, 500 dpi, with USB and UART interfaces. This project uses **UART only**. Sensor UART TX connects to ESP32 RX, and sensor UART RX connects to ESP32 TX. If the connector exposes USB `D+` or `D-`, leave them disconnected; they are not UART signals and must never be connected to GPIO16/GPIO17. Use the labels on the sensor PCB or its exact datasheet and do not trust wire colors blindly, because sellers may use different wire colors. The UART uses 8 data bits, no parity, and 1 stop bit. `TOUCH`, `WAKE`, illumination, and other optional wires are not required for UART connection detection unless the exact JM-101B documentation explicitly assigns them.

Do not connect this JM-101B to 5 V unless the exact physical board and its manufacturer documentation independently confirm 5 V tolerance. For the sensor selected in this project, the documented and recommended VCC is 3.3 V. Incorrect sensor power can prevent detection even when the UART code and wiring are correct.

### 20x4 I2C LCD to ESP32

| I2C LCD | ESP32 |
| --- | --- |
| GND | GND |
| VCC | External 5 V through a bidirectional I2C level shifter, or a confirmed-safe module arrangement |
| SDA | Level shifter → GPIO 21 |
| SCL | Level shifter → GPIO 22 |

All devices must share common ground.

> ESP32 GPIO uses 3.3 V logic. Many 5 V LCD backpacks pull SDA and SCL up to 5 V. With the LCD powered but disconnected from the ESP32, measure SDA and SCL to GND. If either idles above 3.3 V, do not connect it directly: use a bidirectional I2C level shifter with 3.3 V pull-ups on the ESP32 side and 5 V pull-ups on the LCD side. Powering a backpack at 3.3 V is acceptable only when that exact LCD/backpack displays reliably and its logic requirements are confirmed.

### Red LED and active buzzer

| Component | ESP32 |
| --- | --- |
| GPIO 25 | 220Ω or 330Ω resistor, then red LED anode/long leg |
| Red LED cathode/short leg | GND |
| Active buzzer `+` | GPIO 26 |
| Active buzzer `-` | GND |

All devices must share common ground. The buzzer is assumed to be an active buzzer that sounds while GPIO 26 is HIGH.

> Check the buzzer's voltage and current requirement. If its current is unknown or excessive for a GPIO, use an NPN transistor or logic-level MOSFET driver. Add the appropriate flyback protection if the buzzer is magnetic/inductive.

### Green LED

| Component | ESP32 |
| --- | --- |
| GPIO 32 | 220Ω or 330Ω resistor, then green LED anode/long leg |
| Green LED cathode/short leg | GND |

The green LED is reserved for successful access and recognized Admin Master Card events. It remains off during denial and error feedback.

### Servo door

| Servo wire | ESP32 / power |
| --- | --- |
| Signal, orange/yellow | GPIO 13 |
| VCC, red | Regulated external 5 V supply; **not** ESP32 3.3 V and not the USB-powered board rail |
| GND, brown/black | External supply GND joined to ESP32 GND |

The closed angle is 0° and the open angle is 90°. Never power the servo from ESP32 3.3 V. Size the supply for the exact servo's stall current plus the other 5 V loads and margin. If the servo current is unknown, a reputable, current-limited regulated 5 V / 3 A supply is a conservative staging choice for one small servo plus these peripherals; verify the exact servo rating before final installation. A 470–1000 µF electrolytic capacitor rated at least 10 V, in parallel with a 0.1 µF ceramic near the servo supply connection, can help with short transients but cannot compensate for an undersized supply. Observe electrolytic polarity.

### HC-SR04 ultrasonic sensor

| HC-SR04 | ESP32 / power |
| --- | --- |
| VCC | 5 V |
| GND | Common GND |
| TRIG | GPIO 33 |
| ECHO | GPIO 34 through a voltage divider |

HC-SR04 ECHO is normally 5 V and must not connect directly to the ESP32. Use, for example, 1kΩ from ECHO to GPIO 34 and 2kΩ from GPIO 34 to GND to reduce the signal to approximately 3.3 V.

### USB-C / external 5 V module

Do not assume that a USB-C PD/QC output is 5 V. With the project disconnected, measure the module output and confirm correct polarity and approximately 5 V. Repeat the measurement while supplying a representative load if possible. Never connect a 9 V, 12 V, 15 V, or 20 V output to this project. Set or lock any trigger module to 5 V before wiring it, and use a current-limited supply for first power-up.

### Current load and brownout risk

The servo is the dominant, rapidly changing load. The selected JM-101B is specified at about 60 mA from 3.3 V, while the LCD backlight, HC-SR04, buzzer, and RC522 add more load. The exact total cannot be certified without the precise servo, LCD backpack, buzzer, and module datasheets.

Do not use the ESP32 3.3 V regulator or its USB/VIN path as the complete project's power distribution rail. Servo start-up, reversal, or stall can pull the 5 V rail down and cause:

- JM-101B UART connection failures caused by 3.3 V rail or common-ground disturbance;
- ESP32 brownout resets;
- servo jitter;
- ultrasonic timeouts or unstable readings;
- LCD flicker or corruption.

Select the external regulated 5 V supply from the servo's worst-case/stall current plus all other 5 V loads and reasonable margin. Measure the 5 V rail at the peripheral connectors while the servo moves; a supply may read 5 V with no load and still collapse under a current spike.

## Arduino setup

1. Install the ESP32 board package in Arduino IDE.
2. Install **MFRC522**, **Adafruit Fingerprint Sensor Library**, **LiquidCrystal_I2C 2.0.0**, and **ESP32Servo 3.2.1** using Library Manager.
3. Open `SmartOfficeAccessStep1.ino`.
4. Select **ESP32 Dev Module** and the correct serial port.
5. Upload, then open Serial Monitor at **115200 baud** with **Newline** or **Both NL & CR**.

### Fingerprint integration note

The AS608 / JM-101B integration follows the known-working standalone `Fingerprint.ino` structure. It uses the sensor's UART interface, `HardwareSerial(2)`, GPIO 16 as RX, GPIO 17 as TX, `SERIAL_8N1`, and baud 57600. USB D+/D- are not used. Initialization deliberately uses this order:

```cpp
fingerprintSerial.begin(57600, SERIAL_8N1, FINGER_RX_PIN, FINGER_TX_PIN);
finger.begin(57600);
fingerprintReady = finger.verifyPassword();
```

The internal `runFingerprintStandaloneStyleTest()` function and command `F` use that exact 57600 method and print the same detection result plus sensor parameters. They do not run access checks or change attendance/security state. Startup uses the same function first; only startup may try 9600, 19200, and 38400 afterward as recovery rates. The detected rate is stored in `fingerprintWorkingBaud`. UART2 is not restarted from the main loop.

In full-system mode the JM-101B is the first peripheral initialized, before LCD, RFID, alerts, servo, and ultrasonic setup. This makes its startup order match isolation mode and removes earlier peripheral initialization as a possible software interference source. Physically connected modules still load the power rails even before their setup functions run, so full mode must also be tested with only the 3.3 V JM-101B connected before reconnecting other hardware.

If the standalone `Fingerprint.ino` works but the main project does not, compare the `HardwareSerial` object, baud rate, begin order, RX/TX constants, and `verifyPassword()` result. Also verify that no other connected hardware is pulling down the sensor supply.

### Fingerprint-only isolation mode

The sketch contains a permanent compile-time diagnostic switch near the top of `SmartOfficeAccessStep1.ino`:

```cpp
#define FINGERPRINT_ONLY_DEBUG 1
```

- `1`: uses file-level conditional compilation to build an exact copy of the supplied standalone `Fingerprint.ino`. Only Adafruit Fingerprint, Serial Monitor, `HardwareSerial(2)`, and the AS608 test are compiled. RFID, LCD, servo, ultrasonic, LEDs, buzzer, security, attendance, door logic, their libraries, and their global objects are excluded from the firmware image.
- `0`: runs the complete Smart Office Access System normally.

For the isolation test, leave the flag at `1` and connect only:

- ESP32 to the computer by USB;
- JM-101B VCC to ESP32 3.3 V;
- JM-101B GND to ESP32 GND;
- JM-101B UART TX to ESP32 GPIO 16 / RX2;
- JM-101B UART RX to ESP32 GPIO 17 / TX2;
- JM-101B USB D+ and D- left disconnected.

Disconnect the RFID, LCD, servo, ultrasonic sensor, LEDs, buzzer, and external peripheral supply for this test. Open Serial Monitor at 115200 baud. The expected startup result is `SUCCESS: AS608 / JM-101B detected!`, followed by the sensor parameters and live fingerprint matching. If isolation succeeds but full mode fails, reconnect modules one at a time to identify power or initialization interference.

If isolation reports `ERROR: AS608 / JM-101B not detected even in isolation mode`, the cause is outside the full application flow. Check the 3.3 V supply, common ground, labeled UART TX/RX crossover, cable continuity, confirm that USB D+/D- were not mistaken for UART pins, and verify that GPIO 16/17 are available.

#### ESP32 WROVER / PSRAM warning

Some ESP32 WROVER/PSRAM modules reserve GPIO 16 and GPIO 17 internally for PSRAM, so those pins may be unavailable even though a WROOM-based ESP32 DevKit can use them for UART2. Confirm the module marking and board schematic. Do not change this project automatically: only after confirming a WROVER/PSRAM conflict, select alternative UART-capable pins that are actually broken out and account for boot-strapping requirements. GPIO 4 and GPIO 2 are possible candidates on some boards, but they must be checked against that exact board and the rest of this project's pin map before use.

After the isolation test succeeds, change the switch back to:

```cpp
#define FINGERPRINT_ONLY_DEBUG 0
```

Both configurations are kept compileable. The isolation test succeeded, so the flag is now `0` for normal full-system operation.

### Servo-only isolation mode

The sketch also contains a servo diagnostic switch:

```cpp
#define SERVO_ONLY_DEBUG 1
```

Only one isolation switch may be `1` at a time. Both isolation tests succeeded, so the current normal-operation configuration is:

```cpp
#define FINGERPRINT_ONLY_DEBUG 0
#define SERVO_ONLY_DEBUG 0
```

Servo-only mode initializes only Serial Monitor and the `ESP32Servo` object. It follows the working `servo_test.ino` pattern: a simple `attach()`, followed continuously by 0°, 90°, 180°, and 90° commands with one-second delays. RFID, fingerprint, LCD, ultrasonic, LEDs, buzzer, security, attendance, and door-control logic are not initialized or executed.

The standalone file used GPIO 18, but the complete project already reserves GPIO 18 for the RC522 SPI clock. Sharing that line would corrupt both signals. The isolation test therefore uses the project's conflict-free servo pin GPIO 13. Move the servo signal wire to GPIO 13; do not change it to GPIO 18 in the complete system.

Full-system servo setup also defaults to the working simple-attach pattern:

```cpp
#define SERVO_USE_SIMPLE_ATTACH 1
doorServo.attach(SERVO_PIN);  // SERVO_PIN is GPIO 13
```

Setting the option to `0` keeps the advanced 50 Hz / 500–2400 µs attach path available, but simple attach is the tested default. Attachment success is checked with `doorServo.attached()` because ESP32Servo channel `0` is valid.

For this test, connect only:

- ESP32 to the computer by USB;
- servo signal wire to GPIO 13;
- servo VCC to a regulated external 5 V supply;
- servo GND directly to external supply GND;
- external supply GND to ESP32 GND.

Do not connect external +5 V to ESP32 `VIN/5V` while USB is connected. Disconnect the servo horn from the door mechanism, or confirm that the mechanism can safely travel through 0°–180°, because the reference diagnostic exercises the full range. The full door application still uses only 0° closed and 90° open.

Expected Serial output repeats:

```text
=== SERVO ONLY DEBUG MODE ===
SUCCESS: Servo PWM attached.
Moving to 0 degrees
Moving to 90 degrees
Moving to 180 degrees
Back to 90 degrees
```

The GPIO13 isolation test completed successfully, and `SERVO_ONLY_DEBUG` has been returned to `0` to restore the full project. If PWM attachment succeeds but the servo does not move in a future test, the ESP32 cannot detect that mechanically; check external 5 V current capacity, common ground, GPIO 13 wiring, and the servo itself.

The installed ESP32Servo library may return PWM channel `0` from `attach()` for the first successfully attached servo. Channel zero is valid, so the project checks `doorServo.attached()` rather than incorrectly treating `attach(...) > 0` as the success condition. The earlier `ERROR: Servo PWM attach failed!` message on GPIO 13 was caused by that return-value check and did not prove a GPIO failure.

The LCD defaults to I2C address `0x27`:

```cpp
// If the LCD does not work with 0x27, try 0x3F.
#define LCD_ADDRESS 0x27
#define LCD_COLUMNS 20
#define LCD_ROWS 4
```

If the LCD is not detected, check its wiring and change `LCD_ADDRESS` to `0x3F`. The startup Serial output reports whether the configured address responded.

## Commands

| Command | Action |
| --- | --- |
| `1` | Select Main Entrance |
| `2` | Select Company A |
| `3` | Select Company B |
| `4` | Select Server Room |
| `5` | Select Management / Admin |
| `6` | Select Company C |
| `7` | Select Company D |
| `E` | Enroll a fingerprint while Admin Mode is active |
| `R` | Read a fingerprint and check permission |
| `F` | Detect the 3.3 V AS608 / JM-101B over UART2 and print sensor parameters |
| `P` | Simple timed fingerprint capture and database-match diagnostic |
| `V` | Run full software validation mini tests without hardware I/O |
| `M` | Print the command menu |
| `S` | Show system status |
| `T` | Test the red LED and active buzzer |
| `G` | Test the green LED |
| `X` | Enter one-attempt Exit Mode |
| `D` | Test the servo door |
| `U` | Take 10 ultrasonic readings, print timeouts and average distance |
| `W` | Run a non-destructive hardware readiness check; the servo is not moved |

Fingerprint reading happens only after `R`, so the idle loop does not flood the Serial Monitor.

Important states continue to print in full on Serial Monitor and appear in shortened English messages on the 20x4 LCD. Each row is safely limited to 20 characters. The display helper caches all four rendered rows and skips identical updates, preventing repeated `lcd.clear()` calls and visible flicker during the fast loop.

## Alert patterns

| Event | Red LED | Active buzzer |
| --- | --- | --- |
| Access denied or unknown RFID | On for about 2 seconds | Three short beeps |
| Enrollment failed | On for about 1.5 seconds | Two short beeps |
| Invalid command | Off | One short beep |
| Setup or general error | On for about 1 second | Two short beeps |
| Access granted | Red LED off | Buzzer off |

Successful fingerprint access and recognized Admin Master Card events turn the green LED on for approximately two seconds. Error and denial handlers explicitly keep it off.

## Security behavior

- Serious access denials increment a failed-attempt counter.
- At three failed attempts, the system enters Lockdown Mode and blocks fingerprint access.
- Area selection and RFID scanning remain available during lockdown.
- Only the configured Admin Master Card can unlock the system.
- Scanning the Admin Master Card also enables Admin Mode and resets failed attempts.
- Fingerprint enrollment is allowed only during Admin Mode.
- Admin Mode expires 60 seconds after the most recent Admin Master Card scan.
- Successful fingerprint access resets the failed-attempt counter.
- Duplicate entry into an area where the user is already inside is denied and counts as a failed attempt.
- An exit attempt from an area where the user is already outside is denied but does not increment failed attempts.

The failed-attempt counter saturates at `3/3` while locked. Further access attempts are logged and alerted without producing confusing values such as `4/3`.

## Door and presence behavior

- The ultrasonic sensor never opens the door directly.
- Entry Mode is the default and requires a selected area plus a person within 20 cm before `R` starts fingerprint capture.
- A missing person or ultrasonic timeout blocks entry without increasing failed attempts.
- Exit Mode is selected with `X` after selecting an area. The next `R` attempt does not require ultrasonic presence.
- Exit Mode returns to Entry Mode after a completed successful or denied fingerprint attempt.
- Successful permission checks open the servo to 90°.
- The door remains open for at least five seconds and then closes to 0° only when the ultrasonic area is clear.
- Denial, enrollment, Admin RFID, and ultrasonic presence alone never open the door.
- Lockdown blocks normal and Exit Mode fingerprint access. Admin RFID unlocks the system without moving the servo.

## Hardware diagnostics and power

- Startup prints a hardware self-test summary for LCD, RFID, AS608/JM-101B, servo PWM, ultrasonic pins, and duplicate GPIO assignments.
- Startup also prints a nonblocking `[POWER CHECK]` reminder before any hardware initialization.
- `F` identifies the sensor as AS608/JM-101B, reports expected 3.3 V power and unused USB D+/D-, initializes UART2 GPIO 16/17 exactly like the working standalone test, calls `finger.begin(57600)`, verifies the password, and prints status, capacity, security level, packet length, and reported baud. It does not use fallback rates; optional fallback rates are startup recovery only.
- `P` performs a simple 15-second fingerprint capture, conversion, and database search without checking permissions or opening the door.
- `D` sends 0°, 90°, and 0° commands with visible delays. A three-wire servo has no feedback signal, so successful PWM attachment does not prove that the motor physically moved.
- `U` takes ten measurements using a 30 ms Echo timeout, reports every timeout, and averages only valid readings.
- `W` prints the exact AS608/JM-101B profile, 3.3 V expectation, UART GPIO16/17, 57600 baud, and unused USB D+/D-; it then repeats fingerprint detection, takes ten ultrasonic readings, checks servo attachment without moving it, checks RFID/LCD status and GPIO conflicts, and prints `READY` or `NOT READY`. It snapshots and restores selected area, Entry/Exit mode, lockdown, Admin Mode, failed attempts, enrollment state, door software state, presence-prompt state, every attendance mask, and the LCD cache.
- Commands `F`, `P`, `D`, `U`, and `W` run through the diagnostic state guard. Their diagnostic LCD screens are temporary; after the command completes, the guard redraws the previous four-line LCD screen and restores its cached text/timestamp.
- GPIO 34 is input-only and is correctly used for HC-SR04 Echo. Echo must pass through a voltage divider because its raw output is normally 5 V.
- The servo must use a stable external 5 V supply. Join the external supply GND to ESP32 GND, but do not blindly join external +5 V to an ESP32 that is already USB-powered.
- The selected JM-101B is specified at about 60 mA from 3.3 V. If fingerprint failures appear when the externally powered servo moves, measure both the ESP32 3.3 V rail and servo 5 V rail and verify the common ground.
- A USB-C PD/QC module must be measured and confirmed at 5 V before connection; an accidental 9 V, 12 V, or 20 V output can damage the project.

Missing hardware does not trap the program in setup: AS608 failure leaves other functions available, the ultrasonic read has a finite timeout, LCD failure preserves Serial diagnostics, and servo diagnostics report only the commanded PWM because a three-wire servo has no position feedback. These protections prevent software hangs, but they cannot make incorrect voltage wiring safe.

## Software validation command

Command `V` runs isolated mini tests without scanning RFID, capturing a fingerprint, reading the ultrasonic sensor, moving the servo, or activating LEDs and the buzzer. It validates:

- all six exact user permission profiles, including all restricted Company, Server Room, and Management/Admin combinations;
- occupancy-bit independence, entry, duplicate-entry rejection, exit, already-outside rejection, and re-entry;
- failure-counter thresholds and saturation, lockdown state, Admin unlock, counter reset, and Admin Mode timeout boundaries;
- LCD 20x4 sizing, 20-character trimming, and duplicate-screen refresh suppression;
- attendance commit only after door state `OPEN`, servo angles, and the five-second/clear-area door-closing rules;
- 20 cm presence calculations and the different Entry/Exit ultrasonic policies;
- the AS608/JM-101B model, 3.3 V/about-60 mA profile, fingerprint UART2/pin/57600 contract, simple servo attach default, servo GPIO13 versus RFID SCK GPIO18, Serial command uniqueness including `W`, area mappings, and GPIO conflicts.

Before testing, the command snapshots attendance masks, selected area, Entry/Exit mode, lockdown, failed attempts, Admin Mode, enrollment state, door state/timer, ultrasonic cache, presence-prompt state, and cached LCD text/timestamp. It restores that state before returning. A failed test prints its name, expected value, and actual value, followed by a complete summary.

Attendance is now committed only after an authorized servo-open command reaches the software `OPEN` state. If PWM attachment or door opening fails, occupancy and the failed-attempt counter remain unchanged.

The normal access flow opens the servo only after granted entry or exit. Command `D` is the intentional diagnostic exception: it moves the servo through 0° → 90° → 0° for testing and remains blocked during Lockdown Mode.

## POWER SAFETY CHECKLIST BEFORE TESTING

- [ ] ESP32 is connected to computer USB for development
- [ ] External 5 V supply is disconnected from ESP32 `5V`/`VIN` while USB is connected, unless the exact board schematic proves safe power isolation
- [ ] RC522 is connected to ESP32 `3V3` only, never 5 V
- [ ] Servo uses a regulated external 5 V supply, never ESP32 `3V3` or the board's USB/VIN rail
- [ ] Servo supply is rated for stall/start current plus the other loads and margin
- [ ] Servo GND is connected directly to external supply GND, and that GND is joined to ESP32 GND
- [ ] The selected AS608 / JM-101B VCC is connected to ESP32 `3V3`, not 5 V
- [ ] Sensor PCB/datasheet labels were followed instead of relying on wire colors
- [ ] JM-101B GND is connected to ESP32 GND; UART TX → GPIO 16 and UART RX → GPIO 17
- [ ] Any JM-101B USB D+ and D- pins are left disconnected from the ESP32
- [ ] HC-SR04 is powered from 5 V
- [ ] HC-SR04 ECHO → 1 kΩ → GPIO 34, with 2 kΩ from GPIO 34 → GND (or an equivalent 3.3 V-safe level shifter)
- [ ] LCD is a 20x4 I2C display and SDA/SCL idle voltages were checked before connection
- [ ] A bidirectional I2C level shifter is installed if the 5 V backpack pulls SDA/SCL above 3.3 V
- [ ] Red GPIO 25 and green GPIO 32 LEDs each have a 220 Ω or 330 Ω series resistor
- [ ] Buzzer voltage/current was checked; an unknown or high-current buzzer uses a transistor/MOSFET driver
- [ ] USB-C PD/QC or external module output was measured with the project disconnected and confirmed as 5 V with correct polarity
- [ ] No 9 V, 12 V, 15 V, or 20 V source is connected to ESP32 or any project sensor
- [ ] Every device and supply has a common signal GND, with no accidental reverse polarity or loose ground
- [ ] No powered peripheral signal drives an ESP32 that is switched off
- [ ] Power is first applied in stages with a current limit: ESP32 alone, RC522, LCD through safe I2C levels, AS608, HC-SR04, buzzer/LEDs, and servo last
- [ ] Measure the 5 V and 3.3 V rails during servo movement and stop immediately for heat, smell, resets, severe jitter, or rail collapse
- [ ] Run `V` first and confirm `Failed: 0` / `Result: OK`
- [ ] Then test `F`, `P`, `U`, `D`, and `S` one at a time; test `D` only after external servo power is verified

## Anti-passback and occupancy

- Each user has a seven-area `insideMask`; entry and exit change only the selected area's bit.
- A permitted entry changes that user's selected-area state from `OUTSIDE` to `INSIDE` only after the software confirms that the authorized door-open command reached the `OPEN` state.
- Entry is denied if that user is already `INSIDE` the selected area. The LCD shows `Already Inside`, and the normal denial alert and failed-attempt rules apply.
- Exit changes the state to `OUTSIDE` only when the user is currently inside the selected area.
- Exit does not require ultrasonic presence. An already-outside exit keeps the servo closed, displays `Not Inside`, and returns to Entry Mode without incrementing the failure counter.
- Command `S` prints every user's status in all seven areas followed by live occupancy counts.
- Occupancy is stored only in RAM and is reset when the ESP32 restarts.

## Configured fingerprint users

| ID | User | Allowed areas |
| --- | --- | --- |
| 1 | Employee A | Main Entrance, Company A |
| 2 | Employee B | Main Entrance, Company B |
| 3 | IT Admin | Main Entrance, Server Room |
| 4 | Manager | All areas |
| 5 | Employee C | Main Entrance, Company C |
| 6 | Employee D | Main Entrance, Company D |

Enroll a physical fingerprint using one of these IDs to test its matching permission profile. IDs from 7 through 127 can be stored in the sensor, but access will be denied until a matching user is added to the local `users` array.

## Configure the Admin Master Card

1. Scan the RFID card.
2. Copy the uppercase UID printed in Serial Monitor.
3. Replace the placeholder in the sketch:

```cpp
const String ADMIN_RFID_UID = "PUT_ADMIN_CARD_UID_HERE";
```

4. Upload the sketch again.

The RFID admin identity is separate from fingerprint users. It enables Admin Mode and is the only identity that can clear Lockdown Mode.

> Configure and test `ADMIN_RFID_UID` before deliberately triggering Lockdown Mode. When the placeholder is still present, scanned UIDs are printed for configuration but no card can unlock the system.

## Final staged hardware test order

1. Upload code with only ESP32 connected.
2. Open Serial Monitor at 115200.
3. Run `V` and confirm all `PASS`.
4. Connect only the AS608 / JM-101B: VCC to 3.3 V, common GND, UART TX/RX crossed correctly, and USB D+/D- disconnected.
5. Run `F`.
6. If `F` succeeds, run `P`.
7. Connect only servo signal/power safely.
8. Run `D`.
9. Connect ultrasonic with Echo voltage divider.
10. Run `U`.
11. Connect RFID and test Admin UID.
12. Run `S` or `W` for readiness summary.
13. Only then test full entry/exit flow.

## Suggested tests

1. Send `V`; expect every mini test to report `PASS`, followed by `Result: OK` and confirmation that runtime state was restored.
2. Send `F`; expect the UART2 initialization at 57600 to report `SUCCESS: AS608 / JM-101B detected!`, followed by the sensor parameters.
3. Send `P`; place a finger within 15 seconds and verify capture, conversion, ID, and confidence output without access or door actions.
4. Send `U`; verify ten readings, the valid-reading count, average distance, and `Person Near` result.
5. Send `D`; verify the servo sequence 0° → 90° → 0°. If commands print but the motor does not move, check 5 V power and common GND. The command is blocked during lockdown.
6. Select area `2` and send `R` while nobody is near; expect `ENTRY BLOCKED`, no fingerprint capture, no servo movement, and no failed attempt.
7. Stand within 20 cm, send `R`, and use fingerprint ID 1; expect granted entry and the door to open.
8. Remain near after five seconds; verify the door stays open. Move clear and verify it closes.
9. Select area `3`, stand near, send `R`, and use fingerprint ID 1; expect denial, a failed attempt, and no servo movement.
10. After test 7 closes the door, repeat area `2` entry with ID 1; expect `Already Inside`, denial feedback, one failed attempt, and no servo movement.
11. Select area `2`, send `X`, then `R` and fingerprint ID 1 without ultrasonic presence; expect granted exit, `OUTSIDE`, and the door to open.
12. Repeat that exit after the door closes; expect `Exit Denied / Not Inside`, no servo movement, and no failed-attempt increase.
13. Select area `6`, stand near, and use fingerprint ID 5; expect Employee C to enter Company C. Select area `7` with the same user for entry and expect a permission denial.
14. Send `S`; verify all six users and occupancy counts for Main Entrance, Companies A-D, Server Room, and Management/Admin.
15. Trigger lockdown and confirm both Entry and Exit Mode are blocked. Scan Admin RFID and confirm unlock feedback without servo movement.

## Future work — FastAPI and SQLite

FastAPI and SQLite integration are future work and are not part of the current standalone ESP32 verification.

The current ESP32 sketch intentionally contains no Wi-Fi connection, `HTTPClient`, HTTP requests, backend log upload, database synchronization, mobile-app communication, or cloud logic. Any backend and application folders in the wider workspace are separate software components; the ESP32-to-backend communication path has not yet been implemented or tested.

The future integration phase may add ESP32 Wi-Fi/HTTP communication and synchronization with the existing API/database. Until that phase is implemented and tested, local hardcoded permissions, attendance, anti-passback, Admin Mode, and Lockdown remain authoritative inside the standalone ESP32 controller.
