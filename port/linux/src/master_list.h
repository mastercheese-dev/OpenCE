/*
MASTER_LIST.H

The server browser's master servers (master_list.c). While the browser is
open, the official master (or the ones in masters.txt) is asked for its
list, and each dedicated server's host-signed listing is given to the
browser (p2p_lobby.c).
*/

#ifndef __HALO_LINUX_MASTER_LIST_H
#define __HALO_LINUX_MASTER_LIST_H

/* the server browser open (asked every 30 seconds) or closed */
void master_list_browse(int on);
/* Refresh: asked again now */
void master_list_refresh(void);

#endif
