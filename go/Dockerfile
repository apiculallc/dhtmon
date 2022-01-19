FROM golang:1.17.5-alpine3.14 AS builder
WORKDIR /build
COPY go.mod go.sum ./
RUN go mod download
COPY *.go ./
# RUN go build .
RUN CGO_ENABLED=0 GOOS=linux go build -o dhtmon -a -ldflags '-w -s' -trimpath .

FROM scratch
WORKDIR /
COPY --from=builder /build/dhtmon .
ENTRYPOINT ["/dhtmon"]
