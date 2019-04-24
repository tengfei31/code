//
// Created by hujianzhe on 18-8-13.
//

#include "../syslib/error.h"
#include "niosocket.h"
#include <stdlib.h>
#include <string.h>

enum {
	NIO_SOCKET_USER_MESSAGE,
	NIO_SOCKET_CLOSE_MESSAGE,
	NIO_SOCKET_SHUTDOWN_MESSAGE,
	NIO_SOCKET_SHUTDOWN_POST_MESSAGE,
	NIO_SOCKET_RECONNECT_MESSAGE,
	NIO_SOCKET_REG_MESSAGE,
	NIO_SOCKET_PACKET_MESSAGE,
	NIO_SOCKET_RELIABLE_PACKET_MESSAGE
};
enum {
	SEND_SHUTDOWN_ACTION,
	SEND_CONNECT_ACTION,
	SEND_RECONNECT_ACTION,
	SEND_OK_ACTION
};
enum {
	HDR_SYN,
	HDR_SYN_ACK,
	HDR_SYN_ACK_ACK,
	HDR_RECONNECT,
	HDR_RECONNECT_ACK,
	HDR_FIN,
	HDR_FIN_ACK,
	HDR_DATA,
	HDR_ACK
};
enum {
	IDLE_STATUS = 0,
	LISTENED_STATUS,
	SYN_SENT_STATUS,
	SYN_RCVD_STATUS,
	ESTABLISHED_STATUS,
	FIN_WAIT_1_STATUS,
	FIN_WAIT_2_STATUS,
	CLOSE_WAIT_STATUS,
	TIME_WAIT_STATUS,
	LAST_ACK_STATUS,
	CLOSED_STATUS
};
#define	RELIABLE_DGRAM_HDR_LEN	5
#define	HDR_DATA_END_FLAG	0x80
#define	MSL					30000

typedef struct Packet_t {
	NioInternalMsg_t msg;
	struct sockaddr_storage saddr;
	NioSocket_t* s;
	unsigned int seq;
	size_t offset;
	size_t len;
	unsigned char data[1];
} Packet_t;

typedef struct ReliableDgramHalfConnectPacket_t {
	ListNode_t m_listnode;
	FD_t sockfd;
	long long timestamp_msec;
	unsigned short resend_times;
	unsigned short local_port;
	struct sockaddr_storage peer_addr;
} ReliableDgramHalfConnectPacket_t;

typedef struct ReliableDgramDataPacket_t {
	NioInternalMsg_t msg;
	long long resend_timestamp_msec;
	unsigned int resendtimes;
	NioSocket_t* s;
	unsigned int seq;
	size_t len;
	unsigned char data[1];
} ReliableDgramDataPacket_t;

