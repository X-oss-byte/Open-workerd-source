// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/basics.h>
#include <workerd/io/worker-interface.capnp.h>

namespace workerd::api {

// The capability that lets us call remote methods over RPC.
// The client capability is dropped after each callRequest().
class JsRpcTargetImpl final : public rpc::JsRpcTarget::Server {
public:
  JsRpcTargetImpl(
      kj::Own<kj::PromiseFulfiller<void>> callFulfiller,
      IoContext& ctx,
      kj::Maybe<kj::StringPtr> entrypointName)
      : callFulfiller(kj::mv(callFulfiller)), ctx(ctx), entrypointName(entrypointName) {}

  ~JsRpcTargetImpl() {
    // The `GetJsRpcTarget` is waiting on the promise associated with callFulfiller; since we've
    // used the capability, we can fulfill the promise.
    callFulfiller->fulfill();
  }

  // Handles the delivery of JS RPC method calls.
  kj::Promise<void> call(CallContext context) override;

  KJ_DISALLOW_COPY_AND_MOVE(JsRpcTargetImpl);

private:
  kj::Own<kj::PromiseFulfiller<void>> callFulfiller;
  IoContext& ctx;
  kj::Maybe<kj::StringPtr> entrypointName;
};

} // namespace workerd::api
