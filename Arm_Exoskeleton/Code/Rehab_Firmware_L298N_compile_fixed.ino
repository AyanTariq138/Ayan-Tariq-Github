/* ============================================================================
   UPPER-LIMB REHABILITATION DEVICE — Integrated Control Firmware
   MECH5845M | Arduino Mega 2560 + L298N Dual H-Bridge Driver
   ----------------------------------------------------------------------------
   Encoder pins are chosen to avoid the shield's default Arduino pins:
     Elbow encoder A/B -> Mega pins 18/19
     Wrist encoder A/B -> Mega pins 20/21

   IMPORTANT MOTOR IDENTIFICATION
   ------------------------------
   The firmware is configured for a 30:1 elbow gearbox:
     ELBOW_GEAR_RATIO = 30.0
   Pololu's official catalogue identifies the 30:1 encoder model as item #4752
   and item #4753 as 50:1. Verify the label/gear ratio on the motor received.
   If the physical motor is 50:1, change ELBOW_GEAR_RATIO to 50.0 before use
   and update the report calculations.

   Encoder CPR values below already represent full quadrature counting
   (both edges of both channels), so they are NOT multiplied by four again.

   MODES (Serial Monitor, 115200 baud, Newline):
     1  Encoder test — no motor drive
     2  Motor test — open-loop fixed PWM
     3  PID control — closed-loop position control
     4  Current read — open-loop with current logging
     5  Safety test — PID with ROM clamp/current cutoff

   COMMANDS:
     E          select elbow in modes 2/4
     W          select wrist in modes 2/4
     E<angle>   command elbow angle in modes 3/5, e.g. E90
     W<angle>   command wrist angle in modes 3/5, e.g. W20
     ZEROE      set elbow reference to 30 degrees
     ZEROW      set wrist reference to -15 degrees
     PWM<n>     set open-loop PWM in modes 2/4, e.g. PWM40 or PWM-40
     STOP       latched emergency stop
     RESET      clear fault; motion remains disabled until a new command
     MENU or ?  print menu/help

   SAFETY:
     - No motor motion at startup.
     - A joint must be manually referenced before PID movement.
     - Only one joint is energised at a time.
     - ROM limits, speed-limited setpoints and over-current cutoff are included.
     - Use a current-limited bench supply for commissioning.
     - Bench testing only; do not attach to a person.
   ============================================================================ */

#include <Arduino.h>

// =============================================================================
//  L298N MOTOR DRIVER CONFIGURATION
// -----------------------------------------------------------------------------
//  This firmware drives an L298N dual H-bridge (NOT the Pololu VNH5019 shield).
//
//  IMPORTANT HARDWARE NOTES / LIMITATIONS OF THE L298N:
//    * The L298N has NO current sensing. The current_mA column in the CSV is
//      therefore always 0, and the over-current / stall protection layer is
//      DISABLED (see checkOverCurrent). This is a hardware limitation, not a
//      code bug. For real stall protection use a driver with current feedback
//      (e.g. the specified VNH5019).
//    * The L298N has NO fault output pin, so driver-fault detection is disabled.
//    * The L298N drops ~2-4 V internally, so 12 V motors on an ~11 V battery
//      will see roughly 7-9 V and run slower / warmer than on the VNH5019.
//
//  WIRING (match your jumper wires to these pins, or edit the numbers to match
//  what you have already wired):
//
//    ELBOW motor  -> L298N OUT1/OUT2   (Motor A)
//        ENA  -> Mega pin 11   (PWM speed, must be a PWM pin)
//        IN1  -> Mega pin 22   (direction)
//        IN2  -> Mega pin 23   (direction)
//
//    WRIST motor  -> L298N OUT3/OUT4   (Motor B)
//        ENB  -> Mega pin 12   (PWM speed, must be a PWM pin)
//        IN3  -> Mega pin 24   (direction)
//        IN4  -> Mega pin 25   (direction)
//
//    L298N GND  -> Arduino GND  -> battery negative  (COMMON GROUND REQUIRED)
//    L298N +12V -> battery positive       Remove the 5V-enable jumper if you
//    are supplying more than ~12 V, and do NOT power the Mega from the L298N
//    onboard 5 V regulator while also powering it over USB.
//
//    NOTE: the encoders already use interrupt pins 18/19/20/21, so the L298N
//    pins above deliberately avoid them. ENA/ENB must stay on PWM-capable pins
//    (Mega PWM pins: 2-13 and 44-46).
// =============================================================================

