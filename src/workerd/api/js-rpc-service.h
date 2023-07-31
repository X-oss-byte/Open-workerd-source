// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/js-rpc-target.h>

namespace workerd::api {

class GetJsRpcTargetEvent final: public ExtendableEvent {
public:
  explicit GetJsRpcTargetEvent();

  static jsg::Ref<GetJsRpcTargetEvent> constructor(kj::String type) = delete;

  JSG_RESOURCE_TYPE(GetJsRpcTargetEvent) {
    JSG_INHERIT(ExtendableEvent);
  }
};

// `getJsRpcTarget` returns a capability that provides the client a way to call remote methods
// over RPC. We drain the IncomingRequest after the capability is used to run the relevant JS.
class GetJsRpcTargetCustomEventImpl final: public WorkerInterface::CustomEvent {
public:
  GetJsRpcTargetCustomEventImpl(uint16_t typeId)
    : paf(kj::newPromiseAndFulfiller<rpc::JsRpcTarget::Client>()),
      capFulfiller(kj::mv(paf.fulfiller)),
      clientCap(kj::mv(paf.promise)),
      typeId(typeId) {}

  kj::Promise<Result> run(
      kj::Own<IoContext::IncomingRequest> incomingRequest,
      kj::Maybe<kj::StringPtr> entrypointName) override;

  kj::Promise<Result> sendRpc(
      capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      kj::TaskSet& waitUntilTasks,
      rpc::EventDispatcher::Client dispatcher) override;

  uint16_t getType() override {
    return typeId;
  }

  kj::PromiseFulfillerPair<rpc::JsRpcTarget::Client> paf;
  kj::Own<kj::PromiseFulfiller<workerd::rpc::JsRpcTarget::Client>> capFulfiller;

  // We need to set the client/server capability on the event itself to get around CustomEvent's
  // limited return type.
  kj::Promise<rpc::JsRpcTarget::Client> clientCap;
private:
  uint16_t typeId;
};

#define EW_JS_RPC_SERVICE_ISOLATE_TYPES   \
  api::GetJsRpcTargetEvent,               \
  api::GetJsRpcTargetExportedHandler
}  // namespace workerd::api
