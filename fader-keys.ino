// © Kay Sievers <kay@versioduo.com>, 2021-2022
// SPDX-License-Identifier: Apache-2.0

#include <V2Base.h>
#include <V2Buttons.h>
#include <V2Color.h>
#include <V2Device.h>
#include <V2Display.h>
#include <V2LED.h>
#include <V2Link.h>
#include <V2MIDI.h>
#include <V2Mackie.h>

V2DEVICE_METADATA("com.versioduo.fader-keys", 12, "versioduo:samd:key-8");

static constexpr uint8_t n_keys = 8;
static V2LED::WS2812 LED(n_keys, PIN_LED_WS2812, &sercom2, SPI_PAD_0_SCK_1, PIO_SERCOM);

static V2Link::Port Plug(&SerialPlug);
static V2Link::Port Socket(&SerialSocket);

static V2Base::Timer::PWM PWM(V2Base::Timer::PWM::getId(PIN_DISPLAY_BACKLIGHT), 20000);
static V2Display::ST7789 Display(135,
                                 240,
                                 true,
                                 PIN_DISPLAY_DATA,
                                 PIN_DISPLAY_CLOCK,
                                 &sercom1,
                                 SPI_PAD_0_SCK_1,
                                 PIO_SERCOM,
                                 PIN_DISPLAY_SELECT,
                                 PIN_DISPLAY_COMMAND,
                                 PIN_DISPLAY_RESET);

// Circular dependency, provide the pointer to the base-class object to send messages to.
static V2Mackie *Mackiep{};

static class Device : public V2Device {
public:
  Device() : V2Device() {
    metadata.vendor      = "Versio Duo";
    metadata.product     = "fader-keys";
    metadata.description = "Fader Keys Module with Display";
    metadata.home        = "https://versioduo.com/#fader-keys";

    system.download  = "https://versioduo.com/download";
    system.configure = "https://versioduo.com/configure";

    // https://github.com/versioduo/arduino-board-package/blob/main/boards.txt
    usb.pid = 0xe990;
  }

  void setBrightness(float fraction = -1) {
    if (fraction < 0.f)
      fraction = _brightness;

    PWM.setDuty(PIN_DISPLAY_BACKLIGHT, powf(fraction, 2));
  }

  void splash() {
    Display.reset(0, V2Display::Black);
    Display.setArea(0, 1, 135, V2Display::Center, V2Display::White, V2Display::Black);
    Display.print("V2");
    setBrightness(_brightness * 0.25f);
  }

private:
  enum class CC {
    Light   = V2MIDI::CC::Controller89,
    Rainbow = V2MIDI::CC::Controller90,
  };

  float _brightness{};
  float _light_max{100.f / 127.f};
  float _rainbow{};

  struct {
    bool on;
  } _keys[n_keys]{};
  V2MIDI::Packet _midi{};

  void handleReset() override {
    LED.reset();

    _brightness = 0.5;
    _light_max  = 100.f / 127.f;
    _rainbow    = 0;
    setBrightness();
    splash();

    for (uint8_t i = 0; i < n_keys; i++)
      _keys[i] = {};

    for (uint8_t i = 0; i < 8; i++) {
      _midi.setPort(i);
      _midi.set(0, V2MIDI::Packet::Status::SystemReset);
      Socket.send(&_midi);
    }
  }

  void allNotesOff() {
    reset();
  }

  bool handleSend(V2MIDI::Packet *midi) override {
    usb.midi.send(midi);
    Plug.send(midi);
    return true;
  }

  void handlePacket(V2MIDI::Packet *packet) override {
    Mackiep->dispatchPacket(packet);
  }

  void handleSystemExclusive(const uint8_t *buffer, uint32_t len) override {
    Mackiep->dispatchSystemExclusive(buffer, len);
  }

