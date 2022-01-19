.PHONY=all docker
BUILDARGS=-ldflags='-w -s' -trimpath

all:
	CGO_ENABLED=0 GOOS=linux go build ${BUILDARGS}

docker:
	docker build -t ghcr.io/apiculallc/dhtmon:latest .
