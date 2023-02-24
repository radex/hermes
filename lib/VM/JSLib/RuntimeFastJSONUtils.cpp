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
#include "hermes/VM/JSLib/RuntimeCommonStorage.h"

#include "JSONLexer.h"

#include "llvh/ADT/SmallString.h"
#include "llvh/Support/SaveAndRestore.h"

#include "simdjson/src/simdjson.h"
#include "simdutf/src/simdutf.h"

using namespace simdjson;

#define SIMDJSON_CALL(operation)            \
  error = operation;                        \
  if (LLVM_UNLIKELY(error)) {               \
    /* TODO: Actual error messsage */       \
    return rt.raiseSyntaxError(             \
      TwineChar16("JSON fastParse error")   \
    );                                      \
  }

namespace hermes {
namespace vm {

class RuntimeFastJSONParser final {
private:
  Runtime &rt;
  bool isASCII_;
public:
  explicit RuntimeFastJSONParser(
      Runtime &runtime,
      bool isASCII)
      : rt(runtime), isASCII_(isASCII) {}

  static CallResult<HermesValue> parse(Runtime &rt, padded_string_view &json, bool isASCII);
private:
  template<typename T>
  CallResult<Handle<>> parseValue(T &value);
  CallResult<Handle<HermesValue>> parseString(std::string_view &stringView);
  CallResult<Handle<SymbolID>> parseObjectKeySlowPath(IdentifierTable &identifierTable, std::string_view &stringView);
  CallResult<Handle<SymbolID>> parseObjectKey(IdentifierTable &identifierTable, std::string_view &stringView);
  CallResult<HermesValue> parseArray(ondemand::array &array);
  CallResult<HermesValue> parseObject(ondemand::object &object);
};

inline CallResult<Handle<HermesValue>> RuntimeFastJSONParser::parseString(std::string_view &stringView) {
  if (isASCII_) {
    ASCIIRef ascii{stringView.data(), stringView.size()};
    auto string = StringPrimitive::createEfficient(rt, ascii);
    if (LLVM_UNLIKELY(string == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    return rt.makeHandle(*string);
  }

  UTF8Ref utf8{(const uint8_t*)stringView.data(), stringView.size()};
  auto string = StringPrimitive::createEfficient(rt, utf8);
  if (LLVM_UNLIKELY(string == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return rt.makeHandle(*string);
}

CallResult<Handle<SymbolID>> RuntimeFastJSONParser::parseObjectKeySlowPath(IdentifierTable &identifierTable, std::string_view &stringView) {
  UTF8Ref utf8{(const uint8_t*)stringView.data(), stringView.size()};
  auto string = StringPrimitive::createEfficient(rt, utf8);
  if (LLVM_UNLIKELY(string == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return valueToSymbolID(rt, rt.makeHandle(*string));
}

inline CallResult<Handle<SymbolID>> RuntimeFastJSONParser::parseObjectKey(IdentifierTable &identifierTable, std::string_view &stringView) {
  // We can skip some unnecessary work by skipping StringPrimitive creation
  // and going straight for SymbolIDs.
  if (LLVM_LIKELY(isASCII_ || isAllASCII_v2(stringView.data(), stringView.data() + stringView.size()))) {
    ASCIIRef ascii{stringView.data(), stringView.size()};
    return identifierTable.getSymbolHandle(rt, ascii);
  }

  return parseObjectKeySlowPath(identifierTable, stringView);
}

CallResult<HermesValue> RuntimeFastJSONParser::parseArray(ondemand::array &array) {
  simdjson::error_code error;

  auto jsArrayRes = JSArray::create(rt, 4, 0);
  if (LLVM_UNLIKELY(jsArrayRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto jsArray = *jsArrayRes;

  uint32_t index = 0;

  GCScope gcScope{rt};
  auto marker = gcScope.createMarker();

  for (auto valueRes : array) {
    gcScope.flushToMarker(marker);

    ondemand::value value;
    SIMDJSON_CALL(valueRes.get(value));

    auto jsValue = parseValue(value);
    if (LLVM_UNLIKELY(jsValue == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // fast path
    JSArray::setElementAt(jsArray, rt, index, *jsValue);

    index++;
  }
  (void) JSArray::setLengthProperty(jsArray, rt, index, PropOpFlags());

  return jsArray.getHermesValue();
}

CallResult<HermesValue> RuntimeFastJSONParser::parseObject(ondemand::object &object) {
  simdjson::error_code error;
  auto &identifierTable = rt.getIdentifierTable();

  auto jsObject = rt.makeHandle(JSObject::create(rt));

  GCScope gcScope{rt};
  auto marker = gcScope.createMarker();

  for (auto field : object) {
    gcScope.flushToMarker(marker);

    std::string_view key;
    SIMDJSON_CALL(field.unescaped_key().get(key));
    auto jsKey = parseObjectKey(identifierTable, key);
    if (LLVM_UNLIKELY(jsKey == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    ondemand::value value;
    SIMDJSON_CALL(field.value().get(value));

    auto jsValue = parseValue(value);
    if (LLVM_UNLIKELY(jsValue == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    // NOTE: We know that keys can only be strings, that they rarely are duplicated,
    // and that the property descriptors are default and don't change between iterations
    // so we can do this much faster than JSObject::defineOwnComputedPrimitive
    NamedPropertyDescriptor desc;
    auto pos = JSObject::findProperty(
        jsObject,
        rt,
        **jsKey,
        PropertyFlags::defaultNewNamedPropertyFlags(),
        desc);
    if (LLVM_UNLIKELY(pos)) {
      auto updatePropertyResult = JSObject::updateOwnProperty(
        jsObject,
        rt,
        **jsKey,
        *pos,
        desc,
        DefinePropertyFlags::getDefaultNewPropertyFlags(),
        *jsValue,
        PropOpFlags());

      if (LLVM_UNLIKELY(updatePropertyResult == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    } else {
      auto addPropertyResult = JSObject::addOwnPropertyImpl(
          jsObject,
          rt,
          **jsKey,
          PropertyFlags::defaultNewNamedPropertyFlags(),
          *jsValue);

      if (LLVM_UNLIKELY(addPropertyResult == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    }
  }

  return jsObject.getHermesValue();
}

template<typename T>
CallResult<Handle<>> RuntimeFastJSONParser::parseValue(T &value) {
  simdjson::error_code error;

  ondemand::json_type type;
  SIMDJSON_CALL(value.type().get(type));

  switch (type) {
    case ondemand::json_type::string: {
      std::string_view stringView;
      SIMDJSON_CALL(value.get(stringView));
      auto jsString = parseString(stringView);
      if (LLVM_UNLIKELY(jsString == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      return *jsString;
    }
    case ondemand::json_type::number: {
      double doubleValue;
      SIMDJSON_CALL(value.get(doubleValue));
      return rt.makeHandle(HermesValue::encodeDoubleValue(doubleValue));
    }
    case ondemand::json_type::object: {
      ondemand::object objectValue;
      SIMDJSON_CALL(value.get(objectValue));
      auto jsObject = parseObject(objectValue);
      if (LLVM_UNLIKELY(jsObject == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      return rt.makeHandle(*jsObject);
    }
    case ondemand::json_type::array: {
      ondemand::array arrayValue;
      SIMDJSON_CALL(value.get(arrayValue));
      auto jsArray = parseArray(arrayValue);
      if (LLVM_UNLIKELY(jsArray == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
      return rt.makeHandle(*jsArray);
    }
    case ondemand::json_type::boolean: {
      bool boolValue;
      SIMDJSON_CALL(value.get(boolValue));
      return rt.makeHandle(HermesValue::encodeBoolValue(boolValue));
    }
    case ondemand::json_type::null:
      return rt.makeHandle(HermesValue::encodeNullValue());
    default:
      return ExecutionStatus::EXCEPTION;
  }
}

CallResult<HermesValue> RuntimeFastJSONParser::parse(Runtime &rt, padded_string_view &json, bool isASCII) {
  simdjson::error_code error;
  ondemand::document doc;
  auto &jsonParser = rt.getCommonStorage()->simdjsonParser;
  SIMDJSON_CALL(jsonParser.iterate(json).get(doc));

  RuntimeFastJSONParser parser{rt, isASCII};

  auto result = parser.parseValue(doc);
  if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  return result->getHermesValue();
}

CallResult<HermesValue> runtimeFastJSONParse(
    Runtime &rt,
    Handle<StringPrimitive> jsonString,
    Handle<Callable> reviver) {
  // NOTE: StringPrimitives can move during JSON parsing (except if external)
  // so we can't use a pointer to it directly
  // and besides simdjson needs 64B of padding, so we always need to copy
  if (jsonString->isASCII()) {
    SmallXString<uint8_t, 8> storage;
    auto jsonStringView = StringPrimitive::createStringView(rt, jsonString);
    const char *ptr = jsonStringView.castToCharPtr();
    storage.append(ptr, ptr + jsonStringView.length());
    storage.resize(storage.size() + SIMDJSON_PADDING);

    auto json = padded_string_view(storage.begin(), jsonStringView.length(), storage.size());
    return RuntimeFastJSONParser::parse(rt, json, true);
  } else {
    auto stringRef = jsonString->getStringRef<char16_t>();
    auto utf8capacity = simdutf::utf8_length_from_utf16(stringRef.begin(), stringRef.size()) + SIMDJSON_PADDING;
    SmallXString<uint8_t, 8> utf8str;
    utf8str.resize(utf8capacity);

    // I'm not entirely sure if stringRef is still valid after the above allocations. Seems to work
    // (they're not Hermes allocations, could they even trigger GC?), but let's get new pointer
    // out of abundance of caution
    stringRef = jsonString->getStringRef<char16_t>();
    auto utf8size = simdutf::convert_utf16_to_utf8(stringRef.begin(), stringRef.size(), (char *) utf8str.begin());
    if (LLVM_UNLIKELY(!utf8size)) {
      return rt.raiseSyntaxError(TwineChar16("JSON fastParse (UTF transcoding) error"));
    }

    auto json = padded_string_view(utf8str.begin(), utf8size, utf8capacity);
    return RuntimeFastJSONParser::parse(rt, json, false);
  }
}

} // namespace vm
} // namespace hermes