const uint8_t ELBOW_ENA = 6;   // PWM
const uint8_t ELBOW_IN1 = 22;
const uint8_t ELBOW_IN2 = 23;

const uint8_t WRIST_ENB = 5;   // PWM
const uint8_t WRIST_IN3 = 24;
const uint8_t WRIST_IN4 = 25;

// The rest of the firmware thinks in a -400..+400 speed scale (inherited from
// the VNH5019 code). analogWrite on the L298N ENA/ENB pin needs 0..255, so we
// scale. Speeds below MIN_PWM8 rarely overcome friction, but we do not force a
// minimum here so that the PID can command small corrections.
const int L298_MAX_SPEED = 400;   // matches the controller output clamp

// If a motor spins the WRONG physical way, flip the corresponding invert flag
// instead of re-wiring. +speed should move the joint toward INCREASING angle.
const bool ELBOW_INVERT = false;
const bool WRIST_INVERT = false;

inline void l298Write(uint8_t enPin, uint8_t inA, uint8_t inB,
                      int speed, bool invert) {
  if (invert) speed = -speed;
  speed = constrain(speed, -L298_MAX_SPEED, L298_MAX_SPEED);

  // Map |speed| from 0..400 to 0..255 for analogWrite.
  int duty = (int)((long)abs(speed) * 255 / L298_MAX_SPEED);
  duty = constrain(duty, 0, 255);

  if (speed > 0) {
    digitalWrite(inA, HIGH);
    digitalWrite(inB, LOW);
  } else if (speed < 0) {
    digitalWrite(inA, LOW);
    digitalWrite(inB, HIGH);
  } else {
    // Active brake (both low = coast; both high = brake). Use coast/low.
    digitalWrite(inA, LOW);
    digitalWrite(inB, LOW);
    duty = 0;
  }
  analogWrite(enPin, duty);
}

inline void elbowDrive(int speed) {
  l298Write(ELBOW_ENA, ELBOW_IN1, ELBOW_IN2, speed, ELBOW_INVERT);
}
inline void wristDrive(int speed) {
  l298Write(WRIST_ENB, WRIST_IN3, WRIST_IN4, speed, WRIST_INVERT);
}

// ---------------------- Optional battery-voltage sensing ---------------------
// Fit a divider to A2 only if you want battery sensing. Unlike the VNH5019
// version, A0/A1 are free on the L298N, but A2 is kept for consistency.
#define USE_VBAT_SENSE 0
const uint8_t VBAT_PIN = A2;
const float VBAT_DIVIDER = 11.0f;
const float ADC_REF = 5.0f;

// --------------------------- Encoder configuration ---------------------------
const uint8_t ELBOW_ENC_A = 18;
const uint8_t ELBOW_ENC_B = 19;
const uint8_t WRIST_ENC_A = 20;
const uint8_t WRIST_ENC_B = 21;

// Verify the physical elbow motor before testing.
const float ELBOW_GEAR_RATIO = 30.0f;
const float WRIST_GEAR_RATIO = 46.85f;

// Pololu CPR values already assume full x4 quadrature decoding.
const float ELBOW_MOTOR_CPR = 64.0f;
const float WRIST_MOTOR_CPR = 48.0f;

const float ELBOW_DEG_PER_COUNT =
    360.0f / (ELBOW_MOTOR_CPR * ELBOW_GEAR_RATIO);
