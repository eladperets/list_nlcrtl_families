#include <string.h>
#include <linux/nl80211.h>
#include <netlink/netlink.h>
#include <netlink/object.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/ctrl.h>
#include <netlink/object-api.h>
#include <netlink/cache.h>
#include <net/if.h>

#define EXIT_ON_ALLOCATION_FAILURE(ptr)     \
    if (!ptr) {                             \
        printf("Allocation error\n");       \
        exit(-1);                           \
    }

#define MAX_NAME_LEN GENL_NAMSIZ

// List of nlctrl families
struct nlctrl_family_list {
    struct nlctrl_family *head;
};

// nlcrtl family list node
struct nlctrl_family {
    int id;
    char name[MAX_NAME_LEN];
    int version;
    struct multicast_group_list *mc_groups;

    struct nlctrl_family *next;
};

// List of multicast groups
struct multicast_group_list {
    struct multicast_group *head;
};

// Multicast group list node
struct multicast_group {
    unsigned int id;
    char name[MAX_NAME_LEN];
    struct multicast_group *next;
};

// Callback for handling a message containing a list of multicase groups
int multicast_groups_cb(struct nl_msg *msg, void *context) {
    struct multicast_group_list *groups = context;
    struct nlattr *attributes[CTRL_ATTR_MAX + 1];
    struct nlmsghdr* hdr = nlmsg_hdr(msg);
    struct genlmsghdr *gnlh = nlmsg_data(hdr);
    struct nlattr *curr;
    int rem_mcgrp;
   
    // parse the message attributes 
    nla_parse(attributes, CTRL_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), NULL);

    // In some cases (ahm tcp metrics) we can have 0 MC groups.
    if (!attributes[CTRL_ATTR_MCAST_GROUPS]) return NL_SKIP;

    // This is a macro that hides a for loop iterating the nested attributes within attributes[CTRL_ATTR_MCAST_GROUPS]
    nla_for_each_nested(curr, attributes[CTRL_ATTR_MCAST_GROUPS], rem_mcgrp) {
        // Each of the attributes in the multicast groups attribute is diveded to CTRL_ATTR_MCAST_GRP_MAX sub-attributes
        // containing the group data such as name and ID.
        struct nlattr *tb_mcgrp[CTRL_ATTR_MCAST_GRP_MAX + 1];
        nla_parse(tb_mcgrp, CTRL_ATTR_MCAST_GRP_MAX, nla_data(curr), nla_len(curr), NULL);
        
        if (!tb_mcgrp[CTRL_ATTR_MCAST_GRP_NAME] || !tb_mcgrp[CTRL_ATTR_MCAST_GRP_ID]) 
            continue;
        
        // add another node to the list
        struct multicast_group *new_node = malloc(sizeof(struct multicast_group));
        EXIT_ON_ALLOCATION_FAILURE(new_node);

        new_node->id = nla_get_u32(tb_mcgrp[CTRL_ATTR_MCAST_GRP_ID]);
        strcpy(new_node->name, nla_data(tb_mcgrp[CTRL_ATTR_MCAST_GRP_NAME]));
        new_node->next = groups->head;
        groups->head = new_node;
    }

    return NL_OK;
}

// Returns a list of multicast groups
struct multicast_group_list* list_all_multicast_groups(struct nl_sock *socket, const char *family_name){
    int ret;
    struct nl_cb *cb = 0;
    struct multicast_group_list* list = 0;
    struct nl_msg *msg = 0;
    int nlctrl_id;

    // Allocate the socket and connect
    socket = nl_socket_alloc();
    EXIT_ON_ALLOCATION_FAILURE(socket);
    genl_connect(socket);      
    
    // Allocate a message with GETFAMILY command for getting multicast IDs
    msg = nlmsg_alloc();
    EXIT_ON_ALLOCATION_FAILURE(msg);

    // Get the nlctrl driver ID
    // Probably not needed, When following the kernel code in genetlink.c it looks like nlctl is always on 0x10
    nlctrl_id = genl_ctrl_resolve(socket, "nlctrl");
    genlmsg_put(msg, 0, 0, nlctrl_id, 0, 0, CTRL_CMD_GETFAMILY, 0);
    NLA_PUT_STRING(msg, CTRL_ATTR_FAMILY_NAME, family_name);
   
    // Allocate the result list 
    list = malloc(sizeof(struct multicast_group_list));
    list->head = 0;
    EXIT_ON_ALLOCATION_FAILURE(list);
    
