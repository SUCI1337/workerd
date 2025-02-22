// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/worker-rpc.h>
#include <workerd/io/features.h>
#include <workerd/api/global-scope.h>
#include <workerd/jsg/ser.h>

namespace workerd::api {

namespace {
kj::Array<kj::byte> serializeV8(jsg::Lock& js, jsg::JsValue value) {
  jsg::Serializer serializer(js, jsg::Serializer::Options {
    .version = 15,
    .omitHeader = false,
  });
  serializer.write(js, value);
  return serializer.release().data;
}

jsg::JsValue deserializeV8(jsg::Lock& js, kj::ArrayPtr<const kj::byte> ser) {
  jsg::Deserializer deserializer(js, ser, kj::none, kj::none,
      jsg::Deserializer::Options {
    .version = 15,
    .readHeader = true,
  });

  return deserializer.readValue(js);
}
} // namespace

kj::Promise<capnp::Response<rpc::JsRpcTarget::CallResults>> WorkerRpc::sendWorkerRpc(
    jsg::Lock& js,
    kj::StringPtr name,
    const v8::FunctionCallbackInfo<v8::Value>& args) {

  auto& ioContext = IoContext::current();
  auto worker = getClient(ioContext, kj::none, "getJsRpcTarget"_kjc);
  auto event = kj::heap<api::GetJsRpcTargetCustomEventImpl>(WORKER_RPC_EVENT_TYPE);

  rpc::JsRpcTarget::Client client = event->getCap();
  auto builder = client.callRequest();
  builder.setMethodName(name);

  kj::Vector<jsg::JsValue> argv(args.Length());
  for (int n = 0; n < args.Length(); n++) {
    argv.add(jsg::JsValue(args[n]));
  }

  // If we have arguments, serialize them.
  // Note that we may fail to serialize some element, in which case this will throw back to JS.
  if (argv.size() > 0) {
    auto ser = serializeV8(js, js.arr(argv.asPtr()));
    // TODO(soon): We will drop this requirement once we support streaming.
    JSG_ASSERT(ser.size() < MAX_JS_RPC_MESSAGE_SIZE, Error,
        "Serialized RPC request is too large: ", ser.size(), " <= ", MAX_JS_RPC_MESSAGE_SIZE);
    builder.initSerializedArgs().setV8Serialized(kj::mv(ser));
  }

  auto callResult = builder.send();
  auto customEventResult = worker->customEvent(kj::mv(event));

  // If customEvent throws, we'll cancel callResult and propagate the exception. Otherwise, we'll
  // just wait until callResult finishes.
  co_return co_await callResult.exclusiveJoin(customEventResult
      .then([](auto&&) -> kj::Promise<capnp::Response<rpc::JsRpcTarget::CallResults>> {
        return kj::NEVER_DONE;
      }));
}

kj::Maybe<jsg::JsValue> WorkerRpc::getNamed(jsg::Lock& js, kj::StringPtr name) {
  // Named intercept is enabled, this means we won't default to legacy behavior.
  // The return value of the function is a promise that resolves once the remote returns the result
  // of the RPC call.
  return jsg::JsValue(js.wrapReturningFunction(js.v8Context(), [this, methodName=kj::str(name)]
      (jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& args) {
        auto& ioContext = IoContext::current();
        // Wait for the RPC to resolve and then process the result.
        return js.wrapSimplePromise(ioContext.awaitIo(js, sendWorkerRpc(js, methodName, args),
            [](jsg::Lock& js, auto result) -> jsg::Value {
          return jsg::Value(js.v8Isolate, deserializeV8(js, result.getResult().getV8Serialized()));
        }));
      }
  ));
}

// The capability that lets us call remote methods over RPC.
// The client capability is dropped after each callRequest().
class JsRpcTargetImpl final : public rpc::JsRpcTarget::Server {
public:
  JsRpcTargetImpl(
      kj::Own<kj::PromiseFulfiller<void>> callFulfiller,
      IoContext& ctx,
      kj::Maybe<kj::StringPtr> entrypointName)
      : callFulfiller(kj::mv(callFulfiller)), ctx(ctx), entrypointName(entrypointName) {}

  // Handles the delivery of JS RPC method calls.
  kj::Promise<void> call(CallContext callContext) override {
    auto methodName = kj::heapString(callContext.getParams().getMethodName());
    auto serializedArgs = callContext.getParams().getSerializedArgs().getV8Serialized().asBytes();

    // We want to fulfill the callPromise so customEvent can continue executing
    // regardless of the outcome of `call()`.
    KJ_DEFER(callFulfiller->fulfill(););

    // Try to execute the requested method.
    co_return co_await ctx.run(
        [this, methodName=kj::mv(methodName), serializedArgs=kj::mv(serializedArgs), callContext]
        (Worker::Lock& lock) mutable -> kj::Promise<void> {

      jsg::Lock& js = lock;
      // JS RPC is not enabled on the server side, we cannot call any methods.
      JSG_REQUIRE(FeatureFlags::get(js).getJsRpc(), TypeError,
          "The receiving Worker does not allow its methods to be called over RPC.");

      auto& handler = KJ_REQUIRE_NONNULL(lock.getExportedHandler(entrypointName, ctx.getActor()),
                                         "Failed to get handler to worker.");
      auto handle = handler.self.getHandle(lock);

      // We will try to get the function, if we can't we'll throw an error to the client.
      auto fn = tryGetFn(lock, ctx, handle, methodName);

      // We have a function, so let's call it and serialize the result for RPC.
      // If the function returns a promise we will wait for the promise to finish so we can
      // serialize the result.
      return ctx.awaitJs(js, js.toPromise(invokeFn(js, fn, handle, serializedArgs))
          .then(js, ctx.addFunctor([callContext](jsg::Lock& js, jsg::Value value) mutable {
        auto result = serializeV8(js, jsg::JsValue(value.getHandle(js)));
        // TODO(soon): We will drop this requirement once we support streaming.
        JSG_ASSERT(result.size() < MAX_JS_RPC_MESSAGE_SIZE, Error,
            "Serialized RPC response is too large: ", result.size(),
            " <= ", MAX_JS_RPC_MESSAGE_SIZE);
        auto builder = callContext.initResults(capnp::MessageSize { result.size() / 8 + 8, 0 });
        builder.initResult().setV8Serialized(kj::mv(result));
      })));
    });
  }