const float WRIST_DEG_PER_COUNT =
    360.0f / (WRIST_MOTOR_CPR * WRIST_GEAR_RATIO);

// ------------------------------- ROM limits ---------------------------------
const float ELBOW_MIN = 30.0f;
const float ELBOW_MAX = 120.0f;
const float WRIST_MIN = -15.0f;  // radial
const float WRIST_MAX = 35.0f;   // ulnar

// Elbow must be positioned manually at 30 degrees before ZEROE.
// Wrist can be positioned gently at the radial reference before ZEROW.
const float ELBOW_ZERO_ANGLE = 30.0f;
const float WRIST_ZERO_ANGLE = -15.0f;

const float ELBOW_MAX_SPEED = 20.0f; // deg/s
const float WRIST_MAX_SPEED = 10.0f; // deg/s

// -------------------------- Current protection -------------------------------
// NOTE: The L298N has no current sensing, so these thresholds are INACTIVE and
// the over-current protection never triggers. They are retained only so the
// rest of the code compiles unchanged. Do not rely on stall protection with an
// L298N; keep the current-limited bench supply as your protection instead.
unsigned int ELBOW_STALL_MA = 4000;
unsigned int WRIST_STALL_MA = 1400;
const unsigned long STALL_TIME_MS = 400;

// ------------------------------ Loop timing ---------------------------------
const float DT = 0.010f;
const unsigned long DT_MS = 10;

// -------------------------- Quadrature decoding ------------------------------
volatile long elbowCount = 0;
volatile long wristCount = 0;
volatile uint8_t elbowPrevState = 0;
volatile uint8_t wristPrevState = 0;

// Transition-table decoder. Invalid transitions return zero.
// Index = previous AB state (upper two bits) + current AB state (lower two bits).
const int8_t QUAD_TABLE[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};

inline uint8_t readEncoderState(uint8_t pinA, uint8_t pinB) {
  return (digitalRead(pinA) << 1) | digitalRead(pinB);
}

void elbowISR() {
  const uint8_t current = readEncoderState(ELBOW_ENC_A, ELBOW_ENC_B);
  const uint8_t index = (elbowPrevState << 2) | current;
  elbowCount += QUAD_TABLE[index];
  elbowPrevState = current;
}

void wristISR() {
  const uint8_t current = readEncoderState(WRIST_ENC_A, WRIST_ENC_B);
  const uint8_t index = (wristPrevState << 2) | current;
  wristCount += QUAD_TABLE[index];
  wristPrevState = current;
}

// ------------------------------ Controller ----------------------------------
struct JointController {
  float kp = 0.0f;
  float ki = 0.0f;
  float kd = 0.0f;
  float integral = 0.0f;
  float prevError = 0.0f;

  float targetAngle = 0.0f;
  float rampedSetpoint = 0.0f;
  float maxSpeed = 0.0f;
  float minLim = 0.0f;
  float maxLim = 0.0f;
  float degPerCount = 0.0f;
  volatile long *countPtr = nullptr;

  float measuredAngle() const {
    noInterrupts();
    const long count = *countPtr;
    interrupts();
    return count * degPerCount;
  }

  void setMeasuredAngle(float angle) {
    const long newCount = lround(angle / degPerCount);
    noInterrupts();
    *countPtr = newCount;
    interrupts();
    holdHere();
  }

  void holdHere() {
    const float angle = measuredAngle();
    targetAngle = constrain(angle, minLim, maxLim);
    rampedSetpoint = targetAngle;
    integral = 0.0f;
    prevError = 0.0f;
  }

  void setTarget(float angle) {
    targetAngle = constrain(angle, minLim, maxLim);
  }

