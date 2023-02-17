#include "hermes/VM/JSLib/RuntimeFastJSONUtils.h"

#include "Object.h"

#include "hermes/Support/Compiler.h"
#include "hermes/Support/JSON.h"
#include "hermes/Support/UTF16Stream.h"
#include "hermes/VM/ArrayLike.h"
#include "hermes/VM/ArrayStorage.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSProxy.h"
#include "hermes/VM/PrimitiveBox.h"

#include "JSONLexer.h"

#include "llvh/ADT/SmallString.h"
#include "llvh/Support/SaveAndRestore.h"

#include "simdjson/src/simdjson.h"

using namespace simdjson;

#define SIMDJSON_CALL(operation)       \
  error = operation;                   \
  if (LLVM_UNLIKELY(error)) {          \
    return ExecutionStatus::EXCEPTION; \
  }

namespace hermes {
namespace vm {

template<typename T>
CallResult<HermesValue> parseValue(Runtime &rt, T &value);

CallResult<HermesValue> parseArray(Runtime &rt, ondemand::array &array) {
  simdjson::error_code error;

  auto jsArrayRes = JSArray::create(rt, 4, 0);
  if (LLVM_UNLIKELY(jsArrayRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto jsArray = *jsArrayRes;

  uint32_t index = 0;
  MutableHandle<> indexValue{rt};

  GCScope gcScope{rt};
  auto marker = gcScope.createMarker();

  for (auto valueRes : array) {
    gcScope.flushToMarker(marker);

    ondemand::value value;
    SIMDJSON_CALL(valueRes.get(value));

    auto jsValue = parseValue(rt, value);

    indexValue = HermesValue::encodeDoubleValue(index);
    (void)JSObject::defineOwnComputedPrimitive(
          jsArray,
          rt,
          indexValue,
          DefinePropertyFlags::getDefaultNewPropertyFlags(),
          rt.makeHandle(*jsValue));

    index++;
  }

  return jsArray.getHermesValue();
}

CallResult<HermesValue> parseObject(Runtime &rt, ondemand::object &object) {
  simdjson::error_code error;

  auto jsObject = rt.makeHandle(JSObject::create(rt));

  // MutableHandle<StringPrimitive> jsKeyHandle{rt};

  GCScope gcScope{rt};
  auto marker = gcScope.createMarker();

  for (auto field : object) {
    gcScope.flushToMarker(marker);

    std::string_view key;
    SIMDJSON_CALL(field.unescaped_key().get(key));

    UTF8Ref hermesStr{(const uint8_t*)key.data(), key.size()};
    auto jsKey = StringPrimitive::createEfficient(rt, hermesStr);

    ondemand::value value;
    SIMDJSON_CALL(field.value().get(value));

    auto jsValue = parseValue(rt, value);

    (void)JSObject::defineOwnComputedPrimitive(
          jsObject,
          rt,
          rt.makeHandle(*jsKey),
          DefinePropertyFlags::getDefaultNewPropertyFlags(),
          rt.makeHandle(*jsValue));
  }

  return jsObject.getHermesValue();
}

template<typename T>
CallResult<HermesValue> parseValue(Runtime &rt, T &value) {
  simdjson::error_code error;

  ondemand::json_type type;
  error = value.type().get(type);
  switch (type) {
    case ondemand::json_type::array: {
      ondemand::array arrayValue;
      SIMDJSON_CALL(value.get(arrayValue));
      return parseArray(rt, arrayValue);
    }
    case ondemand::json_type::object: {
      ondemand::object objectValue;
      SIMDJSON_CALL(value.get(objectValue));
      return parseObject(rt, objectValue);
    }
    case ondemand::json_type::number:
      double doubleValue;
      SIMDJSON_CALL(value.get(doubleValue));
      return HermesValue::encodeDoubleValue(doubleValue);
    case ondemand::json_type::string: {
      std::string_view stringView;
      SIMDJSON_CALL(value.get(stringView));
      UTF8Ref hermesStr{(const uint8_t*)stringView.data(), stringView.size()};
      return StringPrimitive::createEfficient(rt, hermesStr);
    }
    case ondemand::json_type::boolean:
      bool boolValue;
      SIMDJSON_CALL(value.get(boolValue));
      return HermesValue::encodeBoolValue(boolValue);
    case ondemand::json_type::null:
      return HermesValue::encodeNullValue();
    default:
      return ExecutionStatus::EXCEPTION;
  }
}

CallResult<HermesValue> runtimeFastJSONParse(
    Runtime &runtime,
    Handle<StringPrimitive> jsonString,
    Handle<Callable> reviver) {
  ondemand::parser parser;
  simdjson::error_code error;

  // TODO: Error handling
  // TODO: Support UTF-16

  // auto asciiRef = jsonString->getStringRef<char>();
  // auto json = padded_string(asciiRef.data(), asciiRef.size());

  // TODO: Avoid copying so much?
  UTF16Ref ref;
  SmallU16String<32> storage;
  if (LLVM_UNLIKELY(jsonString->isExternal() && !jsonString->isASCII())) {
    ref = jsonString->getStringRef<char16_t>();
  } else {
    StringPrimitive::createStringView(runtime, jsonString)
        .appendUTF16String(storage);
    ref = storage;
  }
  std::string out;
  convertUTF16ToUTF8WithReplacements(out, ref);
  auto json = padded_string(out);

  ondemand::document doc;
  SIMDJSON_CALL(parser.iterate(json).get(doc));

  return parseValue(runtime, doc);
}

} // namespace vm
} // namespace hermes
