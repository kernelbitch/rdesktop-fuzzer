/*
 * fuzz.c
 *
 *  Created on: Aug 11, 2013
 *      Author: level
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include "rdesktop.h"


STREAM fuzz_handler(STREAM data)
{
	struct sockaddr_in si_other_send;
	struct sockaddr_in si_me_rcv, si_other_rcv;
	int s_rcv, slen_rcv=sizeof(si_other_rcv);
	int s_send, ret=-1, slen_send=sizeof(si_other_send);
	char buf[1496];

	if ((s_rcv=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))!=-1) {
		if ((s_send=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))!=-1) {
			/* define send socket & variables */
			memset((char *) &si_other_send, 0, sizeof(si_other_send));
			si_other_send.sin_family = AF_INET;
			si_other_send.sin_port = htons(9876);
			/* define rcv socket & variables */
			memset((char *) &si_me_rcv, 0, sizeof(si_me_rcv));
			si_me_rcv.sin_family = AF_INET;
			si_me_rcv.sin_port = htons(9876);
			si_me_rcv.sin_addr.s_addr = htonl(INADDR_ANY);
			/* proxy fuzzer ip address, CHANGE BELOW! */
			if (inet_aton("127.0.0.1", &si_other_send.sin_addr)!=0) {
				/* proxy fuzzer ip address, CHANGE ABOVE! */
				/* bind to rcv port */
				if (bind(s_rcv, &si_me_rcv, sizeof(si_me_rcv))!=-1) {
					/* send packet to fuzzer */
					/* had to lower frame size to 1024 otherwise Peach complains */
					if (sendto(s_send, data, 1024, 0, &si_other_send, slen_send)!=-1) {
						DEBUG(("Packet sent to be fuzzed\n"));
					}
					/* rcv packet from fuzzer */
					/* had to lower frame size to 1024 otherwise Peach complains */
					if (recvfrom(s_rcv, buf, 1024, 0, &si_other_rcv, &slen_rcv)!=-1) {
						DEBUG(("Received packet from %s:%d\n", inet_ntoa(si_other_rcv.sin_addr), ntohs(si_other_rcv.sin_port)));
					}
				}
			}
		}
	}
	close(s_rcv);
	close(s_send);
	return buf;
}
