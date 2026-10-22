package zsharp

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"os/exec"
	"reflect"
	"strings"
	"testing"
	"time"
)

type fixtureCounter struct {
	Value  int
	Label  string
	hidden int
}

func (counter *fixtureCounter) Add(amount int) int {
	counter.Value += amount
	return counter.Value
}

func TestWorkerProcessHelper(t *testing.T) {
	if os.Getenv("ZSHARP_GO_WORKER_TEST") != "1" {
		return
	}
	Export("add", func(a, b int64) int64 { return a + b })
	Export("echo", func(value any) any { return value })
	Export("words", func(words []string) []string { return words })
	Export("warn", func() string {
		fmt.Println("user stdout must be stderr")
		return "ok"
	})
	Export("callback", func(value int) (any, error) { return Call("Main:Main:Double", value) })
	Export("objectCallback", func(object *Object) (any, error) { return object.Invoke("Add", 2) })
	Export("background", func() string {
		result := make(chan error, 1)
		go func() { _, err := Call("Main:Main:Double", 1); result <- err }()
		if err := <-result; err != nil {
			return err.Error()
		}
		return "incorrectly accepted"
	})
	Export("panic", func() { panic("test panic") })
	Export("fail", func() error { return errors.New("test error") })
	ExportType("Counter", func(value int) *fixtureCounter { return &fixtureCounter{Value: value, Label: "counter"} })
	if err := Serve(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	os.Exit(0)
}

func TestPersistentWorkerProcess(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	child := exec.CommandContext(ctx, os.Args[0], "-test.run=^TestWorkerProcessHelper$")
	child.Env = append(os.Environ(), "ZSHARP_GO_WORKER_TEST=1")
	stdin, err := child.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}
	stdout, err := child.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	var stderr bytes.Buffer
	child.Stderr = &stderr
	if err := child.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() {
		stdin.Close()
		if err := child.Wait(); err != nil {
			t.Errorf("worker exited: %v; stderr: %s", err, stderr.String())
		}
	}()
	reader := bufio.NewReader(stdout)
	encoder := json.NewEncoder(stdin)
	nextID := 0
	read := func() map[string]any {
		line, err := reader.ReadBytes('\n')
		if err != nil {
			t.Fatalf("read worker: %v; stderr: %s", err, stderr.String())
		}
		var message map[string]any
		if err := json.Unmarshal(line, &message); err != nil {
			t.Fatalf("protocol contaminated: %s: %v", line, err)
		}
		return message
	}
	call := func(operation, target, reference, member string, arguments ...any) map[string]any {
		nextID++
		message := map[string]any{"id": nextID, "op": operation, "target": target,
			"module": "test.zgo", "ref": reference, "member": member, "args": arguments}
		if arguments == nil {
			message["args"] = []any{}
		}
		if err := encoder.Encode(message); err != nil {
			t.Fatal(err)
		}
		result := read()
		if result["op"] != "result" || result["id"] != float64(nextID) {
			t.Fatalf("unexpected worker message: %#v", result)
		}
		return result
	}
	okValue := func(result map[string]any) any {
		if result["ok"] != true {
			t.Fatalf("request failed: %#v", result)
		}
		return result["value"]
	}
	number := func(text string) map[string]any { return map[string]any{"$number": text} }
	if value := okValue(call("call", "add", "", "", number("9007199254740993"), number("7"))); !reflect.DeepEqual(value, number("9007199254741000")) {
		t.Fatalf("integer precision lost: %#v", value)
	}
	if value := okValue(call("call", "echo", "", "", number("0.12345678901234567890123456789"))); !reflect.DeepEqual(value, number("0.12345678901234567890123456789")) {
		t.Fatalf("decimal precision lost: %#v", value)
	}
	if value := okValue(call("call", "words", "", "", []any{"a", "b"})); !reflect.DeepEqual(value, []any{"a", "b"}) {
		t.Fatalf("array roundtrip: %#v", value)
	}
	reference := okValue(call("new", "Counter", "", "", number("10"))).(map[string]any)["$ref"].(string)
	if value := okValue(call("invoke", "", reference, "Add", number("5"))); !reflect.DeepEqual(value, number("15")) {
		t.Fatalf("object method: %#v", value)
	}
	okValue(call("set", "", reference, "Value", number("20")))
	if value := okValue(call("get", "", reference, "Value")); !reflect.DeepEqual(value, number("20")) {
		t.Fatalf("object field did not persist: %#v", value)
	}
	if result := call("get", "", reference, "hidden"); result["ok"] != false {
		t.Fatalf("private field exposed: %#v", result)
	}
	if value := okValue(call("call", "echo", "", "", map[string]any{"$ref": reference})).(map[string]any)["$ref"]; value != reference {
		t.Fatalf("object identity changed: %#v", value)
	}
	okValue(call("release", "", reference, ""))
	if result := call("get", "", reference, "Value"); result["ok"] != false || !strings.Contains(result["error"].(string), "released") {
		t.Fatalf("stale reference accepted: %#v", result)
	}
	for _, target := range []string{"panic", "fail"} {
		if result := call("call", target, "", ""); result["ok"] != false {
			t.Fatalf("function error not propagated: %#v", result)
		}
	}
	if result := call("call", "add", "", "", number("1.5"), number("1")); result["ok"] != false {
		t.Fatalf("fraction silently truncated: %#v", result)
	}
	if value := okValue(call("call", "background", "", "")); !strings.Contains(value.(string), "goroutine") {
		t.Fatalf("background callback not rejected: %#v", value)
	}
	if value := okValue(call("call", "warn", "", "")); value != "ok" {
		t.Fatalf("logging result: %#v", value)
	}
	nextID++
	callbackID := nextID
	if err := encoder.Encode(map[string]any{"id": callbackID, "op": "call", "target": "callback", "args": []any{number("21")}}); err != nil {
		t.Fatal(err)
	}
	callback := read()
	if callback["op"] != "host.call" || callback["target"] != "Main:Main:Double" {
		t.Fatalf("callback not emitted: %#v", callback)
	}
	// A nested request is answered as busy, without stealing the callback reply.
	encoder.Encode(map[string]any{"id": "nested", "op": "call", "target": "warn", "args": []any{}})
	if busy := read(); busy["ok"] != false || busy["id"] != "nested" {
		t.Fatalf("reentrant request accepted: %#v", busy)
	}
	encoder.Encode(map[string]any{"id": callback["id"], "op": "result", "ok": true, "value": number("42")})
	if result := read(); result["id"] != float64(callbackID) || !reflect.DeepEqual(okValue(result), number("42")) {
		t.Fatalf("callback result did not reach caller: %#v", result)
	}
	nextID++
	encoder.Encode(map[string]any{"id": nextID, "op": "call", "target": "objectCallback", "args": []any{map[string]any{"$zref": "z1", "$type": "Counter"}}})
	callback = read()
	if callback["op"] != "host.invoke" || callback["ref"] != "z1" || callback["member"] != "Add" {
		t.Fatalf("Z# object proxy invocation: %#v", callback)
	}
	encoder.Encode(map[string]any{"id": callback["id"], "op": "result", "ok": true, "value": number("12")})
	if result := read(); !reflect.DeepEqual(okValue(result), number("12")) {
		t.Fatalf("Z# object callback result: %#v", result)
	}
}

func TestValueAndConversionGuards(t *testing.T) {
	worker := newServer(strings.NewReader(""), &bytes.Buffer{})
	for _, value := range []any{math.Inf(1), math.NaN(), make(chan int), json.Number("1/2")} {
		if _, err := worker.encode(value, 0); err == nil {
			t.Errorf("unsupported value accepted: %#v", value)
		}
	}
	for _, text := range []string{"", "NaN", "1/2", "0x10", " 10"} {
		if _, err := worker.decode(map[string]any{"$number": text}, 0); err == nil {
			t.Errorf("invalid decimal accepted: %q", text)
		}
	}
	if _, err := convert(json.Number("256"), reflect.TypeOf(uint8(0))); err == nil {
		t.Error("integer overflow accepted")
	}
	if _, err := convert(json.Number("1e3"), reflect.TypeOf(int(0))); err != nil {
		t.Errorf("integral exponent rejected: %v", err)
	}
	if _, err := worker.callback("host.call", "Main:Main:Run", "", "", nil); err == nil {
		t.Error("callback outside an active entry accepted")
	}
}
