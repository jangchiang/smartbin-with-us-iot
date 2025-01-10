/***********************************************************
  ESP32 + 4 Ultrasonic Sensors (HC-SR04)
  Blynk IoT (2.0) Version, with fallback logic
  + Reduced Data Sending

  - 2 sensors for fill (main + backup)
  - 2 sensors for installed check (main + backup)
  - If fill% > 70 => Should Clean = YES

  Sends data to Blynk Virtual Pins:
    V0 = fill% 
    V1 = installed? (1 or 0)
    V2 = shouldClean? (1 or 0)
    V3 = final fill distance (cm)
    V4 = main installed sensor distance (cm)

  1) We measure & (potentially) send data once every minute 
     using BlynkTimer (no blocking delay in loop()).
  2) If the new readings are exactly the same as last time, 
     we skip sending to reduce data usage -- unless we skip 
     10 times in a row, then we force a send.

  Make sure:
  - Voltage dividers on each Echo line if powered at 5V.
  - Adjust wiring & thresholds as needed.

***********************************************************/

// ------------------- BLYNK TEMPLATE MACROS -------------------
#define BLYNK_TEMPLATE_ID   "TMPL6oqkQfN1t"
#define BLYNK_TEMPLATE_NAME "smart bin"
#define BLYNK_AUTH_TOKEN    "wJ0LM7wvQDSUZVbG-3YNV9LGwAhSNwVA"

// ------------------- BLYNK / WIFI LIBRARIES ------------------
#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>

// ------------------- WIFI CREDENTIALS ------------------------
char ssid[] = "Aisuru_2.4G";
char pass[] = "ss0877258141";

// ------------------- PIN ASSIGNMENTS -------------------------
// Fill sensors
#define TRIG_FILL_MAIN   4
#define ECHO_FILL_MAIN   17
#define TRIG_FILL_BACKUP 16
#define ECHO_FILL_BACKUP 18

// Installed sensors
#define TRIG_INST_MAIN   19
#define ECHO_INST_MAIN   21
#define TRIG_INST_BACKUP 23
#define ECHO_INST_BACKUP 22

// ------------------- PROJECT PARAMETERS -----------------------
const int BIN_HEIGHT_CM       = 26;   // from sensor to bin bottom
const int INSTALLED_THRESHOLD = 10;   // <15 cm => bin is installed
const int INVALID_LOW         = 1;    // <=1 => invalid
const int INVALID_HIGH        = 300;  // >300 => invalid
const int CLEAN_THRESHOLD     = 70;   // fill% above => "Should Clean?" = YES

// ------------------- BLYNK VIRTUAL PINS -----------------------
#define VPIN_FILL_LEVEL   V0   // fill% 
#define VPIN_INSTALLED    V1   // installed? (1/0)
#define VPIN_SHOULD_CLEAN V2   // shouldClean? (1/0 if fill% > 70)
#define VPIN_FILL_DISTANCE V3  // final fill distance (cm)
#define VPIN_MAIN_BIN_DIST V4  // main installed sensor distance (cm)

// ------------------- GLOBALS FOR STORING DATA ----------------
// We'll keep track of the last values we sent to Blynk.
long lastFillDistance = -999;
long lastMainInstalledDist = -999;
int  lastFillPercent = -999;
bool lastShouldClean = false;
bool lastBinInstalled = false;

// For skip logic: how many times in a row we've had identical readings
int skipCount = 0;

// BlynkTimer for scheduling
BlynkTimer timer;

// -------------------------------------------------------------
//              FORWARD DECLARATIONS
// -------------------------------------------------------------
long  getFillDistance();
bool  isBinInstalled();
int   computeFillPercent(long distance, int binHeight);

