/*
  * Copyright (c) 2018 Allwinner Technology. All Rights Reserved.
  * Filename    wifid_ctrl.c
  * Author        laumy <liumingyuan@allwinnertech.com>
  * Version       0.0.1
  * Date           2018.11.05
  *
  */

#include <stdbool.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#include "tool.h"
#include "wifid_cmd.h"
#include "wifid_ctrl.h"

#define CLT_VERSION "181025"

struct da_ctl ctl = {
	.socket_created = false,
	.msg_pipe_fd = {-1,-1},
	.enable = false,
};
static int msg_pipe_close(void)
{
	int ret = 0;
	if(ctl.msg_pipe_fd[0] != -1) {
		ret = close(ctl.msg_pipe_fd[0]);
		if(ret != 0){
			wmg_printf(MSG_ERROR,"close msg pipe 0:%s\n",strerror(errno));
		}else {
			ctl.msg_pipe_fd[0] = -1;
		}
	}
	if(ctl.msg_pipe_fd[1] != -1) {
		ret = close(ctl.msg_pipe_fd[1]);
		if(ret != 0){
			wmg_printf(MSG_ERROR,"close msg pipe 1:%s\n",strerror(errno));
		}else {
			ctl.msg_pipe_fd[1] = -1;
		}
	}
	return ret ;
}
static int msg_pipe_write(const char *buffer,size_t len)
{
	int ret = -1;
	const uint8_t *head = (uint8_t *)buffer;
	do {
		ret = write(ctl.msg_pipe_fd[1],head,len);
		if(ret == -1) {
			if(errno == EINTR)
				continue;
			if(errno == EPIPE) {
				wmg_printf(MSG_ERROR,"FIFO endpoint has been closed: %d\n",ctl.msg_pipe_fd[1]);
				goto end;
			}
		}
		head += ret;
		len -= ret;
	} while (len != 0);

end:
	return ret;
}
static void wifid_msg_transport(const struct da_requst *r, const aw_wifi_interface_t *p_wifi_interface,int fd)
{
	enum cmd_status status = DA_CMD_SUCCESS;
	wmg_printf(MSG_DEBUG,"ctl msg fd reply start -+- \n");

	if (pipe(ctl.msg_pipe_fd) == -1) {
		wmg_printf(MSG_ERROR,"Couldn't create FIFO: %s", strerror(errno));
		status = DA_CODE_FAILED;
		goto final;
	}
	wmg_printf(MSG_DEBUG,"created pipe fd[0]=%d,fd[1]=%d\n",ctl.msg_pipe_fd[0],ctl.msg_pipe_fd[1]);

	union {
		char buf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr _align;
	} control_un;

	struct iovec io = { .iov_base = "", .iov_len = 1};
	struct msghdr msg = {
		.msg_iov = &io,
		.msg_iovlen = 1,
		.msg_control = control_un.buf,
		.msg_controllen = sizeof(control_un.buf),
	};
	struct cmsghdr *cmptr = CMSG_FIRSTHDR(&msg);
	cmptr->cmsg_level = SOL_SOCKET;
	cmptr->cmsg_type = SCM_RIGHTS;
	cmptr->cmsg_len = CMSG_LEN(sizeof(int));
	int *fdptr = (int *)CMSG_DATA(cmptr);

	/*pipe fd[1] must write, pipe fd[0] must read*/
	*fdptr = ctl.msg_pipe_fd[0];

	if (sendmsg(fd,&msg,0) == -1)
		goto fail;

	/*client will read this fd,so close it */
	close(*fdptr);
	ctl.msg_pipe_fd[0] = -1;
	goto final;

fail:
	wmg_printf(MSG_ERROR,"send ctl cmd msg fd failed\n");
	msg_pipe_close();
final:
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
	wmg_printf(MSG_DEBUG,"ctl msg fd reply end -+- \n");
}

