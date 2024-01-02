// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/util/thrift_rpc_helper.cpp

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "util/thrift_rpc_helper.h"

#include <sstream>

#include "common/status.h"
#include "gen_cpp/FrontendService.h"
#include "monotime.h"
#include "runtime/client_cache.h"
#include "runtime/exec_env.h"
#include "runtime/runtime_state.h"
#include "util/network_util.h"
#include "util/thrift_util.h"

namespace starrocks {

using apache::thrift::protocol::TProtocol;
using apache::thrift::protocol::TBinaryProtocol;
using apache::thrift::transport::TSocket;
using apache::thrift::transport::TTransport;
using apache::thrift::transport::TBufferedTransport;

ExecEnv* ThriftRpcHelper::_s_exec_env;
const char* ThriftRpcHelper::KInvalidMethodName = "Invalid method name";

void ThriftRpcHelper::setup(ExecEnv* exec_env) {
    _s_exec_env = exec_env;
}

template <typename T>
Status ThriftRpcHelper::rpc(const std::string& ip, const int32_t port,
                            std::function<void(ClientConnection<T>&)> callback, int timeout_ms) {
    TNetworkAddress address = make_network_address(ip, port);
    Status status;
    ClientConnection<T> client(_s_exec_env->get_client_cache<T>(), address, timeout_ms, &status);
    if (!status.ok()) {
        LOG(WARNING) << "Connect frontend failed, address=" << address << ", status=" << status.message();
        return status;
    }
    try {
        try {
            callback(client);
        } catch (apache::thrift::transport::TTransportException& e) {
            status = client.reopen(timeout_ms);
            if (!status.ok()) {
                LOG(WARNING) << "client reopen failed. address=" << address << ", status=" << status.message();
                return status;
            }
            callback(client);
        }
    } catch (apache::thrift::TException& e) {
        std::stringstream ss;
        ss << "call frontend service failed, address=" << address << ", reason=" << e.what();
        LOG(WARNING) << ss.str();
        SleepFor(MonoDelta::FromMilliseconds(config::thrift_client_retry_interval_ms * 2));
        // just reopen to disable this connection
        (void)client.reopen(timeout_ms);
        return Status::ThriftRpcError(ss.str());
    }
    return Status::OK();
}

template Status ThriftRpcHelper::rpc<FrontendServiceClient>(
        const std::string& ip, const int32_t port,
        std::function<void(ClientConnection<FrontendServiceClient>&)> callback, int timeout_ms);

template Status ThriftRpcHelper::rpc<BackendServiceClient>(
        const std::string& ip, const int32_t port,
        std::function<void(ClientConnection<BackendServiceClient>&)> callback, int timeout_ms);

template Status ThriftRpcHelper::rpc<TFileBrokerServiceClient>(
        const std::string& ip, const int32_t port,
        std::function<void(ClientConnection<TFileBrokerServiceClient>&)> callback, int timeout_ms);

} // namespace starrocks