// -------------------------------------------------------------
//                    SETUP
// -------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  Serial.println("Connecting to Blynk...");
  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);

  // Pin modes for ultrasonic sensors
  pinMode(TRIG_FILL_MAIN,    OUTPUT);
  pinMode(ECHO_FILL_MAIN,    INPUT);
  pinMode(TRIG_FILL_BACKUP,  OUTPUT);
  pinMode(ECHO_FILL_BACKUP,  INPUT);

  pinMode(TRIG_INST_MAIN,    OUTPUT);
  pinMode(ECHO_INST_MAIN,    INPUT);
  pinMode(TRIG_INST_BACKUP,  OUTPUT);
  pinMode(ECHO_INST_BACKUP,  INPUT);

  // Set up a timer to measure & possibly send data every 10 seconds
  timer.setInterval(10000L, measureAndSend);

  Serial.println("Setup complete (Blynk).");
}

// -------------------------------------------------------------
//                    LOOP
// -------------------------------------------------------------
void loop() {
  Blynk.run();
  timer.run();  // let the timer handle scheduled tasks
}

// -------------------------------------------------------------
//                MEASURE & SEND FUNCTION
// -------------------------------------------------------------
void measureAndSend() {
  // 1) Read fill distance
  long fillDistance = getFillDistance();

  // 2) Compute fill%
  int fillPercent;
  if (fillDistance < 0) {
    // -1 => both fill sensors invalid => treat as error
    fillPercent = -1;
  } else {
    fillPercent = computeFillPercent(fillDistance, BIN_HEIGHT_CM);
  }

  // 3) Should clean?
  bool shouldClean = false;
  if (fillPercent > CLEAN_THRESHOLD) {
    shouldClean = true;
  }

  // 4) Bin installed & main installed distance
  bool binInstalled = isBinInstalled(); 
  // The function isBinInstalled() will update a global 
  // 'lastMainInstalledDist' for the main sensor distance? 
  // Let's do it differently: we’ll store that inside the function below.

  long currentMainInstalledDist = measureDistance(TRIG_INST_MAIN, ECHO_INST_MAIN);
  // We'll store that so we can show in Blynk, 
  // but also fallback logic might override bin installed.

  // We'll handle fallback for main installed below, 
  // but let's always record the main sensor reading
  // for display in V4.

  // If main reading is invalid, isBinInstalled might have 
  // used the backup. But let's just store the main raw read.
  // We'll do a checkValidReading for display or not:
  // Actually let's store it raw, even if invalid.

  lastMainInstalledDist = currentMainInstalledDist;

  // 5) Decide if data changed enough to send
  bool dataChanged = false;

  // We'll compare fillDistance, fillPercent, shouldClean, binInstalled, and main installed dist.
  // We'll define a small helper function to check each item.

  // check fillDistance
  if (fillDistance != lastFillDistance) {
    dataChanged = true;
  }
  // check fillPercent
  if (fillPercent != lastFillPercent) {
    dataChanged = true;
  }
  // check shouldClean
  if (shouldClean != lastShouldClean) {
    dataChanged = true;
  }
  // check binInstalled
  if (binInstalled != lastBinInstalled) {
    dataChanged = true;
  }
  // check main installed dist
  // (We stored raw. 
  //  If you prefer to check only if it's valid, you can do so.)
  if (currentMainInstalledDist != lastMainInstalledDist) {
    // Actually, we just assigned it. So let's do a local variable 
    // 'temp' approach so we don't override the global yet:
    // We'll do a quick fix below; see notes.
    // For clarity, let's store that in a local variable first:
  }

  // Actually, let's refine: we can't compare `currentMainInstalledDist` 
  // to `lastMainInstalledDist` after we wrote it. 
  // We'll define a separate global for oldMainInstalledDist to compare.
  // For simplicity, let's do this:

  static long oldMainInstalledDist = -999; 
  if (currentMainInstalledDist != oldMainInstalledDist) {
    dataChanged = true;
  }

  // Now let's do skip logic:
  if (!dataChanged) {
    skipCount++;
    // If we've skipped 10 times in a row, force send
    if (skipCount < 10) {
      Serial.println("No change in data, skipping this send...");
      return;  // do not send
    } else {
      Serial.println("No change for 10 intervals, forcing a send...");
      skipCount = 0; // reset skip
    }
  } else {
    // data changed, we'll send and reset skipCount
    skipCount = 0;
  }

  // 6) Send data to Blynk
  if (fillPercent < 0) {
    // indicates error => we can send 999 or do nothing
    Blynk.virtualWrite(VPIN_FILL_LEVEL, 999);
    Blynk.virtualWrite(VPIN_SHOULD_CLEAN, 0);
    Blynk.virtualWrite(VPIN_FILL_DISTANCE, fillDistance); 
  } else {
    Blynk.virtualWrite(VPIN_FILL_LEVEL,   fillPercent);
    Blynk.virtualWrite(VPIN_SHOULD_CLEAN, (shouldClean ? 1 : 0));
    Blynk.virtualWrite(VPIN_FILL_DISTANCE, fillDistance);
  }
  Blynk.virtualWrite(VPIN_INSTALLED, (binInstalled ? 1 : 0));

  // write V4 = main installed sensor distance
  Blynk.virtualWrite(VPIN_MAIN_BIN_DIST, currentMainInstalledDist);

  // 7) Print results for debugging
  Serial.print("Sending data -> FillDist: ");
  Serial.print(fillDistance);
  Serial.print(" cm | Fill%: ");
  if (fillPercent < 0) {
    Serial.print("ERROR");
  } else {
    Serial.print(fillPercent);
    Serial.print("%");
  }
  Serial.print(" | Should Clean? ");
  Serial.print(shouldClean ? "YES" : "NO");
  Serial.print(" | Bin Installed? ");
  Serial.print(binInstalled ? "YES" : "NO");
  Serial.print(" | MainInstalledDist: ");
  Serial.println(currentMainInstalledDist);

  // 8) Update old values after sending
  lastFillDistance    = fillDistance;
  lastFillPercent     = fillPercent;
  lastShouldClean     = shouldClean;
  lastBinInstalled    = binInstalled;
  oldMainInstalledDist = currentMainInstalledDist;

  // skipCount is already reset above.
}

