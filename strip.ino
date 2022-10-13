// © Kay Sievers <kay@versioduo.com>, 2022
// SPDX-License-Identifier: Apache-2.0

#include <V2Base.h>
#include <V2Buttons.h>
#include <V2Color.h>
#include <V2Device.h>
#include <V2LED.h>
#include <V2MIDI.h>
#include <V2Music.h>

V2DEVICE_METADATA("com.versioduo.strip", 9, "versioduo:samd:strip");

static V2LED::WS2812 LED(2, PIN_LED_WS2812, &sercom2, SPI_PAD_0_SCK_1, PIO_SERCOM);
static V2LED::WS2812 LEDExt(128, PIN_LED_WS2812_EXT, &sercom1, SPI_PAD_0_SCK_1, PIO_SERCOM);

// Config, written to EEPROM.
static constexpr struct Configuration {
  enum class Program : uint8_t { Notes, Bar, _count };
  struct {
    uint8_t count{88};
    bool reverse{};
    float power{0.5};
  } leds;
  struct {
    char name[32];
    uint8_t note{V2MIDI::A(-1)};
    uint8_t count{88};
    uint8_t led{};
    Program program{};
    struct {
      uint8_t h{0};
      uint8_t s{0};
      uint8_t v{127};
    } color;
  } channels[16];
} ConfigurationDefault{.channels{
  {{.name = "White"}},
  {{.name = "Red"}, .color{.h{0}, .s{127}}},
  {{.name = "Green"}, .color{.h{42}, .s{127}}},
  {{.name = "Blue"}, .color{.h{85}, .s{127}}},
  {{.name = "Cyan"}, .color{.h{63}, .s{127}}},
  {{.name = "Magenta"}, .color{.h{106}, .s{127}}},
  {{.name = "Yellow"}, .color{.h{21}, .s{127}}},
  {{.name = "Orange"}, .color{.h{11}, .s{127}}},
  {{.name = "Amber"}, .color{.h{8}, .s{127}}},
  {{.name = "Pink"}, .color{.h{116}, .s{100}}},
  {{.name = "Lavender"}, .color{.h{95}, .s{70}}},
  {{.name = "Champagne"}, .color{.h{12}, .s{100}}},
  {{.name = "Desert"}, .color{.h{10}, .s{40}}},
  {{.name = "Aquamarine"}, .color{.h{56}, .s{127}}},
  {{.name = "Cornflower"}, .color{.h{78}, .s{100}}},
  {{.name = "Aqua"}, .color{.h{77}, .s{50}}},
}};

static class Device : public V2Device {
public:
  Device() : V2Device() {
    metadata.vendor      = "Versio Duo";
    metadata.product     = "V2 strip";
    metadata.description = "LED strip";
    metadata.home        = "https://versioduo.com/#strip";

    help.device        = "Drives a WS2812 LED strip. LEDs are controlled by incoming MIDI notes, the "
                         "brightness is controlled by the note velocity. Different MIDI channels use "
                         "different colors.";
    help.configuration = "# Power\n"
                         "The actual LED brightness depends on the version of the WS2812 "
                         "chip. The maximum brightness can be adjusted.\n"
                         "# Channel\n"
                         "The range of consecutive notes, the offset of the first LED to map the "
                         "first note of the range to, and the color can be configured. Using "
                         "different start values allows to split the strip into different zones";

    system.download  = "https://versioduo.com/download";
    system.configure = "https://versioduo.com/configure";

    usb.ports.standard = 0;

    configuration = {.version{2}, .size{sizeof(config)}, .data{&config}};
  }

  enum class CC {
    Volume  = V2MIDI::CC::ChannelVolume,
    Rainbow = V2MIDI::CC::Controller90,
  };

  Configuration config{ConfigurationDefault};

  void updateRainbow(float brightness) {
    LEDExt.rainbow(1, 4.5f - (_rainbow * 4.f), brightness);
  }

private:
  const char *_program_names[(uint8_t)Configuration::Program::_count]{"Notes", "Bar"};

  struct {
    float volume;
    struct {
      bool active;
      V2Music::Playing<128> playing;
    } bar;

