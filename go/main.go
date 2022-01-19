package main

import (
	"context"
	"log"
	"net"
	"os"
	"strconv"
	"sync"
	"time"

	influxdb2 "github.com/influxdata/influxdb-client-go/v2"
)

const (
	DATASIZE = 100
	BUCKET   = "dhtmon"
	ORG      = "home"
)

var resp = []byte("OK")

func envStr(name, def string) string {
	if v, ok := os.LookupEnv(name); ok {
		return v
	} else {
		return def
	}
}
func envInt(name string, def int) int {
	if v, ok := os.LookupEnv(name); ok {
		if x, err := strconv.Atoi(v); err != nil {
			return def
		} else {
			return x
		}
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

func main() {
	host := envStr("APP_HOST", "0.0.0.0")
	port := envInt("APP_PORT", 6543)
	db := envStr("INFLUX_DB", "http://localhost:8086")
	println("DB", db)
	token := envStr("INFLUX_TOKEN", "xyz")
	println("TOKEN", token)
	l, err := net.Listen("tcp", host+":"+strconv.Itoa(port))
	if err != nil {
		log.Fatal(err)
	}
	dp := sync.Pool{
		New: func() interface{} { return make([]byte, DATASIZE) },
	}
	log.Printf("Listening on %s:%d", host, port)
	client := influxdb2.NewClient(db, token)
	defer client.Close()
	if err := setup(client); err != nil {
		log.Fatal("can't init InfluxDB", err)
	}
	writeAPI := client.WriteAPIBlocking(ORG, BUCKET)
	for {
		s, err := l.Accept()
		if err != nil {
			log.Println(err)
			continue
		}
		r := dp.Get().([]byte)
		data, err := s.Read(r)
		if err != nil {
			s.Close()
			log.Println(err)
			continue
		}
		if data <= 5 {
			s.Close()
			continue
		}
		log.Printf("%d:%d:%d:%d", r[0], r[1], r[2], r[3])
		temp := float32(int16(r[0])|int16(r[1])<<8) / 10.0
		humid := float32(int16(r[2])|int16(r[3])<<8) / 10.0
		uuid := string(r[5 : 5+r[4]-1])
		log.Printf("[%s]: Temperature: %.2f C, humidity %.2f%%", uuid, temp, humid)
		data, err = s.Write(resp)
		if data != len(resp) {
			log.Printf("wrote %d bytes", data)
		}
		s.Close()
		p := influxdb2.NewPoint("temphumid", map[string]string{"sensor": uuid}, map[string]interface{}{"temp": temp, "humid": humid}, time.Now())
		go func() {
			dl, c := context.WithTimeout(context.Background(), 1*time.Second)
			defer c()
			if err := writeAPI.WritePoint(dl, p); err != nil {
				log.Printf("can't store data: %v", err)
			}
		}()
	}
}
