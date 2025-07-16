#ifndef __AW_BT_TEST_DEV_H
#define __AW_BT_TEST_DEV_H

#include <pthread.h>
#include <stdbool.h>
#include "bt_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dev_node_t {
	struct dev_node_t *front;
	struct dev_node_t *next;
	char dev_name[MAX_BT_NAME_LEN + 1];
	char dev_addr[MAX_BT_ADDR_LEN + 1];
} dev_node_t;

typedef struct dev_list_t {
	dev_node_t *head;
	dev_node_t *tail;
	int length;
	bool list_cleared;
	pthread_mutex_t lock;
} dev_list_t;

int btmg_dev_list_add_device(dev_list_t *dev_list,const char *name,const char *addr);
dev_node_t *btmg_dev_list_find_device(dev_list_t *dev_list, const char *addr);
bool btmg_dev_list_remove_device(dev_list_t *dev_list, const char *addr);
dev_list_t *btmg_dev_list_new();
void btmg_dev_list_clear(dev_list_t *list);
void btmg_dev_list_free(dev_list_t *list);

#ifdef __cplusplus
}; /*extern "C"*/
#endif

#endif
