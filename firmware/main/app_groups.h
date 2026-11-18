#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Groups: a named set of friend boxes that share one group key.
//   Create: this box makes a random 16-byte key and sends it to each member, sealed with
//           that member's friend key:   K|to|from|sealed(gid|name|keyhex|AAAA,BBBB,...)
//           The member answers  L|owner|me|gid  to confirm the key. Without an answer the key is sent up to 5 times.
//   Talk:   Q|gid|from|sealed-with-group-key(seq|devicename|from|text)   (gid and sender box are authenticated too)
// Only boxes that hold the group key can read group messages. Only a friend can add this box to a group.

#define MAX_GROUPS   8
#define GROUP_MAX_MEMBERS 8          // Not counting this box.

typedef struct {
    char    gid[5];                  // Example: "3fa9".
    char    name[17];
    char    members[GROUP_MAX_MEMBERS][5];
    uint8_t count;                   // Number of members (other boxes).
    char    owner[5];                // Box that created the group.
} group_info_t;

esp_err_t groups_init(void);                     // Call after friends_init().

// Web task.
int  groups_list(group_info_t *out, int max);    // Groups saved on this box. Returns the count.
esp_err_t groups_create(const char *name, const char ids[][5], int n, char gid_out[5]);   // Members must be friends.
esp_err_t groups_delete(const char *gid);        // Forget the group on this box only.
bool groups_get(const char *gid, group_info_t *out);   // One group's details. False = not in that group.

// Radio task.
esp_err_t groups_resend(const char *gid);        // Owner only: send the key to every member again.
int  groups_members_of(const char *gid, char ids[][5], int max);   // Returns -1 if this box is not in that group.

bool groups_on_key(const char *from_id, char *inner);       // Opened K packet from a friend. True = saved, answer L.
void groups_key_got(const char *gid, const char *from_id);  // An L packet: that member has the key.
int  groups_next_packet(char *out, int max);                 // Next K packet to send, or 0.
// Seal or open a group message with the group key. Returns the length, or -1.
int  groups_seal(const char *gid, const char *from_box, const uint8_t *plain, int len, uint8_t *out, int out_max);
int  groups_open(const char *gid, const char *from_box, const uint8_t *sealed, int len, uint8_t *out, int out_max);
