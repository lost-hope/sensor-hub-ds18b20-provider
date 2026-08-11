# DS18B20 Sensor Provider

A [Sensor Hub](../sensor-hub/readme.md) provider usermod for the Dallas/
Maxim DS18B20 - registers `ds18b20_temperature` with the hub by default,
which then handles MQTT, Home Assistant discovery, the JSON API and the
Info tab. Only the first device found on the bus is read (multi-drop buses
with several DS18B20s are not supported).

## Hardware

The DS18B20 connects via a single OneWire data pin (with a 4.7kΩ pull-up
to 3.3V), not the shared I2C bus. Set the **Pin** in this usermod's own
Settings page - it is reserved through WLED's PinManager so it won't
silently clash with LEDs, relays or other usermods.

The temperature conversion (~750ms at the default 12-bit resolution) is
done non-blocking - `loop()` starts it and only reads the result once the
time has elapsed, so WLED's main loop is never stalled waiting on the
sensor.

## Usage

Self-contained out-of-tree usermod (see `library.json` for its
`paulstoffregen/OneWire` and `milesburton/DallasTemperature`
dependencies). Add it to `custom_usermods` next to the
[Sensor Hub](../sensor-hub/readme.md) itself.

## Usermod Settings

| Setting | Default | Description |
|---|---|---|
| Enabled | on | Master on/off switch (also auto-disabled until a pin is set) |
| Pin | unset | OneWire data pin the DS18B20 is wired to |
| Check interval | 30s | How often a new read cycle is started |
| Name prefix | `ds18b20` | Sensor name becomes `<prefix>_temperature` - must be unique across every provider registered with the hub |
| Precision | 1 | Decimal places published |
