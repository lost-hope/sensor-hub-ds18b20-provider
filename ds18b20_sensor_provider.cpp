#include "wled.h"
#include "sensor_bus.h"
#include <OneWire.h>
#include <DallasTemperature.h>

/*
 * DS18B20 temperature sensor provider.
 *
 * Reads a Dallas/Maxim DS18B20 on a single digital OneWire GPIO pin and
 * pushes the reading into the Sensor Hub (see
 * ../sensor-hub/usermod_sensor_hub.cpp and ../sensor-hub/sensor_bus.h) as
 * "<prefix>_temperature". Only the first device found on the bus is read
 * (multi-drop DS18B20 buses with several sensors are not supported here).
 * This usermod never talks to MQTT, the JSON API or the Info tab itself -
 * the hub takes care of all of that once a sensor is registered here.
 *
 * The OneWire pin is not a shared WLED global - it is configured (and
 * reserved via WLED's PinManager, to avoid clashing with LEDs/relays/other
 * usermods) right here in this usermod's own settings.
 *
 * The conversion is done non-blocking (DallasTemperature::
 * setWaitForConversion(false)): requestTemperatures() kicks off a
 * ~750ms(12-bit)/94ms(9-bit) conversion in the background and loop() only
 * reads the result once that time has elapsed, so WLED's main loop is
 * never blocked waiting on the sensor.
 */
class DS18B20SensorUsermod : public Usermod {
  private:
    OneWire* oneWire = nullptr;
    DallasTemperature* sensors = nullptr;
    SensorHub* hub = nullptr;
    uint8_t tempHandle = SENSOR_HANDLE_INVALID;

    bool enabled = true;
    bool sensorFound = false;
    bool initDone = false;
    bool conversionPending = false;

    unsigned long lastCycleStart = 0;
    unsigned long conversionStarted = 0;
    unsigned long lastBeginAttempt = 0;
    uint8_t consecutiveFailures = 0;

    // config
    int8_t pin = -1;                // OneWire data pin, unset by default
    uint16_t checkIntervalS = 30;   // how often to start a new read cycle
    String namePrefix = "ds18b20";  // sensor name becomes "<prefix>_temperature"
    uint8_t precision = 1;          // decimal places published
    uint8_t priority = 100;         // getValue() selection priority - lower wins among sensors of the same SensorType (see sensor_bus.h)

    static const char _name[];
    static const char _enabled[];
    static const char _pin[];
    static const char _checkInterval[];
    static const char _namePrefix[];
    static const char _precision[];
    static const char _priority[];

    void registerSensors() {
      if (!hub || tempHandle != SENSOR_HANDLE_INVALID) return; // already registered
      tempHandle = hub->registerSensor((namePrefix + "_temperature").c_str(), SensorType::Temperature, nullptr, nullptr, precision, priority);
    }

    bool beginSensor() {
      if (!sensors) return false;
      sensors->begin();
      sensors->setWaitForConversion(false); // non-blocking - we poll for the result ourselves
      return sensors->getDeviceCount() > 0;
    }

  public:
    void setup() override {
      // Neither branch touches 'enabled' (the user's own on/off switch,
      // persisted to config) - initDone (left false here) is what actually
      // gates loop(), so a later pin fix takes effect on the next boot
      // instead of staying stuck disabled.
      if (pin < 0) return;
      if (!PinManager::allocatePin(pin, true, PinOwner::UM_Unspecified)) {
        pin = -1; // conflicts with another pin owner - force reconfiguration
        return;
      }
      oneWire = new OneWire(pin);
      sensors = new DallasTemperature(oneWire);
      sensorFound = beginSensor();
      initDone = true;
    }

