## Objective

To create a small, contained and "offline" temperature / humidity monitoring system for hard-to-reach places like attics, garages, crawlspaces, sheds etc.

## Requirements

- GNU make
- GCC
- [ESP-IDF toolkit](https://docs.espressif.com/projects/esp8266-rtos-sdk/en/latest/)
- Docker

## Build

Clone the repository as `git clone --recurse-submodules -j8 git://github.com/apiculallc/dht.git`

### ESP8266 firmware

- follow the instructions to setup the [ESP-IDF toolchain](https://docs.espressif.com/projects/esp8266-rtos-sdk/en/latest/get-started/index.html#setup-toolchain)
- edit `nvs.csv` file to specify the location and name of the sensor. You can skip this step and configure everything using WiFi setup
  - `wifi_ssid` - name of the access point to connect to
  - `wifi_password` - WiFi password 
  - `app_location` - sensor location ( attic, garage etc )
  - `app_mqtt_url` - the URL to MQTT broker - this is typically the same location as where you will have InfluxDB and the Hub running
- run `make all` to build the flash image
- run `make flash` to flash the partition table
- run `make nvs` to flash the NVS table with the probe settings

### Hub
- change directory to `go`
- run `make` to build the executable for your platform - you'll need to have [Golang](https://go.dev/) installed
- run `make docker` to build a docker image

### Deployment

With use of Docker compose everything shall be straightforward:

```bash
docker-compose up -d
```

That should create all the necessary pieces
#### Start Mosquitto

Create a config file `mosquitto.conf` with the content:

```
persistence false

log_dest stdout
log_type warning
log_timestamp true
connection_messages true

listener 1883

## Authentication ##
allow_anonymous true

```
 Then run Mosquitto in Docker as:
```bash
docker run --name mosquitto \
 -d --restart=unless-stopped \ 
 -p 1883:1883 \
 -v `pwd`/mosquitto.conf:/mosquitto/config/mosquitto.conf \
eclipse-mosquitto
```
This will expose Mosquitto on port 1883 and allow anonymous access.
#### Start InfluxDB

Run in Docker as:

```bash
docker run -d -p 8086:8086 \
  --name=dhtmon-influx \
  --restart=unless-stopped \
  -v "/opt/data/influxdb-dhtmon:/var/lib/influxdb2" \
  -v "/opt/config/influxdb-dhtmon:/etc/influxdb2" \
  -e DOCKER_INFLUXDB_INIT_MODE=setup \
  -e DOCKER_INFLUXDB_INIT_USERNAME=admin \
  -e DOCKER_INFLUXDB_INIT_PASSWORD=admin123456 \
  -e DOCKER_INFLUXDB_INIT_ORG=home \
  -e DOCKER_INFLUXDB_INIT_BUCKET=dhtmon \
  -e `DOCKER_INFLUXDB_INIT_ADMIN_TOKEN=token123456 \
  influxdb:2.7-alpine
```

This will provision the instance of InfluxDB locally that exposes port 8086 if you need to check on the connection string.

#### Start Hub

In order to test that the Hub works well, you may use the following command ( assuming you have Golang installed ):
```
APP_MQTT_BROKER=mqtt://127.0.0.1:1883 \
INFLUX_DB=http://127.0.0.1:8086 \
INFLUX_TOKEN=token123456 \
go run main.go
```
This will build the project and start the service. Normal output should look like:
```
DB http://127.0.0.1:8086
TOKEN token123456
2024/07/03 10:11:12 started processing topics
```

Now the system is ready to receive data from the sensors.

Alternatively, use Docker container that you built as:

```
docker run --rm \
  -it \
  -e APP_MQTT_BROKER=mqtt://mosquitto:1883 \
  -e INFLUX_DB=http://dhtmon-influx:8086 \
  -e INFLUX_TOKEN=token123456 \
  --link dhtmon-influx \
  --link mosquitto \
  ghcr.io/apiculallc/dhtmon:latest  

DB http://dhtmon-influx:8086
TOKEN token123456
2024/07/04 10:11:12 started processing topics
```

If the Hub is running without any error, you can automate the process with

```bash
docker run --restart=unless-stopped \
  -it \
  -e APP_MQTT_BROKER=mqtt://mosquitto:1883 \
  -e INFLUX_DB=http://dhtmon-influx:8086 \
  -e INFLUX_TOKEN=token123456 \
  --link dhtmon-influx \
  --link mosquitto \
  ghcr.io/apiculallc/dhtmon:latest  
```
This will start the hub in daemon mode.
### Start sensor

Just plug the sensor - it shall connect to the WiFi access point and then start sending data to the topics. 

## Post-installation steps

There is no LCD screens or anything that can be easily observed by a human being, so it is hard to tell whether the system works as expected. The joy of system level programming! So we shall be creative about our means to verify the functionality.

The easiest way to test that everything works as expected is to use an MQTT client and subscribe to the topics. For example, using [`mqtt-shell`](https://github.com/rainu/mqtt-shell):
```bash
mqtt-shell -b mqtt://127.0.0.1:1883
>> sub /home/sensor/#
/home/sensor/attic/temp | 29.3
/home/sensor/attic/humid | 55.9
/home/sensor/garage/temp | 27.1
/home/sensor/garage/humid | 56.1
```

This indicates that sensors actually transmits data.

To verify that we actually receive something in the database ( InfluxDB ) we need to connect to the DB and run a few queries. As we have the installation that exposes ports 8086 - we can simply open [http://127.0.0.1:8086](http://127.0.0.1:8086) and use login  `admin` and password `admin123456` to peek into the storage.  Choose `Buckets` and select `dhtmon`:
![[explorer.png]]

From here, you may want to configure Grafana.

#### [Grafana](https://grafana.com/)

This is fairly sophisticated and powerful tool for monitoring and alerting in DevOps world. We're going to use it to render the charts and get the most recent data from the set of probes that we setup.

###### Installation

Pull the docker image and link it to a container that runs InfluxDB.

```bash
docker run --restart=unless-stopped \
	-e GF_SECURITY_ADMIN_USER=admin \
	-e GF_SECURITY_ADMIN_PASSWORD=admin123 \
	--link influxdb \
	-p 3000:3000 \
	-v ./grafana/provisioning/:/etc/grafana/provisioning/ \
	-v ./grafana/dashboards/:/var/lib/grafana/dashboards/ \
	grafana/grafana:10.0.10
```

This will populate the default data source with the InfluxDB and then create a dashboard that shall look like following ( if you have some data in there from the sensors ).

To "prime" some data and verify that this part works fine, you can use the following command:

```bash
cat <<EOF | mqtt-shell -b mqtt://127.0.0.1:1883
pub /home/sensor/garage/temp 32.1
pub /home/sensor/garage/humid 55.0
pub /home/sensor/attic/humid 35.0
pub /home/sensor/attic/temp 40.0
pub /home/sensor/crawlspace/temp 27.0
pub /home/sensor/crawlspace/humid 65.0
EOF
Successfully connected to mqtt broker.
```

The result can look like:

![[dashboard.png]]
After you run that for some time, the result will be much more interesting:

![[dashboard-30days.png]]