  int update() {
    const float maxStep = maxSpeed * DT;

    if (rampedSetpoint < targetAngle) {
      rampedSetpoint = min(rampedSetpoint + maxStep, targetAngle);
    } else if (rampedSetpoint > targetAngle) {
      rampedSetpoint = max(rampedSetpoint - maxStep, targetAngle);
    }

    rampedSetpoint = constrain(rampedSetpoint, minLim, maxLim);

    const float angle = measuredAngle();
    const float error = rampedSetpoint - angle;

    integral += error * DT;
    integral = constrain(integral, -50.0f, 50.0f);

    const float derivative = (error - prevError) / DT;
    prevError = error;

    const float output = kp * error + ki * integral + kd * derivative;
    return static_cast<int>(constrain(output, -400.0f, 400.0f));
  }
};

JointController elbow;
JointController wrist;

// ------------------------------ System state --------------------------------
enum Mode {
  MODE_MENU = 0,
  MODE_ENCODER = 1,
  MODE_MOTOR = 2,
  MODE_PID = 3,
  MODE_CURRENT = 4,
  MODE_SAFETY = 5
};

enum ActiveJoint {
  J_NONE,
  J_ELBOW,
  J_WRIST
};

Mode mode = MODE_MENU;
ActiveJoint active = J_NONE;

bool elbowHomed = false;
bool wristHomed = false;
bool enabled = false;
bool faulted = false;
bool driverFaultReported = false;

int openLoopPWM = 0;
unsigned long overCurrentSince = 0;
unsigned long lastLoop = 0;

// Explicit function prototypes. Arduino IDE sometimes generates prototypes
// before custom struct/enum declarations, causing false "does not name a type"
// errors. Keeping these here makes compilation order unambiguous.
JointController *activeCtrl();
bool activeHomed();
unsigned int activeStallThreshold();
unsigned int activeCurrentMA();
void driveActive(int pwm);
const __FlashStringHelper *jointName(ActiveJoint joint);
void doZero(ActiveJoint joint);
void doMove(ActiveJoint joint, float angle);
void setMode(Mode newMode);
void handleCommand(String command);

// -------------------------------- Helpers -----------------------------------
void stopAll() {
  elbowDrive(0);
  wristDrive(0);
}

JointController *activeCtrl() {
  if (active == J_ELBOW) return &elbow;
  if (active == J_WRIST) return &wrist;
  return nullptr;
}

bool activeHomed() {
  if (active == J_ELBOW) return elbowHomed;
  if (active == J_WRIST) return wristHomed;
  return false;
}

unsigned int activeStallThreshold() {
  if (active == J_ELBOW) return ELBOW_STALL_MA;
  if (active == J_WRIST) return WRIST_STALL_MA;
  return 0;
}

unsigned int activeCurrentMA() {
  // The L298N provides no current measurement. Always 0.
  return 0;
}

void driveActive(int pwm) {
  pwm = constrain(pwm, -L298_MAX_SPEED, L298_MAX_SPEED);

  if (active == J_ELBOW) {
    wristDrive(0);
    elbowDrive(pwm);
  } else if (active == J_WRIST) {
    elbowDrive(0);
    wristDrive(pwm);
  } else {
    stopAll();
  }
}

#if USE_VBAT_SENSE
float readBatteryVolts() {
  return (analogRead(VBAT_PIN) * ADC_REF / 1023.0f) * VBAT_DIVIDER;
}
#endif

const __FlashStringHelper *jointName(ActiveJoint joint) {
  if (joint == J_ELBOW) return F("ELBOW");
  if (joint == J_WRIST) return F("WRIST");
  return F("NONE");
}

void printBanner() {
  Serial.println(F("=============================================="));
  Serial.println(F(" Upper-Limb Rehabilitation Device Firmware"));
  Serial.println(F(" Arduino Mega 2560 | L298N dual H-bridge"));
  Serial.println(F(" Bench commissioning only — no human testing"));
  Serial.println(F("----------------------------------------------"));
  Serial.print(F(" Elbow ratio configured: "));
  Serial.println(ELBOW_GEAR_RATIO, 2);
  Serial.print(F(" Wrist ratio configured: "));
  Serial.println(WRIST_GEAR_RATIO, 2);
  Serial.println(F(" Current sensing: NOT AVAILABLE (L298N)"));
  Serial.println(F(" Over-current protection: DISABLED (L298N)"));
#if USE_VBAT_SENSE
  Serial.print(F(" Battery: "));
  Serial.print(readBatteryVolts(), 2);
  Serial.println(F(" V"));
#endif
  Serial.println(F("=============================================="));
}

