package main

import (
	"context"
	"log"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	MQTT "github.com/eclipse/paho.mqtt.golang"
	influxdb2 "github.com/influxdata/influxdb-client-go/v2"
)

const (
	BUCKET = "dhtmon"
	ORG    = "home"
)

var resp = []byte("OK")

func envStr(name, def string) string {
	if v, ok := os.LookupEnv(name); ok {
		return v
	} else {
		return def
	}
}
func setup(client influxdb2.Client) error {
	ctx, c := context.WithTimeout(context.TODO(), 5*time.Second)
	defer c()
	orgApi := client.OrganizationsAPI()
	org, err := orgApi.FindOrganizationByName(ctx, ORG)
	if err != nil || org == nil {
		if org, err = orgApi.CreateOrganizationWithName(ctx, ORG); err != nil {
			return err
		}
	}
	bApi := client.BucketsAPI()
	b, err := bApi.FindBucketByName(ctx, BUCKET)
	if err != nil || b == nil {
		if b, err = bApi.CreateBucketWithName(ctx, org, BUCKET); err != nil {
			return err
		}
	}
	return nil
}

func extractTopicId(t string) string {
	// topic is of format: /home/sensor/masterbed/xxx/yyy
	if e := strings.LastIndex(t, "/"); e > 0 {
		if s := strings.LastIndex(t[:e-1], "/"); s > -1 {
			return t[s+1 : e]
		}
	}
	return ""
}

func main() {
	c := make(chan os.Signal, 1)
	signal.Notify(c, os.Interrupt, syscall.SIGTERM, syscall.SIGINT, syscall.SIGKILL)
	broker := envStr("APP_MQTT_BROKER", "mqtt://10.0.0.61:1883")
	opts := MQTT.NewClientOptions()
	opts.AddBroker(broker)
	db := envStr("INFLUX_DB", "http://localhost:8086")
	println("DB", db)
	token := envStr("INFLUX_TOKEN", "xyz")
	println("TOKEN", token)
	client := influxdb2.NewClient(db, token)
	defer client.Close()
	if err := setup(client); err != nil {
		log.Fatal("can't init InfluxDB", err)
	}
	writeAPI := client.WriteAPIBlocking(ORG, BUCKET)
	var onReceive = func(metric string) func(MQTT.Client, MQTT.Message) {
		return func(client MQTT.Client, msg MQTT.Message) {
			if uuid := extractTopicId(msg.Topic()); uuid != "" {
				pld := string(msg.Payload())
				v, err := strconv.ParseFloat(pld, 32)
				if err != nil {
					log.Println("can't parse ", pld, err)
					return
				}
				p := influxdb2.NewPoint("temphumid", map[string]string{"sensor": uuid}, map[string]interface{}{metric: v}, time.Now())
				go func() {
					dl, c := context.WithTimeout(context.Background(), 1*time.Second)
					defer c()
					if err := writeAPI.WritePoint(dl, p); err != nil {
						log.Printf("can't store data: %v", err)
					}
				}()
			} else {
				log.Println("can't extract topic id from ", msg.Topic())
			}
		}
	}
	opts.OnConnect = func(c MQTT.Client) {
		if token := c.Subscribe("/home/sensor/+/temp", 1, onReceive("temp")); token.Wait() && token.Error() != nil {
			panic(token.Error())
		}
		if token := c.Subscribe("/home/sensor/+/humid", 1, onReceive("humid")); token.Wait() && token.Error() != nil {
			panic(token.Error())
		}
	}
	mqttClient := MQTT.NewClient(opts)
	if token := mqttClient.Connect(); token.Wait() && token.Error() != nil {
		log.Fatal(token.Error())
	}
	defer mqttClient.Disconnect(0)
	log.Println("started processing topics")
	<-c
}
