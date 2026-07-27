/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t hosted_wifi_init(void);
bool hosted_wifi_is_connected(void);
