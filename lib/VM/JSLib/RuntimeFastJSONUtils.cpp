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
  for (auto valueRes : array) {
    ondemand::value value;
    error = valueRes.get(value);
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

template<typename T>
CallResult<HermesValue> parseValue(Runtime &rt, T &value) {
  simdjson::error_code error;

  ondemand::json_type type;
  error = value.type().get(type);
  switch (type) {
    case ondemand::json_type::array: {
      ondemand::array arrayValue;
      error = value.get(arrayValue);
      return parseArray(rt, arrayValue);
    }
  //   case ondemand::json_type::object:
    case ondemand::json_type::number:
      double doubleValue;
      error = value.get(doubleValue);
      return HermesValue::encodeDoubleValue(doubleValue);
    case ondemand::json_type::string: {
      std::string_view stringView;
      error = value.get(stringView);
      UTF8Ref hermesStr{(const uint8_t*)stringView.data(), stringView.size()};
      return StringPrimitive::createEfficient(rt, hermesStr);
    }
    case ondemand::json_type::boolean:
      bool boolValue;
      error = value.get(boolValue);
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
  // TODO: GC safety?
  // TODO: Support UTF-16
  auto asciiRef = jsonString->getStringRef<char>();
  auto json = padded_string(asciiRef.data(), asciiRef.size());

  ondemand::document doc;
  error = parser.iterate(json).get(doc);

  return parseValue(runtime, doc);
}

} // namespace vm
} // namespace hermes
