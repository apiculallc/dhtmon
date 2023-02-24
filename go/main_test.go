package main

import (
	"github.com/stretchr/testify/assert"
	"testing"
)

func TestParseTopic(t *testing.T) {
	for _, td := range [][]string{{"/home/sensor/bed/temp", "bed"}, {"/home/sensor/x", "sensor"}, {"/home", ""}} {
		t.Run("should match "+td[0]+" with "+td[1], func(t *testing.T) {
			assert.Equal(t, td[1], extractTopicId(td[0]))
		})
	}
}