  void handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) override {
    switch (controller) {
      case V2MIDI::CC::Expression:
        _brightness = (float)value / 127;
        setBrightness(_brightness);
        break;

      case (uint8_t)CC::Light:
        _light_max = (float)value / 127.f;
        if (_rainbow > 0.f)
          LED.rainbow(1, 4.5f - (_rainbow * 4.f), _light_max, true);
        break;

      case (uint8_t)CC::Rainbow:
        _rainbow = (float)value / 127.f;
        if (_rainbow <= 0.f)
          LED.reset();
        else
          LED.rainbow(1, 4.5f - (_rainbow * 4.f), _light_max, true);
        break;

      case V2MIDI::CC::AllSoundOff:
      case V2MIDI::CC::AllNotesOff:
        allNotesOff();
        return;
    }
  }

  void handleSystemReset() override {
    Mackiep->reset();
    reset();
  }

  void exportInput(JsonObject json) override {
    JsonArray json_controllers = json.createNestedArray("controllers");
    {
      JsonObject json_controller = json_controllers.createNestedObject();
      json_controller["name"]    = "Display";
      json_controller["number"]  = V2MIDI::CC::Expression;
      json_controller["value"]   = (uint8_t)(_brightness * 127.f);
    }
    {
      JsonObject json_controller = json_controllers.createNestedObject();
      json_controller["name"]    = "Buttons";
      json_controller["number"]  = (uint8_t)CC::Light;
      json_controller["value"]   = (uint8_t)(_light_max * 127.f);
    }
    {
      JsonObject json_controller = json_controllers.createNestedObject();
      json_controller["name"]    = "Rainbow";
      json_controller["number"]  = (uint8_t)CC::Rainbow;
    }
  }
} Device;

static class Fader : public V2MIDI::Port {
public:
  Fader(uint8_t index) : V2MIDI::Port(index, 16), _index(index) {}

  void forwardHost(V2MIDI::Packet *midi) {
    if (!V2Mackie::setStripIndex(midi, _index))
      return;

    Device.send(midi);
  }

  void sendDisplay(uint8_t row, const char *text) {
    uint8_t *buffer   = getSystemExclusiveBuffer();
    const uint8_t len = V2Mackie::setStripText(buffer, 0, row, text);
    sendSystemExclusive(NULL, len);
  }

private:
  uint8_t _index;

  bool handleSend(V2MIDI::Packet *midi) override {
    return Socket.send(midi);
  }

} Faders[8] = {
  Fader(0),
  Fader(1),
  Fader(2),
  Fader(3),
  Fader(4),
  Fader(5),
  Fader(6),
  Fader(7),
};

static class Mackie : public V2Mackie {
private:
  V2MIDI::Packet _midi{};

  void handleStripDisplay(bool global, uint8_t strip, uint8_t row) override {
    if (global)
      return;

    char text[8];
    getStripDisplay(strip, row, text);
    Faders[strip].sendDisplay(row, text);
  }

  void handleStripVPotDisplay(uint8_t strip, uint8_t value) override {
    Faders[strip].send(V2Mackie::setStripVPotDisplay(&_midi, 0, value));
  }

  void handleStripMeter(uint8_t strip, float fraction, bool overload) override {
    Faders[strip].send(V2Mackie::setStripMeter(&_midi, 0, fraction));
  }

  void handleStripMeterOverload(uint8_t strip, bool overload) override {
    Faders[strip].send(V2Mackie::setStripMeterOverload(&_midi, 0, overload));
  }

  void handleStripButton(uint8_t strip, V2Mackie::StripButton button, bool on) override {
    Faders[strip].send(V2Mackie::setStripButton(&_midi, 0, button, on));
  }

  void handleStripFader(uint8_t strip, float fraction) override {
    Faders[strip].send(V2Mackie::setStripFader(&_midi, 0, fraction));
  }

  void handleTimeout() override {
    Device.reset();
  }
} Mackie;

static class Button : public V2Buttons::Button {
public:
  Button(uint8_t function) : V2Buttons::Button(&_config, PIN_KEY + function), _function(function) {}
  enum Function {
    PortDown,
    PortUp,
    BankDown,
    BankUp,
    ChannelDown,
    ChannelUp,
    RowUp,
    RowDown,
    _count,
  };

private:
  const V2Buttons::Config _config{.click_usec{150 * 1000}, .hold_usec{350 * 1000}};
  uint8_t _function;
  V2MIDI::Packet _midi{};

