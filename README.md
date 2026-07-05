# esphome-solartemp
I have 3 heatpipe solar panel each with 30 tubes.

They are connected in series - but as the sun moves over the sky, some of them may get into the shadow on the late afternoon.

As the controller temp-sensor is only mounted on the last - it sometimes results in that the pump stops - and will not start again even if the two others still has sun on them and gets really hot.

To improve this - I have now put PT1000 sensors in each of the 3 modules, and hooked them up to a ESP 8266 board.

The ESP is programmed with [ESPhome](https://esphome.io/), where it makes the data available on the built in web-interface, exposing to Prometheus as well as publishing the metrics to an MQTT bus - that I then use to feed data to the Solar controller module.

I did not want a fully wired solution as that can be sensitive to lightning, so just feeding low voltage power to the waterproof box I have outside - and letting it use the WiFi for data transfer works good.

## Hardware
Getting reliable high-res temp data from the PT1000 sensors was not that easy, especially as I had 3 of them.

Ended up with using PT1000 converters which gave me a very precise output digital signal that was easy to hook up to the ESP.

PT1000 sensors with Silicon cable to handle the heat. Normal cables may melt as it can get up to several hundred degrees.

As my original idea was to use a multiplexer and some high-res A/D converter I used the 2-wire probes, but as I later went with the adapter-boards that supports 4-wire, that would have been a better option - as it gives even more precise values.

### Parts
* D1 Mini - ESP8266 12-F board
* Adafruit MAX31865 (PT1000 interface)
* PT1000 sensors with silicon cables (4-wire prefered)


### Wiring D1Mini
#### Breadboard Testing
![Testing](images/poc-breadboard.jpg)

#### Final setup where sensors are connected to a PCB using headers.
![Completed](images/final-pcb.jpg)

I have some more images and a wiring diagram as well - somewhere.
Ping me if you want it - and I'll dig it up.

## Installation
Clone the repository and create a companion `secrets.yaml` file in the root of the project with the following fields:
```
wifi_ssid: <your wifi SSID>
wifi_password: <your wifi password>
solartemp_password: <password used for OTA updates and the fallback AP>
```
The `solartemp_password` can be set to any value before the initial upload of the firmware.

Flash the firmware with ESPHome as usual:
```
esphome run solartemp.yaml
```
On the first flash connect the D1 Mini over USB; afterwards updates can be pushed Over-The-Air (OTA) over WiFi.

Once running, the device publishes its sensor values to MQTT and is picked up automatically in Home Assistant via MQTT discovery (under the `solarheating` prefix). The metrics are also available on the built-in web interface (port 80) and the Prometheus endpoint.

If you do not receive any data, verify the MQTT broker address in `solartemp.yaml`, confirm the device joined WiFi, and set the log level to `DEBUG` in `solartemp.yaml` for more feedback.

## Testing
Monitor what's being sent to the MQTT bus with the following command:
```mosquitto_sub -h 192.150.23.16 -p 1883  -t solarheating/+/# -v```

## Technical documentation
Specification overview:
* https://esphome.io/
* https://www.amazon.se/AZDelivery-D1-Mini-ESP8266-12F-WLAN-modul/dp/B0754W6Z2F?th=1

* https://www.adafruit.com/product/3648