#ifdef __cplusplus
extern "C" {
#endif

static void update_timestamp(long long* dst, long long timestamp) {
	if (*dst <= 0 || *dst > timestamp)
		*dst = timestamp;
}

static NioLoop_t* nioloop_exec_msg(NioLoop_t* loop, ListNode_t* msgnode) {
	int need_wake;
	criticalsectionEnter(&loop->m_msglistlock);
	need_wake = !loop->m_msglist.head;
	listInsertNodeBack(&loop->m_msglist, loop->m_msglist.tail, msgnode);
	criticalsectionLeave(&loop->m_msglistlock);
	if (need_wake) {
		char c;
		socketWrite(loop->m_socketpair[1], &c, sizeof(c), 0, NULL);
	}
	else {
		nioloopWake(loop);
	}
	return loop;
}

static NioLoop_t* nioloop_exec_msglist(NioLoop_t* loop, List_t* msglist) {
	int need_wake;
	criticalsectionEnter(&loop->m_msglistlock);
	need_wake = !loop->m_msglist.head;
	listMerge(&loop->m_msglist, msglist);
	criticalsectionLeave(&loop->m_msglistlock);
	if (need_wake) {
		char c;
		socketWrite(loop->m_socketpair[1], &c, sizeof(c), 0, NULL);
	}
	else {
		nioloopWake(loop);
	}
	return loop;
}

static int reactorsocket_read(NioSocket_t* s) {
	struct sockaddr_storage saddr;
	int opcode;
	if (!s->m_valid)
		return 0;
	else if (NIOSOCKET_TRANSPORT_LISTEN == s->transport_side && SOCK_STREAM == s->socktype) {
		opcode = REACTOR_ACCEPT;
		saddr.ss_family = s->domain;
	}
	else {
		opcode = REACTOR_READ;
	}
	if (!s->m_readol) {
		s->m_readol = reactorMallocOverlapped(opcode, NULL, 0, SOCK_STREAM != s->socktype ? 65000 : 0);
		if (!s->m_readol) {
			s->m_valid = 0;
			return 0;
		}
	}
	if (reactorCommit(&s->m_loop->m_reactor, s->fd, opcode, s->m_readol, &saddr))
		return 1;
	s->m_valid = 0;
	return 0;
}

static int reactorsocket_write(NioSocket_t* s) {
	struct sockaddr_storage saddr;
	if (!s->m_writeol) {
		s->m_writeol = reactorMallocOverlapped(REACTOR_WRITE, NULL, 0, 0);
		if (!s->m_writeol) {
			s->m_valid = 0;
			return 0;
		}
	}
	if (reactorCommit(&s->m_loop->m_reactor, s->fd, REACTOR_WRITE, s->m_writeol, &saddr))
		return 1;
	s->m_valid = 0;
	return 0;
}

static void free_io_resource(NioSocket_t* s) {
	if (INVALID_FD_HANDLE != s->fd) {
		socketClose(s->fd);
		s->fd = INVALID_FD_HANDLE;
	}
	if (s->m_readol) {
		reactorFreeOverlapped(s->m_readol);
		s->m_readol = NULL;
	}
	if (s->m_writeol) {
		reactorFreeOverlapped(s->m_writeol);
		s->m_writeol = NULL;
	}
}

static void free_inbuf(NioSocket_t* s) {
	free(s->m_inbuf);
	s->m_inbuf = NULL;
	s->m_inbuflen = 0;
	s->m_inbufoffset = 0;
}

static int data_packet_handler(NioSocket_t* s, unsigned char* data, int len, int* decode_len, int* decode_pkgcnt, const struct sockaddr_storage* saddr) {
	NioMsg_t* msgptr;
	*decode_len = 0;
	*decode_pkgcnt = 0;
	if (len) {
		int res, offset = 0;
		while (offset < len) {
			msgptr = NULL;
			res = s->decode_packet(s, data + offset, len - offset, saddr, &msgptr);
			if (res < 0)
				return res;
			else if (0 == res)
				break;
			if (msgptr) {
				msgptr->sock = s;
				msgptr->internal.type = NIO_SOCKET_USER_MESSAGE;
				dataqueuePush(s->m_loop->m_msgdq, &msgptr->internal.m_listnode);
			}
			offset += res;
			*decode_len += res;
			(*decode_pkgcnt)++;
		}
		return offset;
	}
	else if (SOCK_STREAM != s->socktype) {
		msgptr = NULL;
		s->decode_packet(s, NULL, 0, saddr, &msgptr);
		if (msgptr) {
			msgptr->sock = s;
			msgptr->internal.type = NIO_SOCKET_USER_MESSAGE;
			dataqueuePush(s->m_loop->m_msgdq, &msgptr->internal.m_listnode);
		}
	}
	return 0;
}

static void data_packet_reconnect_push(NioSocket_t* s, long long timestamp_msec) {
	ListNode_t* cur;
	for (cur = s->m_sendpacketlist.head; cur; cur = cur->next) {
		ReliableDgramDataPacket_t* packet = pod_container_of(cur, ReliableDgramDataPacket_t, msg.m_listnode);
		if (packet->seq < s->reliable.m_cwndseq ||
			packet->seq - s->reliable.m_cwndseq >= s->reliable.cwndsize)
		{
			break;
		}
		socketWrite(s->fd, packet->data, packet->len, 0, &s->reliable.peer_saddr);
		packet->resendtimes = 0;
		packet->resend_timestamp_msec = timestamp_msec + s->reliable.rto;
		update_timestamp(&s->m_loop->m_event_msec, packet->resend_timestamp_msec);
	}
}

static void reliable_dgram_packet_merge(NioSocket_t* s, unsigned char* data, int len, const struct sockaddr_storage* saddr) {
	unsigned char hdr_data_end_flag = data[0] & HDR_DATA_END_FLAG;
	len -= RELIABLE_DGRAM_HDR_LEN;
	data += RELIABLE_DGRAM_HDR_LEN;
	if (!s->m_inbuf && hdr_data_end_flag) {
		int decode_len, decode_pkgcnt;
		data_packet_handler(s, data, len, &decode_len, &decode_pkgcnt, saddr);
	}
	else {
		unsigned char* ptr = (unsigned char*)realloc(s->m_inbuf, s->m_inbuflen + len);
		if (ptr) {
			s->m_inbuf = ptr;
			memcpy(s->m_inbuf + s->m_inbuflen, data, len);
			s->m_inbuflen += len;
			if (!hdr_data_end_flag)
				return;
			else {
				int decode_len, decode_pkgcnt;
				data_packet_handler(s, s->m_inbuf, s->m_inbuflen, &decode_len, &decode_pkgcnt, saddr);
			}
		}
		free_inbuf(s);
	}
}

static void reliable_dgram_send_fin_packet(NioSocket_t* s, long long timestamp_msec) {
	unsigned char fin = HDR_FIN;
	socketWrite(s->fd, &fin, sizeof(fin), 0, &s->reliable.peer_saddr);
	s->reliable.m_fin_msec = timestamp_msec + s->reliable.rto;
	if (ESTABLISHED_STATUS == s->reliable.m_status) {
		s->reliable.m_status = FIN_WAIT_1_STATUS;
		update_timestamp(&s->m_loop->m_event_msec, s->reliable.m_fin_msec);
	}
	else if (CLOSE_WAIT_STATUS == s->reliable.m_status) {
		s->reliable.m_status = LAST_ACK_STATUS;
		update_timestamp(&s->m_loop->m_event_msec, s->reliable.m_fin_msec);
	}
}

static void reliable_dgram_shutdown(NioSocket_t* s, long long timestamp_msec) {
	if (NIOSOCKET_TRANSPORT_LISTEN == s->transport_side) {
		s->m_lastactive_msec = timestamp_msec;
		s->m_valid = 0;
		update_timestamp(&s->m_loop->m_event_msec, s->m_lastactive_msec + s->m_close_timeout_msec);
	}
	else if (NIOSOCKET_TRANSPORT_CLIENT == s->transport_side || NIOSOCKET_TRANSPORT_SERVER == s->transport_side) {
		s->m_sendaction = SEND_SHUTDOWN_ACTION;
		if (ESTABLISHED_STATUS == s->reliable.m_status || CLOSE_WAIT_STATUS == s->reliable.m_status) {
			if (!s->m_sendpacketlist.head) {
				reliable_dgram_send_fin_packet(s, timestamp_msec);
			}
			s->sendprobe_timeout_sec = 0;
			s->m_sendprobe_msec = 0;
		}
	}
}

static void reliable_dgram_send_packet(NioSocket_t* s, ReliableDgramDataPacket_t* packet, long long timestamp_msec) {
	*(unsigned int*)(packet->data + 1) = htonl(s->reliable.m_sendseq);
	packet->seq = s->reliable.m_sendseq++;
	listInsertNodeBack(&s->m_sendpacketlist, s->m_sendpacketlist.tail, &packet->msg.m_listnode);
	if (SEND_OK_ACTION != s->m_sendaction)
		return;
	if (packet->seq >= s->reliable.m_cwndseq &&
		packet->seq - s->reliable.m_cwndseq < s->reliable.cwndsize)
	{
		socketWrite(s->fd, packet->data, packet->len, 0, &s->reliable.peer_saddr);
		packet->resend_timestamp_msec = timestamp_msec + s->reliable.rto;
		update_timestamp(&s->m_loop->m_event_msec, packet->resend_timestamp_msec);
	}
}

static void reliable_dgram_reconnect(NioSocket_t* s, long long timestamp_msec) {
	unsigned char reconnect_pkg;
	if (NIOSOCKET_TRANSPORT_CLIENT != s->transport_side || SEND_OK_ACTION != s->m_sendaction || ESTABLISHED_STATUS != s->reliable.m_status)
		return;
	reconnect_pkg = HDR_RECONNECT;
	socketWrite(s->fd, &reconnect_pkg, sizeof(reconnect_pkg), 0, &s->reliable.peer_saddr);

	s->m_valid = 1;
	s->m_sendaction = SEND_RECONNECT_ACTION;
	s->m_lastactive_msec = timestamp_msec;
	s->m_sendprobe_msec = 0;
	s->reliable.m_reconnect_times = 0;
	s->reliable.m_reconnect_msec = timestamp_msec + s->reliable.rto;
	update_timestamp(&s->m_loop->m_event_msec, s->reliable.m_reconnect_msec);
}

static int reliable_dgram_recv_handler(NioSocket_t* s, unsigned char* buffer, int len, const struct sockaddr_storage* saddr, long long timestamp_msec) {
	unsigned char hdr_type;
	if (TIME_WAIT_STATUS == s->reliable.m_status || CLOSED_STATUS == s->reliable.m_status)
		return 1;
	hdr_type = buffer[0] & (~HDR_DATA_END_FLAG);
	if (HDR_SYN == hdr_type) {
		unsigned char syn_ack[3];
		if (s->m_shutdown)
			return 1;
		else if (LISTENED_STATUS == s->reliable.m_status) {
			ReliableDgramHalfConnectPacket_t* halfcon = NULL;
			ListNode_t* cur, *next;
			for (cur = s->m_recvpacketlist.head; cur; cur = next) {
				next = cur->next;
				halfcon = pod_container_of(cur, ReliableDgramHalfConnectPacket_t, m_listnode);
				if (!memcmp(&halfcon->peer_addr, saddr, sizeof(halfcon->peer_addr)))
					break;
				halfcon = NULL;
			}
			if (halfcon) {
				syn_ack[0] = HDR_SYN_ACK;
				*(unsigned short*)(syn_ack + 1) = htons(halfcon->local_port);
			}
			else {
				FD_t new_fd;
				IPString_t ipstr;
				unsigned short local_port;
				struct sockaddr_storage local_saddr = s->local_listen_saddr;
				if (!sockaddrSetPort(&local_saddr, 0))
					return 1;
				new_fd = socket(s->domain, s->socktype, s->protocol);
				if (new_fd == INVALID_FD_HANDLE)
					return 1;
				if (!socketBindAddr(new_fd, &local_saddr)) {
					socketClose(new_fd);
					return 1;
				}
				if (!socketGetLocalAddr(new_fd, &local_saddr)) {
					socketClose(new_fd);
					return 1;
				}
				if (!sockaddrDecode(&local_saddr, ipstr, &local_port)) {
					socketClose(new_fd);
					return 1;
				}
				if (!socketNonBlock(new_fd, TRUE)) {
					socketClose(new_fd);
					return 1;
				}
				halfcon = (ReliableDgramHalfConnectPacket_t*)malloc(sizeof(ReliableDgramHalfConnectPacket_t));
				if (!halfcon) {
					socketClose(new_fd);
					return 1;
				}
				halfcon->local_port = local_port;
				halfcon->resend_times = 0;
				halfcon->peer_addr = *saddr;
				halfcon->sockfd = new_fd;
				halfcon->timestamp_msec = timestamp_msec + s->reliable.rto;

				listInsertNodeBack(&s->m_recvpacketlist, s->m_recvpacketlist.tail, &halfcon->m_listnode);
				update_timestamp(&s->m_loop->m_event_msec, halfcon->timestamp_msec);

				syn_ack[0] = HDR_SYN_ACK;
				*(unsigned short*)(syn_ack + 1) = htons(local_port);
			}
			socketWrite(s->fd, syn_ack, sizeof(syn_ack), 0, saddr);
			s->m_lastactive_msec = timestamp_msec;
		}
		else if (SYN_RCVD_STATUS == s->reliable.m_status) {
			if (AF_UNSPEC == s->reliable.peer_saddr.ss_family) {
				s->reliable.peer_saddr = *saddr;
				s->reliable.m_synrcvd_msec = timestamp_msec + s->reliable.rto;
				update_timestamp(&s->m_loop->m_event_msec, s->reliable.m_synrcvd_msec);
			}
			else if (memcmp(&s->reliable.peer_saddr, saddr, sizeof(*saddr)))
				return 1;
			syn_ack[0] = HDR_SYN_ACK;
			socketWrite(s->fd, syn_ack, 1, 0, saddr);
			s->m_lastactive_msec = timestamp_msec;
		}
	}
	else if (HDR_SYN_ACK_ACK == hdr_type) {
		ListNode_t* cur, *next;
		if (s->m_shutdown)
			return 1;
		else if (LISTENED_STATUS == s->reliable.m_status) {
			struct sockaddr_storage peer_addr;
			for (cur = s->m_recvpacketlist.head; cur; cur = next) {
				ReliableDgramHalfConnectPacket_t* halfcon = pod_container_of(cur, ReliableDgramHalfConnectPacket_t, m_listnode);
				next = cur->next;
				if (memcmp(&halfcon->peer_addr, saddr, sizeof(halfcon->peer_addr)))
					continue;
				if (socketRead(halfcon->sockfd, NULL, 0, 0, &peer_addr))
					break;
				listRemoveNode(&s->m_recvpacketlist, cur);
				s->accept_callback(s, halfcon->sockfd, &peer_addr);
				free(halfcon);
				break;
			}
			s->m_lastactive_msec = timestamp_msec;
		}
		else if (SYN_RCVD_STATUS == s->reliable.m_status) {
			if (memcmp(&s->reliable.peer_saddr, saddr, sizeof(*saddr)))
				return 1;
			s->m_sendaction = SEND_OK_ACTION;
			s->m_lastactive_msec = timestamp_msec;
			s->m_sendprobe_msec = timestamp_msec;
			if (s->sendprobe_timeout_sec > 0) {
				update_timestamp(&s->m_loop->m_event_msec, s->m_sendprobe_msec + s->sendprobe_timeout_sec * 1000);
			}
			s->reliable.m_status = ESTABLISHED_STATUS;
		}
	}
	else if (HDR_SYN_ACK == hdr_type) {
		unsigned char syn_ack_ack;
		if (len < 3)
			return 1;
		if (memcmp(saddr, &s->peer_listen_saddr, sizeof(s->peer_listen_saddr)))
			return 1;
		syn_ack_ack = HDR_SYN_ACK_ACK;
		socketWrite(s->fd, &syn_ack_ack, sizeof(syn_ack_ack), 0, saddr);
		if (SYN_SENT_STATUS == s->reliable.m_status) {
			if (len >= 3) {
				unsigned short peer_port;
				peer_port = *(unsigned short*)(buffer + 1);
				peer_port = ntohs(peer_port);
				sockaddrSetPort(&s->reliable.peer_saddr, peer_port);
			}
			s->reliable.m_status = ESTABLISHED_STATUS;
			s->m_sendaction = SEND_OK_ACTION;
			s->m_regerrno = 0;
			dataqueuePush(s->m_loop->m_msgdq, &s->m_regmsg.m_listnode);
		}
		if (memcmp(&s->peer_listen_saddr, &s->reliable.peer_saddr, sizeof(s->reliable.peer_saddr)))
			socketWrite(s->fd, NULL, 0, 0, &s->reliable.peer_saddr);
		s->m_lastactive_msec = timestamp_msec;
		s->m_sendprobe_msec = timestamp_msec;
		if (s->sendprobe_timeout_sec > 0) {
			update_timestamp(&s->m_loop->m_event_msec, s->m_sendprobe_msec + s->sendprobe_timeout_sec * 1000);
		}
	}
	else if (HDR_RECONNECT == hdr_type) {
		unsigned char reconnect_ack;
		if (NIOSOCKET_TRANSPORT_SERVER != s->transport_side || SEND_OK_ACTION != s->m_sendaction)
			return 1;
		else if (memcmp(&s->reliable.peer_saddr, saddr, sizeof(*saddr))) {
			s->reliable.peer_saddr = *saddr;
			data_packet_reconnect_push(s, timestamp_msec);
		}
		reconnect_ack = HDR_RECONNECT_ACK;
		socketWrite(s->fd, &reconnect_ack, sizeof(reconnect_ack), 0, saddr);
	}
	else if (HDR_RECONNECT_ACK == hdr_type) {
		if (NIOSOCKET_TRANSPORT_CLIENT != s->transport_side || SEND_RECONNECT_ACTION != s->m_sendaction)
			return 1;
		if (memcmp(&s->reliable.peer_saddr, saddr, sizeof(*saddr)))
			return 1;
		s->m_sendaction = SEND_OK_ACTION;
		s->m_regerrno = 0;
		_xchg16(&s->m_shutdown, 0);
		dataqueuePush(s->m_loop->m_msgdq, &s->m_reconnectmsg.m_listnode);
		data_packet_reconnect_push(s, timestamp_msec);
	}
	else if (HDR_FIN == hdr_type) {
		if (memcmp(saddr, &s->reliable.peer_saddr, sizeof(*saddr)))
			return 1;
		else {
			unsigned char fin_ack = HDR_FIN_ACK;
			socketWrite(s->fd, &fin_ack, sizeof(fin_ack), 0, &s->reliable.peer_saddr);
			if (ESTABLISHED_STATUS == s->reliable.m_status) {
				s->reliable.m_status = CLOSE_WAIT_STATUS;
				s->m_lastactive_msec = timestamp_msec;
				s->m_sendaction = SEND_SHUTDOWN_ACTION;
				if (0 == _xchg16(&s->m_shutdown, 1) && !s->m_sendpacketlist.head) {
					reliable_dgram_send_fin_packet(s, timestamp_msec);
				}
				dataqueuePush(s->m_loop->m_msgdq, &s->m_shutdownmsg.m_listnode);
			}
			else if (FIN_WAIT_1_STATUS == s->reliable.m_status ||
				FIN_WAIT_2_STATUS == s->reliable.m_status)
			{
				s->reliable.m_status = TIME_WAIT_STATUS;
				s->m_lastactive_msec = timestamp_msec;
				s->m_valid = 0;
				_xchg16(&s->m_shutdown, 1);
				dataqueuePush(s->m_loop->m_msgdq, &s->m_shutdownmsg.m_listnode);
			}
			s->m_sendprobe_msec = 0;
			s->sendprobe_timeout_sec = 0;
		}
	}
	else if (HDR_FIN_ACK == hdr_type) {
		if (memcmp(saddr, &s->reliable.peer_saddr, sizeof(*saddr)))
			return 1;
		else if (LAST_ACK_STATUS == s->reliable.m_status) {
			s->reliable.m_status = CLOSED_STATUS;
			s->m_lastactive_msec = timestamp_msec;
			s->m_valid = 0;
		}
		else if (FIN_WAIT_1_STATUS == s->reliable.m_status) {
			s->reliable.m_status = FIN_WAIT_2_STATUS;
			s->m_lastactive_msec = timestamp_msec;
		}
	}
	else if (HDR_ACK == hdr_type) {
		ListNode_t* cur;
		unsigned int seq, cwnd_skip, ack_valid;
		if (len < RELIABLE_DGRAM_HDR_LEN)
			return 1;
		if (ESTABLISHED_STATUS > s->reliable.m_status)
			return 1;
		if (memcmp(saddr, &s->reliable.peer_saddr, sizeof(*saddr)))
			return 1;

		s->m_lastactive_msec = timestamp_msec;
		s->m_sendprobe_msec = timestamp_msec;
		seq = *(unsigned int*)(buffer + 1);
		seq = ntohl(seq);
		cwnd_skip = 0;
		ack_valid = 0;

		for (cur = s->m_sendpacketlist.head; cur; cur = cur->next) {
			ReliableDgramDataPacket_t* packet = pod_container_of(cur, ReliableDgramDataPacket_t, msg.m_listnode);
			if (seq < packet->seq)
				break;
			if (packet->seq == seq) {
				ListNode_t* next = cur->next;
				listRemoveNode(&s->m_sendpacketlist, cur);
				free(packet);
				if (seq == s->reliable.m_cwndseq) {
					if (next) {
						packet = pod_container_of(next, ReliableDgramDataPacket_t, msg.m_listnode);
						s->reliable.m_cwndseq = packet->seq;
						cwnd_skip = 1;
					}
					else
						++s->reliable.m_cwndseq;
				}
				ack_valid = 1;
				break;
			}
		}
		if (cwnd_skip) {
			for (cur = s->m_sendpacketlist.head; cur; cur = cur->next) {
				ReliableDgramDataPacket_t* packet = pod_container_of(cur, ReliableDgramDataPacket_t, msg.m_listnode);
				if (packet->seq < s->reliable.m_cwndseq ||
					packet->seq - s->reliable.m_cwndseq >= s->reliable.cwndsize)
				{
					break;
				}
				socketWrite(s->fd, packet->data, packet->len, 0, &s->reliable.peer_saddr);
				packet->resend_timestamp_msec = timestamp_msec + s->reliable.rto;
				update_timestamp(&s->m_loop->m_event_msec, packet->resend_timestamp_msec);
			}
		}
		if (ack_valid && !s->m_sendpacketlist.head && SEND_SHUTDOWN_ACTION == s->m_sendaction) {
			reliable_dgram_send_fin_packet(s, timestamp_msec);
		}
	}
	else if (HDR_DATA == hdr_type) {
		ListNode_t* cur, *next;
		ReliableDgramDataPacket_t* packet;
		unsigned int seq;
		unsigned char ack[RELIABLE_DGRAM_HDR_LEN];
		if (len < RELIABLE_DGRAM_HDR_LEN)
			return 1;
		if (ESTABLISHED_STATUS > s->reliable.m_status)
			return 1;
		if (memcmp(saddr, &s->reliable.peer_saddr, sizeof(*saddr)))
			return 1;

		s->m_lastactive_msec = timestamp_msec;
		seq = *(unsigned int*)(buffer + 1);
		ack[0] = HDR_ACK;
		*(unsigned int*)(ack + 1) = seq;
		socketWrite(s->fd, ack, sizeof(ack), 0, saddr);

		seq = ntohl(seq);
		if (seq < s->reliable.m_recvseq)
			return 1;
		else if (seq == s->reliable.m_recvseq) {
			s->reliable.m_recvseq++;
			reliable_dgram_packet_merge(s, buffer, len, saddr);
			for (cur = s->m_recvpacketlist.head; cur; cur = next) {
				packet = pod_container_of(cur, ReliableDgramDataPacket_t, msg.m_listnode);
				if (packet->seq != s->reliable.m_recvseq)
					break;
				next = cur->next;
				s->reliable.m_recvseq++;
				reliable_dgram_packet_merge(s, packet->data, packet->len, saddr);
				listRemoveNode(&s->m_recvpacketlist, cur);
				free(packet);
			}
		}
		else {
			for (cur = s->m_recvpacketlist.head; cur; cur = cur->next) {
				packet = pod_container_of(cur, ReliableDgramDataPacket_t, msg.m_listnode);
				if (packet->seq > seq)
					break;
				else if (packet->seq == seq)
					return 1;
			}
			packet = (ReliableDgramDataPacket_t*)malloc(sizeof(ReliableDgramDataPacket_t) + len);
			if (!packet) {
				//s->valid = 0;
				return 0;
			}
			packet->msg.type = NIO_SOCKET_RELIABLE_PACKET_MESSAGE;
			packet->s = s;
			packet->seq = seq;
			packet->len = len;
			memcpy(packet->data, buffer, len);
			if (cur)
				listInsertNodeFront(&s->m_recvpacketlist, cur, &packet->msg.m_listnode);
			else
				listInsertNodeBack(&s->m_recvpacketlist, s->m_recvpacketlist.tail, &packet->msg.m_listnode);
		}
	}
	return 1;
}

static void reliable_dgram_update(NioLoop_t* loop, NioSocket_t* s, long long timestamp_msec) {
	if (LISTENED_STATUS == s->reliable.m_status) {
		ListNode_t* cur, *next;
		for (cur = s->m_recvpacketlist.head; cur; cur = next) {
			ReliableDgramHalfConnectPacket_t* halfcon = pod_container_of(cur, ReliableDgramHalfConnectPacket_t, m_listnode);
			next = cur->next;
			if (halfcon->timestamp_msec > timestamp_msec) {
				update_timestamp(&loop->m_event_msec, halfcon->timestamp_msec);
			}
			else if (halfcon->resend_times >= s->reliable.resend_maxtimes) {
				socketClose(halfcon->sockfd);
				listRemoveNode(&s->m_recvpacketlist, cur);
				free(halfcon);
			}
			else {
				unsigned char syn_ack[3];
				syn_ack[0] = HDR_SYN_ACK;
				*(unsigned short*)(syn_ack + 1) = htons(halfcon->local_port);
				socketWrite(s->fd, syn_ack, sizeof(syn_ack), 0, &halfcon->peer_addr);
				++halfcon->resend_times;
				halfcon->timestamp_msec = timestamp_msec + s->reliable.rto;
				update_timestamp(&loop->m_event_msec, halfcon->timestamp_msec);
			}
		}
	}
	else if (SYN_RCVD_STATUS == s->reliable.m_status) {
		if (AF_UNSPEC == s->reliable.peer_saddr.ss_family || s->m_shutdown) {
			return;
		}
		else if (s->reliable.m_synrcvd_msec > timestamp_msec) {
			update_timestamp(&loop->m_event_msec, s->reliable.m_synrcvd_msec);
		}
		else if (s->reliable.m_synrcvd_times >= s->reliable.resend_maxtimes) {
			s->reliable.m_synrcvd_times = 0;
			s->reliable.peer_saddr.ss_family = AF_UNSPEC;
		}
		else {
			unsigned int syn_ack = HDR_SYN_ACK;
			socketWrite(s->fd, &syn_ack, 1, 0, &s->reliable.peer_saddr);
			++s->reliable.m_synrcvd_times;
			s->reliable.m_synrcvd_msec = timestamp_msec + s->reliable.rto;
			update_timestamp(&loop->m_event_msec, s->reliable.m_synrcvd_msec);
		}
	}
	else if (SYN_SENT_STATUS == s->reliable.m_status) {
		if (s->reliable.m_synsent_msec > timestamp_msec) {
			update_timestamp(&loop->m_event_msec, s->reliable.m_synsent_msec);
		}
		else if (s->reliable.m_synsent_times >= s->reliable.resend_maxtimes) {
			s->reliable.m_status = TIME_WAIT_STATUS;
			s->m_lastactive_msec = timestamp_msec;
			s->m_valid = 0;
			s->m_sendaction = SEND_SHUTDOWN_ACTION;
			update_timestamp(&loop->m_event_msec, s->m_lastactive_msec + s->m_close_timeout_msec);
			s->m_regerrno = ETIMEDOUT;
			s->shutdown_callback = NULL;
			dataqueuePush(loop->m_msgdq, &s->m_regmsg.m_listnode);
		}
		else {
			unsigned char syn = HDR_SYN;
			socketWrite(s->fd, &syn, sizeof(syn), 0, &s->peer_listen_saddr);
			++s->reliable.m_synsent_times;
			s->reliable.m_synsent_msec = timestamp_msec + s->reliable.rto;
			update_timestamp(&loop->m_event_msec, s->reliable.m_synsent_msec);
		}
	}
	else if (FIN_WAIT_1_STATUS == s->reliable.m_status || LAST_ACK_STATUS == s->reliable.m_status) {
		if (s->reliable.m_fin_msec > timestamp_msec) {
			update_timestamp(&loop->m_event_msec, s->reliable.m_fin_msec);
		}
		else if (s->reliable.m_fin_times >= s->reliable.resend_maxtimes) {
			s->m_lastactive_msec = timestamp_msec;
			s->m_valid = 0;
			update_timestamp(&loop->m_event_msec, s->m_lastactive_msec + s->m_close_timeout_msec);
		}
		else {
			unsigned char fin = HDR_FIN;
			socketWrite(s->fd, &fin, sizeof(fin), 0, &s->reliable.peer_saddr);
			++s->reliable.m_fin_times;
			s->reliable.m_fin_msec = timestamp_msec + s->reliable.rto;
			update_timestamp(&loop->m_event_msec, s->reliable.m_fin_msec);
		}
	}
	else if (ESTABLISHED_STATUS == s->reliable.m_status || CLOSE_WAIT_STATUS == s->reliable.m_status) {
		if (SEND_RECONNECT_ACTION == s->m_sendaction) {
			if (s->reliable.m_reconnect_msec > timestamp_msec) {
				update_timestamp(&loop->m_event_msec, s->reliable.m_reconnect_msec);
			}
			else if (s->reliable.m_reconnect_times >= s->reliable.resend_maxtimes) {
				s->reliable.m_status = TIME_WAIT_STATUS;
				s->m_lastactive_msec = timestamp_msec;
				s->m_valid = 0;
				s->m_sendaction = SEND_SHUTDOWN_ACTION;
				update_timestamp(&loop->m_event_msec, s->m_lastactive_msec + s->m_close_timeout_msec);
				s->m_regerrno = ETIMEDOUT;
				dataqueuePush(loop->m_msgdq, &s->m_reconnectmsg.m_listnode);
			}
			else {
				unsigned char reconnect_pkg = HDR_RECONNECT;
				socketWrite(s->fd, &reconnect_pkg, sizeof(reconnect_pkg), 0, &s->reliable.peer_saddr);
				++s->reliable.m_reconnect_times;
				s->reliable.m_reconnect_msec = timestamp_msec + s->reliable.rto;
				update_timestamp(&loop->m_event_msec, s->reliable.m_reconnect_msec);
			}
		}
		else {
			ListNode_t* cur;
			for (cur = s->m_sendpacketlist.head; cur; cur = cur->next) {
				ReliableDgramDataPacket_t* packet = pod_container_of(cur, ReliableDgramDataPacket_t, msg.m_listnode);
				if (packet->seq < s->reliable.m_cwndseq ||
					packet->seq - s->reliable.m_cwndseq >= s->reliable.cwndsize)
				{
					break;
				}
				if (packet->resend_timestamp_msec > timestamp_msec) {
					update_timestamp(&loop->m_event_msec, packet->resend_timestamp_msec);
					continue;
				}
				if (packet->resendtimes >= s->reliable.resend_maxtimes) {
					s->m_lastactive_msec = timestamp_msec;
					if (SEND_SHUTDOWN_ACTION == s->m_sendaction) {
						s->m_valid = 0;
						update_timestamp(&loop->m_event_msec, s->m_lastactive_msec + s->m_close_timeout_msec);
					}
					break;
				}
				socketWrite(s->fd, packet->data, packet->len, 0, &s->reliable.peer_saddr);
				packet->resendtimes++;
				packet->resend_timestamp_msec = timestamp_msec + s->reliable.rto;
				update_timestamp(&loop->m_event_msec, packet->resend_timestamp_msec);
			}
		}
	}
}

static void reactor_socket_do_read(NioSocket_t* s, long long timestamp_msec) {
	if (SOCK_STREAM == s->socktype) {
		struct sockaddr_storage saddr;
		if (NIOSOCKET_TRANSPORT_LISTEN == s->transport_side) {
			FD_t connfd;
			for (connfd = reactorAcceptFirst(s->fd, s->m_readol, &saddr);
				connfd != INVALID_FD_HANDLE;
				connfd = reactorAcceptNext(s->fd, &saddr))
			{
				if (s->accept_callback)
					s->accept_callback(s, connfd, &saddr);
				else
					socketClose(connfd);
			}
			s->m_lastactive_msec = timestamp_msec;
		}
		else {
			unsigned char *ptr;
			int res = socketTcpReadableBytes(s->fd);
			if (res <= 0) {
				s->m_valid = 0;
				return;
			}
			ptr = (unsigned char*)realloc(s->m_inbuf, s->m_inbuflen + res);
			if (!ptr) {
				s->m_valid = 0;
				return;
			}
			s->m_inbuf = ptr;
			res = socketRead(s->fd, s->m_inbuf + s->m_inbuflen, res, 0, &saddr);
			if (res < 0) {
				if (errnoGet() != EWOULDBLOCK) {
					s->m_valid = 0;
				}
				return;
			}
			else if (res == 0) {
				s->m_valid = 0;
				return;
			}
			else {
				int decode_len, decode_pkgcnt;
				s->m_inbuflen += res;
				s->m_lastactive_msec = timestamp_msec;
				s->m_sendprobe_msec = timestamp_msec;
				if (data_packet_handler(s, s->m_inbuf + s->m_inbufoffset, s->m_inbuflen - s->m_inbufoffset,
					&decode_len, &decode_pkgcnt, &saddr) < 0)
				{
					s->m_inbuflen = s->m_inbufoffset;
				}
				else {
					s->m_inbufoffset += decode_len;
					if (s->m_inbufoffset >= s->m_inbuflen) {
						free_inbuf(s);
					}
				}
				s->reliable.m_recvseq += decode_pkgcnt;
			}
		}
	}
	else if (SOCK_DGRAM == s->socktype) {
		struct sockaddr_storage saddr;
		unsigned char buffer[0xffff], *p_data;
		unsigned int readtimes, readmaxtimes = s->m_recvpacket_maxcnt;
		for (readtimes = 0; readtimes < readmaxtimes; ++readtimes) {
			int res;
			if (0 == readtimes) {
				Iobuf_t iov;
				if (0 == reactorEventOverlappedData(s->m_readol, &iov, &saddr)) {
					++readmaxtimes;
					continue;
				}
				res = iobufLen(&iov);
				p_data = (unsigned char*)iobufPtr(&iov);
			}
			else {
				p_data = buffer;
				res = socketRead(s->fd, buffer, sizeof(buffer), 0, &saddr);
			}

			if (res < 0) {
				if (errnoGet() != EWOULDBLOCK) {
					s->m_valid = 0;
					s->m_sendaction = SEND_SHUTDOWN_ACTION;
					if (s->reliable.m_status) {
						s->m_lastactive_msec = timestamp_msec;
						update_timestamp(&s->m_loop->m_event_msec, s->m_lastactive_msec + s->m_close_timeout_msec);
					}
				}
				break;
			}
			else if (s->reliable.m_status) {
				if (0 == res)
					continue;
				if (!reliable_dgram_recv_handler(s, p_data, res, &saddr, timestamp_msec))
					break;
			}
			else {
				int decode_len, decode_pkgcnt;
				data_packet_handler(s, p_data, res, &decode_len, &decode_pkgcnt, &saddr);
				s->m_lastactive_msec = timestamp_msec;
			}
		}
	}
}

static void stream_send_packet(NioSocket_t* s, Packet_t* packet) {
	criticalsectionEnter(&s->m_lock);
	packet->seq = s->reliable.m_sendseq;
	++s->reliable.m_sendseq;
	if (s->m_sendpacketlist.head) {
		packet->offset = 0;
		listInsertNodeBack(&s->m_sendpacketlist, s->m_sendpacketlist.tail, &packet->msg.m_listnode);
	}
	else if (SEND_OK_ACTION == s->m_sendaction) {
		int res = socketWrite(s->fd, packet->data, packet->len, 0, NULL);
		if (res < 0) {
			if (errnoGet() != EWOULDBLOCK) {
				criticalsectionLeave(&s->m_lock);
				s->m_valid = 0;
				free(packet);
				return;
			}
			res = 0;
		}
		else if (res >= packet->len) {
			criticalsectionLeave(&s->m_lock);
			free(packet);
			return;
		}
		packet->offset = res;
		listInsertNodeBack(&s->m_sendpacketlist, s->m_sendpacketlist.tail, &packet->msg.m_listnode);
		reactorsocket_write(s);
	}
	criticalsectionLeave(&s->m_lock);
}

static void stream_send_packet_continue(NioSocket_t* s) {
	List_t freepacketlist;
	ListNode_t* cur, *next;
	listInit(&freepacketlist);

	criticalsectionEnter(&s->m_lock);
	for (cur = s->m_sendpacketlist.head; cur; cur = next) {
		int res;
		Packet_t* packet = pod_container_of(cur, Packet_t, msg.m_listnode);
		next = cur->next;
		res = socketWrite(s->fd, packet->data + packet->offset, packet->len - packet->offset, 0, NULL);
		if (res < 0) {
			if (errnoGet() != EWOULDBLOCK) {
				s->m_valid = 0;
				break;
			}
			res = 0;
		}
		packet->offset += res;
		if (packet->offset >= packet->len) {
			listRemoveNode(&s->m_sendpacketlist, cur);
			listInsertNodeBack(&freepacketlist, freepacketlist.tail, cur);
			//free(packet);
			continue;
		}
		reactorsocket_write(s);
		break;
	}
	criticalsectionLeave(&s->m_lock);

	for (cur = freepacketlist.head; cur; cur = next) {
		Packet_t* packet = pod_container_of(cur, Packet_t, msg.m_listnode);
		next = cur->next;
		free(packet);
	}
}

static void reactor_socket_do_write(NioSocket_t* s, long long timestamp_msec) {
	if (SOCK_STREAM != s->socktype)
		return;
	if (SEND_CONNECT_ACTION == s->m_sendaction ||
		SEND_RECONNECT_ACTION == s->m_sendaction)
	{
		int sendaction = s->m_sendaction;
		s->m_regerrno = reactorConnectCheckSuccess(s->fd) ? 0 : errnoGet();
		if (s->m_regerrno) {
			s->m_valid = 0;
			s->m_sendaction = SEND_SHUTDOWN_ACTION;
			s->shutdown_callback = NULL;
		}
		else if (!reactorsocket_read(s)) {
			s->m_regerrno = errnoGet();
			s->m_valid = 0;
			s->m_sendaction = SEND_SHUTDOWN_ACTION;
			s->shutdown_callback = NULL;
		}
		else {
			s->m_sendaction = SEND_OK_ACTION;
			s->m_lastactive_msec = timestamp_msec;
			s->m_sendprobe_msec = timestamp_msec;
			if (s->sendprobe_timeout_sec > 0) {
				update_timestamp(&s->m_loop->m_event_msec, s->m_sendprobe_msec + s->sendprobe_timeout_sec * 1000);
			}
			if (SEND_CONNECT_ACTION == sendaction) {
				dataqueuePush(s->m_loop->m_msgdq, &s->m_regmsg.m_listnode);
			}
			else {
				_xchg16(&s->m_shutdown, 0);
				dataqueuePush(s->m_loop->m_msgdq, &s->m_reconnectmsg.m_listnode);
			}
		}
	}
	else if (SEND_OK_ACTION == s->m_sendaction && s->m_valid) {
		stream_send_packet_continue(s);
	}
}

NioSocket_t* niosocketSend(NioSocket_t* s, const void* data, unsigned int len, const struct sockaddr_storage* saddr) {
	Iobuf_t iov = iobufStaticInit(data, len);
	return niosocketSendv(s, &iov, 1, saddr);
}

NioSocket_t* niosocketSendv(NioSocket_t* s, const Iobuf_t iov[], unsigned int iovcnt, const struct sockaddr_storage* saddr) {
	unsigned int i, nbytes;
	if (!s->m_valid || s->m_shutdown)
		return NULL;
	if (!iov || !iovcnt) {
		if (SOCK_STREAM == s->socktype)
			return s;
		iovcnt = 0;
		nbytes = 0;
	}
	else {
		for (nbytes = 0, i = 0; i < iovcnt; ++i)
			nbytes += iobufLen(iov + i);
		if (0 == nbytes) {
			if (SOCK_STREAM == s->socktype)
				return s;
			iovcnt = 0;
		}
	}
	if (ESTABLISHED_STATUS == s->reliable.m_status) {
		ReliableDgramDataPacket_t* packet;
		if (nbytes) {
			unsigned int offset, packetlen, copy_off, i_off;
			List_t packetlist;
			listInit(&packetlist);
			for (i = i_off = offset = 0; offset < nbytes; offset += packetlen) {
				packetlen = nbytes - offset > s->reliable.mtu ? s->reliable.mtu : nbytes - offset;
				packet = (ReliableDgramDataPacket_t*)malloc(sizeof(ReliableDgramDataPacket_t) + RELIABLE_DGRAM_HDR_LEN + packetlen);
				if (!packet)
					break;
				packet->msg.type = NIO_SOCKET_RELIABLE_PACKET_MESSAGE;
				packet->s = s;
				packet->resendtimes = 0;
				packet->data[0] = HDR_DATA;
				packet->len = RELIABLE_DGRAM_HDR_LEN + packetlen;

				copy_off = 0;
				while (i < iovcnt) {
					unsigned int copy_len;
					if (iobufLen(iov + i) - i_off > packetlen - copy_off) {
						copy_len = packetlen - copy_off;
						memcpy(packet->data + RELIABLE_DGRAM_HDR_LEN + copy_off, iobufPtr(iov + i) + i_off, copy_len);
						i_off += copy_len;
						break;
					}
					else {
						copy_len = iobufLen(iov + i) - i_off;
						memcpy(packet->data + RELIABLE_DGRAM_HDR_LEN + copy_off, iobufPtr(iov + i) + i_off, copy_len);
						copy_off += copy_len;
						i_off = 0;
						++i;
					}
				}
				listInsertNodeBack(&packetlist, packetlist.tail, &packet->msg.m_listnode);
			}
			if (offset >= nbytes) {
				packet->data[0] |= HDR_DATA_END_FLAG;
				nioloop_exec_msglist(s->m_loop, &packetlist);
			}
			else {
				ListNode_t* cur, *next;
				for (cur = packetlist.head; cur; cur = next) {
					next = cur->next;
					free(cur);
				}
				return NULL;
			}
		}
		else {
			packet = (ReliableDgramDataPacket_t*)malloc(sizeof(ReliableDgramDataPacket_t) + RELIABLE_DGRAM_HDR_LEN);
			if (!packet)
				return NULL;
			packet->msg.type = NIO_SOCKET_RELIABLE_PACKET_MESSAGE;
			packet->s = s;
			packet->resendtimes = 0;
			packet->data[0] = HDR_DATA | HDR_DATA_END_FLAG;
			packet->len = RELIABLE_DGRAM_HDR_LEN;
			nioloop_exec_msg(s->m_loop, &packet->msg.m_listnode);
		}
	}
	else {
		Packet_t* packet = (Packet_t*)malloc(sizeof(Packet_t) + nbytes);
		if (!packet)
			return NULL;
		packet->msg.type = NIO_SOCKET_PACKET_MESSAGE;
		if (saddr && SOCK_STREAM != s->socktype)
			packet->saddr = *saddr;
		else
			packet->saddr.ss_family = AF_UNSPEC;
		packet->s = s;
		packet->offset = 0;
		packet->len = nbytes;
		for (nbytes = 0, i = 0; i < iovcnt; ++i) {
			memcpy(packet->data + nbytes, iobufPtr(iov + i), iobufLen(iov + i));
			nbytes += iobufLen(iov + i);
		}
		nioloop_exec_msg(s->m_loop, &packet->msg.m_listnode);
	}
	return s;
}

void niosocketClientReconnect(NioSocket_t* s) {
	if (NIOSOCKET_TRANSPORT_CLIENT != s->transport_side)
		return;
	else if (_xchg16(&s->m_shutdown, 1))
		return;
	nioloop_exec_msg(s->m_loop, &s->m_reconnectmsg.m_listnode);
}

void niosocketTcpTransportReplace(NioSocket_t* old_s, NioSocket_t* new_s) {
	if (new_s->socktype == old_s->socktype && SOCK_STREAM == new_s->socktype) {
		List_t sendpacketlist;
		ListNode_t* cur;
		unsigned int sendseq;

		criticalsectionEnter(&old_s->m_lock);
		sendseq = old_s->reliable.m_sendseq;
		sendpacketlist = old_s->m_sendpacketlist;
		listInit(&old_s->m_sendpacketlist);
		criticalsectionLeave(&old_s->m_lock);

		for (cur = sendpacketlist.head; cur; cur = cur->next) {
			Packet_t* packet = pod_container_of(cur, Packet_t, msg.m_listnode);
			packet->s = new_s;
		}

		criticalsectionEnter(&new_s->m_lock);
		new_s->reliable.m_sendseq = sendseq;
		new_s->m_sendpacketlist = sendpacketlist;
		criticalsectionLeave(&new_s->m_lock);
	}
}

void niosocketShutdown(NioSocket_t* s) {
	if (_xchg16(&s->m_shutdown, 1))
		return;
	nioloop_exec_msg(s->m_loop, &s->m_shutdownpostmsg.m_listnode);
}

NioSocket_t* niosocketCreate(FD_t fd, int domain, int socktype, int protocol, NioSocket_t*(*pmalloc)(void), void(*pfree)(NioSocket_t*)) {
	NioSocket_t* s = pmalloc();
	if (!s)
		return NULL;
	if (INVALID_FD_HANDLE == fd) {
		fd = socket(domain, socktype, protocol);
		if (INVALID_FD_HANDLE == fd) {
			if (pfree)
				pfree(s);
			return NULL;
		}
	}
	if (!socketNonBlock(fd, TRUE)) {
		socketClose(fd);
		if (pfree)
			pfree(s);
		return NULL;
	}
	if (SOCK_STREAM == s->socktype) {
		if (!criticalsectionCreate(&s->m_lock)) {
			socketClose(fd);
			if (pfree)
				pfree(s);
			return NULL;
		}
		s->m_lockinit = 1;
	}
	else {
		s->m_lockinit = 0;
	}
	s->fd = fd;
	s->domain = domain;
	s->socktype = socktype;
	s->protocol = protocol;
	s->sendprobe_timeout_sec = 0;
	s->keepalive_timeout_sec = 0;
	s->sessionid = NULL;
	s->userdata = NULL;
	s->transport_side = NIOSOCKET_TRANSPORT_NOSIDE;
	s->local_listen_saddr.ss_family = AF_UNSPEC;
	s->peer_listen_saddr.ss_family = AF_UNSPEC;
	s->accept_callback = NULL;
	s->reg_callback = NULL;
	s->reconnect_callback = NULL;
	s->decode_packet = NULL;
	s->send_probe = NULL;
	s->shutdown_callback = NULL;
	s->close = NULL;
	s->m_valid = 1;
	s->m_sendaction = SEND_SHUTDOWN_ACTION;
	s->m_shutdown = 0;
	s->m_regerrno = 0;
	s->m_close_timeout_msec = 0;
	s->m_regmsg.type = NIO_SOCKET_REG_MESSAGE;
	s->m_shutdownmsg.type = NIO_SOCKET_SHUTDOWN_MESSAGE;
	s->m_shutdownpostmsg.type = NIO_SOCKET_SHUTDOWN_POST_MESSAGE;
	s->m_reconnectmsg.type = NIO_SOCKET_RECONNECT_MESSAGE;
	s->m_closemsg.type = NIO_SOCKET_CLOSE_MESSAGE;
	s->m_hashnode.key = &s->fd;
	s->m_loop = NULL;
	s->m_free = pfree;
	s->m_readol = NULL;
	s->m_writeol = NULL;
	s->m_lastactive_msec = 0;
	s->m_sendprobe_msec = 0;
	s->m_inbuf = NULL;
	s->m_inbufoffset = 0;
	s->m_inbuflen = 0;
	s->m_recvpacket_maxcnt = 8;
	listInit(&s->m_recvpacketlist);
	listInit(&s->m_sendpacketlist);

	s->reliable.peer_saddr.ss_family = AF_UNSPEC;
	s->reliable.mtu = 1464 - RELIABLE_DGRAM_HDR_LEN;
	s->reliable.rto = 200;
	s->reliable.resend_maxtimes = 5;
	s->reliable.cwndsize = 10;
	s->reliable.enable = 0;
	s->reliable.m_status = IDLE_STATUS;
	s->reliable.m_synrcvd_times = 0;
	s->reliable.m_synsent_times = 0;
	s->reliable.m_reconnect_times = 0;
	s->reliable.m_fin_times = 0;
	s->reliable.m_synrcvd_msec = 0;
	s->reliable.m_synsent_msec = 0;
	s->reliable.m_reconnect_msec = 0;
	s->reliable.m_fin_msec = 0;
	s->reliable.m_cwndseq = 0;
	s->reliable.m_recvseq = 0;
	s->reliable.m_sendseq = 0;
	return s;
}

static void niosocket_free(NioSocket_t* s) {
	ListNode_t *cur, *next;
	free_io_resource(s);
	free_inbuf(s);
	if (SOCK_STREAM == s->socktype && s->m_lockinit) {
		criticalsectionClose(&s->m_lock);
		s->m_lockinit = 0;
	}
	else if (SOCK_DGRAM == s->socktype && s->reliable.enable) {
		for (cur = s->m_recvpacketlist.head; cur; cur = next) {
			next = cur->next;
			if (LISTENED_STATUS == s->reliable.m_status) {
				ReliableDgramHalfConnectPacket_t* halfcon = pod_container_of(cur, ReliableDgramHalfConnectPacket_t, m_listnode);
				socketClose(halfcon->sockfd);
				free(halfcon);
			}
			else {
				free(pod_container_of(cur, ReliableDgramDataPacket_t, msg.m_listnode));
			}
		}
	}
	for (cur = s->m_sendpacketlist.head; cur; cur = next) {
		next = cur->next;
		free(cur);
	}

	if (s->m_free)
		s->m_free(s);
}

void niosocketManualClose(NioSocket_t* s) {
	if (s->m_loop) {
		nioloop_exec_msg(s->m_loop, &s->m_closemsg.m_listnode);
	}
	else {
		niosocket_free(s);
	}
}

static int sockht_keycmp(const struct HashtableNode_t* node, const void* key) {
	return pod_container_of(node, NioSocket_t, m_hashnode)->fd != *(FD_t*)key;
}

static unsigned int sockht_keyhash(const void* key) { return *(FD_t*)key; }

static void sockcloselist_update(NioLoop_t* loop, long long timestamp_msec) {
	ListNode_t* cur, *next;
	List_t expirelist;
	listInit(&expirelist);
	for (cur = loop->m_sockcloselist.head; cur; cur = next) {
		NioSocket_t* s = pod_container_of(cur, NioSocket_t, m_closemsg.m_listnode);
		next = cur->next;
		if (s->m_lastactive_msec + s->m_close_timeout_msec > timestamp_msec) {
			update_timestamp(&loop->m_event_msec, s->m_lastactive_msec + s->m_close_timeout_msec);
			continue;
		}
		free_io_resource(s);
		free_inbuf(s);
		listRemoveNode(&loop->m_sockcloselist, cur);
		listInsertNodeBack(&expirelist, expirelist.tail, cur);
	}
	dataqueuePushList(loop->m_msgdq, &expirelist);
}

static void sockht_update(NioLoop_t* loop, long long timestamp_msec) {
	HashtableNode_t *cur, *next;
	for (cur = hashtableFirstNode(&loop->m_sockht); cur; cur = next) {
		NioSocket_t* s = pod_container_of(cur, NioSocket_t, m_hashnode);
		next = hashtableNextNode(cur);
		if (s->m_valid) {
			if (s->keepalive_timeout_sec > 0 && s->m_lastactive_msec + s->keepalive_timeout_sec * 1000 <= timestamp_msec) {
				s->m_valid = 0;
				free_inbuf(s);
				if (SOCK_STREAM == s->socktype || s->keepalive_timeout_sec * 1000 >= s->m_close_timeout_msec)
					free_io_resource(s);
			}
			else {
				if (NIOSOCKET_TRANSPORT_CLIENT == s->transport_side &&
					s->m_sendprobe_msec > 0 && s->send_probe && s->sendprobe_timeout_sec > 0)
				{
					if (s->m_sendprobe_msec + s->sendprobe_timeout_sec * 1000 <= timestamp_msec) {
						s->m_sendprobe_msec = timestamp_msec;
						s->send_probe(s);
					}
					update_timestamp(&loop->m_event_msec, s->m_sendprobe_msec + s->sendprobe_timeout_sec * 1000);
				}
				if (s->keepalive_timeout_sec > 0)
					update_timestamp(&loop->m_event_msec, s->m_lastactive_msec + s->keepalive_timeout_sec * 1000);
				if (s->reliable.m_status)
					reliable_dgram_update(loop, s, timestamp_msec);
				continue;
			}
		}
		hashtableRemoveNode(&loop->m_sockht, cur);
		listInsertNodeBack(&loop->m_sockcloselist, loop->m_sockcloselist.tail, &s->m_closemsg.m_listnode);
		_xchg16(&s->m_shutdown, 1);
	}
}

int nioloopHandler(NioLoop_t* loop, NioEv_t e[], int n, long long timestamp_msec, int wait_msec) {
	ListNode_t *cur, *next;
	if (loop->m_event_msec > timestamp_msec) {
		int checkexpire_wait_msec = loop->m_event_msec - timestamp_msec;
		if (checkexpire_wait_msec < wait_msec || wait_msec < 0)
			wait_msec = checkexpire_wait_msec;
	}
	else if (loop->m_event_msec) {
		wait_msec = 0;
	}

	n = reactorWait(&loop->m_reactor, e, n, wait_msec);
	if (n < 0) {
		return n;
	}
	else if (n > 0) {
		int i;
		timestamp_msec = gmtimeMillisecond();
		for (i = 0; i < n; ++i) {
			HashtableNode_t* find_node;
			NioSocket_t* s;
			int event;
			void* ol;
			FD_t fd = reactorEventFD(e + i);
			if (fd == loop->m_socketpair[0]) {
				struct sockaddr_storage saddr;
				char c[512];
				socketRead(fd, c, sizeof(c), 0, NULL);
				reactorCommit(&loop->m_reactor, fd, REACTOR_READ, loop->m_readol, &saddr);
				_xchg16(&loop->m_wake, 0);
				continue;
			}
			find_node = hashtableSearchKey(&loop->m_sockht, &fd);
			if (!find_node)
				continue;
			s = pod_container_of(find_node, NioSocket_t, m_hashnode);
			if (!s->m_valid)
				continue;
			reactorEventOpcodeAndOverlapped(e + i, &event, &ol);
			switch (event) {
				case REACTOR_READ:
					reactor_socket_do_read(s, timestamp_msec);
					reactorsocket_read(s);
					break;
				case REACTOR_WRITE:
					reactor_socket_do_write(s, timestamp_msec);
					break;
				default:
					s->m_valid = 0;
			}
			if (s->m_valid)
				continue;
			hashtableRemoveNode(&loop->m_sockht, &s->m_hashnode);
			s->m_sendaction = SEND_SHUTDOWN_ACTION;
			_xchg16(&s->m_shutdown, 1);
			free_inbuf(s);
			if (SOCK_STREAM == s->socktype) {
				free_io_resource(s);
				if (s->keepalive_timeout_sec > 0) {
					s->m_close_timeout_msec = s->keepalive_timeout_sec * 1000;
				}
				dataqueuePush(loop->m_msgdq, &s->m_shutdownmsg.m_listnode);
			}
			if (s->m_close_timeout_msec > 0) {
				s->m_lastactive_msec = timestamp_msec;
				update_timestamp(&loop->m_event_msec, s->m_lastactive_msec + s->m_close_timeout_msec);
				listInsertNodeBack(&loop->m_sockcloselist, loop->m_sockcloselist.tail, &s->m_closemsg.m_listnode);
			}
			else
				dataqueuePush(loop->m_msgdq, &s->m_closemsg.m_listnode);
		}
	}
	else {
		timestamp_msec += wait_msec;
	}

	criticalsectionEnter(&loop->m_msglistlock);
	cur = loop->m_msglist.head;
	listInit(&loop->m_msglist);
	criticalsectionLeave(&loop->m_msglistlock);

	for (; cur; cur = next) {
		NioInternalMsg_t* message;
		next = cur->next;
		message = pod_container_of(cur, NioInternalMsg_t, m_listnode);
		if (NIO_SOCKET_CLOSE_MESSAGE == message->type) {
			NioSocket_t* s = pod_container_of(message, NioSocket_t, m_closemsg);
			niosocket_free(s);
		}
		else if (NIO_SOCKET_SHUTDOWN_POST_MESSAGE == message->type) {
			NioSocket_t* s = pod_container_of(message, NioSocket_t, m_shutdownpostmsg);
			if (SOCK_STREAM == s->socktype) {
				if (NIOSOCKET_TRANSPORT_LISTEN == s->transport_side) {
					hashtableRemoveNode(&loop->m_sockht, &s->m_hashnode);
					dataqueuePush(loop->m_msgdq, &s->m_closemsg.m_listnode);
				}
				else if (SEND_OK_ACTION == s->m_sendaction) {
					s->m_sendaction = SEND_SHUTDOWN_ACTION;
					s->m_sendprobe_msec = 0;
					socketShutdown(s->fd, SHUT_WR);
				}
			}
			else {
				reliable_dgram_shutdown(s, timestamp_msec);
			}
		}
		else if (NIO_SOCKET_PACKET_MESSAGE == message->type) {
			Packet_t* packet = pod_container_of(message, Packet_t, msg);
			NioSocket_t* s = packet->s;
			if (!s->m_valid || SEND_SHUTDOWN_ACTION == s->m_sendaction) {
				free(packet);
				continue;
			}
			if (SOCK_STREAM == s->socktype)
				stream_send_packet(packet->s, packet);
			else {
				struct sockaddr_storage* saddrptr = (packet->saddr.ss_family != AF_UNSPEC ? &packet->saddr : NULL);
				socketWrite(s->fd, packet->data, packet->len, 0, saddrptr);
				free(packet);
			}
		}
		else if (NIO_SOCKET_RELIABLE_PACKET_MESSAGE == message->type) {
			ReliableDgramDataPacket_t* packet = pod_container_of(message, ReliableDgramDataPacket_t, msg);
			NioSocket_t* s = packet->s;
			if (!s->m_valid || SEND_SHUTDOWN_ACTION == s->m_sendaction) {
				free(packet);
				continue;
			}
			reliable_dgram_send_packet(s, packet, timestamp_msec);
		}
		else if (NIO_SOCKET_RECONNECT_MESSAGE == message->type) {
			NioSocket_t* s = pod_container_of(message, NioSocket_t, m_reconnectmsg);
			if (SOCK_STREAM == s->socktype) {
				int ok;
				if (NIOSOCKET_TRANSPORT_CLIENT != s->transport_side || SEND_OK_ACTION != s->m_sendaction)
					continue;
				hashtableRemoveNode(&loop->m_sockht, &s->m_hashnode);
				socketClose(s->fd);
				do {
					ok = 0;
					s->fd = socket(s->domain, s->socktype, s->protocol);
					if (INVALID_FD_HANDLE == s->fd)
						break;
					if (!s->m_writeol) {
						s->m_writeol = reactorMallocOverlapped(REACTOR_CONNECT, NULL, 0, 0);
						if (!s->m_writeol)
							break;
					}
					if (!reactorCommit(&loop->m_reactor, s->fd, REACTOR_CONNECT, s->m_writeol, &s->peer_listen_saddr))
						break;
					s->m_valid = 1;
					s->m_sendaction = SEND_RECONNECT_ACTION;
					s->m_lastactive_msec = timestamp_msec;
					s->m_sendprobe_msec = 0;
					free_inbuf(s);
					hashtableReplaceNode(hashtableInsertNode(&loop->m_sockht, &s->m_hashnode), &s->m_hashnode);
					ok = 1;
				} while (0);
				if (!ok) {
					s->m_valid = 0;
					s->m_sendaction = SEND_SHUTDOWN_ACTION;
					socketClose(s->fd);
					s->fd = INVALID_FD_HANDLE;
					s->m_regerrno = errnoGet();
					dataqueuePush(loop->m_msgdq, &s->m_reconnectmsg.m_listnode);
				}
			}
			else {
				reliable_dgram_reconnect(s, timestamp_msec);
			}
		}
		else if (NIO_SOCKET_REG_MESSAGE == message->type) {
			NioSocket_t* s = pod_container_of(message, NioSocket_t, m_regmsg);
			int reg_ok = 0, immedinate_call_reg = 0;
			do {
				if (!reactorReg(&loop->m_reactor, s->fd))
					break;
				s->m_lastactive_msec = timestamp_msec;
				if (SOCK_STREAM == s->socktype) {
					if (NIOSOCKET_TRANSPORT_CLIENT == s->transport_side) {
						BOOL has_connected;
						if (!socketIsConnected(s->fd, &has_connected))
							break;
						if (has_connected) {
							if (!reactorsocket_read(s))
								break;
							s->m_sendprobe_msec = timestamp_msec;
							s->m_sendaction = SEND_OK_ACTION;
							immedinate_call_reg = 1;
						}
						else {
							if (!s->m_writeol) {
								s->m_writeol = reactorMallocOverlapped(REACTOR_CONNECT, NULL, 0, 0);
								if (!s->m_writeol)
									break;
							}
							if (!reactorCommit(&loop->m_reactor, s->fd, REACTOR_CONNECT, s->m_writeol, &s->peer_listen_saddr))
								break;
							s->m_sendaction = SEND_CONNECT_ACTION;
						}
					}
					else {
						if (NIOSOCKET_TRANSPORT_LISTEN == s->transport_side) {
							BOOL has_listen;
							if (AF_UNSPEC == s->local_listen_saddr.ss_family) {
								if (!socketGetLocalAddr(s->fd, &s->local_listen_saddr))
									break;
							}
							if (!socketIsListened(s->fd, &has_listen))
								break;
							if (!has_listen && !socketTcpListen(s->fd))
								break;
						}
						else {
							s->m_sendaction = SEND_OK_ACTION;
							s->m_sendprobe_msec = timestamp_msec;
						}
						if (!reactorsocket_read(s))
							break;
						immedinate_call_reg = 1;
					}
				}
				else {
					if (s->reliable.enable) {
						if (NIOSOCKET_TRANSPORT_CLIENT == s->transport_side && s->peer_listen_saddr.ss_family != AF_UNSPEC) {
							unsigned char syn = HDR_SYN;
							socketWrite(s->fd, &syn, sizeof(syn), 0, &s->peer_listen_saddr);
							s->m_sendaction = SEND_CONNECT_ACTION;
							s->reliable.m_status = SYN_SENT_STATUS;
							s->reliable.peer_saddr = s->peer_listen_saddr;
							s->reliable.m_synsent_msec = timestamp_msec + s->reliable.rto;
							update_timestamp(&loop->m_event_msec, s->reliable.m_synsent_msec);
						}
						else if (NIOSOCKET_TRANSPORT_LISTEN == s->transport_side) {
							if (AF_UNSPEC == s->local_listen_saddr.ss_family) {
								if (!socketGetLocalAddr(s->fd, &s->local_listen_saddr))
									break;
							}
							if (s->accept_callback) {
								s->reliable.m_status = LISTENED_STATUS;
								if (s->m_recvpacket_maxcnt < 200)
									s->m_recvpacket_maxcnt = 200;
								immedinate_call_reg = 1;
							}
							else {
								s->reliable.m_status = SYN_RCVD_STATUS;
							}
						}
						else {
							s->m_sendaction = SEND_OK_ACTION;
							s->reliable.m_status = ESTABLISHED_STATUS;
							s->m_sendprobe_msec = timestamp_msec;
							immedinate_call_reg = 1;
						}
						s->m_close_timeout_msec = MSL + MSL;
					}
					else {
						s->m_sendaction = SEND_OK_ACTION;
						immedinate_call_reg = 1;
					}
					if (!reactorsocket_read(s))
						break;
				}
				hashtableReplaceNode(hashtableInsertNode(&loop->m_sockht, &s->m_hashnode), &s->m_hashnode);
				if (s->m_sendprobe_msec > 0 && s->sendprobe_timeout_sec > 0) {
					update_timestamp(&loop->m_event_msec, s->m_sendprobe_msec + s->sendprobe_timeout_sec * 1000);
				}
				if (s->keepalive_timeout_sec > 0) {
					update_timestamp(&loop->m_event_msec, s->m_lastactive_msec + s->keepalive_timeout_sec * 1000);
				}
				reg_ok = 1;
			} while (0);
			if (reg_ok) {
				if (s->reg_callback && immedinate_call_reg) {
					dataqueuePush(loop->m_msgdq, &s->m_regmsg.m_listnode);
				}
			}
			else {
				List_t msglist;
				listInit(&msglist);
				listInsertNodeBack(&msglist, msglist.tail, &s->m_regmsg.m_listnode);
				listInsertNodeBack(&msglist, msglist.tail, &s->m_closemsg.m_listnode);
				s->m_sendaction = SEND_SHUTDOWN_ACTION;
				s->shutdown_callback = NULL;
				s->m_loop = NULL;
				s->m_regerrno = errnoGet();
				dataqueuePushList(loop->m_msgdq, &msglist);
			}
		}
	}
	if (loop->m_event_msec && timestamp_msec >= loop->m_event_msec) {
		loop->m_event_msec = 0;
		sockht_update(loop, timestamp_msec);
		sockcloselist_update(loop, timestamp_msec);
	}
	return n;
}

NioLoop_t* nioloopCreate(NioLoop_t* loop, DataQueue_t* msgdq) {
	struct sockaddr_storage saddr;
	loop->m_initok = 0;

	if (!socketPair(SOCK_STREAM, loop->m_socketpair))
		return NULL;

	loop->m_readol = reactorMallocOverlapped(REACTOR_READ, NULL, 0, 0);
	if (!loop->m_readol) {
		socketClose(loop->m_socketpair[0]);
		socketClose(loop->m_socketpair[1]);
		return NULL;
	}

	if (!reactorCreate(&loop->m_reactor)) {
		reactorFreeOverlapped(loop->m_readol);
		socketClose(loop->m_socketpair[0]);
		socketClose(loop->m_socketpair[1]);
		return NULL;
	}

	if (!socketNonBlock(loop->m_socketpair[0], TRUE) ||
		!socketNonBlock(loop->m_socketpair[1], TRUE) ||
		!reactorReg(&loop->m_reactor, loop->m_socketpair[0]) ||
		!reactorCommit(&loop->m_reactor, loop->m_socketpair[0], REACTOR_READ, loop->m_readol, &saddr))
	{
		reactorFreeOverlapped(loop->m_readol);
		socketClose(loop->m_socketpair[0]);
		socketClose(loop->m_socketpair[1]);
		reactorClose(&loop->m_reactor);
		return NULL;
	}

	if (!criticalsectionCreate(&loop->m_msglistlock)) {
		reactorFreeOverlapped(loop->m_readol);
		socketClose(loop->m_socketpair[0]);
		socketClose(loop->m_socketpair[1]);
		reactorClose(&loop->m_reactor);
		return NULL;
	}

	loop->m_msgdq = msgdq;
	listInit(&loop->m_msglist);
	listInit(&loop->m_sockcloselist);
	hashtableInit(&loop->m_sockht,
		loop->m_sockht_bulks, sizeof(loop->m_sockht_bulks) / sizeof(loop->m_sockht_bulks[0]),
		sockht_keycmp, sockht_keyhash);
	loop->m_initok = 1;
	loop->m_wake = 0;
	loop->m_event_msec = 0;
	return loop;
}

NioLoop_t* nioloopWake(NioLoop_t* loop) {
	if (0 == _cmpxchg16(&loop->m_wake, 1, 0)) {
		char c;
		socketWrite(loop->m_socketpair[1], &c, sizeof(c), 0, NULL);
	}
	return loop;
}

void nioloopReg(NioLoop_t* loop, NioSocket_t* s[], size_t n) {
	size_t i;
	List_t list;
	listInit(&list);
	for (i = 0; i < n; ++i) {
		s[i]->m_loop = loop;
		listInsertNodeBack(&list, list.tail, &s[i]->m_regmsg.m_listnode);
	}
	nioloop_exec_msglist(loop, &list);
}

void nioloopDestroy(NioLoop_t* loop) {
	if (loop && loop->m_initok) {
		reactorFreeOverlapped(loop->m_readol);
		socketClose(loop->m_socketpair[0]);
		socketClose(loop->m_socketpair[1]);
		reactorClose(&loop->m_reactor);
		criticalsectionClose(&loop->m_msglistlock);
		do {
			ListNode_t* cur, *next;
			for (cur = loop->m_msglist.head; cur; cur = next) {
				NioInternalMsg_t* msgbase = pod_container_of(cur, NioInternalMsg_t, m_listnode);
				next = cur->next;
				if (NIO_SOCKET_PACKET_MESSAGE == msgbase->type ||
					NIO_SOCKET_RELIABLE_PACKET_MESSAGE == msgbase->type)
				{
					free(cur);
				}
			}
		} while (0);
		do {
			HashtableNode_t *cur, *next;
			for (cur = hashtableFirstNode(&loop->m_sockht); cur; cur = next) {
				next = hashtableNextNode(cur);
				niosocket_free(pod_container_of(cur, NioSocket_t, m_hashnode));
			}
		} while (0);
	}
}

void niomsgHandler(DataQueue_t* dq, int max_wait_msec, void (*user_msg_callback)(NioMsg_t*, void*), void* arg) {
	ListNode_t* cur, *next;
	for (cur = dataqueuePop(dq, max_wait_msec, ~0); cur; cur = next) {
		NioInternalMsg_t* message = pod_container_of(cur, NioInternalMsg_t, m_listnode);
		next = cur->next;
		if (NIO_SOCKET_USER_MESSAGE == message->type) {
			user_msg_callback((NioMsg_t*)message, arg);
		}
		else if (NIO_SOCKET_SHUTDOWN_MESSAGE == message->type) {
			NioSocket_t* s = pod_container_of(message, NioSocket_t, m_shutdownmsg);
			if (s->shutdown_callback) {
				s->shutdown_callback(s);
				s->shutdown_callback = NULL;
			}
		}
		else if (NIO_SOCKET_CLOSE_MESSAGE == message->type) {
			NioSocket_t* s = pod_container_of(message, NioSocket_t, m_closemsg);
			if (s->shutdown_callback) {
				s->shutdown_callback(s);
				s->shutdown_callback = NULL;
			}
			if (s->close) {
				s->close(s);
				s->close = NULL;
			}
			else {
				nioloop_exec_msg(s->m_loop, cur);
			}
		}
		else if (NIO_SOCKET_REG_MESSAGE == message->type) {
			NioSocket_t* s = pod_container_of(message, NioSocket_t, m_regmsg);
			if (s->reg_callback) {
				s->reg_callback(s, s->m_regerrno);
				s->reg_callback = NULL;
			}
		}
		else if (NIO_SOCKET_RECONNECT_MESSAGE == message->type) {
			NioSocket_t* s = pod_container_of(message, NioSocket_t, m_reconnectmsg);
			if (s->reconnect_callback) {
				s->reconnect_callback(s, s->m_regerrno);
				s->reg_callback = NULL;
			}
		}
	}
}

void niomsgClean(DataQueue_t* dq, void(*deleter)(NioMsg_t*)) {
	ListNode_t *cur = dataqueuePop(dq, 0, ~0);
	if (deleter) {
		ListNode_t *next;
		for (; cur; cur = next) {
			NioInternalMsg_t* message = pod_container_of(cur, NioInternalMsg_t, m_listnode);
			next = cur->next;
			if (NIO_SOCKET_USER_MESSAGE == message->type)
				deleter((NioMsg_t*)message);
		}
	}
}

#ifdef __cplusplus
}
#endif
