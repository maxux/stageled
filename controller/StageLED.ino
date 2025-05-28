#include <OctoWS2811.h>
#include <QNEthernet.h>

#define SERIAL_DEBUG  0
#define NETSYNC_FREQ  200   // interval in ms between network heartbeat

#define Monitoring Serial1
char monbuffer[512];

// line by line buffer
char monliner[128];
size_t monlinerlen = 0;

// internal core temperature prototype
extern float tempmonGetTemp(void);

typedef struct __attribute__ ((packed)) server_stats_t {
  uint64_t state;
  uint64_t old_frames;
  uint64_t frames;
  uint64_t fps;
  uint64_t time_last_frame;
  uint64_t time_current;

  uint16_t main_ac_voltage;

  uint16_t main_core_temperature;
  uint16_t mon_core_temperature;
  uint16_t ext_power_temperature;
  uint16_t ext_compute_temperature;

  uint16_t psu0_volt;
  uint16_t psu0_amps;
  uint16_t psu1_volt;
  uint16_t psu1_amps;
  uint16_t psu2_volt;
  uint16_t psu2_amps;

  uint16_t padding;

} server_stats_t;

using namespace qindesign::network;
EthernetUDP udp(16);

#define SEG_PER_LANE 8
#define LED_PER_SEG  120
#define PER_LANE     (SEG_PER_LANE * LED_PER_SEG)
#define NUM_LANES    3
#define TOTAL_LEDS   (NUM_LANES * PER_LANE)

// 8 * 120 leds = 960 leds
// reset time = 300us
// pixel time = 30us
// frame time = 29100us
// maximum frame rate = 1000 / 29.1 = 34 fps

const int bytes_per_led = 3;
const int dma_size = TOTAL_LEDS * bytes_per_led / 4;
byte stripe_pins_list[NUM_LANES] = {14, 15, 16};

DMAMEM int display_memory[dma_size];
int drawing_memory[dma_size];

const int config = WS2811_RGB | WS2811_800kHz;
OctoWS2811 leds(PER_LANE, display_memory, drawing_memory, config, NUM_LANES, stripe_pins_list);

//////////////////////////////

bool netstate = false;
bool linkinit = false;
elapsedMillis main_temp_update = 0;

server_stats_t mainstats;

void setup() {
  #if SERIAL_DEBUG
  Serial.begin(250000);
  Serial.println("[+] initializing stage-led controler");
  #endif

  Monitoring.begin(250000);
  Monitoring.addMemoryForRead(monbuffer, sizeof(monbuffer));

  // Starting from a clean buffer
  memset(monliner, 0x00, sizeof(monliner));

  leds.begin();
  leds.show();

  #if SERIAL_DEBUG
  Serial.print("[+] leds configured: ");
  Serial.println(TOTAL_LEDS);
  #endif

  pinMode(LED_BUILTIN, OUTPUT);

  // initializing default light
  for(int i = 0; i < leds.numPixels(); i++)
    leds.setPixel(i, 10, 0, 0);

  leds.show();

  // Ethernet.setDHCPEnabled(false);
  Ethernet.setHostname("ledstage");
  Ethernet.begin();

  udp.beginWithReuse(1111);
  memset(&mainstats, 0x00, sizeof(server_stats_t));
  mainstats.state = 1;
}

