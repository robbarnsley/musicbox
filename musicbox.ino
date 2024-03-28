#include <math.h>
#include <SPI.h>
#include <SD.h>

#include <Adafruit_VS1053.h>
#include "Adafruit_TPA2016.h"
#include "WiFiS3.h"
#include <SPI.h>
#include <Adafruit_PN532.h>
#include <FastLED.h>        // https://github.com/FastLED/FastLED/pull/1523

#include "secrets.h"

// Pin definitions.
//
#define LED_DATA_PIN 4       // LED data pin
#define PN532_IRQ 3          // Interrupt pin for NFC shield    
#define CLK 13               // SPI Clock, shared with SD card
#define MISO 12              // Input data, from VS1053/SD card
#define MOSI 11              // Output data, to VS1053/SD card
#define SHIELD_RESET  8      // VS1053 reset pin (unused!)
#define SHIELD_CS     6      // VS1053 chip select pin (output)
#define SHIELD_DCS    7      // VS1053 Data/command select pin (output)
#define CARDCS 9             // Card chip select pin
#define DREQ 2               // VS1053 Data request, ideally an Interrupt pin

// Audiobook definitions.
//
struct Audiobook {
  String name;
  uint8_t nfc_uid[7];
  String file_path;
  String file_name;
};

Audiobook AUDIOBOOKS[] = {
  {"GRUFFALO", {0x04, 0x00, 0x0E, 0x4A, 0xE8, 0x14, 0x91}, "/GR16M16.WAV", "GR16M16.WAV"},
  {"GRUFFALO", {0x1B, 0xD5, 0x9F, 0xE6}, "/16M16.WAV", "16M16.WAV"},
};

// LED colour cycle definition.
//
const CRGB::HTMLColorCode LED_COLOUR_CYCLE[] = {
  CRGB::Red, 
  CRGB::Orange, 
  CRGB::Green, 
  CRGB::Blue, 
  CRGB::Yellow
};

// Other definitions.
//
const int NUMBER_OF_LEDS = 30;                                                                                        // Number of LEDs
const int NUM_AUDIOBOOKS = sizeof(AUDIOBOOKS) / sizeof(AUDIOBOOKS[0]);                                                // Number of audiobooks
const int NUM_LED_COLOURS_IN_CYCLE = sizeof(LED_COLOUR_CYCLE) / sizeof(LED_COLOUR_CYCLE[0]);                          // Number of colours in the cycle
const int DELAY_BETWEEN_READS = 500;                                                                                  // Time between successive NFC card reads
volatile int VOLUME = 40;                                                                                             // Music volume (default=40, max=255)
volatile int LED_BRIGHTNESS = 50;                                                                                     // LED brightness (default=50, max=255)
volatile bool VOLUME_POT_ENABLED = true;                                                                              // is the volume pot enabled? disable for remote

char ssid[] = SECRET_SSID;
char pass[] = SECRET_PASS;

int status = WL_IDLE_STATUS;

CRGB leds[NUMBER_OF_LEDS];

WiFiServer server(80);
Adafruit_TPA2016 tpa2016 = Adafruit_TPA2016();
Adafruit_VS1053_FilePlayer player = Adafruit_VS1053_FilePlayer(SHIELD_RESET, SHIELD_CS, SHIELD_DCS, DREQ, CARDCS);
Adafruit_PN532 nfc(PN532_IRQ, 5);                                                                                     // second argument isn't used.

