#pragma once

/* Run the FTP command loop for one client control connection.
 * first_line is the first line already read from the socket, or ""
 * if no data arrived yet (FTP is server-speaks-first; handle_ftp_session
 * sends the 220 greeting before entering the command loop). */
void handle_ftp_session(int ctrl_fd, const char *first_line);