static void wifid_connect(const struct da_requst *r, const aw_wifi_interface_t *p_wifi_interface,int fd)
{
	enum cmd_status status = DA_CMD_SUCCESS;
	enum cn_event event = DA_UNKNOWN;
	enum wmgEvent wmg_event;

	wmg_printf(MSG_DEBUG,"ctl command: connecet start -+- \n");

	if(NULL == p_wifi_interface)
		goto code_failed;

	p_wifi_interface->connect_ap(r->ssid,r->pwd,fd);

	if(aw_wifi_get_wifi_state() == NETWORK_CONNECTED){
		event = DA_CONNECTED;
	} else {
		wmg_event = aw_wifi_get_wifi_event();
		wmg_printf(MSG_DEBUG,"wifi manager event:%s\n",wmg_event_txt(wmg_event));
		switch(wmg_event){
			case WSE_CONNECTED_TIMEOUT:
				event = DA_CONNECTED_TIMEOUT;
				break;
			case WSE_AP_ASSOC_REJECT:
				event = DA_AP_ASSOC_REJECT;
				break;
			case WSE_NETWORK_NOT_EXIST:
				event = DA_NETWORK_NOT_FOUND;
				break;
			case WSE_PASSWORD_INCORRECT:
				event = DA_PASSWORD_INCORRECT;
				break;
			case WSE_OBTAINED_IP_TIMEOUT:
				event = DA_OBTAINED_IP_TIMEOUT;
				break;
			case WSE_DEV_BUSING:
				event = DA_DEV_BUSING;
				break;
			case WSE_CMD_OR_PARAMS_ERROR:
				event = DA_CMD_OR_PARAMS_ERROR;
				break;
			case WSE_KEYMT_NO_SUPPORT:
				event = DA_KEYMT_NO_SUPPORT;
				break;
			case WSE_WPA_TERMINATING:
			default:
				event  = DA_UNKNOWN;
				break;
		}
	}

	if(msg_pipe_write((char*)&event,sizeof(event)) >= 0)
		goto end;

code_failed:
	status = DA_CODE_FAILED;
end:
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
	wmg_printf(MSG_DEBUG,"ctl command: connecet end -+- \n");
}
static void wifid_net_status(const struct da_requst *r, const aw_wifi_interface_t *p_wifi_interface,int fd)
{
	enum cmd_status status = DA_CMD_SUCCESS;
	struct wifi_status s = {
		.state = STATE_UNKNOWN ,
		.ssid = {'\0'},
	};

	wmg_printf(MSG_DEBUG,"ctl cmd: status start -+- \n");

	if(NULL == p_wifi_interface)
		goto code_failed;

	if(p_wifi_interface->get_status(&s) < 0)
		goto code_failed;

	if(msg_pipe_write((char*)&s,sizeof(s)) >= 0)
		goto end;

code_failed:
	status = DA_CODE_FAILED;
end:
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
	wmg_printf(MSG_DEBUG,"ctl cmd: status end -+- \n");
}

static void wifid_scan(const struct da_requst *r, const aw_wifi_interface_t *p_wifi_interface,int fd)
{
	enum cmd_status status = DA_CMD_SUCCESS;
	char scan_results[SCAN_MAX] = {0};
	int len = SCAN_MAX;

	wmg_printf(MSG_DEBUG,"ctl cmd: scan start -+- \n");

	if(NULL == p_wifi_interface)
		goto code_failed;

	if( p_wifi_interface->get_scan_results(scan_results,&len) < 0)
		goto code_failed;

	if(msg_pipe_write(scan_results,len) >= 0)
		goto end;

code_failed:
	status = DA_CODE_FAILED;
end:
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
	wmg_printf(MSG_DEBUG,"ctl cmd: scan end -+- \n");

}

static void wifid_list_network(const struct da_requst *r, const aw_wifi_interface_t *p_wifi_interface, int fd)
{
	enum cmd_status status = DA_CMD_SUCCESS;
	char list_network_results[LIST_NETWORK_MAX] = {0};

	wmg_printf(MSG_DEBUG,"ctl cmd: list network start -+- \n");

	if(NULL == p_wifi_interface)
		goto code_failed;

	if(p_wifi_interface->list_networks(list_network_results,LIST_NETWORK_MAX,fd) < 0)
		goto code_failed;

	if(msg_pipe_write(list_network_results,LIST_NETWORK_MAX) >= 0)
		goto end;

code_failed:
	status = DA_CODE_FAILED;
end:
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
	wmg_printf(MSG_DEBUG,"ctl cmd: list network end -+- \n");

}
static void wifid_remove_net(const struct da_requst *r, const aw_wifi_interface_t *p_wifi_interface, int fd)
{
	enum cmd_status status = DA_CMD_SUCCESS;
	struct WmgStaEvt StaEvt = {CONNECTED,WSE_UNKNOWN};

	wmg_printf(MSG_DEBUG,"ctl command: disconnect start -+- \n");

	if(NULL == p_wifi_interface)
		goto code_failed;

	if( p_wifi_interface->clear_network(r->ssid) >= 0)
		goto end;

code_failed:
	status = DA_CODE_FAILED;
end:
	send(fd, &status, sizeof(status), MSG_NOSIGNAL);
	wmg_printf(MSG_DEBUG,"ctl command: disconnect end -+- \n");

}
void da_ctl_free(void)
{
	size_t i;
	for (i = 0; i < ARRAYSIZE(ctl.pfds); i++)
		if (ctl.pfds[i].fd != -1)
			close(ctl.pfds[i].fd);

	if (ctl.socket_created) {
		char tmp[256] = WIFIDAEMOIN_RUN_STATE_DIR "/";
		unlink(strcat(tmp, CLT_VERSION));
		ctl.socket_created = false;
	}
	ctl.enable = false;
}