void handle_nfc(long &time_last_card_read, boolean &reader_is_disabled, bool verbose=true) {
  uint8_t success = false;
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };  // Buffer to store the returned UID
  uint8_t uid_length;                       // Length of the UID (4 or 7 bytes depending on ISO14443A card type)

  // Read the NFC tag's info.
  success = nfc.readDetectedPassiveTargetID(uid, &uid_length);
  Serial.println(success ? "NFC read successful." : "Read failed (not a card?)");

  if (success) {
    // Display some basic information about the card.
    if (verbose) {
      Serial.println("Found an ISO14443A card: ");
      Serial.print("- UID Length: "); Serial.print(uid_length, DEC); Serial.println(" bytes");
      Serial.print("- UID Value: "); nfc.PrintHex(uid, uid_length);
    }

    // Compare the uid to that in the audiobooks array and play if applicable.
    for (int i = 0; i < NUM_AUDIOBOOKS; i++) {
      if ((memcmp(AUDIOBOOKS[i].nfc_uid, uid, uid_length)) == 0) {
        Serial.print("Card UID matches audiobook: ");
        Serial.println(AUDIOBOOKS[i].name);

        bool start_new_audiobook = false;
        if (player.playingMusic) {
          if (strcmp(player.currentTrack.name(), AUDIOBOOKS[i].file_name.c_str()) == 0) {
            Serial.println("This audiobook is already playing.");
          }
        } else {
          if (strcmp(player.currentTrack.name(), AUDIOBOOKS[i].file_name.c_str()) == 0) {
            if (player.paused()) {
              Serial.println("Resuming playback.");
              tpa2016.enableChannel(true, true);
              player.pausePlaying(false);
            } else if (player.stopped()) {
              tpa2016.enableChannel(false, false);
              Serial.println("Playback stopped.");
              player.stopPlaying();
            }
          } else {
            Serial.println("Found new audiobook.");
            start_new_audiobook = true;
          }
        }
        if (start_new_audiobook) {
          Serial.println("Playing new audiobook.");
          tpa2016.enableChannel(true, true);
          player.stopPlaying();
          player.startPlayingFile(AUDIOBOOKS[i].file_path.c_str());
        }
      }
    }
    time_last_card_read = millis();
  }
  reader_is_disabled = true;
}

String list_dir_contents_html(File dir) {
  String contents = "<table>";
    while(true) {
      File entry = dir.openNextFile();
      if (!entry) {
        break;
      }
      contents += "<tr><td>";
      contents += entry.name();
      contents += "</td>";
      if (!entry.isDirectory()) {
        contents += "<td>";
        contents += entry.size();
        contents += "</td>";
      }
      contents += "</tr>";
      entry.close();
    }
    contents += "</table>";
    return contents;
}

void read_inputs(int *inputs) {
  inputs[0] = analogRead(14);       // A0 (pot)
  inputs[1] = digitalRead(15);      // A1 (black switch)
  inputs[2] = digitalRead(16);      // A2 (white switch)
  inputs[3] = digitalRead(17);      // A3 (red switch)
  // 4 and 5 reserved for I2C SDA/SCL.
}

void send_client_response_200_OK(WiFiClient client, String message) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE HTML>");
  client.print("<html>");
  client.print(message);
  client.println("</html>");
}

void send_client_response_400_BAD_REQUEST(WiFiClient client, String message) {
  client.println("HTTP/1.1 400 BAD REQUEST");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE HTML>");
  client.print("<html>");
  client.print(message);
  client.println("</html>");
}

void send_client_response_404_NOT_FOUND(WiFiClient client, String message) {
  client.println("HTTP/1.1 400 NOT FOUND");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
  client.println("<!DOCTYPE HTML>");
  client.print("<html>");
  client.print(message);
  client.println("</html>");
}

void set_led_brightness(int new_brightness) {
  LED_BRIGHTNESS = new_brightness;
  FastLED.setBrightness(LED_BRIGHTNESS);
  FastLED.show();
}

void set_led_colour(CRGB::HTMLColorCode new_colour) {
  for (int led_index=0; led_index<NUMBER_OF_LEDS; led_index++) {
    leds[led_index] = new_colour;
  }
  FastLED.show();
}