void printMenu() {
  Serial.println(F("\n--- MAIN MENU ---"));
  Serial.println(F(" 1  Encoder test"));
  Serial.println(F(" 2  Motor test (open loop)"));
  Serial.println(F(" 3  PID position control"));
  Serial.println(F(" 4  Current read"));
  Serial.println(F(" 5  Safety test"));
  Serial.println(F(""));
  Serial.println(F("Open-loop: E or W, then PWM<n>"));
  Serial.println(F("PID: ZEROE/ZEROW, then E<angle>/W<angle>"));
  Serial.println(F("STOP | RESET | MENU | ?"));
}

void printCsvHeader() {
  Serial.println(F(
      "t_ms,mode,joint,target_deg,setpoint_deg,"
      "elbow_angle_deg,wrist_angle_deg,"
      "elbow_count,wrist_count,pwm,current_mA,state"));
}

void doStop() {
  enabled = false;
  faulted = true;
  openLoopPWM = 0;
  stopAll();
  Serial.println(F("# EMERGENCY STOP LATCHED"));
}

void doReset() {
  stopAll();
  faulted = false;
  enabled = false;
  openLoopPWM = 0;
  overCurrentSince = 0;
  driverFaultReported = false;

  if (activeCtrl() != nullptr && activeHomed()) {
    activeCtrl()->holdHere();
  }

  Serial.println(F("# RESET: fault cleared; motion disabled"));
}

void doZero(ActiveJoint joint) {
  stopAll();
  enabled = false;
  openLoopPWM = 0;
  active = joint;

  if (joint == J_ELBOW) {
    elbow.setMeasuredAngle(ELBOW_ZERO_ANGLE);
    elbowHomed = true;
    Serial.print(F("# ELBOW referenced at "));
    Serial.print(ELBOW_ZERO_ANGLE, 2);
    Serial.println(F(" deg"));
  } else if (joint == J_WRIST) {
    wrist.setMeasuredAngle(WRIST_ZERO_ANGLE);
    wristHomed = true;
    Serial.print(F("# WRIST referenced at "));
    Serial.print(WRIST_ZERO_ANGLE, 2);
    Serial.println(F(" deg"));
  }

  faulted = false;
  overCurrentSince = 0;
  Serial.println(F("# Motion remains disabled until a new command"));
}

void doMove(ActiveJoint joint, float angle) {
  active = joint;

  if (!activeHomed()) {
    Serial.println(F("# REJECTED: reference this joint first"));
    return;
  }

  if (mode != MODE_PID && mode != MODE_SAFETY) {
    Serial.println(F("# REJECTED: use mode 3 or 5 for angle commands"));
    return;
  }

  activeCtrl()->setTarget(angle);
  faulted = false;
  enabled = true;
  overCurrentSince = 0;

  Serial.print(F("# TARGET "));
  Serial.print(jointName(joint));
  Serial.print(F(" "));
  Serial.print(activeCtrl()->targetAngle, 2);
  Serial.println(F(" deg"));
}

void checkOverCurrent(unsigned int currentMA) {
  // The L298N cannot measure current, so there is nothing to check. This is
  // intentionally a no-op. The parameter is kept so call sites are unchanged.
  (void)currentMA;
  (void)ELBOW_STALL_MA;
  (void)WRIST_STALL_MA;
  (void)STALL_TIME_MS;
  overCurrentSince = 0;
}

