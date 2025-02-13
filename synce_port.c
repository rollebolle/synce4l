/**
 * @file synce_port.c
 * @brief Interface between synce device and port controller module.
 * @note SPDX-FileCopyrightText: Copyright 2022 Intel Corporation
 * @note SPDX-License-Identifier: GPL-2.0+
 */
#include <stdlib.h>
#include <errno.h>
#include <sys/queue.h>
#include <net/if.h>
#include <stdbool.h>
#include <linux/limits.h>

#include "util.h"
#include "synce_port.h"
#include "print.h"
#include "config.h"
#include "synce_port_ctrl.h"
#include "synce_msg.h"

enum port_mode {
	NON_SYNC_MODE = 0,
	SYNC_MODE,
};

enum port_state {
	PORT_UNKNOWN = 0,
	PORT_CREATED,
	PORT_INITED,
	PORT_RUNNING,
	PORT_FAILED,
	PORT_NOT_USED,
};

static int init_ext_tlv(struct synce_port *port, struct synce_msg_ext_ql *msg)
{
	memset(msg, 0, sizeof(*msg));

	if (generate_clock_identity(&msg->clockIdentity, port->name)) {
		pr_err("failed to generate a clock identity");
		return -ENXIO;
	}

	return 0;
}

static int ext_ql_msg_start_chain(struct synce_msg_ext_ql *ext_ql_msg)
{
	if (!ext_ql_msg) {
		pr_err("ext_ql_msg is NULL");
		return -EFAULT;
	}

	/* This is first node in chain */
	ext_ql_msg->cascaded_EEcs = 0;
	ext_ql_msg->cascaded_eEEcs = 1;

	return 0;
}

static int ext_ql_msg_update_chain(struct synce_port *port)
{
	int rx_ext_tlv = synce_port_ctrl_rx_ext_tlv(port->pc);

	if (rx_ext_tlv == 1) {
		/* if extended tlv came on best port just increase eEEC by one */
		port->ext_ql_msg.cascaded_eEEcs++;
	} else if (rx_ext_tlv == 0) {
		/* If extended tlv was not on the wire, the chain has just started.
		 * Previous known number of EEC is 1.
		 * This node is first eEEC in the new chain.
		 * The flags set accordingly.
		 * This behavior is described in the SyncE specification.
		 */
		port->ext_ql_msg.cascaded_EEcs = 1;
		port->ext_ql_msg.cascaded_eEEcs = 1;
		port->ext_ql_msg.flag |= (MIXED_EEC_CHAIN_FLAG |
					  PARTIAL_EEC_CHAIN_FLAG);
	} else {
		pr_err("failed rx_ext_tlv on %s", port->name);
		return -ENODEV;
	}

	return 0;
}

static int init_port_ql_val(struct synce_port *port, uint8_t forced_ql,
			    int network_option)
{
	if (!port) {
		pr_err("%s port is NULL", __func__);
		return -EFAULT;
	}

	port->ql_forced = forced_ql;
	port->ql_dnu = synce_get_dnu_value(network_option, false);
	port->ql = port->ql_dnu;

	return 0;
}

static int init_port_ext_ql(struct synce_port *port, uint8_t forced_ext_ql,
			    int network_option)
{
	int ret;

	if (!port) {
		pr_err("%s port is NULL", __func__);
		return -EFAULT;
	}

	ret = init_ext_tlv(port, &port->ext_ql_msg_dnu);
	if (ret) {
		pr_err("init ext_ql_msg_dnu failed on %s", port->name);
		return ret;
	}

	port->ext_ql_msg_dnu.enhancedSsmCode =
		synce_get_dnu_value(network_option, true);
	ret = ext_ql_msg_start_chain(&port->ext_ql_msg_dnu);
	if (ret) {
		pr_err("start chain failed on %s", port->name);
		return ret;
	}

	memcpy(&port->ext_ql_msg, &port->ext_ql_msg_dnu,
	       sizeof(port->ext_ql_msg));
	memcpy(&port->ext_ql_msg_forced, &port->ext_ql_msg,
	       sizeof(port->ext_ql_msg_forced));

	port->ext_ql_msg_forced.enhancedSsmCode = forced_ext_ql;

	return ret;
}

static int new_tx_ql_and_rebuild(struct synce_port *port, uint8_t ql,
				 struct synce_msg_ext_ql *ext_ql_msg)
{
	int ret = synce_port_ctrl_set_tx_ql(port->pc, ql);