    struct {
      uint8_t velocity[128];
    } notes;
  } _channels[16]{};

  struct {
    struct {
      uint8_t channel;
      uint8_t velocity;
    } notes;
  } _leds[128]{};

  float _rainbow{};
  unsigned long _timeout_usec{};
  V2Music::ForcedStop _force;

  void handleReset() override {
    _timeout_usec = 0;
    _force.reset();

    allNotesOff();
  }

  void handleLoop() override {
    if (_timeout_usec > 0 && (unsigned long)(micros() - _timeout_usec) > 900 * 1000 * 1000)
      reset();
  }

  void allNotesOff() {
    if (_force.trigger()) {
      reset();
      return;
    }

    for (uint8_t ch = 0; ch < 16; ch++) {
      _channels[ch].volume = 100.f / 127.f;
      _channels[ch].bar.playing.reset();

      for (uint8_t i = 0; i < V2Base::countof(_channels[0].notes.velocity); i++)
        _channels[ch].notes.velocity[i] = 0;
    }

    for (uint8_t i = 0; i < V2Base::countof(_leds); i++)
      _leds[i] = {};

    _rainbow = 0;
    LED.reset();
    LED.setHSV(V2Color::Orange, 0.8, 0.15);

    LEDExt.reset();
  }

  // Flash an LED bar, the length depending on the note velocity.
  bool updateLEDsBar() {
    for (int8_t ch = 15; ch >= 0; ch--) {
      if (config.channels[ch].program != Configuration::Program::Bar)
        continue;

      uint8_t note;
      uint8_t velocity;
      if (!_channels[ch].bar.playing.getLast(note, velocity)) {
        if (!_channels[ch].bar.active)
          continue;

        for (uint8_t i = 0; i < config.channels[ch].count; i++)
          LEDExt.setBrightness(config.channels[ch].led + i, 0);

        _channels[ch].bar.active = false;
        continue;
      }

      const float h = (float)config.channels[ch].color.h / 127.f * 360.f;
      const float s = (float)config.channels[ch].color.s / 127.f;
      const float v = (float)config.channels[ch].color.v / 127.f;

      const float fraction   = (float)velocity / 127.f;
      const float adjusted   = 0.1f + (0.9f * fraction);
      const float brightness = _channels[ch].volume * adjusted * v;

      const uint8_t count = ceilf((float)config.channels[ch].count * fraction);
      for (uint8_t i = 0; i < count; i++)
        LEDExt.setHSV(config.channels[ch].led + i, h, s, brightness);

      for (uint8_t i = count; i < config.channels[ch].count; i++)
        LEDExt.setBrightness(config.channels[ch].led + i, 0);

      _channels[ch].bar.active = true;
      return true;
    }

    return false;
  }

  // Show individual notes.
  void updateLEDsNotes(bool force = false) {
    for (uint8_t i = 0; i < config.leds.count; i++) {
      uint8_t channel  = 0;
      uint8_t velocity = 0;

      // Find an active note in one of the 16 channels. The note in the
      // highest channel number wins.
      for (int8_t ch = 15; ch >= 0; ch--) {
        if (config.channels[ch].program != Configuration::Program::Notes)
          continue;

        if (i < config.channels[ch].led)
          continue;

        if (i >= config.channels[ch].led + config.channels[ch].count)
          continue;

        const uint8_t note = config.channels[ch].note + i - config.channels[ch].led;
        if (note > 127)
          continue;

        if (_channels[ch].notes.velocity[note] == 0)
          continue;

        channel  = ch;
        velocity = _channels[ch].notes.velocity[note];
        break;
      }

      if (!force && _leds[i].notes.channel == channel && _leds[i].notes.velocity == velocity)
        continue;

      _leds[i].notes.channel  = channel;
      _leds[i].notes.velocity = velocity;

      if (velocity == 0) {
        LEDExt.setBrightness(i, 0);
        continue;
      }

      const float h        = (float)config.channels[channel].color.h / 127.f * 360.f;
      const float s        = (float)config.channels[channel].color.s / 127.f;
      const float v        = (float)config.channels[channel].color.v / 127.f;
      const float fraction = (float)velocity / 127.f;
      LEDExt.setHSV(i, h, s, _channels[channel].volume * fraction * v);
    }
  }