    void loop() override {
      if (!enabled || !initDone || !sensors) return;

      if (!hub) hub = getSensorHub(); // Sensor Hub usermod may finish init after us
      if (hub) registerSensors();

      unsigned long now = millis();

      if (!sensorFound) {
        if (now - lastBeginAttempt < 10000) return;
        lastBeginAttempt = now;
        sensorFound = beginSensor();
        if (!sensorFound) return;
      }

      if (!conversionPending) {
        if (now - lastCycleStart < (unsigned long)checkIntervalS * 1000UL) return;
        lastCycleStart = now;
        sensors->requestTemperatures();
        conversionStarted = now;
        conversionPending = true;
        return;
      }

      // 12-bit (default) conversion takes up to 750ms - give it a comfortable margin.
      if (now - conversionStarted < 800) return;
      conversionPending = false;

      float t = sensors->getTempCByIndex(0);
      if (t == DEVICE_DISCONNECTED_C) {
        consecutiveFailures++;
        if (hub && tempHandle != SENSOR_HANDLE_INVALID && consecutiveFailures >= 3) hub->setSensorAvailable(tempHandle, false);
        if (consecutiveFailures >= 10) sensorFound = false; // force a fresh begin() next loop
        return;
      }

      consecutiveFailures = 0;
      if (hub && tempHandle != SENSOR_HANDLE_INVALID) {
        hub->setSensorAvailable(tempHandle, true);
        hub->updateSensor(tempHandle, t);
      }
    }

    void addToConfig(JsonObject& root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)] = enabled;
      top[FPSTR(_pin)] = pin;
      top[FPSTR(_checkInterval)] = checkIntervalS;
      top[FPSTR(_namePrefix)] = namePrefix;
      top[FPSTR(_precision)] = precision;
      top[FPSTR(_priority)] = priority;
    }

    bool readFromConfig(JsonObject& root) override {
      int8_t oldPin = pin;

      JsonObject top = root[FPSTR(_name)];
      bool configComplete = !top.isNull();
      configComplete &= getJsonValue(top[FPSTR(_enabled)], enabled);
      configComplete &= getJsonValue(top[FPSTR(_pin)], pin);
      configComplete &= getJsonValue(top[FPSTR(_checkInterval)], checkIntervalS);
      configComplete &= getJsonValue(top[FPSTR(_namePrefix)], namePrefix);
      configComplete &= getJsonValue(top[FPSTR(_precision)], precision);
      configComplete &= getJsonValue(top[FPSTR(_priority)], priority);

      if (initDone && pin != oldPin) {
        // pin changed at runtime via the Settings UI - release the old one and re-init on the new one
        if (oldPin >= 0) PinManager::deallocatePin(oldPin, PinOwner::UM_Unspecified);
        delete sensors;
        delete oneWire;
        sensors = nullptr;
        oneWire = nullptr;
        initDone = false;
        conversionPending = false;
        setup();
      }
      return configComplete;
    }

    void appendConfigData(Print& settingsScript) override {
      settingsScript.print(F("addInfo('DS18B20Sensor:pin',1,'OneWire data pin');"));
      settingsScript.print(F("addInfo('DS18B20Sensor:checkInterval',1,'seconds between read cycles');"));
      settingsScript.print(F("addInfo('DS18B20Sensor:namePrefix',1,'sensor name becomes &lt;prefix&gt;_temperature - must be unique across all sensor providers');"));
      settingsScript.print(F("addInfo('DS18B20Sensor:precision',1,'decimal places published');"));
      settingsScript.print(F("addInfo('DS18B20Sensor:priority',1,'getValue() selection priority - lower wins if another provider also registers a Temperature sensor');"));
    }
};

const char DS18B20SensorUsermod::_name[]          PROGMEM = "DS18B20Sensor";
const char DS18B20SensorUsermod::_enabled[]       PROGMEM = "enabled";
const char DS18B20SensorUsermod::_pin[]           PROGMEM = "pin";
const char DS18B20SensorUsermod::_checkInterval[] PROGMEM = "checkInterval";
const char DS18B20SensorUsermod::_namePrefix[]    PROGMEM = "namePrefix";
const char DS18B20SensorUsermod::_precision[]     PROGMEM = "precision";
const char DS18B20SensorUsermod::_priority[]      PROGMEM = "priority";

static DS18B20SensorUsermod ds18b20_sensor;
REGISTER_USERMOD(ds18b20_sensor);
