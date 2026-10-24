
#include <math.h>

// ===== STEPPER MOTOR =====
const int STEP_PIN = 3;
const int DIR_PIN = 6;
const int EN_PIN = 8;

// ===== HC-SR04 SENSOR =====
const int TRIG_PIN = 9;
const int ECHO_PIN = 10;

// ===== ADJUSTABLE SETTINGS =====
const int SWING_STEPS = 200;        // Total steps between ends
const float WIGGLE_SPEED = 0.7;     // Complete cycles per second
const int WIGGLE_COUNT = 2;         // Number of full wiggles

const int DETECT_DISTANCE = 25;     // Trigger within 25 cm
const int MIN_DISTANCE = 3;         // Ignore very close readings

const unsigned long SENSOR_INTERVAL = 60;
const unsigned long COOLDOWN = 500;

// ===== STATE =====
bool isWiggling = false;
bool sensorArmed = true;

unsigned long wiggleStart = 0;
unsigned long lastSensorCheck = 0;
unsigned long wiggleFinished = 0;

long currentStep = 0;

// ===== SETUP =====
void setup() {
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(EN_PIN, OUTPUT);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  digitalWrite(STEP_PIN, LOW);
  digitalWrite(TRIG_PIN, LOW);
  digitalWrite(EN_PIN, LOW);

  Serial.begin(9600);
  Serial.println("Tail ready!");
}

// ===== DISTANCE READING =====
float readDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration =
      pulseIn(ECHO_PIN, HIGH, 25000UL);

  if (duration == 0) return -1;

  return duration * 0.0343 / 2.0;
}

// ===== MOVE MOTOR =====
void moveToStep(long targetStep) {
  if (targetStep == currentStep) return;

  bool direction = targetStep > currentStep;

  digitalWrite(DIR_PIN, direction ? HIGH : LOW);
  delayMicroseconds(5);

  digitalWrite(STEP_PIN, HIGH);
  delayMicroseconds(5);
  digitalWrite(STEP_PIN, LOW);

  currentStep += direction ? 1 : -1;
}

// ===== START WIGGLE =====
void startWiggle() {
  isWiggling = true;
  sensorArmed = false;
  wiggleStart = millis();

  Serial.println("Wiggling!");
}

// ===== MAIN LOOP =====
void loop() {
  unsigned long now = millis();

  // ===== WIGGLING MODE =====
  if (isWiggling) {

    float elapsed = (now - wiggleStart) / 1000.0;

    float totalDuration =
        WIGGLE_COUNT / WIGGLE_SPEED;

    if (elapsed >= totalDuration) {

      // Return to center before finishing
      if (currentStep != 0) {
        moveToStep(0);
      } else {
        isWiggling = false;
        wiggleFinished = millis();

        Serial.println("Wiggle finished!");
      }

      return;
    }

    // Smooth sine-wave movement
    float angle =
        2.0 * PI * WIGGLE_SPEED * elapsed;

    long targetStep = round(
        (SWING_STEPS / 2.0) * sin(angle)
    );

    moveToStep(targetStep);

    // Sensor is completely ignored here
    return;
  }

  // ===== COOLDOWN =====
  if (now - wiggleFinished < COOLDOWN &&
      !sensorArmed) {
    return;
  }

  // ===== SENSOR CHECK =====
  if (now - lastSensorCheck >= SENSOR_INTERVAL) {
    lastSensorCheck = now;

    float distance = readDistance();

    if (distance < 0) return;

    // Rearm only after hand moves away
    if (distance > DETECT_DISTANCE) {
      sensorArmed = true;
    }

    // Trigger when hand comes into range
    if (sensorArmed &&
        distance >= MIN_DISTANCE &&
        distance <= DETECT_DISTANCE) {

      startWiggle();
    }
  }
}