  void updateLEDs(bool force = false) {
    if (updateLEDsBar())
      return;

    updateLEDsNotes(force);
  }

  void handleNote(uint8_t channel, uint8_t note, uint8_t velocity) override {
    switch (config.channels[channel].program) {
      case Configuration::Program::Notes:
        _channels[channel].notes.velocity[note] = velocity;
        break;

      case Configuration::Program::Bar:
        _channels[channel].bar.playing.update(note, velocity);
        break;
    }

    updateLEDs();
    _timeout_usec = micros();
  }

  void handleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) override {
    handleNote(channel, note, 0);
    _timeout_usec = micros();
  }

  void handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) override {
    switch (controller) {
      case (uint8_t)CC::Volume:
        _channels[channel].volume = (float)value / 127.f;
        if (_rainbow > 0.f)
          updateRainbow(_channels[channel].volume);

        else
          updateLEDs(true);
        break;

      case (uint8_t)CC::Rainbow:
        _rainbow = (float)value / 127.f;
        if (_rainbow <= 0.f) {
          LEDExt.reset();
          updateLEDs(true);

        } else
          updateRainbow(_channels[channel].volume);
        break;

      case V2MIDI::CC::AllSoundOff:
      case V2MIDI::CC::AllNotesOff:
        allNotesOff();
        break;
    }

    _timeout_usec = micros();
  }

  void handleSystemExclusive(const uint8_t *buffer, uint32_t len) override {
    if (len < 10)
      return;

    // 0x7d == SysEx prototype/research/private ID
    if (buffer[1] != 0x7d)
      return;

    // Handle only JSON messages.
    if (buffer[2] != '{' || buffer[len - 2] != '}')
      return;

    // Read incoming message.
    StaticJsonDocument<4096> json;
    if (deserializeJson(json, buffer + 2, len - 1))
      return;

    JsonObject json_led = json["led"];
    if (!json_led)
      return;

    JsonArray json_colors = json_led["colors"];
    if (!json_colors)
      return;

    const uint8_t start = json_led["start"];
    for (uint8_t i = 0; i + start < 128; i++) {
      JsonArray json_color = json_colors[i];
      if (!json_color)
        break;

      // Empty array, skip LED.
      if (json_color[2].isNull())
        continue;

      uint8_t hue = json_color[0];
      if (hue > 127)
        hue = 127;

      uint8_t saturation = json_color[1];
      if (saturation > 127)
        saturation = 127;

      uint8_t brightness = json_color[2];
      if (brightness > 127)
        brightness = 127;

      LEDExt.setHSV(i + start, (float)hue / 127.f * 360.f, (float)saturation / 127.f, (float)brightness / 127.f);
    }

    _timeout_usec = micros();
  }

  void handleSystemReset() override {
    reset();
  }

  void exportInput(JsonObject json) override {
    JsonArray json_channels = json.createNestedArray("channels");
    for (uint8_t i = 0; i < 16; i++) {
      JsonObject json_channel = json_channels.createNestedObject();
      json_channel["number"]  = i;
      json_channel["name"]    = config.channels[i].name;

      JsonArray json_controllers = json_channel.createNestedArray("controllers");
      {
        JsonObject json_controller = json_controllers.createNestedObject();
        json_controller["name"]    = "Volume";
        json_controller["number"]  = (uint8_t)CC::Volume;
        json_controller["value"]   = (uint8_t)(_channels[i].volume * 127.f);
      }
      {
        JsonObject json_controller = json_controllers.createNestedObject();
        json_controller["name"]    = "Rainbow";
        json_controller["number"]  = (uint8_t)CC::Rainbow;
        json_controller["value"]   = (uint8_t)(_rainbow * 127.f);
      }

      JsonObject json_chromatic = json_channel.createNestedObject("chromatic");
      json_chromatic["start"]   = config.channels[i].note;
      json_chromatic["count"]   = config.channels[i].count;
    }
  }

  void exportSettings(JsonArray json) override {
    {
      JsonObject setting = json.createNestedObject();
      setting["type"]    = "number";
      setting["title"]   = "LED";
      setting["label"]   = "Count";
      setting["min"]     = 1;
      setting["max"]     = 128;
      setting["default"] = ConfigurationDefault.leds.count;
      setting["path"]    = "leds/count";
    }
    {
      JsonObject setting = json.createNestedObject();
      setting["type"]    = "toggle";
      setting["label"]   = "Reverse";
      setting["default"] = ConfigurationDefault.leds.reverse;
      setting["path"]    = "leds/reverse";
    }
    {
      JsonObject setting = json.createNestedObject();
      setting["type"]    = "number";
      setting["label"]   = "Power";
      setting["min"]     = 0;
      setting["max"]     = 1;
      setting["step"]    = 0.01;
      setting["default"] = ConfigurationDefault.leds.power;
      setting["path"]    = "leds/power";
    }

    for (uint8_t ch = 0; ch < 16; ch++) {
      {
        JsonObject setting = json.createNestedObject();
        setting["type"]    = "title";

        char name[64];
        sprintf(name, "Channel %d", ch + 1);
        setting["title"] = name;
      }
      {
        JsonObject setting = json.createNestedObject();
        setting["type"]    = "text";
        setting["label"]   = "Name";

        char path[64];
        sprintf(path, "channels[%d]/name", ch);
        setting["path"] = path;
      }
      {
        JsonObject setting = json.createNestedObject();
        setting["type"]    = "note";
        setting["label"]   = "Note";

        setting["default"] = ConfigurationDefault.channels[ch].note;

        char path[64];
        sprintf(path, "channels[%d]/note", ch);
        setting["path"] = path;
      }
      {
        JsonObject setting = json.createNestedObject();
        setting["type"]    = "number";
        setting["label"]   = "Note";
        setting["text"]    = "Count";
        setting["max"]     = 128;

        char path[64];
        sprintf(path, "channels[%d]/count", ch);
        setting["path"] = path;
      }
      {
        JsonObject setting = json.createNestedObject();
        setting["type"]    = "number";
        setting["label"]   = "Program";
        setting["max"]     = (uint8_t)Configuration::Program::_count - 1;
        setting["input"]   = "select";
        JsonArray names    = setting.createNestedArray("names");
        for (uint8_t i = 0; i < (uint8_t)Configuration::Program::_count; i++)
          names.add(_program_names[i]);

        char path[64];
        sprintf(path, "channels[%d]/program", ch);
        setting["path"] = path;
      }
      {
        JsonObject setting = json.createNestedObject();
        setting["type"]    = "color";
        setting["ruler"]   = true;

        char path[64];
        sprintf(path, "channels[%d]/color", ch);
        setting["path"] = path;
      }
    }
  }

  void importConfiguration(JsonObject json) override {
    JsonObject json_leds = json["leds"];
    if (json_leds) {
      if (!json_leds["count"].isNull()) {
        uint8_t count = json_leds["count"];
        if (count > 128)
          count = 128;
        config.leds.count = count;
      }

      if (!json_leds["reverse"].isNull())
        config.leds.reverse = json_leds["reverse"];

      if (!json_leds["power"].isNull())
        config.leds.power = json_leds["power"];
    }

    JsonArray json_channels = json["channels"];
    if (json_channels) {
      for (uint8_t ch = 0; ch < 16; ch++) {
        if (json_channels[ch].isNull())
          break;

        const char *name = json_channels[ch]["name"];
        if (name)
          strlcpy(config.channels[ch].name, name, sizeof(config.channels[ch].name));

        if (!json_channels[ch]["note"].isNull()) {
          uint16_t note = json_channels[ch]["note"];
          if (note > 127)
            note = 127;

          config.channels[ch].note = note;
        }

        if (!json_channels[ch]["count"].isNull()) {
          uint16_t count = json_channels[ch]["count"];
          if (count > 127)
            count = 127;

          config.channels[ch].count = count;
        }

        if (!json_channels[ch]["led"].isNull()) {
          uint16_t led = json_channels[ch]["led"];
          if (led > 127)
            led = 127;

          config.channels[ch].led = led;
        }

        if (!json_channels[ch]["program"].isNull()) {
          uint16_t program = json_channels[ch]["program"];
          if (program > (uint8_t)Configuration::Program::_count - 1)
            program = (uint8_t)Configuration::Program::_count - 1;

          config.channels[ch].program = (Configuration::Program)program;
        }

        JsonArray json_color = json_channels[ch]["color"];
        if (json_color) {
          uint8_t color = json_color[0];
          if (color > 127)
            color = 127;
          config.channels[ch].color.h = color;

          uint8_t saturation = json_color[1];
          if (saturation > 127)
            saturation = 127;
          config.channels[ch].color.s = saturation;

          uint8_t brightness = json_color[2];
          if (brightness > 127)
            brightness = 127;
          config.channels[ch].color.v = brightness;
        }
      }
    }

    LEDExt.setNumLEDs(config.leds.count);
    LEDExt.setDirection(config.leds.reverse);
    LEDExt.setMaxBrightness(config.leds.power);
    updateLEDs(true);
  }

  void exportConfiguration(JsonObject json) override {
    {
      json["#leds"]        = "The properties of the connected LEDs";
      JsonObject json_leds = json.createNestedObject("leds");
      json_leds["#count"]  = "The number of LEDs to drive";
      json_leds["count"]   = config.leds.count;

      json_leds["#reverse"] = "The direction of the strip";
      json_leds["reverse"]  = config.leds.reverse;

      json_leds["#power"] = "The maximum brightness of the LEDs (0..1)";
      json_leds["power"]  = serialized(String(config.leds.power, 2));
    }

    json["#channels"]       = "The MIDI channels with different colors and zones";
    JsonArray json_channels = json.createNestedArray("channels");
    for (uint8_t i = 0; i < 16; i++) {
      JsonObject json_channel = json_channels.createNestedObject();
      json_channel["name"]    = config.channels[i].name;

      if (i == 0)
        json_channel["#note"] = "The first MIDI note to map";
      json_channel["note"] = config.channels[i].note;

      if (i == 0)
        json_channel["#count"] = "The number of notes to map";
      json_channel["count"] = config.channels[i].count;

      if (i == 0)
        json_channel["#led"] = "The position of the first LED on the strip to map the notes to";
      json_channel["led"] = config.channels[i].led;

      if (i == 0)
        json_channel["#program"] = "The channel\'s program, individual notes or a bar";
      json_channel["program"] = (uint8_t)config.channels[i].program;

      JsonArray json_color = json_channel.createNestedArray("color");
      json_color.add(config.channels[i].color.h);
      json_color.add(config.channels[i].color.s);
      json_color.add(config.channels[i].color.v);
    }
  }
} Device;

