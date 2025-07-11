/*
  * Copyright (c) 2018 Allwinner Technology. All Rights Reserved.
  * Filename    wifid_cmd_handle.c
  * Author        laumy <liumingyuan@allwinnertech.com>
  * Version       0.0.1
  * Date           2018.11.05
  *
  */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wifid_ctrl.h"
#include "wifid_cmd.h"
#include "wifid_cmd_iface.h"
#include "tool.h"
const char* connect_event_txt(enum cn_event event)
{
	switch (event) {
	case DA_CONNECTED:
		return "connected";
	case DA_PASSWORD_INCORRECT:
		return "password error";
	case DA_NETWORK_NOT_FOUND:
		return "network not found";
	case DA_CONNECTED_TIMEOUT:
		return "connected timeout";
	case DA_AP_ASSOC_REJECT:
		return "ap assoc reject";
	case DA_OBTAINED_IP_TIMEOUT:
		return "obtained ip timeout";
	case DA_DEV_BUSING:
		return "device busing";
	case DA_CMD_OR_PARAMS_ERROR:
		return "command or params error";
	case DA_KEYMT_NO_SUPPORT:
		return "keymt no support";
	default:
		return "unknown";
	}
}

int aw_wifid_get_scan_results(char *results,int len)
{
	struct da_requst req = {
		.command = DA_COMMAND_SCAN,
		.ssid = {0},
		.pwd = {0},
	};

	struct client cli = {
		.enable_pipe = true,
	};

	int ret = -1;
	if(handle_command(&req,&cli) < 0)
		return -1;
	if(cli.enable_pipe)
		ret = read_command_message(cli.pipe_fd,results,len);
	handle_command_free(&cli);
	return ret ;
}

int aw_wifid_get_status(struct wifi_status *sptr)
{
	struct da_requst req = {
		.command = DA_COMMAND_NET_STATUS,
		.ssid = {0},
		.pwd = {0},
	};
	struct client cli = {
		.enable_pipe = true,
	};

	int ret = -1;

	if(handle_command(&req,&cli) < 0)
		return -1;

	if(cli.enable_pipe){
		ret = read_command_message(cli.pipe_fd,(char*)sptr,sizeof(struct wifi_status));
	}
	handle_command_free(&cli);
	return ret ;
}

int aw_wifid_connect_ap(const char *ssid, const char *passwd,enum cn_event *ptrEvent)
{

	struct da_requst req = {
		.command = DA_COMMAND_CONNECT,
		.ssid = {0},
		.pwd = {0},
	};
	struct client cli = {
		.enable_pipe = true,
	};

	int ret = -1;

	if (NULL != ssid){
		strncpy(req.ssid,ssid,strlen(ssid));
	}

	if (NULL != passwd){
		strncpy(req.pwd,passwd,strlen(passwd));
	}

	if(handle_command(&req,&cli) < 0)
		return -1;

	if(cli.enable_pipe){
		ret = read_command_message(cli.pipe_fd,(char*)ptrEvent,sizeof(enum cn_event));
	}
	handle_command_free(&cli);
	return ret;
}

int aw_wifid_remove_networks(char *pssid,int len)
{
	struct da_requst req = {
		.command = DA_COMMAND_REMOVE_NET,
		.ssid = {0},
		.pwd = {0},
	};
	struct client cli = {
		.enable_pipe = false,
	};

	if (NULL != pssid){
		strncpy(req.ssid,pssid,len);
	}

	if(handle_command(&req,&cli) < 0)
		return -1;

	handle_command_free(&cli);
	return 0 ;
}

int aw_wifid_list_networks(char *reply, size_t len)
{
	struct da_requst req = {
		.command = DA_COMMAND_LIST_NETWOTK,
		.ssid = {0},
		.pwd = {0},
	};
	struct client cli = {
		.enable_pipe = true,
	};
	int ret = -1;
	if(handle_command(&req,&cli) < 0)
		return -1;
	ret = read_command_message(cli.pipe_fd,reply,len);
	handle_command_free(&cli);
	return ret ;

}

void aw_wifid_open(void)
{
	if (get_process_state("wifi_daemon",11) == -1){
		wmg_printf(MSG_DEBUG,"opening wifi daemon......\n");
		system("/etc/init.d/wifi_daemon start");
		sleep(2);
	} else {
		wmg_printf(MSG_INFO,"Wifi daemon is already open\n");
	}
}

void aw_wifid_close(void)
{
	wmg_printf(MSG_DEBUG,"closing wifi daemon......\n");
	system("/etc/init.d/wifi_daemon stop");
	sleep(1);
}
