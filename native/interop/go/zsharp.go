// Package zsharp exposes Go functions and live objects to the desktop Z# VM.
// Compile each module as an executable ending in .zgo (.zgo.exe on Windows),
// register its public API in main, then call Serve.
package zsharp

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"math/big"
	"os"
	"reflect"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
)

var registry = struct {
	sync.Mutex
	functions map[string]reflect.Value
	types     map[string]reflect.Value
	worker    *server
}{functions: make(map[string]reflect.Value), types: make(map[string]reflect.Value)}

// Export registers a function under the exact name used by Function.call.
// The function may return one value, multiple values (an array), and/or a final
// error. An error becomes a Z# runtime error rather than an ordinary result.
func Export(name string, function any) {
	register(name, function, false)
}

// ExportType registers a constructor returning a struct or a pointer to one.
// Only exported Go fields and methods are accessible through the bridge.
func ExportType(name string, constructor any) {
	register(name, constructor, true)
}

func register(name string, function any, constructor bool) {
	if !validName(name) {
		panic("zsharp: export requires a nonempty identifier or qualified identifier")
	}
	value := reflect.ValueOf(function)
	if !value.IsValid() || value.Kind() != reflect.Func || value.IsNil() {
		panic("zsharp: export requires a function")
	}
	registry.Lock()
	defer registry.Unlock()
	if registry.worker != nil {
		panic("zsharp: exports must be registered before Serve")
	}
	target := registry.functions
	if constructor {
		target = registry.types
	}
	if _, exists := target[name]; exists {
		panic("zsharp: duplicate export " + name)
	}
	target[name] = value
}