void setMode(Mode newMode) {
  mode = newMode;
  enabled = false;
  faulted = false;
  active = J_NONE;
  openLoopPWM = 0;
  overCurrentSince = 0;
  driverFaultReported = false;
  stopAll();

  Serial.print(F("# MODE "));
  Serial.println(static_cast<int>(mode));
  printCsvHeader();
}

void handleCommand(String command) {
  command.trim();
  if (command.length() == 0) return;

  String upper = command;
  upper.toUpperCase();

  if (command.length() == 1 &&
      command[0] >= '1' && command[0] <= '5') {
    setMode(static_cast<Mode>(command[0] - '0'));
    return;
  }

  if (upper == "MENU" || upper == "?") {
    printMenu();
    return;
  }

  if (upper == "STOP") {
    doStop();
    return;
  }

  if (upper == "RESET") {
    doReset();
    return;
  }

  if (upper == "ZEROE") {
    doZero(J_ELBOW);
    return;
  }

  if (upper == "ZEROW") {
    doZero(J_WRIST);
    return;
  }

  if (upper.startsWith("PWM")) {
    if (mode != MODE_MOTOR && mode != MODE_CURRENT) {
      Serial.println(F("# PWM is allowed only in modes 2 and 4"));
      return;
    }

    if (active == J_NONE) {
      Serial.println(F("# Select a joint first using E or W"));
      return;
    }

    const int requestedPWM =
        constrain(upper.substring(3).toInt(), -300, 300);

    openLoopPWM = requestedPWM;
    faulted = false;
    enabled = true;
    overCurrentSince = 0;

    Serial.print(F("# PWM "));
    Serial.println(openLoopPWM);
    return;
  }

  if (upper[0] == 'E' || upper[0] == 'W') {
    const ActiveJoint joint =
        (upper[0] == 'E') ? J_ELBOW : J_WRIST;

    String remainder = command.substring(1);
    remainder.trim();

    if (mode == MODE_MOTOR || mode == MODE_CURRENT) {
      active = joint;
      enabled = false;
      openLoopPWM = 0;
      stopAll();

      Serial.print(F("# Joint selected: "));
      Serial.println(jointName(joint));
      Serial.println(F("# Send PWM<n> to start"));
      return;
    }

    if (remainder.length() == 0) {
      Serial.println(F("# Provide an angle, e.g. E90 or W20"));
      return;
    }

    doMove(joint, remainder.toFloat());
    return;
  }

  Serial.println(F("# UNKNOWN COMMAND"));
}

// -------------------------------- Setup -------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.setTimeout(20);

  // L298N control pins
  pinMode(ELBOW_ENA, OUTPUT);
  pinMode(ELBOW_IN1, OUTPUT);
  pinMode(ELBOW_IN2, OUTPUT);
  pinMode(WRIST_ENB, OUTPUT);
  pinMode(WRIST_IN3, OUTPUT);
  pinMode(WRIST_IN4, OUTPUT);
  stopAll();

  pinMode(ELBOW_ENC_A, INPUT);
  pinMode(ELBOW_ENC_B, INPUT);
  pinMode(WRIST_ENC_A, INPUT);
  pinMode(WRIST_ENC_B, INPUT);

  elbowPrevState = readEncoderState(ELBOW_ENC_A, ELBOW_ENC_B);
  wristPrevState = readEncoderState(WRIST_ENC_A, WRIST_ENC_B);

  attachInterrupt(
      digitalPinToInterrupt(ELBOW_ENC_A), elbowISR, CHANGE);
  attachInterrupt(
      digitalPinToInterrupt(ELBOW_ENC_B), elbowISR, CHANGE);
  attachInterrupt(
      digitalPinToInterrupt(WRIST_ENC_A), wristISR, CHANGE);
  attachInterrupt(
      digitalPinToInterrupt(WRIST_ENC_B), wristISR, CHANGE);

  // Initial values only; tune cautiously on the real mechanism.
  elbow.kp = 8.0f;
  elbow.ki = 0.0f;
  elbow.kd = 0.8f;
  elbow.maxSpeed = ELBOW_MAX_SPEED;
  elbow.minLim = ELBOW_MIN;
  elbow.maxLim = ELBOW_MAX;
  elbow.degPerCount = ELBOW_DEG_PER_COUNT;
  elbow.countPtr = &elbowCount;

  wrist.kp = 6.0f;
  wrist.ki = 0.0f;
  wrist.kd = 0.6f;
  wrist.maxSpeed = WRIST_MAX_SPEED;
  wrist.minLim = WRIST_MIN;
  wrist.maxLim = WRIST_MAX;
  wrist.degPerCount = WRIST_DEG_PER_COUNT;
  wrist.countPtr = &wristCount;

  printBanner();
  printMenu();
}