    // Setup the callback
    cb = nl_cb_alloc(NL_CB_DEFAULT);
    EXIT_ON_ALLOCATION_FAILURE(cb);
    nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, multicast_groups_cb, list);
    
    // Send
    ret = nl_send_auto(socket, msg);
    if (ret < 0) goto finish;

    // Wait for answer
    nl_recvmsgs(socket, cb);

    // nla_put_failure is required by NLA_PUT_STRING
    nla_put_failure:
    finish:
    nl_socket_free(socket);
    nlmsg_free(msg);
    if (cb) nl_cb_put(cb);
    return list;
}

// Callback for dumping nl cache object containing nlctrl family information
void nl_object_dump_cb(struct nl_dump_params * params, char * buff) {
    struct nlctrl_family_list *lst = params->dp_data;
    struct nlctrl_family *new_node = malloc(sizeof(struct nlctrl_family));
    EXIT_ON_ALLOCATION_FAILURE(new_node);

    // although we can parse the multicast groups when doing detailed dump,
    // this property will be populated at later stage with good old "GET_FAMILY" netlink calls
    // Why? becuase it's more fun!
    new_node->mc_groups = 0;

    sscanf(buff, "%x %s version %d", &new_node->id, new_node->name, &new_node->version);
    new_node->next = lst->head;
    lst->head = new_node;
}

// Callback for iterating the nlctrl cache 
void list_families_cb(struct nl_object * obj, void * context) {
    struct nlctrl_family_list *family_list = context; 

    // dump the cache object
    struct nl_dump_params params = { 
        .dp_type = NL_DUMP_LINE, // Other options can include more detailed dump
        .dp_cb = nl_object_dump_cb,
        .dp_data = family_list
    };

    nl_object_dump(obj, &params);
}

// Get a list of all nlctrl families
struct nlctrl_family_list * list_families(struct nl_sock* sock) {
    struct nl_cache * cache;
    struct nlctrl_family_list *family_list;
    family_list = malloc(sizeof(struct nlctrl_family_list));
    EXIT_ON_ALLOCATION_FAILURE(family_list);
    family_list->head = 0;
    
    // allocate cache containing families information
    genl_ctrl_alloc_cache(sock, &cache);

    // iterate the cache objects
    nl_cache_foreach(cache, list_families_cb, family_list);

    return family_list;
} 

// --------------------------------------------------------------
// Cleanup stuff
// --------------------------------------------------------------

void free_multicast_groups_rec(struct multicast_group * head) {
    if (!head) return;

    free_multicast_groups_rec(head->next);
    free(head);
}

void free_multicast_group_list(struct multicast_group_list *lst) {
    if (!lst) return;
    free_multicast_groups_rec(lst->head);
}

void free_nlctrl_families_rec(struct nlctrl_family * head) {
    if (!head) return;

    free_nlctrl_families_rec(head->next);
    free(head);
}

void free_nlctrl_family_list(struct nlctrl_family_list *lst) {
    if (!lst) return;
    free_nlctrl_families_rec(lst->head);
}

// Get all nlctrl families including the multicast groups in each family
struct nlctrl_family_list *get_nlctrl_families() {
    struct nl_sock *socket = 0;
    struct nlctrl_family_list * lst;
    struct nlctrl_family * curr;

    // Allocate the socket and connect
    socket = nl_socket_alloc();
    EXIT_ON_ALLOCATION_FAILURE(socket);
    genl_connect(socket);      
    
    lst = list_families(socket);
    curr = lst->head;
    
    while(curr){
        curr->mc_groups = list_all_multicast_groups(socket, curr->name);
        curr = curr->next;
    }

    nl_socket_free(socket);
    return lst;
}

int main() {
    struct nlctrl_family * nlctrl_family_curr_node;
    struct multicast_group *mc_group_curr_node;
    struct nlctrl_family_list *family_list = get_nlctrl_families(); 

    nlctrl_family_curr_node = family_list->head;
    while(nlctrl_family_curr_node) {
        printf("------------------------------------------------------------------\n");
        printf("Family: %s, ID: %d, Version: %d\n", nlctrl_family_curr_node->name, nlctrl_family_curr_node->id, nlctrl_family_curr_node->version);
        
        mc_group_curr_node = nlctrl_family_curr_node->mc_groups->head;
        if (mc_group_curr_node) {
            printf("Multicast groups:\n");
        } else {
            printf("No multicast groups\n");
        }

        while(mc_group_curr_node) {
            printf("(%s, %d)\n", mc_group_curr_node->name, mc_group_curr_node->id);
            mc_group_curr_node = mc_group_curr_node->next;
        }

        nlctrl_family_curr_node = nlctrl_family_curr_node->next;
    }

    free_nlctrl_family_list(family_list);
}
