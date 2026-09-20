/*
 * MiBee Cam v0.1 — ONVIF 板级适配层接口
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ONVIF_PORT_H
#define ONVIF_PORT_H

#include "esp_err.h"

/** 启动 ONVIF（组件注册 + WS-Discovery + mDNS）。main.c STA 连接后调用。 */
esp_err_t onvif_port_start(void);

#endif /* ONVIF_PORT_H */