func validName(name string) bool {
	for _, part := range strings.Split(name, ".") {
		if part == "" {
			return false
		}
		for index, ch := range part {
			if ch != '_' && !(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') &&
				!(index > 0 && ch >= '0' && ch <= '9') {
				return false
			}
		}
	}
	return true
}

// Call synchronously calls a Z# brain from the function currently handling the
// VM request. Background goroutines and calls outside that request are rejected.
func Call(target string, arguments ...any) (any, error) {
	if strings.TrimSpace(target) == "" {
		return nil, errors.New("zsharp: callback target must not be empty")
	}
	return hostCall("host.call", target, "", "", arguments)
}

// Object is a live VM-owned Z# object, not a snapshot of its fields.
type Object struct {
	reference string
	typeName  string
}

func (object *Object) Get(member string) (any, error) {
	return hostCall("host.get", "", object.reference, member, nil)
}

func (object *Object) Set(member string, value any) error {
	_, err := hostCall("host.set", "", object.reference, member, []any{value})
	return err
}

func (object *Object) Invoke(member string, arguments ...any) (any, error) {
	return hostCall("host.invoke", "", object.reference, member, arguments)
}

func hostCall(operation, target, reference, member string, arguments []any) (any, error) {
	registry.Lock()
	worker := registry.worker
	registry.Unlock()
	if worker == nil {
		return nil, errors.New("zsharp: callbacks are only available inside the ZVM worker")
	}
	return worker.callback(operation, target, reference, member, arguments)
}

type request struct {
	ID     any    `json:"id"`
	Op     string `json:"op"`
	Module string `json:"module"`
	Target string `json:"target"`
	Ref    string `json:"ref"`
	Member string `json:"member"`
	Args   []any  `json:"args"`
	OK     bool   `json:"ok"`
	Value  any    `json:"value"`
	Error  string `json:"error"`
}

type objectIdentity struct {
	typeOf reflect.Type
	ptr    uintptr
}

type server struct {
	input        *bufio.Scanner
	output       io.Writer
	functions    map[string]reflect.Value
	types        map[string]reflect.Value
	objects      map[string]any
	identities   map[objectIdentity]string
	nextObject   uint64
	nextCallback uint64
	owner        uint64
	active       atomic.Bool
}

func newServer(input io.Reader, output io.Writer) *server {
	scanner := bufio.NewScanner(input)
	scanner.Buffer(make([]byte, 4096), 1024*1024)
	return &server{input: scanner, output: output, functions: registry.functions,
		types: registry.types, objects: make(map[string]any),
		identities: make(map[objectIdentity]string), owner: goroutineID()}
}

// Serve runs the persistent, newline-delimited JSON worker until the ZVM closes
// the connection. It redirects normal stdout to stderr to protect the protocol.
// It does not read the microphone, start a network service, or spawn processes.
func Serve() error {
	registry.Lock()
	if registry.worker != nil {
		registry.Unlock()
		return errors.New("zsharp: a worker is already running")
	}
	protocol := os.Stdout
	os.Stdout = os.Stderr
	worker := newServer(os.Stdin, protocol)
	registry.worker = worker
	registry.Unlock()
	defer func() {
		worker.active.Store(false)
		registry.Lock()
		registry.worker = nil
		registry.Unlock()
		os.Stdout = protocol
	}()
	for worker.input.Scan() {
		message, err := parseRequest(worker.input.Bytes())
		if err != nil {
			if err = worker.respond(nil, nil, err); err != nil {
				return err
			}
			continue
		}
		worker.active.Store(true)
		value, callError := worker.dispatch(message)
		worker.active.Store(false)
		if err := worker.respond(message.ID, value, callError); err != nil {
			return err
		}
	}
	return worker.input.Err()
}

func parseRequest(line []byte) (request, error) {
	var message request
	decoder := json.NewDecoder(strings.NewReader(string(line)))
	decoder.UseNumber()
	if err := decoder.Decode(&message); err != nil {
		return message, err
	}
	if message.Op == "" {
		return message, errors.New("zsharp: request operation is required")
	}
	var trailing any
	if err := decoder.Decode(&trailing); err != io.EOF {
		return message, errors.New("zsharp: trailing data after bridge request")
	}
	return message, nil
}

func (worker *server) send(value any) error {
	encoded, err := json.Marshal(value)
	if err != nil {
		return err
	}
	if len(encoded) > 1024*1024 {
		return errors.New("Go bridge message exceeds 1 MiB")
	}
	_, err = worker.output.Write(append(encoded, '\n'))
	return err
}

func (worker *server) respond(id, value any, err error) error {
	if err == nil {
		value, err = worker.encode(value, 0)
	}
	if err != nil {
		return worker.send(map[string]any{"id": id, "op": "result", "ok": false, "error": err.Error()})
	}
	return worker.send(map[string]any{"id": id, "op": "result", "ok": true, "value": value})
}

// Go intentionally has no public goroutine-ID API. The runtime's own stack
// header provides a fail-closed ownership check here; if it changes, callbacks
// are rejected rather than allowing a goroutine to consume another request.
func goroutineID() uint64 {
	var buffer [96]byte
	fields := strings.Fields(string(buffer[:runtime.Stack(buffer[:], false)]))
	if len(fields) < 2 || fields[0] != "goroutine" {
		return 0
	}
	id, _ := strconv.ParseUint(fields[1], 10, 64)
	return id
}

func (worker *server) callback(operation, target, reference, member string, arguments []any) (any, error) {
	if worker.owner == 0 || goroutineID() != worker.owner || !worker.active.Load() {
		return nil, errors.New("zsharp: callbacks must run on the goroutine handling the current foreign call")
	}
	encoded := make([]any, len(arguments))
	for index, argument := range arguments {
		value, err := worker.encode(argument, 0)
		if err != nil {
			return nil, err
		}
		encoded[index] = value
	}
	worker.nextCallback++
	id := -int64(worker.nextCallback)
	message := map[string]any{"id": id, "op": operation, "args": encoded}
	if target != "" {
		message["target"] = target
	}
	if reference != "" {
		message["ref"], message["member"] = reference, member
	}
	if err := worker.send(message); err != nil {
		return nil, err
	}
	for worker.input.Scan() {
		response, err := parseRequest(worker.input.Bytes())
		if err != nil {
			return nil, err
		}
		if response.Op != "result" {
			if err := worker.respond(response.ID, nil, errors.New("reentrant call to the active Go worker is not supported")); err != nil {
				return nil, err
			}
			continue
		}
		responseID, validID := response.ID.(json.Number)
		parsedID, idError := responseID.Int64()
		if !validID || idError != nil || parsedID != id {
			return nil, errors.New("zsharp: unexpected callback response ID")
		}
		if !response.OK {
			return nil, fmt.Errorf("Z# callback failed: %s", response.Error)
		}
		return worker.decode(response.Value, 0)
	}
	if err := worker.input.Err(); err != nil {
		return nil, err
	}
	return nil, errors.New("zsharp: ZVM connection closed during callback")
}

func (worker *server) dispatch(message request) (result any, err error) {
	defer func() {
		if recovered := recover(); recovered != nil {
			result, err = nil, fmt.Errorf("Go function panicked: %v", recovered)
		}
	}()
	arguments := make([]any, len(message.Args))
	for index, argument := range message.Args {
		arguments[index], err = worker.decode(argument, 0)
		if err != nil {
			return nil, err
		}
	}
	if message.Op == "call" {
		function, exists := worker.functions[message.Target]
		if !exists {
			return nil, fmt.Errorf("Go function %q is not exported", message.Target)
		}
		return invoke(function, arguments)
	}
	if message.Op == "new" {
		constructor, exists := worker.types[message.Target]
		if !exists {
			return nil, fmt.Errorf("Go type %q is not exported", message.Target)
		}
		value, err := invoke(constructor, arguments)
		if err != nil {
			return nil, err
		}
		if _, err := structValue(value); err != nil {
			return nil, errors.New("Go exported constructor must return a non-nil struct or pointer to struct")
		}
		return value, nil
	}
	object, exists := worker.objects[message.Ref]
	if !exists || message.Ref == "" {
		return nil, errors.New("Go object reference is invalid or has been released")
	}
	if message.Op == "release" {
		delete(worker.objects, message.Ref)
		if key, ok := identity(object); ok {
			delete(worker.identities, key)
		}
		return nil, nil
	}
	if !validName(message.Member) || strings.Contains(message.Member, ".") {
		return nil, errors.New("Go object member must be one exported identifier")
	}
	if message.Op == "invoke" {
		method := reflect.ValueOf(object).MethodByName(message.Member)
		if !method.IsValid() {
			return nil, fmt.Errorf("Go method %q is not exported", message.Member)
		}
		return invoke(method, arguments)
	}
	value, err := structValue(object)
	if err != nil {
		return nil, err
	}
	field := value.FieldByName(message.Member)
	definition, found := value.Type().FieldByName(message.Member)
	if !found || definition.PkgPath != "" || !field.IsValid() || !field.CanInterface() {
		return nil, fmt.Errorf("Go field %q is not exported", message.Member)
	}
	switch message.Op {
	case "get":
		if len(arguments) != 0 {
			return nil, errors.New("Go field get does not accept arguments")
		}
		return field.Interface(), nil
	case "set":
		if len(arguments) != 1 || !field.CanSet() {
			return nil, errors.New("Go field set requires one value and a writable exported field")
		}
		converted, err := convert(arguments[0], field.Type())
		if err != nil {
			return nil, err
		}
		field.Set(converted)
		return nil, nil
	default:
		return nil, fmt.Errorf("unsupported Go bridge operation %q", message.Op)
	}
}

func structValue(object any) (reflect.Value, error) {
	value := reflect.ValueOf(object)
	if value.IsValid() && value.Kind() == reflect.Pointer {
		if value.IsNil() {
			return reflect.Value{}, errors.New("nil Go object")
		}
		value = value.Elem()
	}
	if !value.IsValid() || value.Kind() != reflect.Struct {
		return reflect.Value{}, errors.New("Go value is not a struct object")
	}
	return value, nil
}

func invoke(function reflect.Value, arguments []any) (any, error) {
	typeOf := function.Type()
	minimum := typeOf.NumIn()
	if typeOf.IsVariadic() {
		minimum--
	}
	if len(arguments) < minimum || (!typeOf.IsVariadic() && len(arguments) != minimum) {
		return nil, fmt.Errorf("Go function expects %d arguments; received %d", minimum, len(arguments))
	}
	values := make([]reflect.Value, len(arguments))
	for index, argument := range arguments {
		var target reflect.Type
		if typeOf.IsVariadic() && index >= typeOf.NumIn()-1 {
			target = typeOf.In(typeOf.NumIn() - 1).Elem()
		} else {
			target = typeOf.In(index)
		}
		value, err := convert(argument, target)
		if err != nil {
			return nil, fmt.Errorf("Go argument %d: %w", index+1, err)
		}
		values[index] = value
	}
	outputs := function.Call(values)
	errorType := reflect.TypeOf((*error)(nil)).Elem()
	if len(outputs) > 0 && outputs[len(outputs)-1].Type().Implements(errorType) {
		last := outputs[len(outputs)-1]
		nilable := last.Kind() == reflect.Interface || last.Kind() == reflect.Pointer ||
			last.Kind() == reflect.Map || last.Kind() == reflect.Slice ||
			last.Kind() == reflect.Func || last.Kind() == reflect.Chan
		if !nilable || !last.IsNil() {
			return nil, last.Interface().(error)
		}
		outputs = outputs[:len(outputs)-1]
	}
	if len(outputs) == 0 {
		return nil, nil
	}
	if len(outputs) == 1 {
		return outputs[0].Interface(), nil
	}
	result := make([]any, len(outputs))
	for index, value := range outputs {
		result[index] = value.Interface()
	}
	return result, nil
}

func convert(value any, target reflect.Type) (reflect.Value, error) {
	if value == nil {
		switch target.Kind() {
		case reflect.Interface, reflect.Pointer, reflect.Map, reflect.Slice, reflect.Func:
			return reflect.Zero(target), nil
		default:
			return reflect.Value{}, fmt.Errorf("null cannot be converted to %s", target)
		}
	}
	source := reflect.ValueOf(value)
	if source.Type().AssignableTo(target) {
		return source, nil
	}
	if number, ok := value.(json.Number); ok {
		text := number.String()
		converted := reflect.New(target).Elem()
		switch target.Kind() {
		case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
			rational, ok := new(big.Rat).SetString(text)
			if ok && rational.IsInt() && rational.Num().IsInt64() && !converted.OverflowInt(rational.Num().Int64()) {
				converted.SetInt(rational.Num().Int64())
				return converted, nil
			}
		case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64:
			rational, ok := new(big.Rat).SetString(text)
			if ok && rational.IsInt() && rational.Num().IsUint64() && !converted.OverflowUint(rational.Num().Uint64()) {
				converted.SetUint(rational.Num().Uint64())
				return converted, nil
			}
		case reflect.Float32, reflect.Float64:
			parsed, err := strconv.ParseFloat(text, target.Bits())
			if err == nil && !math.IsInf(parsed, 0) && !math.IsNaN(parsed) {
				converted.SetFloat(parsed)
				return converted, nil
			}
		}
		return reflect.Value{}, fmt.Errorf("number %q cannot be converted exactly/in-range to %s", text, target)
	}
	if array, ok := value.([]any); ok && (target.Kind() == reflect.Slice || target.Kind() == reflect.Array) {
		var converted reflect.Value
		if target.Kind() == reflect.Array {
			if len(array) != target.Len() {
				return reflect.Value{}, fmt.Errorf("array length does not match %s", target)
			}
			converted = reflect.New(target).Elem()
		} else {
			converted = reflect.MakeSlice(target, len(array), len(array))
		}
		for index, item := range array {
			element, err := convert(item, target.Elem())
			if err != nil {
				return reflect.Value{}, err
			}
			converted.Index(index).Set(element)
		}
		return converted, nil
	}
	return reflect.Value{}, fmt.Errorf("bridge value %T cannot be converted to %s", value, target)
}

func (worker *server) decode(value any, depth int) (any, error) {
	if depth > 128 {
		return nil, errors.New("Go bridge value nesting limit exceeded")
	}
	switch value := value.(type) {
	case map[string]any:
		if number, exists := value["$number"]; exists {
			text, ok := number.(string)
			if !ok || strings.TrimSpace(text) != text || text == "" {
				return nil, errors.New("$number must contain decimal text")
			}
			if _, ok := new(big.Rat).SetString(text); !ok || strings.ContainsAny(text, "/pPxX") {
				return nil, errors.New("$number must contain a finite decimal number")
			}
			return json.Number(text), nil
		}
		if reference, exists := value["$ref"]; exists {
			text, ok := reference.(string)
			object, found := worker.objects[text]
			if !ok || !found {
				return nil, errors.New("Go object reference is invalid or has been released")
			}
			return object, nil
		}
		if reference, exists := value["$zref"]; exists {
			text, ok := reference.(string)
			if !ok || text == "" {
				return nil, errors.New("invalid Z# object reference")
			}
			typeName, _ := value["$type"].(string)
			return &Object{reference: text, typeName: typeName}, nil
		}
		if fields, exists := value["$record"]; exists {
			record, ok := fields.(map[string]any)
			if !ok {
				return nil, errors.New("$record must contain an object")
			}
			result := make(map[string]any, len(record))
			for key, item := range record {
				decoded, err := worker.decode(item, depth+1)
				if err != nil {
					return nil, err
				}
				result[key] = decoded
			}
			return result, nil
		}
		return nil, errors.New("unknown Go bridge value tag")
	case []any:
		result := make([]any, len(value))
		for index, item := range value {
			decoded, err := worker.decode(item, depth+1)
			if err != nil {
				return nil, err
			}
			result[index] = decoded
		}
		return result, nil
	default:
		return value, nil
	}
}

func identity(value any) (objectIdentity, bool) {
	object := reflect.ValueOf(value)
	if object.IsValid() && object.Kind() == reflect.Pointer && !object.IsNil() {
		return objectIdentity{typeOf: object.Type(), ptr: object.Pointer()}, true
	}
	return objectIdentity{}, false
}

func (worker *server) encode(value any, depth int) (any, error) {
	if depth > 128 {
		return nil, errors.New("Go bridge value nesting limit exceeded")
	}
	if value == nil {
		return nil, nil
	}
	if object, ok := value.(*Object); ok {
		if object == nil {
			return nil, nil
		}
		return map[string]any{"$zref": object.reference, "$type": object.typeName}, nil
	}
	if number, ok := value.(json.Number); ok {
		if _, err := worker.decode(map[string]any{"$number": number.String()}, 0); err != nil {
			return nil, err
		}
		return map[string]any{"$number": number.String()}, nil
	}
	object := reflect.ValueOf(value)
	switch object.Kind() {
	case reflect.String:
		return object.String(), nil
	case reflect.Bool:
		return object.Bool(), nil
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		return map[string]any{"$number": strconv.FormatInt(object.Int(), 10)}, nil
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64:
		return map[string]any{"$number": strconv.FormatUint(object.Uint(), 10)}, nil
	case reflect.Float32, reflect.Float64:
		number := object.Float()
		if math.IsNaN(number) || math.IsInf(number, 0) {
			return nil, errors.New("non-finite Go number cannot cross the bridge")
		}
		return map[string]any{"$number": strconv.FormatFloat(number, 'g', -1, object.Type().Bits())}, nil
	case reflect.Slice, reflect.Array:
		if object.Kind() == reflect.Slice && object.IsNil() {
			return nil, nil
		}
		result := make([]any, object.Len())
		for index := range result {
			item, err := worker.encode(object.Index(index).Interface(), depth+1)
			if err != nil {
				return nil, err
			}
			result[index] = item
		}
		return result, nil
	case reflect.Map:
		if object.Type().Key().Kind() != reflect.String {
			return nil, errors.New("Go records require string map keys")
		}
		fields := make(map[string]any)
		iterator := object.MapRange()
		for iterator.Next() {
			item, err := worker.encode(iterator.Value().Interface(), depth+1)
			if err != nil {
				return nil, err
			}
			fields[iterator.Key().String()] = item
		}
		return map[string]any{"$record": fields}, nil
	case reflect.Pointer:
		if object.IsNil() {
			return nil, nil
		}
		if object.Elem().Kind() != reflect.Struct {
			return nil, errors.New("only Go struct pointers can become live object references")
		}
	case reflect.Struct:
		copy := reflect.New(object.Type())
		copy.Elem().Set(object)
		value, object = copy.Interface(), copy
	default:
		return nil, fmt.Errorf("Go value %s cannot cross the bridge", object.Type())
	}
	key, _ := identity(value)
	reference, exists := worker.identities[key]
	if !exists {
		worker.nextObject++
		reference = strconv.FormatUint(worker.nextObject, 10)
		worker.identities[key], worker.objects[reference] = reference, value
	}
	return map[string]any{"$ref": reference, "$type": object.Elem().Type().String()}, nil
}
