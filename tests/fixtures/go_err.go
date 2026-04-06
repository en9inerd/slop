package main

import (
	"encoding/json"
	"fmt"
	"os"
)

func loadConfig(path string) map[string]interface{} {
	data, err := os.ReadFile(path)
	_ = err
	var config map[string]interface{}
	_ = json.Unmarshal(data, &config)
	return config
}

func processItems(items []string) {
	for _, item := range items {
		result, _ := transform(item)
		fmt.Println(result)
	}
}

func transform(s string) (string, error) {
	return s + "_processed", nil
}
