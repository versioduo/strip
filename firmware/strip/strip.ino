#include <V2Base.h>
#include <V2Buttons.h>
#include <V2Device.h>
#include <V2LED.h>
#include <V2MIDI.h>
#include <V2Music.h>

V2DEVICE_METADATA("com.versioduo.strip", 16, "versioduo:samd:strip");

static V2LED::WS2812 LED(2, PIN_LED_WS2812, &sercom2, SPI_PAD_0_SCK_1, PIO_SERCOM);
static V2LED::WS2812 LEDExt(128, PIN_LED_WS2812_EXT, &sercom1, SPI_PAD_0_SCK_1, PIO_SERCOM);

// Config, written to EEPROM.
static constexpr struct Configuration {
  enum class Program : uint8_t { Notes, Bar, _count };
  struct {
    uint8_t count{88};
    bool    reverse{};
    float   power{0.5};
  } leds;

  struct {
    char    name[32];
    uint8_t note{V2MIDI::A(-1)};
    uint8_t count{88};
    uint8_t start{};
    Program program{};
    struct {
      uint8_t h{0};
      uint8_t s{0};
      uint8_t v{127};
    } color;
  } channels[16];

  float cNotes{};
} ConfigurationDefault{.channels{
  {.name{"White"}},
  {.name{"Red"}, .color{.h{0}, .s{127}}},
  {.name{"Green"}, .color{.h{42}, .s{127}}},
  {.name{"Blue"}, .color{.h{85}, .s{127}}},
  {.name{"Cyan"}, .color{.h{63}, .s{127}}},
  {.name{"Magenta"}, .color{.h{106}, .s{127}}},
  {.name{"Yellow"}, .color{.h{21}, .s{127}}},
  {.name{"Orange"}, .color{.h{11}, .s{127}}},
  {.name{"Amber"}, .color{.h{8}, .s{127}}},
  {.name{"Pink"}, .color{.h{116}, .s{100}}},
  {.name{"Lavender"}, .color{.h{95}, .s{70}}},
  {.name{"Champagne"}, .color{.h{12}, .s{100}}},
  {.name{"Desert"}, .color{.h{10}, .s{40}}},
  {.name{"Aquamarine"}, .color{.h{56}, .s{127}}},
  {.name{"Cornflower"}, .color{.h{78}, .s{100}}},
  {.name{"Aqua"}, .color{.h{77}, .s{50}}},
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
    CNotes  = V2MIDI::CC::Controller85,
    Rainbow = V2MIDI::CC::Controller90,
  };

  Configuration config{ConfigurationDefault};

  void updateRainbow(float brightness) {
    LEDExt.rainbow(1, 4.5f - (_rainbow * 4.f), brightness);
  }

private:
  const char* _programNames[(uint8_t)Configuration::Program::_count]{"Notes", "Bar"};

  struct {
    uint8_t aftertouch;

    struct {
      bool                  active;
      V2Music::Playing<128> playing;
    } bar;

    struct {
      uint8_t velocity;
      uint8_t aftertouch;
    } notes[128];
  } _channels[16]{};

  struct {
    uint8_t channel;
    uint8_t brightness;
  } _leds[128]{};

  float               _volume{100.f / 127.f};
  float               _cNotes{};
  float               _rainbow{};
  uint32_t            _timeoutUsec{};
  V2Music::ForcedStop _force;

  void handleReset() override {
    _timeoutUsec = 0;
    _force.reset();

    allNotesOff();
  }

  void handleLoop() override {
    if (_timeoutUsec > 0 && V2Base::getUsecSince(_timeoutUsec) > 900 * 1000 * 1000)
      reset();
  }

  void allNotesOff() {
    if (_force.trigger()) {
      reset();
      return;
    }

    for (uint8_t ch = 0; ch < 16; ch++) {
      _channels[ch].aftertouch = 0;
      _channels[ch].bar.playing.reset();

      for (uint8_t i = 0; i < V2Base::countof(_channels[0].notes); i++) {
        _channels[ch].notes[i].aftertouch = 0;
        _channels[ch].notes[i].velocity   = 0;
      }
    }

    for (uint8_t i = 0; i < V2Base::countof(_leds); i++)
      _leds[i] = {};

    _volume  = 100.f / 127.f;
    _cNotes  = config.cNotes;
    _rainbow = 0;
    LED.reset();
    LED.setHSV(V2Colour::Orange, 0.95, 0.25);

    LEDExt.reset();
    updateLEDs(true);
  }

  // Show individual notes.
  void updateLEDsNotes(bool force = false) {
    for (uint8_t i = 0; i < config.leds.count; i++) {
      uint8_t channel    = 0;
      uint8_t brightness = 0;

      // Find an active note in one of the 16 channels. The note in the
      // highest channel number wins.
      for (int8_t ch = 15; ch >= 0; ch--) {
        if (config.channels[ch].program != Configuration::Program::Notes)
          continue;

        if (i < config.channels[ch].start)
          continue;

        if (i >= config.channels[ch].start + config.channels[ch].count)
          continue;

        const uint8_t note = config.channels[ch].note + i - config.channels[ch].start;
        if (note > 127)
          continue;

        if (_channels[ch].notes[note].velocity == 0)
          continue;

        channel = ch;
        if (_channels[ch].notes[note].aftertouch > 0) {
          brightness = _channels[ch].notes[note].aftertouch;
        }

        else if (_channels[channel].aftertouch > 0) {
          brightness = _channels[channel].aftertouch;
        }

        else {
          brightness = _channels[ch].notes[note].velocity;
        }

        break;
      }

      if (brightness == 0 && _cNotes > 0.f) {
        uint8_t note = V2MIDI::A(-1) + i;
        uint8_t key  = note % 12;
        if (key == 0)
          brightness = _cNotes * 127.f;
      }

      if (!force && _leds[i].channel == channel && _leds[i].brightness == brightness)
        continue;

      _leds[i].channel    = channel;
      _leds[i].brightness = brightness;

      if (brightness == 0) {
        LEDExt.setBrightness(i, 0);
        continue;
      }

      const float h        = (float)config.channels[channel].color.h / 127.f * 360.f;
      const float s        = (float)config.channels[channel].color.s / 127.f;
      const float v        = (float)config.channels[channel].color.v / 127.f;
      const float fraction = (float)brightness / 127.f;
      const float adjusted = 0.1f + (0.9f * fraction);
      LEDExt.setHSV(i, h, s, _volume * adjusted * v);
    }
  }

  // Flash an LED bar, the length depending on the note velocity.
  void updateLEDsBar() {
    for (int8_t ch = 15; ch >= 0; ch--) {
      if (config.channels[ch].program != Configuration::Program::Bar)
        continue;

      uint8_t note;
      uint8_t velocity;
      if (!_channels[ch].bar.playing.getLast(note, velocity)) {
        if (!_channels[ch].bar.active)
          continue;

        for (uint8_t i = 0; i < config.channels[ch].count; i++)
          LEDExt.setBrightness(config.channels[ch].start + i, 0);

        _channels[ch].bar.active = false;
        continue;
      }

      const float h = (float)config.channels[ch].color.h / 127.f * 360.f;
      const float s = (float)config.channels[ch].color.s / 127.f;
      const float v = (float)config.channels[ch].color.v / 127.f;

      const float   fractionBrightness = (float)(_channels[ch].aftertouch > 0 ? _channels[ch].aftertouch : 127.f) / 127.f;
      const float   brightness         = _volume * fractionBrightness * v;
      const uint8_t count              = ceilf((float)config.channels[ch].count * ((float)velocity / 127.f));
      for (uint8_t i = 0; i < count; i++)
        LEDExt.setHSV(config.channels[ch].start + i, h, s, brightness);

      for (uint8_t i = count; i < config.channels[ch].count; i++)
        LEDExt.setBrightness(config.channels[ch].start + i, 0);

      _channels[ch].bar.active = true;
    }
  }

  void updateLEDs(bool force = false) {
    updateLEDsNotes(force);
    updateLEDsBar();
  }

  void handleNote(uint8_t channel, uint8_t note, uint8_t velocity) override {
    switch (config.channels[channel].program) {
      case Configuration::Program::Notes:
        _channels[channel].notes[note].velocity = velocity;
        if (velocity == 0) {
          _channels[channel].aftertouch             = 0;
          _channels[channel].notes[note].aftertouch = 0;
        }
        break;

      case Configuration::Program::Bar:
        _channels[channel].bar.playing.update(note, velocity);
        break;
    }

    updateLEDs();
    _timeoutUsec = V2Base::getUsec();
  }

  void handleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) override {
    handleNote(channel, note, 0);
    _timeoutUsec = V2Base::getUsec();
  }

  void handleAftertouch(uint8_t channel, uint8_t note, uint8_t pressure) override {
    _channels[channel].notes[note].aftertouch = pressure;

    updateLEDs();
    _timeoutUsec = V2Base::getUsec();
  }

  void handleAftertouchChannel(uint8_t channel, uint8_t pressure) override {
    _channels[channel].aftertouch = pressure;

    updateLEDs();
    _timeoutUsec = V2Base::getUsec();
  }

  void handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) override {
    switch (controller) {
      case (uint8_t)CC::Volume:
        _volume = (float)value / 127.f;
        if (_rainbow > 0.f)
          updateRainbow(_volume);

        else
          updateLEDs(true);
        break;

      case (uint8_t)CC::CNotes:
        _cNotes = (float)value / 127.f;
        updateLEDs(true);
        break;

      case (uint8_t)CC::Rainbow:
        _rainbow = (float)value / 127.f;
        if (_rainbow <= 0.f) {
          LEDExt.reset();
          updateLEDs(true);

        } else {
          updateRainbow(_volume);
        }
        break;

      case V2MIDI::CC::AllSoundOff:
      case V2MIDI::CC::AllNotesOff:
        allNotesOff();
        break;
    }

    _timeoutUsec = V2Base::getUsec();
  }

  void handleSystemExclusive(const uint8_t* buffer, uint32_t len) override {
    if (len < 10)
      return;

    // 0x7d == SysEx prototype/research/private ID
    if (buffer[1] != 0x7d)
      return;

    // Handle only JSON messages.
    if (buffer[2] != '{' || buffer[len - 2] != '}')
      return;

    // Read incoming message.
    JsonDocument json;
    if (deserializeJson(json, buffer + 2, len - 1))
      return;

    JsonObject jsonLed = json["led"];
    if (!jsonLed)
      return;

    JsonArray jsonColours = jsonLed["colors"];
    if (!jsonColours)
      return;

    const uint8_t start = jsonLed["start"];
    for (uint8_t i = 0; i + start < 128; i++) {
      JsonArray jsonColour = jsonColours[i];
      if (!jsonColour)
        break;

      // Empty array, skip LED.
      if (jsonColour[2].isNull())
        continue;

      uint8_t hue = jsonColour[0];
      if (hue > 127)
        hue = 127;

      uint8_t saturation = jsonColour[1];
      if (saturation > 127)
        saturation = 127;

      uint8_t brightness = jsonColour[2];
      if (brightness > 127)
        brightness = 127;

      LEDExt.setHSV(i + start, (float)hue / 127.f * 360.f, (float)saturation / 127.f, (float)brightness / 127.f);
    }

    _timeoutUsec = V2Base::getUsec();
  }

  void handleSystemReset() override {
    reset();
  }

  void exportInput(JsonObject json) override {
    JsonArray jsonChannels = json["channels"].to<JsonArray>();
    for (uint8_t ch = 0; ch < 16; ch++) {
      JsonObject jsonChannel = jsonChannels.add<JsonObject>();
      jsonChannel["number"]  = ch;
      jsonChannel["name"]    = config.channels[ch].name;

      JsonArray jsonControllers = jsonChannel["controllers"].to<JsonArray>();
      {
        JsonObject jsonController = jsonControllers.add<JsonObject>();
        jsonController["name"]    = "Volume";
        jsonController["number"]  = (uint8_t)CC::Volume;
        jsonController["value"]   = (uint8_t)(_volume * 127.f);
      }
      {
        JsonObject jsonController = jsonControllers.add<JsonObject>();
        jsonController["name"]    = "C Notes";
        jsonController["number"]  = (uint8_t)CC::CNotes;
        jsonController["value"]   = (uint8_t)(_cNotes * 127.f);
      }
      {
        JsonObject jsonController = jsonControllers.add<JsonObject>();
        jsonController["name"]    = "Rainbow";
        jsonController["number"]  = (uint8_t)CC::Rainbow;
        jsonController["value"]   = (uint8_t)(_rainbow * 127.f);
      }
      {
        JsonObject jsonAftertouch = jsonChannel["aftertouch"].to<JsonObject>();
        jsonAftertouch["value"]   = _channels[ch].aftertouch;
      }
      {
        JsonObject jsonChromatic = jsonChannel["chromatic"].to<JsonObject>();
        switch (config.channels[ch].program) {
          case Configuration::Program::Notes:
            jsonChromatic["start"] = config.channels[ch].note;
            jsonChromatic["count"] = config.channels[ch].count;
            break;

          case Configuration::Program::Bar:
            jsonChromatic["start"] = 0;
            jsonChromatic["count"] = 127;
            break;
        }
      }
    }
  }

  void exportSettings(JsonArray json) override {
    {
      JsonObject setting = json.add<JsonObject>();
      setting["type"]    = "number";
      setting["title"]   = "LED";
      setting["label"]   = "Count";
      setting["min"]     = 1;
      setting["max"]     = 128;
      setting["default"] = ConfigurationDefault.leds.count;
      setting["path"]    = "leds/count";
    }
    {
      JsonObject setting = json.add<JsonObject>();
      setting["type"]    = "toggle";
      setting["label"]   = "Reverse";
      setting["default"] = ConfigurationDefault.leds.reverse;
      setting["path"]    = "leds/reverse";
    }
    {
      JsonObject setting = json.add<JsonObject>();
      setting["type"]    = "number";
      setting["label"]   = "Power";
      setting["min"]     = 0;
      setting["max"]     = 1;
      setting["step"]    = 0.01;
      setting["default"] = ConfigurationDefault.leds.power;
      setting["path"]    = "leds/power";
    }
    {
      JsonObject setting = json.add<JsonObject>();
      setting["type"]    = "number";
      setting["title"]   = "Guide";
      setting["label"]   = "C Notes";
      setting["min"]     = 0;
      setting["max"]     = 1;
      setting["step"]    = 0.01;
      setting["default"] = ConfigurationDefault.cNotes;
      setting["path"]    = "cNotes";
    }

    for (uint8_t ch = 0; ch < 16; ch++) {
      {
        JsonObject setting = json.add<JsonObject>();
        setting["type"]    = "title";

        char name[64];
        sprintf(name, "Channel %d", ch + 1);
        setting["title"] = name;
      }
      {
        JsonObject setting = json.add<JsonObject>();
        setting["type"]    = "text";
        setting["label"]   = "Name";

        char path[64];
        sprintf(path, "channels[%d]/name", ch);
        setting["path"] = path;
      }
      {
        JsonObject setting = json.add<JsonObject>();
        setting["type"]    = "number";
        setting["label"]   = "Program";
        setting["max"]     = (uint8_t)Configuration::Program::_count - 1;
        setting["input"]   = "select";
        JsonArray names    = setting["names"].to<JsonArray>();
        for (uint8_t i = 0; i < (uint8_t)Configuration::Program::_count; i++)
          names.add(_programNames[i]);

        char path[64];
        sprintf(path, "channels[%d]/program", ch);
        setting["path"] = path;
      }
      {
        JsonObject setting = json.add<JsonObject>();
        setting["type"]    = "number";
        setting["label"]   = "LED";
        setting["text"]    = "Start";
        setting["min"]     = 1;
        setting["max"]     = 128;

        char path[64];
        sprintf(path, "channels[%d]/start", ch);
        setting["path"] = path;
      }
      {
        JsonObject setting = json.add<JsonObject>();
        setting["type"]    = "number";
        setting["label"]   = "LED";
        setting["text"]    = "Count";
        setting["max"]     = 128;

        char path[64];
        sprintf(path, "channels[%d]/count", ch);
        setting["path"] = path;
      }
      {
        JsonObject setting = json.add<JsonObject>();
        setting["type"]    = "note";
        setting["label"]   = "Note";

        setting["default"] = ConfigurationDefault.channels[ch].note;

        char path[64];
        sprintf(path, "channels[%d]/note", ch);
        setting["path"] = path;
      }
      {
        JsonObject setting = json.add<JsonObject>();
        setting["type"]    = "color";
        setting["ruler"]   = true;

        char path[64];
        sprintf(path, "channels[%d]/color", ch);
        setting["path"] = path;
      }
    }
  }

  void importConfiguration(JsonObject json) override {
    JsonObject jsonLeds = json["leds"];
    if (jsonLeds) {
      if (!jsonLeds["count"].isNull()) {
        uint8_t count = jsonLeds["count"];
        if (count > 128)
          count = 128;
        config.leds.count = count;
      }

      if (!jsonLeds["reverse"].isNull())
        config.leds.reverse = jsonLeds["reverse"];

      if (!jsonLeds["power"].isNull())
        config.leds.power = jsonLeds["power"];
    }

    if (!json["cNotes"].isNull()) {
      float cNotes = json["cNotes"];
      if (cNotes > 1.f) {
        cNotes = 1;
      }

      else if (cNotes < 0.f) {
        cNotes = 0;
      }

      config.cNotes = cNotes;
    }

    JsonArray jsonChannels = json["channels"];
    if (jsonChannels) {
      for (uint8_t ch = 0; ch < 16; ch++) {
        if (jsonChannels[ch].isNull())
          break;

        const char* name = jsonChannels[ch]["name"];
        if (name)
          strlcpy(config.channels[ch].name, name, sizeof(config.channels[ch].name));

        if (!jsonChannels[ch]["program"].isNull()) {
          uint16_t program = jsonChannels[ch]["program"];
          if (program > (uint8_t)Configuration::Program::_count - 1)
            program = (uint8_t)Configuration::Program::_count - 1;

          config.channels[ch].program = (Configuration::Program)program;
        }

        if (!jsonChannels[ch]["start"].isNull()) {
          uint16_t led = jsonChannels[ch]["start"];
          if (led < 1)
            led = 1;
          if (led > 128)
            led = 128;

          config.channels[ch].start = led - 1;
        }

        if (!jsonChannels[ch]["count"].isNull()) {
          uint16_t count = jsonChannels[ch]["count"];
          if (count > 128)
            count = 128;

          config.channels[ch].count = count;
        }

        if (!jsonChannels[ch]["note"].isNull()) {
          uint16_t note = jsonChannels[ch]["note"];
          if (note > 127)
            note = 127;

          config.channels[ch].note = note;
        }

        JsonArray jsonColour = jsonChannels[ch]["color"];
        if (jsonColour) {
          uint8_t color = jsonColour[0];
          if (color > 127)
            color = 127;
          config.channels[ch].color.h = color;

          uint8_t saturation = jsonColour[1];
          if (saturation > 127)
            saturation = 127;
          config.channels[ch].color.s = saturation;

          uint8_t brightness = jsonColour[2];
          if (brightness > 127)
            brightness = 127;
          config.channels[ch].color.v = brightness;
        }
      }
    }

    _cNotes = config.cNotes;
    LEDExt.setNumLEDs(config.leds.count);
    LEDExt.setDirection(config.leds.reverse);
    LEDExt.setMaxBrightness(config.leds.power);
    updateLEDs(true);
  }

  void exportConfiguration(JsonObject json) override {
    {
      json["#leds"]       = "The properties of the connected LEDs";
      JsonObject jsonLeds = json["leds"].to<JsonObject>();
      jsonLeds["#count"]  = "The number of LEDs to drive";
      jsonLeds["count"]   = config.leds.count;

      jsonLeds["#reverse"] = "The direction of the strip";
      jsonLeds["reverse"]  = config.leds.reverse;

      jsonLeds["#power"] = "The maximum brightness of the LEDs (0..1)";
      jsonLeds["power"]  = serialized(String(config.leds.power, 2));
    }

    json["#cNotes"] = "Mark the C notes with a blue dot (0..1)";
    json["cNotes"]  = serialized(String(config.cNotes, 2));

    json["#channels"]      = "The MIDI channels with different colors and zones";
    JsonArray jsonChannels = json["channels"].to<JsonArray>();
    for (uint8_t i = 0; i < 16; i++) {
      JsonObject jsonChannel = jsonChannels.add<JsonObject>();
      jsonChannel["name"]    = config.channels[i].name;

      if (i == 0)
        jsonChannel["#program"] = "The channel\'s program, individual notes or a bar";
      jsonChannel["program"] = (uint8_t)config.channels[i].program;

      if (i == 0)
        jsonChannel["#start"] = "The position of the first LED on the strip";
      jsonChannel["start"] = config.channels[i].start + 1;

      if (i == 0)
        jsonChannel["#count"] = "The number of leds to use";
      jsonChannel["count"] = config.channels[i].count;

      if (i == 0)
        jsonChannel["#note"] = "The first MIDI note to map";
      jsonChannel["note"] = config.channels[i].note;

      JsonArray jsonColour = jsonChannel["color"].to<JsonArray>();
      jsonColour.add(config.channels[i].color.h);
      jsonColour.add(config.channels[i].color.s);
      jsonColour.add(config.channels[i].color.v);
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
  const V2Buttons::Config _config{.clickUsec{200 * 1000}, .holdUsec{500 * 1000}};

  void handleClick(uint8_t count) override {
    Device.reset();
  }

  void handleHold(uint8_t count) override {
    LED.setHSV(V2Colour::Cyan, 0.8, 0.15);
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