void setup_amp(bool verbose=true, bool enable_L_channel=false, bool enable_R_channel=false, int gain=0, int limit=0) {
  if (tpa2016.begin()) {
    Serial.println("Amplifier initialisation successful.");
  } else {
    Serial.println("Failed to initialise amplifier.");
    while (1) delay(10);
  }
  tpa2016.enableChannel(enable_L_channel, enable_R_channel);
  tpa2016.setAGCCompression(TPA2016_AGC_8);
  tpa2016.setAttackControl(0);
  tpa2016.setHoldControl(0);
  tpa2016.setReleaseControl(0);
  tpa2016.setGain(gain);
  tpa2016.setLimitLevelOn();
  tpa2016.setLimitLevel(limit);
  Serial.println("Amplifier setup complete.");
}

void setup_inputs(bool verbose=true) {
  pinMode(A0, INPUT);           // (pot)
  pinMode(A1, INPUT_PULLUP);    // (black switch)
  pinMode(A2, INPUT_PULLUP);    // (white switch)
  pinMode(A3, INPUT_PULLUP);    // (red switch)
  // 4 and 5 reserved for I2C SDA/SCL
}

void setup_led_strip(bool verbose=true) {
  FastLED.addLeds<WS2812, LED_DATA_PIN, GRB>(leds, NUMBER_OF_LEDS);
  set_led_brightness(LED_BRIGHTNESS);
  set_led_colour(LED_COLOUR_CYCLE[0]);
}

void setup_music_player(bool verbose=true, bool use_dreq=true) {
  if (!player.begin()) {
     Serial.println("Couldn't find VS1053, do you have the right pins defined?");
     while (true) delay(10);
  }

  // Set interrupts.
  //
  if (use_dreq) {
    player.useInterrupt(VS1053_FILEPLAYER_PIN_INT);     // using DREQ pin
  } else {
    player.useInterrupt(VS1053_FILEPLAYER_TIMER0_INT);  // using Timer interrupts
  }

  // Set the volume to default (will be overriden by pot if enabled).
  player.setVolume(VOLUME, VOLUME);

  Serial.println("VS1053 setup complete.");
}

void setup_nfc(bool verbose=true) {
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("Didn't find PN53x board.");
    while (true) delay(10);
  }

  if (verbose) {
    Serial.print("Found chip PN5"); Serial.println((versiondata>>24) & 0xFF, HEX);
    Serial.print("Firmware version: "); Serial.print((versiondata>>16) & 0xFF, DEC);
    Serial.print('.'); Serial.println((versiondata>>8) & 0xFF, DEC);
  }
}

void setup_sd(bool verbose=true) {
  if (!SD.begin(CARDCS)) {
    Serial.println("SD setup failed or card not present.");
    while (true) delay(10);
  } 

  Serial.println("SD setup complete."); 
}

void setup_wifi(bool verbose=true) {
  // check module is connected.
  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("Communication with WiFi module failed.");
    while (true) delay(10);
  }

  // Create network AP.
  //
  //WiFi.config(IPAddress(192, 168, 0, 1));
  //status = WiFi.beginAP(ssid, pass);
  //if (status != WL_AP_LISTENING) {
  //  Serial.println("Creating access point failed.");
  //  while (true) delay(10);
  //}
  //Serial.println("WiFi AP created.");

  // Connect to existing AP.
  //
  while (status != WL_CONNECTED) {
    status = WiFi.begin(ssid, pass);
  }
  Serial.println("Joined WiFi network.");

  if (verbose) {
    Serial.print("- SSID: ");
    Serial.println(WiFi.SSID());
    Serial.print("- IP Address: ");
    Serial.println(WiFi.localIP());
  }

  delay(10000);

  server.begin();
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  setup_led_strip(true);            // sets LED to Red (loading)
  setup_amp(true);
  setup_inputs(true);
  setup_nfc(true);
  setup_wifi(true);
  setup_sd(true);
  setup_music_player(true, true);

  set_led_colour(CRGB::Green);    // sets LED to Green (loading done)
}