static void wifid_close(const struct da_requst *r, const aw_wifi_interface_t *p_wifi_interface, int fd)
{
	da_ctl_free();
}


int wifi_daemon_ctl_init(void)
{
	size_t i;
	for (i = 0; i < ARRAYSIZE(ctl.pfds); i++) {
		ctl.pfds[i].events = POLLIN;
		ctl.pfds[i].fd = -1;
	}

	struct sockaddr_un saddr = { .sun_family = AF_UNIX };
	snprintf(saddr.sun_path, sizeof(saddr.sun_path) - 1,
			WIFIDAEMOIN_RUN_STATE_DIR "/%s", CLT_VERSION);

	if (mkdir(WIFIDAEMOIN_RUN_STATE_DIR, 0755) == -1 && errno != EEXIST)
		goto fail;
	if ((ctl.pfds[CTL_IDX_SRV].fd = socket(PF_UNIX, SOCK_SEQPACKET, 0)) == -1)
		goto fail;
	if (bind(ctl.pfds[CTL_IDX_SRV].fd, (struct sockaddr *)(&saddr), sizeof(saddr)) == -1)
		goto fail;
	ctl.socket_created = true;
	if (chmod(saddr.sun_path, 0660) == -1)
		goto fail;
	if (listen(ctl.pfds[CTL_IDX_SRV].fd, 2) == -1)
		goto fail;
	ctl.enable = true;
	return 0;

fail:
	da_ctl_free();
	return -1;
}

void ctl_loop(const aw_wifi_interface_t *p_wifi_interface)
{
	static void (*commands[__DA_COMMAND_MAX])(const struct da_requst *,const aw_wifi_interface_t * ,int) = {
		[DA_COMMAND_CONNECT] = wifid_connect,
		[DA_COMMAND_SCAN] = wifid_scan,
		[DA_COMMAND_REMOVE_NET] = wifid_remove_net,
		[DA_COMMAND_LIST_NETWOTK] = wifid_list_network,
		[DA_COMMAND_MSG_TRANSPORT] = wifid_msg_transport,
		[DA_COMMAND_NET_STATUS] = wifid_net_status,
	};

	wmg_printf(MSG_INFO,"Starting controller loop\n");
	while(ctl.enable){
		if (poll(ctl.pfds, ARRAYSIZE(ctl.pfds), -1) == -1) {
			if (errno == EINTR)
				continue;
			wmg_printf(MSG_ERROR,"Controller poll error: %s", strerror(errno));
			break;
		}

		struct pollfd *pfd = NULL;
		size_t i;

		/* handle data transmission with connected clients */
		for (i = __CTL_IDX_MAX; i < __CTL_IDX_MAX + WIFI_MAX_CLIENTS; i++) {
			const int fd = ctl.pfds[i].fd;

			if(fd == -1) {
				/*pointed to clt.pfds */
				pfd = &ctl.pfds[i];
				continue;
			}
			if (ctl.pfds[i].revents & POLLIN) {

				struct da_requst request;
				ssize_t len;
				if ((len = recv(fd, &request, sizeof(request), MSG_DONTWAIT)) != sizeof(request)) {
					/* if the request cannot be retrieved, release resources */
					if (len == 0)
						wmg_printf(MSG_DEBUG,"Client closed connection: %d\n", fd);
					else
						wmg_printf(MSG_DEBUG,"Invalid request length: %zd != %zd\n", len, sizeof(request));

					close(fd);
					ctl.pfds[i].fd = -1;

					/*when client is closed,pipe closed*/
					msg_pipe_close();
					wmg_printf(MSG_DEBUG,"+-+-+-+-+-+-+-+-+-+-+-+-+-+-\n\n");
					continue;
				}

				/* validate and execute requested command */
				if (request.command < __DA_COMMAND_MAX && commands[request.command] != NULL)
					commands[request.command](&request, p_wifi_interface,fd);
				else
					wmg_printf(MSG_WARNING,"Invalid command: %u\n", request.command);

			}

		}

		/* process new connections to our controller */
		if (ctl.pfds[CTL_IDX_SRV].revents & POLLIN && pfd != NULL) {

			struct pollfd fd = { -1, POLLIN, 0 };
			uint16_t ver = 0;
			fd.fd = accept(ctl.pfds[CTL_IDX_SRV].fd, NULL, NULL);
			wmg_printf(MSG_DEBUG,"+-+-+-+-+-+-+-+-+-+-+-+-+-+-\n");
			wmg_printf(MSG_DEBUG,"Received new connection: %d\n", fd.fd);
			//add poll waiting queue
			pfd->fd = fd.fd;
		}
	}
}
