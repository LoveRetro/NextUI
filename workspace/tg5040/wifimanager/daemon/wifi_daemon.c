/*
  * Copyright (c) 2018 Allwinner Technology. All Rights Reserved.
  * Filename    wifi_daemon.c
  * Author        laumy
  * Version       0.0.1
  * Date           2018.11.05
  *
  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <wifi_intf.h>
#include <pthread.h>
#include <getopt.h>


#include "wmg_debug.h"
#include "wifid_ctrl.h"
#include "wifi_udhcpc.h"

#define TRY_CONNECT_WPA_MAX_TIME 5

static void wifi_state_handle(struct Manager *w, int event_label)
{
    wmg_printf(MSG_DEBUG,"event_label 0x%x\n", event_label);

    switch(w->StaEvt.state)
    {
		 case CONNECTING:
		 {
			 wmg_printf(MSG_INFO,"Connecting to the network......\n");
			 break;
		 }
		 case CONNECTED:
		 {
			 wmg_printf(MSG_INFO,"Connected to the AP\n");
			 start_udhcpc();
			 break;
		 }

		 case OBTAINING_IP:
		 {
			 wmg_printf(MSG_INFO,"Getting ip address......\n");
			 break;
		 }

		 case NETWORK_CONNECTED:
		 {
			 wmg_printf(MSG_INFO,"Successful network connection\n");
			 break;
		 }
		case DISCONNECTED:
		{
		    wmg_printf(MSG_ERROR,"Disconnected,the reason:%s\n",wmg_event_txt(w->StaEvt.event));
		    break;
		}
    }
}
static void ctl_loop_stop(int sig) {
	/* Call to this handler restores the default action, so on the
	 * second call the program will be forcefully terminated. */
	struct sigaction sigact = { .sa_handler = SIG_DFL };
	sigaction(sig, &sigact, NULL);
	da_ctl_free();
}
static void usage()
{
	printf("  -s = log output to syslog instead of stdout\n");
	printf("  -f = log output to debug file instead of stdout\n");
	printf("  -d = increase debugging verbosity (-dd even more)\n");
	printf("  -h = show this help text\n");
}

int main(int argc, char *argv[]){
    int ret = 0, len = 0;
    int times = 0, event_label = 0;;
    char ssid[256] = {0}, scan_results[4096] = {0};
	int debug_syslog = 0;
	const char *debug_file_path = NULL;
	int debug_level = 0;
	int c;

	int i,connect_wpa_time = 0;

    const aw_wifi_interface_t *p_wifi_interface = NULL;
	for (;;) {
		c = getopt(argc, argv,"hf:sd");
		if(c < 0)
			break;
		switch(c){
			case 'h':
				usage();
				return 0;
			case 'f':
				debug_file_path = optarg;
				break;
			case 's':
				debug_syslog ++;
				break;
			case 'd':
				debug_level ++;
				break;
			default:
				usage();
				return 0;
		}
	}
	if (NULL != debug_file_path) {
		printf("debug output file:%s\n",debug_file_path);
		wmg_debug_open_file(debug_file_path);
	}
	if (debug_syslog)
		wmg_debug_open_syslog();

	printf("debug levle:%d\n",MSG_INFO+debug_level);

	wmg_set_debug_level(MSG_INFO+debug_level);
	if(wifi_daemon_ctl_init() < 0){
		wmg_printf(MSG_ERROR,"Failed to start wifi daemon.\n");
		return 0;
	}

    event_label = rand();
	for (i = 0 ; i<= TRY_CONNECT_WPA_MAX_TIME ;i++) {
		p_wifi_interface = aw_wifi_on(wifi_state_handle, event_label);
		if(p_wifi_interface != NULL) {
			wmg_printf(MSG_DEBUG,"connect wpa_supplicant Successful.\n");
			break;
		}
		ms_sleep(1000);
		wmg_printf(MSG_DEBUG,"try connect wpa_supplicant :%d times\n",i+1);
	}
	if(p_wifi_interface == NULL){
	    wmg_printf(MSG_ERROR,"wifi on failed\n");
		goto failed;
	}
	/* In order to receive EPIPE while writing to the pipe whose reading end
	 * is closed, the SIGPIPE signal has to be handled. For more information
	 * see the msg_pipe_write() function. */
	struct sigaction sigact = { .sa_handler = SIG_IGN };
	sigaction(SIGPIPE, &sigact, NULL);

	/* free ctl resource */
	sigact.sa_handler = ctl_loop_stop;
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGINT, &sigact, NULL);

	ctl_loop(p_wifi_interface);
	wmg_printf(MSG_DEBUG,"Exiting ctl loop\n");
    return 0;
failed:
	da_ctl_free();
	return -1;
}
