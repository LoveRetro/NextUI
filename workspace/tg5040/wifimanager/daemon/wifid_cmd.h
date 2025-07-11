#ifndef __WIFID_CMD_H_
#define __WIFID_CMD_H_

#if __cplusplus
extern "C" {
#endif

#include <wifi_intf.h>

enum cn_event {
	DA_CONNECTED,
	DA_PASSWORD_INCORRECT,
	DA_NETWORK_NOT_FOUND,
	DA_CONNECTED_TIMEOUT,
	DA_AP_ASSOC_REJECT,
	DA_OBTAINED_IP_TIMEOUT,
	DA_DEV_BUSING,
	DA_CMD_OR_PARAMS_ERROR,
	DA_KEYMT_NO_SUPPORT,
	DA_UNKNOWN,
};

#define SCAN_MAX 4096
#define LIST_NETWORK_MAX 4096

int aw_wifid_connect_ap(const char *ssid, const char *passwd,enum cn_event *ptrEvent);
int aw_wifid_get_scan_results(char *results,int len);
int aw_wifid_list_networks(char *reply, size_t len);
int aw_wifid_get_status(struct wifi_status *sptr);
int aw_wifid_remove_networks(char *pssid,int len);
const char* connect_event_txt(enum cn_event event);
void aw_wifid_open(void);
void aw_wifid_close(void);


#if __cplusplus
};
#endif

#endif
