/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "lender_error.hpp"
#include "wire.pb.h"

using StatusCode = nvidia::storage_lender::wire::v1::StatusCode;

inline StatusCode to_status_code(LenderError e)
{
    switch (e) {
    case LenderError::NOT_FOUND:
        return StatusCode::NOT_FOUND;
    case LenderError::INVALID_ARGUMENT:
        return StatusCode::INVALID_ARGUMENT;
    case LenderError::PERMISSION_DENIED:
        return StatusCode::PERMISSION_DENIED;
    case LenderError::RESOURCE_EXHAUSTED:
        return StatusCode::RESOURCE_EXHAUSTED;
    case LenderError::DEADLINE_EXCEEDED:
        return StatusCode::DEADLINE_EXCEEDED;
    case LenderError::FAILED_PRECONDITION:
        return StatusCode::FAILED_PRECONDITION;
    case LenderError::INTERNAL:
        return StatusCode::INTERNAL;
    }
    return StatusCode::INTERNAL;
}
