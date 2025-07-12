#ifndef __WIFID_CMD_HANDLE_H_
#define __WIFID_CMD_HANDLE_H_

#if __cplusplus
extern "C" {
#endif

struct client {
	int da_fd;
	int pipe_fd;
	bool enable_pipe;
};

int read_command_message(int fd,char *buffer,int len);
int handle_command(struct da_requst *ptr_req,struct client *c);
void handle_command_free(struct client *c);

#if __cplusplus
};
#endif

#endif