	if (ret) {
		pr_err("set QL on %s failed", port->name);
		return ret;
	}

	if (ext_ql_msg) {
		ret = synce_port_ctrl_set_tx_ext_ql(port->pc,
						    ext_ql_msg);
		if (ret) {
			pr_err("set ext QL on %s failed", port->name);
			return ret;
		}
	}

	ret = synce_port_ctrl_rebuild_tx(port->pc);
	if (ret) {
		pr_err("set rebuild tx on %s failed", port->name);
	}

	return ret;
}

struct synce_port *synce_port_create(const char *port_name)
{
	struct synce_port *p = NULL;

	if (!port_name) {
		pr_err("%s failed - port_name not provided", __func__);
		return NULL;
	}

	p = malloc(sizeof(struct synce_port));
	if (!p) {
		pr_err("%s failed", __func__);
		return NULL;
	}
	memset(p, 0, sizeof(struct synce_port));
	memcpy(p->name, port_name, sizeof(p->name));
	p->state = PORT_CREATED;

	return p;
}

int synce_port_init(struct synce_port *port, struct config *cfg,
		    int network_option, int is_extended,
		    int rx_enabled, int recovery_time,
		    uint8_t forced_ql, uint8_t forced_ext_ql)
{
	int ret;

	if (!port) {
		pr_err("%s port is NULL", __func__);
		return -ENODEV;
	}

	if (port->state != PORT_CREATED) {
		goto err_port;
	}

	if (rx_enabled) {
		port->recover_clock_enable_cmd =
			config_get_string(cfg, port->name,
					  "recover_clock_enable_cmd");
		if (!port->recover_clock_enable_cmd) {
			pr_err("recover_clock_enable_cmd config not provided for %s",
			       port->name);
			goto err_port;
		}
		port->recover_clock_disable_cmd =
			config_get_string(cfg, port->name,
					  "recover_clock_disable_cmd");
		if (!port->recover_clock_disable_cmd) {
			pr_err("recover_clock_disable_cmd config not provided for %s",
			       port->name);
			goto err_port;
		}
	} else {
		port->recover_clock_enable_cmd = NULL;
		port->recover_clock_disable_cmd = NULL;
	}

	port->pc = synce_port_ctrl_create(port->name);
	if (!port->pc) {
		pr_err("port_ctrl create failed on %s", port->name);
		goto err_port;
	}

	ret = init_port_ql_val(port, forced_ql, network_option);
	if (ret) {
		pr_err("init port QL values failed on %s", port->name);
		return ret;
	}

	if (is_extended) {
		ret = init_port_ext_ql(port, forced_ext_ql, network_option);
		if (ret) {
			pr_err("init port ext QL values failed on %s",
			       port->name);
			return ret;
		}
	}

	ret = synce_port_ctrl_init(port->pc, cfg, rx_enabled, is_extended,
				   recovery_time, network_option);
	if (ret) {
		pr_err("port_ctrl init failed on port %s", port->name);
		return ret;
	}

	ret = synce_port_set_tx_ql_dnu(port, is_extended);
	if (ret) {
		pr_err("tlv init failed on port %s", port->name);
		return ret;
	}

	ret = synce_port_ctrl_enable_tx(port->pc);
	if (ret) {
		pr_err("enabled tx failed on port %s", port->name);
		return ret;
	}

	port->state = PORT_INITED;

	return ret;
err_port:
	port->state = PORT_FAILED;
	return -ENODEV;
}

void synce_port_destroy(struct synce_port *port)
{
	if (!port) {
		pr_err("%s port is NULL", __func__);
		return;
	}

	if (port->pc) {
		synce_port_ctrl_destroy(port->pc);
		free(port->pc);
	} else {
		pr_warning("%s pc is NULL", __func__);
	}
}

int synce_port_thread_running(struct synce_port *port)
{
	if (!port) {
		pr_err("%s port is NULL", __func__);
		return -EFAULT;
	}

	return synce_port_ctrl_running(port->pc);
}

int synce_port_rx_ql_failed(struct synce_port *port)
{
	if (!port) {
		pr_err("%s port is NULL", __func__);
		return -EFAULT;
	}

	return synce_port_ctrl_rx_ql_failed(port->pc);
}

