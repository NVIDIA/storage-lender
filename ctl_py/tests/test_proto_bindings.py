# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
# in compliance with the License. You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the License
# is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
# or implied. See the License for the specific language governing permissions and limitations under
# the License.
"""Smoke test for the generated management protocol bindings.

Fails when protoc output and the installed google.protobuf runtime disagree, which is the one
failure mode a pure import can catch before any ctl code exists.
"""

from storage_lender_ctl.proto import ctl_pb2, wire_pb2


def test_wire_envelope_round_trips() -> None:
    response = wire_pb2.Response(
        status_code=wire_pb2.PERMISSION_DENIED, error_message="root required"
    )
    parsed = wire_pb2.Response.FromString(response.SerializeToString())
    assert parsed.status_code == wire_pb2.PERMISSION_DENIED
    assert parsed.error_message == "root required"


def test_ctl_method_ids_match_the_proto_contract() -> None:
    assert ctl_pb2.GET_QUOTA_STATE == 1
    assert ctl_pb2.GET_COMMAND_LATENCY == 2
    assert ctl_pb2.MethodId.Name(ctl_pb2.GET_COMMAND_LATENCY) == "GET_COMMAND_LATENCY"
