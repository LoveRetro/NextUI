/*
  * Copyright (c) 2018 Allwinner Technology. All Rights Reserved.
  * Filename    wifid_cmd_iface.c
  * Author        laumy <liumingyuan@allwinnertech.com>
  * Version       0.0.1
  * Date           2018.11.05
  *
  */

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <netinet/in.h>

#include "tool.h"
#include "wifid_cmd.h"
#include "wifid_ctrl.h"
#include "wifid_cmd_iface.h"


#define CLT_CLIENT_VERSION "181025"

static int wifid_cmd_status(const enum cmd_status status) {
	switch (status) {
	case DA_CMD_SUCCESS:
		wmg_printf(MSG_DEBUG,"daemon reply code success\n");
		return 0;
	case DA_CODE_FAILED:
		wmg_printf(MSG_DEBUG,"daemon reply code error unkown\n");
		return -1;
	case DA_CMD_ERROR:
	default:
		wmg_printf(MSG_DEBUG,"daemon reply code failed\n");
		return -1;
	}
}

static int wifid_send_request(int fd, const struct da_requst *req_ptr) {

	enum cmd_status  status = { 0xAB };

	if (send(fd,req_ptr, sizeof(struct da_requst), MSG_NOSIGNAL) == -1){
		wmg_printf(MSG_ERROR,"send command %d error:%s\n",req_ptr->command,strerror(errno));
		return -1;
	}
	if (read(fd, &status, sizeof(status)) == -1){
		wmg_printf(MSG_ERROR,"read command %d error:%s\n",req_ptr->command,strerror(errno));
		return -1;
	}
	return wifid_cmd_status(status);
}

static int get_cmd_msg_transport(int fd)
{
	const struct da_requst req = {
		.command = DA_COMMAND_MSG_TRANSPORT,
		.ssid = {0},
		.pwd = {0},
	};
	char buf[256] = "";
	int msg_fd = -1;
	enum cmd_status  status = { 0xAB };

	struct iovec io = {
		.iov_base = &status,
		.iov_len = sizeof(status),
	};
	struct msghdr msg = {
		.msg_iov = &io,
		.msg_iovlen = 1,
		.msg_control = buf,
		.msg_controllen = sizeof(buf),
	};
	ssize_t len;

	if(fd <= 0){
		wmg_printf(MSG_ERROR,"damon socket fd is illegal\n");
		return -1;
	}
	wmg_printf(MSG_DEBUG,"get command message reply fd\n");

	if (send(fd,&req, sizeof(req), MSG_NOSIGNAL) == -1){
		wmg_printf(MSG_ERROR,"send command error:%s\n",strerror(errno));
		return -1;
	}

	if ((len = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC)) == -1)
		return -1;

	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	if (cmsg == NULL ||
			cmsg->cmsg_level == IPPROTO_IP ||
			cmsg->cmsg_type == IP_TTL) {
		/* in case of error, status message is returned */
		wmg_printf(MSG_ERROR,"cmsg error\n");
		return -1;
	}

	if (read(fd, &status, sizeof(status)) == -1){
		wmg_printf(MSG_ERROR,"read command error:%s\n",strerror(errno));
		return -1;
	}

	if(wifid_cmd_status(status) != 0)
		return -1;

	msg_fd = *((int *)CMSG_DATA(cmsg));
	wmg_printf(MSG_DEBUG,"get message reply fd:%d\n",msg_fd);
	return  msg_fd;
}

static int wifi_connect_daemon(const char *interface,struct client *c)
{
	int fd, err;

	struct sockaddr_un saddr = { .sun_family = AF_UNIX };
	snprintf(saddr.sun_path, sizeof(saddr.sun_path) - 1,
			WIFIDAEMOIN_RUN_STATE_DIR "/%s", interface);

	if ((fd = socket(PF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)) == -1)
		return -1;

	wmg_printf(MSG_DEBUG,"Connecting to socket: %s\n", saddr.sun_path);
	if (connect(fd, (struct sockaddr *)(&saddr), sizeof(saddr)) == -1) {
		err = errno;
		close(fd);
		errno = err;
		wmg_printf(MSG_ERROR,"wifi daemon has been closed,Please open wifi daemon first.\n", saddr.sun_path);
		return -1;
	}
	wmg_printf(MSG_DEBUG,"client fd :%d\n",fd);

	c->da_fd = fd;

	if(c->enable_pipe){
		c->pipe_fd = get_cmd_msg_transport(fd);
		if(c->pipe_fd < 0){
			wmg_printf(MSG_DEBUG,"get message pipe fd failed\n");
			return -1;
		}
	}
	return 0;
}

void handle_command_free(struct client *c)
{
	if(c->da_fd != -1)
		close(c->da_fd);
	if(c->enable_pipe && c->pipe_fd != -1)
		close(c->pipe_fd);
	wmg_printf(MSG_DEBUG,"disconnect from daemon\n");
}
int handle_command(struct da_requst *ptr_req,struct client *c)
{

	if(wifi_connect_daemon(CLT_CLIENT_VERSION,c) < 0)
		return -1;

	if(wifid_send_request(c->da_fd,ptr_req) < 0)
		return -1;

}
int read_command_message(int fd,char *buffer,int len)
{
	int ret = -1;
	while ((ret = read(fd, buffer,len)) == -1 && errno == EINTR)
		continue;
	if(ret == 0) {
		wmg_printf(MSG_DEBUG,"message FIFO has been closed:%d\n",fd);
	}
	if(errno == EBADF)
		ret = 0;
	return ret;
}
