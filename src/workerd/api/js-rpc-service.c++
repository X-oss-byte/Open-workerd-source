// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "js-rpc-service.h"

namespace workerd::api {

GetJsRpcTargetEvent::GetJsRpcTargetEvent()
    : ExtendableEvent("getJsRpcTarget") {};

kj::Promise<WorkerInterface::CustomEvent::Result> GetJsRpcTargetCustomEventImpl::run(
    kj::Own<IoContext::IncomingRequest> incomingRequest,
    kj::Maybe<kj::StringPtr> entrypointName) {
  incomingRequest->delivered();
  auto targetPaf = kj::newPromiseAndFulfiller<void>();
  auto callPromise = kj::mv(targetPaf.promise);
  capFulfiller->fulfill(kj::heap<JsRpcTargetImpl>(
      kj::mv(targetPaf.fulfiller), incomingRequest->getContext(), entrypointName));

  incomingRequest->getContext().addWaitUntil(callPromise.attach(
      kj::defer([incomingRequest=kj::mv(incomingRequest)]() mutable {
    // We can drain once our RPC call completes.
    auto& context = incomingRequest->getContext();
    context.addWaitUntil(incomingRequest->drain().attach(kj::mv(incomingRequest)));
  })));

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
  this->capFulfiller->fulfill(req.send().getServer());
  return WorkerInterface::CustomEvent::Result {
    .outcome = EventOutcome::OK
  };
}

}  // namespace workerd::api