int synce_port_rx_ql_changed(struct synce_port *port)
{
	if (!port) {
		pr_err("%s port is NULL", __func__);
		return -EFAULT;
	}

	return synce_port_ctrl_rx_ql_changed(port->pc);
}

int synce_port_set_tx_ql_dnu(struct synce_port *port, int extended)
{
	if (!port) {
		pr_err("%s port is NULL", __func__);
		return -EFAULT;
	}

	return new_tx_ql_and_rebuild(port, port->ql_dnu,
				     extended ?
				     &port->ext_ql_msg_dnu : NULL);
}

int synce_port_set_tx_ql_forced(struct synce_port *port, int extended)
{
	if (!port) {
		pr_err("%s port is NULL", __func__);
		return -EFAULT;
	}

	return new_tx_ql_and_rebuild(port, port->ql_forced,
				     extended ?
				     &port->ext_ql_msg_forced : NULL);
}

int synce_port_set_tx_ql_from_best_input(struct synce_port *port,
					 struct synce_port *best_p,
					 int extended)
{
	struct synce_msg_ext_ql rx_ext_ql_msg;
	int ret = -EFAULT;
	uint8_t rx_ql;

	if (!port) {
		pr_err("%s port is NULL", __func__);
		return ret;
	}

	if (!best_p) {
		pr_err("%s best_p is NULL", __func__);
		return ret;
	}

	ret = synce_port_ctrl_get_rx_ql(best_p->pc, &rx_ql);
	if (ret) {
		pr_err("get rx QL failed on %s", best_p->name);
		return ret;
	}
	port->ql = rx_ql;

	if (extended) {
		ret = synce_port_ctrl_get_rx_ext_ql(best_p->pc,
						    &rx_ext_ql_msg);
		if (ret) {
			pr_err("get ext rx QL failed on %s", best_p->name);
			return ret;
		}
		memcpy(&port->ext_ql_msg, &rx_ext_ql_msg,
		       sizeof(port->ext_ql_msg));
		ret = ext_ql_msg_update_chain(port);
		if (ret) {
			pr_err("failed to update chain on %s",
			       port->name);
			return ret;
		}
	}
	ret = new_tx_ql_and_rebuild(port, port->ql,
				    extended ? &port->ext_ql_msg : NULL);

	return ret;
}

int synce_port_is_rx_dnu(struct synce_port *port)
{
	if (!port) {
		pr_err("%s port is NULL", __func__);
		return -EFAULT;
	}

	return synce_port_ctrl_rx_dnu(port->pc, port->ql_dnu);
}

struct synce_port *synce_port_compare_ql(struct synce_port *left,
					 struct synce_port *right)
{
	struct synce_port_ctrl *best;

	best = synce_port_ctrl_compare_ql(left ? left->pc : NULL,
					  right ? right->pc : NULL);
	if (!best) {
		return NULL;
	}

	if (left && best == left->pc) {
		return left;
	} else if (right && best == right->pc) {
		return right;
	}

	return NULL;
}

const char *synce_port_get_name(struct synce_port *port)
{
	if (!port) {
		pr_err("%s port is NULL", __func__);
		return NULL;
	}

	return port->name;
}

int synce_port_enable_recover_clock(struct synce_port *port)
{
	if (!port) {
		pr_err("%s port is NULL", __func__);
		return -EINVAL;
	}

	if (!port->recover_clock_enable_cmd) {
		pr_err("recover_clock_enable_cmd is null on %s", port->name);
		return -EINVAL;
	}

	pr_debug("using recover_clock_enable_cmd: %s on %s",
		 port->recover_clock_enable_cmd, port->name);

	return system(port->recover_clock_enable_cmd);
}

int synce_port_disable_recover_clock(struct synce_port *port)
{
	if (!port) {
		pr_err("%s port is NULL", __func__);
		return -EINVAL;
	}

	if (!port->recover_clock_disable_cmd) {
		pr_err("recover_clock_disable_cmd is null on %s", port->name);
		return -EINVAL;
	}

	pr_debug("using recover_clock_disable_cmd: %s on %s",
		 port->recover_clock_disable_cmd, port->name);

	return system(port->recover_clock_disable_cmd);
}

void synce_port_invalidate_rx_ql(struct synce_port *port)
{
	synce_port_ctrl_invalidate_rx_ql(port->pc);
}