// -------------------------------- Loop --------------------------------------
void loop() {
  if (Serial.available()) {
    handleCommand(Serial.readStringUntil('\n'));
  }

  const unsigned long now = millis();
  if (now - lastLoop < DT_MS) return;
  lastLoop = now;

  int pwm = 0;
  unsigned int currentMA = 0;
  const char *state = "IDLE";

  switch (mode) {
    case MODE_ENCODER:
      stopAll();
      state = "ENCODER";
      break;

    case MODE_MOTOR:
    case MODE_CURRENT:
      if (enabled && !faulted && active != J_NONE) {
        pwm = openLoopPWM;
        driveActive(pwm);
        currentMA = activeCurrentMA();
        checkOverCurrent(currentMA);
        state = faulted ? "FAULT" : "OPEN_LOOP";
      } else {
        stopAll();
        state = faulted ? "FAULT" : "IDLE";
      }
      break;

    case MODE_PID:
    case MODE_SAFETY:
      if (enabled && !faulted &&
          active != J_NONE && activeHomed()) {
        pwm = activeCtrl()->update();
        driveActive(pwm);
        currentMA = activeCurrentMA();
        checkOverCurrent(currentMA);
        state = faulted ? "FAULT" : "RUNNING";
      } else {
        stopAll();
        if (faulted) {
          state = "FAULT";
        } else if (active != J_NONE && !activeHomed()) {
          state = "NOT_HOMED";
        } else {
          state = "IDLE";
        }
      }
      break;

    default:
      stopAll();
      state = "MENU";
      break;
  }

  // The L298N has no fault output, so there is no driver fault to poll here.
  (void)driverFaultReported;

  if (mode != MODE_MENU) {
    const float target =
        (activeCtrl() != nullptr) ? activeCtrl()->targetAngle : 0.0f;
    const float setpoint =
        (activeCtrl() != nullptr) ? activeCtrl()->rampedSetpoint : 0.0f;
    // Print both joint angles every row so the encoder test (Mode 1, no active
    // joint) still shows live angles, not zeros.
    const float elbowAngle = elbow.measuredAngle();
    const float wristAngle = wrist.measuredAngle();

    noInterrupts();
    const long elbowCountCopy = elbowCount;
    const long wristCountCopy = wristCount;
    interrupts();

    Serial.print(now);
    Serial.print(',');
    Serial.print(static_cast<int>(mode));
    Serial.print(',');
    Serial.print(active == J_ELBOW ? F("E") :
                 active == J_WRIST ? F("W") : F("-"));
    Serial.print(',');
    Serial.print(target, 2);
    Serial.print(',');
    Serial.print(setpoint, 2);
    Serial.print(',');
    Serial.print(elbowAngle, 3);
    Serial.print(',');
    Serial.print(wristAngle, 3);
    Serial.print(',');
    Serial.print(elbowCountCopy);
    Serial.print(',');
    Serial.print(wristCountCopy);
    Serial.print(',');
    Serial.print(pwm);
    Serial.print(',');
    Serial.print(currentMA);
    Serial.print(',');
    Serial.println(state);
  }
}