// Dispatch MIDI packets
static class MIDI {
public:
  void loop() {
    if (!Device.usb.midi.receive(&_midi))
      return;

    if (_midi.getPort() != 0)
      return;

    Device.dispatch(&Device.usb.midi, &_midi);
  }

private:
  V2MIDI::Packet _midi{};
} MIDI;

static class Button : public V2Buttons::Button {
public:
  Button() : V2Buttons::Button(&_config, PIN_BUTTON) {}

private:
  const V2Buttons::Config _config{.click_usec{150 * 1000}, .hold_usec{300 * 1000}};

  void handleClick(uint8_t count) override {
    Device.reset();
  }

  void handleHold(uint8_t count) override {
    LED.setHSV(V2Color::Cyan, 0.8, 0.15);
    Device.updateRainbow(0.75);
  }

  void handleRelease() override {
    Device.reset();
  }
} Button;

void setup() {
  Serial.begin(9600);

  LED.begin();
  LED.setMaxBrightness(0.5);

  LEDExt.begin();
  LEDExt.setMaxBrightness(Device.config.leds.power);

  Button.begin();
  Device.begin();
  Device.reset();
}

void loop() {
  LED.loop();
  LEDExt.loop();
  MIDI.loop();
  V2Buttons::loop();
  Device.loop();

  if (Device.idle())
    Device.sleep();
}