#if SERIAL_DEBUG
void ethernet_status() {
  uint8_t mac[6];
  char macstr[32];

  // retreive hardware mac address
  Ethernet.macAddress(mac);
  sprintf(macstr, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  bool state = Ethernet.linkState();
  int speed = Ethernet.linkSpeed();

  Serial.print("[+] ");
  Serial.print(macstr);

  Serial.print(": link ");
  Serial.print(state);

  Serial.print(", speed: ");
  Serial.print(speed);
  Serial.println(" Mbps");
}
#endif

int received = 0;
uint32_t lastcheck = 0;

void monitoring_reset_buffer() {
  memset(monliner, 0x00, sizeof(monliner));
  monlinerlen = 0;
}

char *monitoring_prefix_map[] = {
  "PSUV 0",
  "PSUV 1",
  "PSUV 2",
  "PSUA 0",
  "PSUA 1",
  "PSUA 2",
  "TEMP EXT 0",
  "TEMP EXT 1",
  "TEMP CORE 0",
  "MAIN INV 0",
};

uint16_t *monitoring_target_map[] = {
  &mainstats.psu0_volt,
  &mainstats.psu1_volt,
  &mainstats.psu2_volt,
  &mainstats.psu0_amps,
  &mainstats.psu1_amps,
  &mainstats.psu2_amps,
  &mainstats.ext_power_temperature,
  &mainstats.ext_compute_temperature,
  &mainstats.mon_core_temperature,
  &mainstats.main_ac_voltage,
};

void monitoring_parse_line() {
  #if SERIAL_DEBUG
  Serial.print("<< ");
  Serial.print(monliner);
  #endif

  if(monliner[0] != '+') {
    // Serial.println("[-] string prefix malformed");
    return;
  }

  for(int i = 0; i < sizeof(monitoring_prefix_map) / sizeof(uint16_t *); i++) {
    size_t prelen = strlen(monitoring_prefix_map[i]);

    if(strncmp(monliner + 6, monitoring_prefix_map[i], prelen) == 0) {
      *monitoring_target_map[i] = atoi(monliner + 6 + prelen + 1);
      return;
    }
  }
}

void loop() {
  if(Monitoring.available() > 0) {
    int incomingByte = Monitoring.read();
    monliner[monlinerlen] = incomingByte;
    monlinerlen += 1;

    if(monlinerlen >= sizeof(monliner)) {
      #if SERIAL_DEBUG
      Serial.println("[-] overflow from monitoring serial, resetting buffer");
      #endif

      Monitoring.clear();
      monitoring_reset_buffer();

    } else if(incomingByte == '\n') {
      monitoring_parse_line();
      monitoring_reset_buffer();
    }

    // char buffer[8];
    // sprintf(buffer, "%c", incomingByte);
    // Serial.println(buffer);
  }

  bool linkstate = Ethernet.linkState();
  if(!linkstate && !linkinit) {
    waiting_network();
    return;
  }

  int packetsize = udp.parsePacket();

  if(packetsize >= 0) {
    #if SERIAL_DEBUG
    // Serial.println(packetsize);
    #endif

    mainstats.state = 2; // frame received

    digitalWrite(LED_BUILTIN, HIGH);

    uint8_t *data = (uint8_t *) udp.data();
    int led = 0;
    int maximum = leds.numPixels();

    for(int i = 0; i < packetsize; i += 3) {
      leds.setPixel(led, data[i], data[i + 1], data[i + 2]);
      led += 1;

      if(led >= maximum)
        break;
    }

    leds.show();

    digitalWrite(LED_BUILTIN, LOW);

    mainstats.frames += 1;
    mainstats.time_last_frame = millis();
    received += 1;
  }

  if(millis() > lastcheck + NETSYNC_FREQ) {
    mainstats.time_current = millis();
    mainstats.fps = (mainstats.frames - mainstats.old_frames) * (1000 / NETSYNC_FREQ);

    if(main_temp_update > 1000) {
      mainstats.main_core_temperature = (tempmonGetTemp() * 100);
      main_temp_update = 0;
    }

    // broadcasting feedback
    udp.send("10.241.0.255", 1111, (uint8_t *) &mainstats, sizeof(mainstats));

    lastcheck = millis();
    mainstats.old_frames = mainstats.frames;
  }
}

//
// no network debug state
//
uint32_t lastnetcheck = 0;

void waiting_network() {
  int stripes = NUM_LANES * SEG_PER_LANE;
  int maximum = leds.numPixels();

  for(int i = 0; i < maximum; i++)
    leds.setPixel(i, 0);

  for(int i = 0; i < stripes; i++) {
    int index = i * LED_PER_SEG;

    for(int stripe = 0; stripe <= i; stripe++) {
      leds.setPixel(index + stripe, 10, 0, 0);
    }
  }

  leds.show();

  if(millis() > lastnetcheck + 1000) {
    lastnetcheck = millis();

    #if SERIAL_DEBUG
    Serial.printf("[+] still waiting for network link [uptime: %u sec]\n", millis() / 1000);
    ethernet_status();
    #endif
  }
}