void start_nfc_passive_detection(long &time_last_card_read, boolean &reader_is_disabled, int &irq_previous, int &irq_current) {
  irq_previous = irq_current = HIGH;
  if (!nfc.startPassiveTargetIDDetection(PN532_MIFARE_ISO14443A)) {     // must be called everytime an interrupt occurs to set device in passive mode.
  } else {
    Serial.println("NFC card present. Handling...");
    handle_nfc(time_last_card_read, reader_is_disabled, true);
  }
}

void loop() {
  // NFC initialisation.
  //
  long time_last_card_read = 0;
  bool reader_is_disabled = false;
  int irq_previous;
  int irq_current;
  start_nfc_passive_detection(time_last_card_read, reader_is_disabled, irq_previous, irq_current);
  int last_inputs[4] = {0,0,0,0};
  int led_colour_cycle_index = 0;
  while (true) {
    // --------------
    // Handle inputs.
    //
    int these_inputs[4];
    read_inputs(these_inputs);

    // INPUT: volume pot (A0)
    int read_volume = map(these_inputs[0], 0, 1024, 0, 150);
    // change the volume if it's changed and the pot's enabled
    if (VOLUME_POT_ENABLED) {
      if (read_volume != VOLUME) {
        VOLUME = read_volume;
        player.setVolume(VOLUME, VOLUME);
      }
    }

    // INPUT: colour momentary buttons (A1, A2, A3)
    if (last_inputs[1] && !these_inputs[1]) {
      Serial.println("Input 1 triggered. LEDs off.");
      set_led_colour(CRGB::Black);
    } else if (last_inputs[2] && !these_inputs[2]) {
      Serial.println("Input 2 triggered. LEDs on (white).");
      set_led_colour(CRGB::White);
    } else if (last_inputs[3] && !these_inputs[3]) {
      Serial.println("Input 3 triggered. Cycling colour.");
      set_led_colour(LED_COLOUR_CYCLE[led_colour_cycle_index]);
      if (led_colour_cycle_index < NUM_LED_COLOURS_IN_CYCLE) {
        led_colour_cycle_index++;
      } else { 
        led_colour_cycle_index = 0;
      }
    }

    // copy into last_inputs
    memcpy(last_inputs, these_inputs, sizeof(these_inputs[0])*4);

    // ----------
    // Handle NFC.
    //
    if (reader_is_disabled) {
      if (millis() - time_last_card_read > DELAY_BETWEEN_READS) {
        reader_is_disabled = false;
        start_nfc_passive_detection(time_last_card_read, reader_is_disabled, irq_previous, irq_current);
      }
    } else {
      irq_current = digitalRead(PN532_IRQ);
      if (irq_current == LOW && irq_previous == HIGH) {   // when the IRQ is pulled low there is a card nearby.
        Serial.println("NFC card detected.");
        handle_nfc(time_last_card_read, reader_is_disabled, true);
      } else {
        if (player.playingMusic) {
          Serial.println("NFC card not detected. Pausing playback.");
          player.pausePlaying(true);
          tpa2016.enableChannel(false, false);
        }
      }
      irq_previous = irq_current;
    }

    // ------------------------
    // Handle WiFi connections.
    //
    if (player.stopped() || player.paused()) {
      WiFiClient client = server.available();
      if (client) {
        Serial.println("New client connected.");
        boolean current_line_is_blank = true;   // an HTTP request ends with a blank line
        String request = "";                    // string to store entire request
        while (client.connected()) {
          if (client.available()) {
            char c = client.read();
            request += c;

            // If at end of the line (received a newline character) and the line is blank, the 
            // HTTP request has ended so send a reply.
            //
            if (c == '\n' && current_line_is_blank) {
              // Extract the path from the full request for routing.
              String path;
              int start_index = request.indexOf(' ');                 // find the first whitespace
              if (start_index != -1) {
                start_index += 1;                                     // move index to start of path after whitespace
                int end_index = request.indexOf(' ', start_index);    // find the end of the path
                if (end_index != -1) {
                  path = request.substring(start_index, end_index);   // extract the path
                }
              }

              // Do something depending on the route requested.
              //
              Serial.print("Request received: ");
              Serial.println(path);
              if (path.indexOf("/list") != -1) {
                String contents = list_dir_contents_html(SD.open("/"));
                send_client_response_200_OK(client, contents);
              } else if (path.indexOf("/start/") != -1) {
                int start_index = path.lastIndexOf('/') + 1;                      // find the index of the last '/'
                int end_index = path.length();                                    // end index is the length of the string
                if (start_index < end_index) {
                  String file = "/" + path.substring(start_index, end_index);     // extract the file path as a string
                  player.startPlayingFile(file.c_str());
                  send_client_response_200_OK(client, "Started playing " + file + ".");
                } else {
                  send_client_response_400_BAD_REQUEST(client, "Route not understood.");
                }
              } else if (path.indexOf("/stop") != -1) {
                player.stopPlaying();
                send_client_response_200_OK(client, "Stopping playback.");
              } else if (path.indexOf("/pause") != -1) {
                player.pausePlaying(true);
                send_client_response_200_OK(client, "Pausing playback.");
              } else if (path.indexOf("/resume") != -1) {
                player.pausePlaying(false);
                send_client_response_200_OK(client, "Resuming playback.");
              } else if (path.indexOf("/inputs/volume/disable") != -1) {
                VOLUME_POT_ENABLED = false;
                send_client_response_200_OK(client, "Disabled volume pot.");
              } else if (path.indexOf("/inputs/volume/enable") != -1) {
                VOLUME_POT_ENABLED = true;
                send_client_response_200_OK(client, "Enabled volume pot.");
              } else if (path.indexOf("/volume/") != -1) {
                int start_index = path.lastIndexOf('/') + 1;                      // find the index of the last '/'
                int end_index = path.length();                                    // end index is the length of the string
                if (start_index < end_index) {
                  String volume = path.substring(start_index, end_index);         // extract the volume as a string
                  VOLUME = volume.toInt();                                        // convert the volume to an integer
                  VOLUME_POT_ENABLED = false;                                     // disable the pot
                  player.setVolume(VOLUME, VOLUME);
                  send_client_response_200_OK(client, "Set volume to " + String(VOLUME) + ".");
                } else {
                  send_client_response_400_BAD_REQUEST(client, "Route not understood.");
                }
              } else if (path.indexOf("/volume") != -1) {
                send_client_response_200_OK(client, "Volume is set to " + String(VOLUME) + ".");
              } else if (path.indexOf("/brightness/") != -1) {
                int start_index = path.lastIndexOf('/') + 1;                      // find the index of the last '/'
                int end_index = path.length();                                    // end index is the length of the string
                if (start_index < end_index) {
                  String brightness = path.substring(start_index, end_index);     // extract the brightness as a string
                  set_led_brightness(brightness.toInt());                         // convert brightness to integer and set
                  send_client_response_200_OK(client, "Set brightness to " + String(LED_BRIGHTNESS) + ".");
                } else {
                  send_client_response_400_BAD_REQUEST(client, "Route not understood.");
                }
              } else if (path.indexOf("/brightness") != -1) {
                send_client_response_200_OK(client, "Brightness is set to " + String(LED_BRIGHTNESS) + ".");
              } else {
                send_client_response_404_NOT_FOUND(client, "The requested resource does not exist.");
              }
              break;
            }
            if (c == '\n') {                    // starting a new line
              current_line_is_blank = true;
            } else if (c != '\r') {             // character is on the current line
              current_line_is_blank = false;
            }
          }
        }
        delay(1);   // give the web browser time to receive the data

        // Close the connection.
        //
        client.stop();
        Serial.println("Client disconnected.");
      }
    }
  }
}

