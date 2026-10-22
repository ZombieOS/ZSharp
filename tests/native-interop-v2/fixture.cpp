#include "zsharp_cpp.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
class Counter {
public:
    std::string value;
    explicit Counter(std::string initial) : value(std::move(initial)) {}
};
std::map<std::string, std::unique_ptr<Counter>> counters;
unsigned long long next_handle = 0;

std::string argument(const char *json) {
    std::string value(json);
    if (value.size() < 3 || value.front() != '[' || value.back() != ']')
        throw std::runtime_error("Counter requires one wire value");
    return value.substr(1, value.size() - 2);
}
int emit(const ZSharpInteropResultV2 *sink, const std::string &json,
         char *error, size_t size) {
    if (!sink || !sink->write) throw std::runtime_error("missing result sink");
    return sink->write(sink->context, json.data(), json.size(), error, size);
}
}

extern "C" ZSHARP_CPP_EXPORT int zsharp_cpp_call_v2(
    const ZSharpInteropHostV2 *host, const ZSharpInteropRequestV2 *request,
    const ZSharpInteropResultV2 *result, char *error, size_t error_size) {
    try {
        if (!request || request->abi_version != ZSHARP_INTEROP_ABI_V2
            || request->struct_size < sizeof(*request) || !request->operation
            || !request->target || !request->member || !request->handle
            || !request->arguments_json)
            throw std::runtime_error("unsupported interop request ABI");
        const std::string operation(request->operation);
        if (operation == "call") {
            if (std::strcmp(request->target, "echo") == 0)
                return emit(result, request->arguments_json, error, error_size);
            if (std::strcmp(request->target, "callback") == 0) {
                if (!host || host->abi_version != ZSHARP_INTEROP_ABI_V2
                    || host->struct_size < sizeof(*host) || !host->invoke)
                    throw std::runtime_error("missing Z# callback host");
                auto callback = *request;
                callback.target = "Main:Main:Callback";
                return host->invoke(host->context, &callback, result, error, error_size);
            }
            throw std::runtime_error("unknown exported C++ function");
        }
        if (operation == "new") {
            if (std::strcmp(request->target, "Counter") != 0)
                throw std::runtime_error("unknown exported C++ class");
            auto object = std::make_unique<Counter>(argument(request->arguments_json));
            auto handle = std::to_string(++next_handle);
            counters.emplace(handle, std::move(object));
            return emit(result, "{\"$ref\":\"" + handle + "\",\"$type\":\"Counter\"}", error, error_size);
        }
        auto found = counters.find(request->handle);
        if (found == counters.end()) throw std::runtime_error("released or unknown object handle");
        if (operation == "release") {
            counters.erase(found);
            return emit(result, "null", error, error_size);
        }
        if (operation == "get" && std::strcmp(request->member, "value") == 0)
            return emit(result, found->second->value, error, error_size);
        if (operation == "set" && std::strcmp(request->member, "value") == 0) {
            found->second->value = argument(request->arguments_json);
            return emit(result, "null", error, error_size);
        }
        if (operation == "invoke" && std::strcmp(request->member, "echo") == 0)
            return emit(result, request->arguments_json, error, error_size);
        throw std::runtime_error("unknown C++ object member or operation");
    } catch (const std::exception &failure) {
        if (error && error_size) std::snprintf(error, error_size, "%s", failure.what());
        return 0;
    } catch (...) {
        if (error && error_size) std::snprintf(error, error_size, "unknown C++ exception");
        return 0;
    }
}

extern "C" ZSHARP_CPP_EXPORT int zsharp_cpp_call_v1(
    uint32_t version, const char *function, const ZSharpCppValue *arguments,
    size_t count, ZSharpCppValue *result, char *error, size_t error_size) {
    (void)arguments;
    if (version != ZSHARP_CPP_ABI_VERSION || count || std::strcmp(function, "legacy")) {
        if (error && error_size) std::snprintf(error, error_size, "unexpected legacy C++ call");
        return 0;
    }
    result->type = ZSHARP_CPP_NUMBER;
    result->number = 88;
    result->text = nullptr;
    return 1;
}
