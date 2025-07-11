#ifndef __WIFID_CTL_H_
#define __WIFID_CTL_H_

#if __cplusplus
extern "C" {
#endif
#include <poll.h>
#include <wifi_intf.h>

enum da_command {
	DA_COMMAND_CONNECT,
	DA_COMMAND_SCAN,
	DA_COMMAND_REMOVE_NET,
	DA_COMMAND_LIST_NETWOTK,
	DA_COMMAND_MSG_TRANSPORT,
	DA_COMMAND_NET_STATUS,
	__DA_COMMAND_MAX
};

enum cmd_status {
	DA_CMD_SUCCESS,
	DA_CMD_ERROR,
	DA_CODE_FAILED,
};

struct da_requst {
	enum da_command command;
	char ssid[64];
	char pwd[48];
};

#define ARRAYSIZE(a) (sizeof(a) / sizeof(*(a)))

#define WIFIDAEMOIN_RUN_STATE_DIR "/var/run/wifidaemon"


/* Indexes of special file descriptors in the poll array. */
#define CTL_IDX_SRV 0
#define __CTL_IDX_MAX 1

#define WIFI_MAX_CLIENTS 1

struct da_ctl {
	bool socket_created;
	struct pollfd pfds[__CTL_IDX_MAX + WIFI_MAX_CLIENTS];
	int msg_pipe_fd[2];
	bool enable;
};

void da_ctl_free(void);
int wifi_daemon_ctl_init(void);
void ctl_loop(const aw_wifi_interface_t *p_wifi_interface);

#if __cplusplus
};
#endif

#endif
