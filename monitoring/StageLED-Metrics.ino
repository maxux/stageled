#include <OneWire.h>
#include <DallasTemperature.h>
#include <ZMPT101B.h>
#include "ADS1X15.h"

// Hardware Serial pretty name
#define Upstream Serial1

// Temperatures sensors
#define ONE_WIRE_BUS 14

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature temperature(&oneWire);

unsigned long sensors_cooldown = 0;

// internal core temperature prototype
extern float tempmonGetTemp(void);

// PSU Modules Instances
typedef struct psu_t {
  ADS1115 *raw;
  
  unsigned long updated;
  float volts;
  float amps;

} psu_t;

ADS1115 rPSU1(0x48);
psu_t PSU1 = {.raw = &rPSU1};

ADS1115 rPSU2(0x49);
psu_t PSU2 = {.raw = &rPSU2};

ADS1115 rPSU3(0x4A);
psu_t PSU3 = {.raw = &rPSU3};

psu_t *psus[] = {&PSU1, &PSU2, &PSU3};

// AC Voltage module
#define AC_VOLTAGE_SENSITIVITY  1423.0f
ZMPT101B acvoltage(A9, 50.0);

// Status LED pins
#define POWER_MAIN_LED  3 // Digital PWM

float vdc15_r1 = 6800.0;  // 15 VDC max.
float vdc15_r2 = 3300.0;

void setup() {
  Serial.begin(250000); // Not needed but boot is truncated
  Upstream.begin(250000);

  Serial.println("");
  Serial.println("[+] initializing stage led metrics monitor");
  Upstream.println("+INIT StageLED Monitoring");

  Serial.println("[+] initializing realtime status led");
  pinMode(POWER_MAIN_LED, OUTPUT);
  analogWrite(POWER_MAIN_LED, 2);

  Serial.println("[+] initializing temperature sensors");
  temperature.begin();
  
  int sensors = temperature.getDeviceCount();
  Serial.print("[+] temperature: ");
  Serial.print(sensors, DEC);
  Serial.println(" devices found");

  Upstream.print("+DISCOVER Temperature ");
  Upstream.println(sensors);

  Serial.println("[+] initializing current and voltage sensors");
  Wire.begin();

  for(size_t i = 0; i < sizeof(psus) / sizeof(*psus); i++) {
    psu_t *psu = psus[i];

    Serial.print("[+] initializing module: psu ");
    Serial.println(i + 1);

    if(!psu->raw->begin()) {
      Serial.println("[-] psu module could not initialize");
      continue;
    }

    if(!psu->raw->isConnected()) {
      Serial.println("[-] psu module not connected");
      continue;
    }

    psu->raw->setDataRate(1);

    psu->updated = 0;
    psu->volts = 0.0;
    psu->amps = 0.0;

    Serial.println("[+] module initialized");
    Upstream.print("+DISCOVER PSU-");
    Upstream.println(i + 1);
  }

  acvoltage.setSensitivity(AC_VOLTAGE_SENSITIVITY);

  Serial.println("[+] metrics system initialized, measuring");
}

void loop() {
  int state = 0;

  digitalWrite(13, HIGH);
  analogWrite(POWER_MAIN_LED, 2);

  Serial.print("Metrics: ");
 
  // float main_voltage = acvoltage.getRmsVoltage();
  float main_voltage = 0.00;
  
  Serial.print(main_voltage);
  Serial.print(" v | ");

  Upstream.print("+DATA MAIN INV 0 ");
  Upstream.println((int) (main_voltage * 100));

  for(size_t i = 0; i < sizeof(psus) / sizeof(*psus); i++) {
    psu_t *psu = psus[i];

    int16_t diff = psu->raw->readADC_Differential_0_1();

    int16_t v1 = psu->raw->readADC(2);
    float vv1 = (psu->raw->toVoltage(v1) / 5.0) * 15.0;
    
    float hallVoltage = psu->raw->toVoltage(diff);
    float hallAmps = hallVoltage * (20.0 / 0.625); // 20 ampere

    /*
    Serial.print("Current: ");
    Serial.print(hallAmps);
    Serial.print(" amp, ");
    // Serial.println(hallVoltage);
    Serial.print(hallAmps * vv1);
    Serial.println(" watt");
    */

    psu->updated = micros();
    psu->volts = vv1;
    psu->amps = hallAmps;
  }


  for(size_t i = 0; i < sizeof(psus) / sizeof(*psus); i++) {
    psu_t *psu = psus[i];

    Serial.print(psu->volts, 2);
    Serial.print(" v, ");
    Serial.print(psu->amps, 2);
    Serial.print(" A | ");

    Upstream.print("+DATA PSUV ");
    Upstream.print(i);
    Upstream.print(" ");
    Upstream.println((int) (psu->volts * 100));

    Upstream.print("+DATA PSUA ");
    Upstream.print(i);
    Upstream.print(" ");
    Upstream.println((int) (psu->amps * 100));
  }

  //
  // temperature sensors rate limitted
  //

  if(sensors_cooldown > millis()) {
    Serial.println("---");
    delay(100);
    return;
  }

  temperature.requestTemperatures();

  for(size_t i = 0; i < temperature.getDeviceCount(); i++) {
    float temp = temperature.getTempCByIndex(i);
    Upstream.print("+DATA TEMP EXT ");
    Upstream.print(i);
    Upstream.print(" ");
    Upstream.println((int) (temp * 100));

    Serial.print(temp, 2);
    Serial.print("°C | ");
  }

  float core = tempmonGetTemp();
  Serial.print(core, 2);
  Serial.print("°C | ");

  Upstream.print("+DATA TEMP CORE 0 ");
  Upstream.println((int) (core * 100));

  // Cooldown for 5 seconds
  sensors_cooldown = millis() + 5000;
  Serial.println("");
}