// -------------------------------------------------------------
//        HELPER: measureDistance(trig, echo)
// -------------------------------------------------------------
long measureDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, 30000UL);  // 30ms => ~5m
  if (duration == 0) {
    return 0; // no echo => invalid
  }
  // Convert microseconds to cm
  long distanceCm = duration / 29 / 2;
  return distanceCm;
}

// -------------------------------------------------------------
//        HELPER: checkValidReading(dist)
// -------------------------------------------------------------
bool checkValidReading(long distCm) {
  if (distCm <= INVALID_LOW)  return false;
  if (distCm >  INVALID_HIGH) return false;
  return true;
}

// -------------------------------------------------------------
//        HELPER: getFillDistance() with fallback
// -------------------------------------------------------------
long getFillDistance() {
  long distMain = measureDistance(TRIG_FILL_MAIN, ECHO_FILL_MAIN);
  if (checkValidReading(distMain)) {
    return distMain;
  } else {
    long distBackup = measureDistance(TRIG_FILL_BACKUP, ECHO_FILL_BACKUP);
    if (checkValidReading(distBackup)) {
      return distBackup;
    } else {
      return -1; // error sentinel
    }
  }
}

// -------------------------------------------------------------
//        HELPER: isBinInstalled() with fallback
// -------------------------------------------------------------
bool isBinInstalled() {
  // measure main installed sensor
  long distMain = measureDistance(TRIG_INST_MAIN, ECHO_INST_MAIN);

  if (checkValidReading(distMain)) {
    // main valid
    return (distMain < INSTALLED_THRESHOLD);
  } else {
    // fallback to backup
    long distBackup = measureDistance(TRIG_INST_BACKUP, ECHO_INST_BACKUP);
    if (checkValidReading(distBackup)) {
      return (distBackup < INSTALLED_THRESHOLD);
    } else {
      return false;
    }
  }
}

// -------------------------------------------------------------
//        HELPER: computeFillPercent()
// -------------------------------------------------------------
int computeFillPercent(long distance, int binHeight) {
  if (distance <= 0) {
    // sensor error => treat as full or handle differently
    return 100;
  }
  if (distance >= binHeight) {
    return 0; // empty
  }
  float fill = 100.0 * (1.0 - (float)distance / (float)binHeight);
  if (fill < 0)   fill = 0;
  if (fill > 100) fill = 100;
  return (int)fill;
}