  void set(bool on) {
    switch (_function) {
      case PortDown:
        Device.send(V2Mackie::setStripButton(&_midi, 0, V2Mackie::StripButton::Select, on));
        break;

      case PortUp:
        Device.send(V2Mackie::setStripButton(&_midi, 0, V2Mackie::StripButton::Arm, on));
        break;

      case BankDown:
        Device.send(V2Mackie::setBankButton(&_midi, V2Mackie::BankButton::Previous, on));
        break;

      case BankUp:
        Device.send(V2Mackie::setBankButton(&_midi, V2Mackie::BankButton::Next, on));
        break;

      case ChannelDown:
        Device.send(V2Mackie::setBankButton(&_midi, V2Mackie::BankButton::PreviousChannel, on));
        break;

      case ChannelUp:
        Device.send(V2Mackie::setBankButton(&_midi, V2Mackie::BankButton::NextChannel, on));
        break;

      case RowUp:
        Device.send(V2Mackie::setNavigationButton(&_midi, V2Mackie::NavigationButton::Up, on));
        break;

      case RowDown:
        Device.send(V2Mackie::setNavigationButton(&_midi, V2Mackie::NavigationButton::Down, on));
        break;
    }
  }

  void handleDown() override {
    set(true);
  }

  void handleUp() override {
    set(false);
  }
} Buttons[Button::_count]{
  Button(Button::PortDown),
  Button(Button::PortUp),
  Button(Button::BankDown),
  Button(Button::BankUp),
  Button(Button::ChannelDown),
  Button(Button::ChannelUp),
  Button(Button::RowUp),
  Button(Button::RowDown),
};

// Dispatch MIDI packets
static class MIDI {
public:
  void loop() {
    if (!Device.usb.midi.receive(&_midi))
      return;

    if (_midi.getPort() == 0) {
      Device.dispatch(&Device.usb.midi, &_midi);

    } else {
      _midi.setPort(_midi.getPort() - 1);
      Socket.send(&_midi);
    }
  }

private:
  V2MIDI::Packet _midi{};
} MIDI;

// Dispatch Link packets
static class Link : public V2Link {
public:
  Link() : V2Link(&Plug, &Socket) {}

private:
  V2MIDI::Packet _midi{};

  // Receive a host event from our parent device
  void receivePlug(V2Link::Packet *packet) override {
    if (packet->getType() == V2Link::Packet::Type::MIDI) {
      packet->receive(&_midi);
      Device.dispatch(&Plug, &_midi);
    }
  }

  // Forward children device events to the host
  void receiveSocket(V2Link::Packet *packet) override {
    if (packet->getType() == V2Link::Packet::Type::MIDI) {
      uint8_t address = packet->getAddress();

      if (Device.usb.ports.current == 1) {
        if (address > 7)
          return;

        packet->receive(&_midi);
        Faders[address].forwardHost(&_midi);
        return;
      }

      if (address == 0x0f)
        return;

      if (Device.usb.midi.connected()) {
        packet->receive(&_midi);
        _midi.setPort(address + 1);
        Device.usb.midi.send(&_midi);
      }
    }
  }
} Link;

void setup() {
  Serial.begin(9600);

  LED.begin();
  LED.setMaxBrightness(0.75);

  Plug.begin();
  Socket.begin();
  Device.link = &Link;

  // Set the SERCOM interrupt priority, it requires a stable ~300 kHz interrupt
  // frequency. This needs to be after begin().
  setSerialPriority(&SerialPlug, 2);
  setSerialPriority(&SerialSocket, 2);

  Device.begin();
  for (uint8_t i = 0; i < n_keys; i++)
    Buttons[i].begin();

  PWM.begin();
  V2Base::Timer::PWM::setupPin(PIN_DISPLAY_BACKLIGHT);

  Display.begin();
  Device.reset();
  Mackie.begin();
  Mackiep = &Mackie;
  for (uint8_t i = 0; i < 8; i++)
    Faders[i].begin();
}

void loop() {
  LED.loop();
  MIDI.loop();
  Link.loop();
  V2Buttons::loop();
  Display.loop();
  Device.loop();
  Mackie.loop();
  for (uint8_t i = 0; i < 8; i++)
    Faders[i].loopSystemExclusive();

  if (Device.idle())
    Device.sleep();
}
