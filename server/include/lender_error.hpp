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

#include <string>

enum class LenderError {
    NOT_FOUND,
    INVALID_ARGUMENT,
    PERMISSION_DENIED,
    RESOURCE_EXHAUSTED,
    DEADLINE_EXCEEDED,
    FAILED_PRECONDITION,
    INTERNAL,
};

inline std::string lender_error_message(LenderError e)
{
    switch (e) {
    case LenderError::NOT_FOUND:
        return "not found";
    case LenderError::INVALID_ARGUMENT:
        return "invalid argument";
    case LenderError::PERMISSION_DENIED:
        return "permission denied";
    case LenderError::RESOURCE_EXHAUSTED:
        return "resource exhausted";
    case LenderError::DEADLINE_EXCEEDED:
        return "deadline exceeded";
    case LenderError::FAILED_PRECONDITION:
        return "failed precondition";
    case LenderError::INTERNAL:
        return "internal error";
    }
    return "unknown error";
}