  KJ_DISALLOW_COPY_AND_MOVE(JsRpcTargetImpl);

private:
  // The following names are reserved by the Workers Runtime and cannot be called over RPC.
  bool isReservedName(kj::StringPtr name) {
    if (name == "fetch" ||
        name == "connect" ||
        name == "alarm" ||
        name == "webSocketMessage" ||
        name == "webSocketClose" ||
        name == "webSocketError") {
      return true;
    }
    return false;
  }

  // If the `methodName` is a known public method, we'll return it.
  inline v8::Local<v8::Function> tryGetFn(
      Worker::Lock& lock,
      IoContext& ctx,
      v8::Local<v8::Object> handle,
      kj::StringPtr methodName) {
    auto methodStr = jsg::v8StrIntern(lock.getIsolate(), methodName);
    auto fnHandle = jsg::check(handle->Get(lock.getContext(), methodStr));

    jsg::Lock& js(lock);
    v8::Local<v8::Object> obj = js.obj();
    auto objProto = obj->GetPrototype().As<v8::Object>();

    // Get() will check the Object and the prototype chain. We want to verify that the function
    // we intend to call is not the one defined on the Object prototype.
    bool isImplemented = fnHandle != jsg::check(objProto->Get(js.v8Context(), methodStr));

    JSG_REQUIRE(isImplemented && fnHandle->IsFunction(), TypeError,
        kj::str("The RPC receiver does not implement the method \"", methodName, "\"."));
    JSG_REQUIRE(!isReservedName(methodName), TypeError,
        kj::str("'", methodName, "' is a reserved method and cannot be called over RPC."));
    return fnHandle.As<v8::Function>();
  }

  // Deserializes the arguments and passes them to the given function.
  v8::Local<v8::Value> invokeFn(
      jsg::Lock& js,
      v8::Local<v8::Function> fn,
      v8::Local<v8::Object> thisArg,
      kj::ArrayPtr<const kj::byte> serializedArgs) {
    // We received arguments from the client, deserialize them back to JS.
    if (serializedArgs.size() > 0) {
      auto args = KJ_REQUIRE_NONNULL(
          deserializeV8(js, serializedArgs).tryCast<jsg::JsArray>(),
          "expected JsArray when deserializing arguments.");
      // Call() expects a `Local<Value> []`... so we populate an array.
      KJ_STACK_ARRAY(v8::Local<v8::Value>, arguments, args.size(), 8, 8);
      for (size_t i = 0; i < args.size(); ++i) {
        arguments[i] = args.get(js, i);
      }
      return jsg::check(fn->Call(js.v8Context(), thisArg, args.size(), arguments.begin()));
    } else {
      return jsg::check(fn->Call(js.v8Context(), thisArg, 0, nullptr));
    }
  };

  // We use the callFulfiller to let the custom event know we've finished executing the method.
  kj::Own<kj::PromiseFulfiller<void>> callFulfiller;
  IoContext& ctx;
  kj::Maybe<kj::StringPtr> entrypointName;
};

kj::Promise<WorkerInterface::CustomEvent::Result> GetJsRpcTargetCustomEventImpl::run(
    kj::Own<IoContext::IncomingRequest> incomingRequest,
    kj::Maybe<kj::StringPtr> entrypointName) {
  incomingRequest->delivered();
  auto [callPromise, callFulfiller] = kj::newPromiseAndFulfiller<void>();
  capFulfiller->fulfill(kj::heap<JsRpcTargetImpl>(
      kj::mv(callFulfiller), incomingRequest->getContext(), entrypointName));

  // `callPromise` resolves once `JsRpcTargetImpl::call()` (invoked by client) completes.
  co_await callPromise;
  co_await incomingRequest->drain();
  co_return WorkerInterface::CustomEvent::Result {
    .outcome = EventOutcome::OK
  };
}

kj::Promise<WorkerInterface::CustomEvent::Result>
  GetJsRpcTargetCustomEventImpl::sendRpc(
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory,
    kj::TaskSet& waitUntilTasks,
    rpc::EventDispatcher::Client dispatcher) {
  auto req = dispatcher.getJsRpcTargetRequest();
  auto sent = req.send();
  this->capFulfiller->fulfill(sent.getServer());

  auto resp = co_await sent;
  co_return WorkerInterface::CustomEvent::Result {
    .outcome = EventOutcome::OK
  };
}
}; // namespace workerd::api
