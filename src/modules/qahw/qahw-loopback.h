/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version
 * 2.1 and only version 2.1 as published by the Free Software Foundation
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

#ifndef fooqahwpaloopbackhfoo
#define fooqahwpaloopbackhfoo

#include <pulsecore/core.h>
#include <pulsecore/card.h>

#include <qahw_api.h>
#include <qahw_defs.h>

void pa_qahw_loopback_init(qahw_module_handle_t *module_handle, pa_core *core, pa_card *card);
void pa_qahw_loopback_deinit(void);

#endif
