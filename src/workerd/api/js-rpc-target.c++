// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/js-rpc-target.h>
#include <workerd/api/global-scope.h>

namespace workerd::api {

kj::Promise<void> JsRpcTargetImpl::call(CallContext callContext) {
  auto methodName = kj::heapString(callContext.getParams().getMethodName());
  // TODO(now): Deserialize the arguments.
  //    - Current demo doesn't take args, once we pass args on the client side I'll deserialize
  //      on the server side.
  auto serializedArgs = callContext.getParams().getSerializedArgs().getV8Serialized().asBytes();

  // Try to execute the requested method.
  try {
    co_await ctx.run(
        [this,
         methodName=kj::mv(methodName),
         serializedArgs = kj::mv(serializedArgs),
         entrypointName = entrypointName,
         &callContext] (Worker::Lock& lock) mutable {

      auto reader = callContext.initResults(capnp::MessageSize { 4, 1 }).initResult();

      auto handler = lock.getExportedHandler(entrypointName, ctx.getActor());
      KJ_IF_MAYBE(h, handler) {
        jsg::Lock& js = lock;

        auto handle = h->self.getHandle(lock);
        auto methodStr = jsg::v8StrIntern(lock.getIsolate(), methodName);
        auto fnHandle = jsg::check(handle->Get(lock.getContext(), methodStr));
        if (!(fnHandle->IsFunction() && !fnHandle->IsPrivate())) {
          auto errString = "The RPC receiver does not implement the requested method."_kj;
          reader.setV8Serialized(
              jsg::serializeV8Rpc(js,
              jsg::JsValue(js.v8Error(errString))));
        } else {
          auto fn = fnHandle.As<v8::Function>();
          // TODO(now): Currently, no args are passed. Once we get around to sending them from the
          // client and deserializing here, we should pass them to the function as well.
          // TODO(now): How do we differentiate throwing an error from JS vs. returning one?
          auto result = jsg::check(fn->Call(lock.getContext(), fn, 0, nullptr));

          // Set rpc call response.
          reader.setV8Serialized(jsg::serializeV8Rpc(js, jsg::JsValue(result)));
        }
      } else {
        // TODO(now): What does this mean? Are we not an actor?
        KJ_FAIL_REQUIRE("Failed to get handler to worker.");
      }
      return kj::READY_NOW; });
  } catch(kj::Exception e) {
    if (auto desc = e.getDescription();
        !jsg::isTunneledException(desc) && !jsg::isDoNotLogException(desc)) {
      LOG_EXCEPTION("JsRpcTargetCall"_kj, e);
    }
  }
  co_return;
}

}
