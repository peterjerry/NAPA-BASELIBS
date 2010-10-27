/*
 *          
 *
 *
 * 
 *	Agnieszka Witczak & Szymon Kuc
 *     
 */


#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <unistd.h>
#include <string.h>
#ifndef WIN32
#include <sys/uio.h>
#include <netinet/in.h>
#endif

#include "queueManagement.h"
#include "udpSocket.h"

#include "../transmissionHandler.h"

FILE *file = NULL;

PacketQueue TXqueue;
PacketQueue RTXqueue;

unsigned int sentRTXDataPktCounter = 0;

int TXmaxSize = 6000*1500;			//bytes
int RTXmaxSize = 6000*1500;			//bytes

struct timeval maxTimeToHold = {5,0};

PacketContainer* createPacketContainer(const int uSoc,struct iovec *ioVector,int iovlen,struct sockaddr_in *sockAddress, unsigned char prior) {
	
	PacketContainer *packet = malloc(sizeof(PacketContainer));
	packet->udpSocket = uSoc;
	packet->iovlen = iovlen;
	packet->next = NULL;
	packet->pktLen = ioVector[0].iov_len + ioVector[1].iov_len + ioVector[2].iov_len + ioVector[3].iov_len;

	packet->priority = prior;

 	int i;
        packet->iov = malloc(sizeof(struct iovec) * iovlen);
        for (i=0; i<iovlen; i++){
                packet->iov[i].iov_len = ioVector[i].iov_len;
                packet->iov[i].iov_base = malloc(packet->iov[i].iov_len);
                memcpy(packet->iov[i].iov_base, ioVector[i].iov_base, packet->iov[i].iov_len);
        }

        packet->socketaddr = malloc(sizeof(struct sockaddr_in));
        memcpy(packet->socketaddr, sockAddress, sizeof(struct sockaddr_in));

	int packet_len = ioVector[0].iov_len + ioVector[1].iov_len + ioVector[2].iov_len + ioVector[3].iov_len;
	return packet;
}

void destroyPacketContainer(PacketContainer* pktContainer){

        int i;
        for (i=0; i < pktContainer->iovlen; i++) free(pktContainer->iov[i].iov_base);
        free(pktContainer->iov);
        free(pktContainer->socketaddr);
        free(pktContainer);
}

int addPacketTXqueue(PacketContainer *packet) {
	if ((TXqueue.size + packet->pktLen) > TXmaxSize) {
		fprintf(stderr,"[queueManagement::addPacketTXqueue] -- Sorry! Max size for TX queue. Packet will be discarded. \n");
		destroyPacketContainer(packet);
		return THROTTLE;
	}
	TXqueue.size += packet->pktLen;	

	if (TXqueue.head == NULL) {			//adding first element
		TXqueue.head = packet;
		TXqueue.tail = packet;		
	} else {					//adding at the end of the queue
		TXqueue.tail->next = packet;
		TXqueue.tail = packet;
	}
	
	//logQueueOccupancy();
	return OK;
}

PacketContainer* takePacketToSend() {			//returns pointer to packet or NULL if queue is empty

	//logQueueOccupancy();
	if (TXqueue.head != NULL) {
		PacketContainer *packet = TXqueue.head;
		
		TXqueue.size -= packet->pktLen;
		TXqueue.head = TXqueue.head->next;
		packet->next = NULL;
					
		return packet;
	}
	else return NULL;	
}


void addPacketRTXqueue(PacketContainer *packet) {
	//removing old packets - because of maxTimeToHold
	struct timeval now, age;
	gettimeofday(&now, NULL);
 
	PacketContainer *tmp = RTXqueue.head;
	while (tmp != NULL) {
		age.tv_sec = now.tv_sec - tmp->timeStamp.tv_sec;
		age.tv_usec = now.tv_usec - tmp->timeStamp.tv_usec;

		if (age.tv_sec > maxTimeToHold.tv_sec) {

			tmp = tmp->next;
			removeOldestPacket();
		}
		else break;
	}


	if ((RTXqueue.size + packet->pktLen) > RTXmaxSize) {
		removeOldestPacket();
	}

	//adding timeStamp
	gettimeofday(&(packet->timeStamp), NULL);

	//finally - adding packet
	RTXqueue.size += packet->pktLen;
	packet->next = NULL;

	if (RTXqueue.head == NULL) {			//adding first element
		RTXqueue.head = packet;
		RTXqueue.tail = packet;
		return;	
	} else {					//adding at the end of the queue
		RTXqueue.tail->next = packet;
		RTXqueue.tail = packet;
		return;
	}
}

int removeOldestPacket() {			//return 0 if success, else (queue empty) return 1
	if (RTXqueue.head != NULL) {
		RTXqueue.size -= RTXqueue.head->pktLen;
		PacketContainer *pointer = RTXqueue.head;		
		RTXqueue.head = RTXqueue.head->next;
		destroyPacketContainer(pointer);
		return 0;
	}
	return 1;	
}

void setQueuesParams (int TXsize, int RTXsize, double maxTTHold) {
	TXmaxSize = TXsize * 1024;
	RTXmaxSize = RTXsize * 1024;
	maxTimeToHold.tv_sec = (int)floor(maxTTHold);
	maxTimeToHold.tv_usec = (int)(1000000.0 * fmod(maxTTHold, 1.0));
}

int isQueueEmpty() {
	
	if (TXqueue.head == NULL) return 1;
	return 0;
}

int getFirstPacketSize() {

	if (TXqueue.head != NULL) return TXqueue.head->pktLen;
	return 0;
}

PacketContainer* searchPacketInRTX(int connID, int msgSeqNum, int offset) {
	
	connID = ntohl(connID);
	msgSeqNum = ntohl(msgSeqNum);
	offset = ntohl(offset);

	//fprintf(stderr,"***************Searching for packet... connID: %d msgSeqNum: %d offset: %d\n",connID,ntohl(msgSeqNum),ntohl(offset));

	PacketContainer *tmp = RTXqueue.head;
	PacketContainer *prev = NULL;

	while (tmp != NULL) {
		struct msg_header *msg_h;
		
		msg_h = (struct msg_header *) tmp->iov[0].iov_base;

		//fprintf(stderr,"^^^^^^^^^Searching^^^^^^^^^^^^ connID: %d msgSeqNum: %d offset: %d\n",connID,ntohl(msgSeqNum),offset);
		//fprintf(stderr,"^^^^^^^^^In queue^^^^^^^^^^^^ connID: %d msgSeqNum: %d offset: %d\n",msg_h->local_con_id,ntohl(msg_h->msg_seq_num),ntohl(msg_h->offset));

		if ((msg_h->local_con_id == connID) && (msg_h->msg_seq_num == msgSeqNum) && (msg_h->offset == offset)) {
		//fprintf(stderr,"*****************Packet found: ConID: %d  MsgseqNum: %d Offset: %d \n", ntohl(connID),ntohl(msgSeqNum),ntohl(offset));
			if (tmp == RTXqueue.head) RTXqueue.head = tmp->next;
			else if (tmp == RTXqueue.tail) { RTXqueue.tail = prev; prev->next == NULL; }
			else prev->next = tmp->next;
			RTXqueue.size -= tmp->pktLen;
			return tmp;
		}
		prev = tmp;
		tmp = tmp->next;
	}

return NULL;
}

int rtxPacketsFromTo(int connID, int msgSeqNum, int offsetFrom, int offsetTo) {
        int offset = offsetFrom;
        //fprintf(stderr,"\t\t\t\t Retransmission request From: %d To: %d\n",offsetFrom, offsetTo);

        while (offset < offsetTo) {
                PacketContainer *packetToRTX = searchPacketInRTX(connID,msgSeqNum,offset);
                if (packetToRTX == NULL) return 1;

                //sending packet
                //fprintf(stderr,"\t\t\t\t\t Retransmitting packet: %d of msg_seq_num %d.\n",offset/1349,msgSeqNum);
                sendPacket(packetToRTX->udpSocket, packetToRTX->iov, 4, packetToRTX->socketaddr);
                sentRTXDataPktCounter++;
                //queueOrSendPacket(packetToRTX->udpSocket,packetToRTX->iov,packetToRTX->pktLen,packetToRTX->socketaddr,HP);
                offset += packetToRTX->iov[3].iov_len;                  //????????????????????
        }
        return 0;
}

// void logQueueOccupancy() {
// 	struct timeval now;
// 	gettimeofday(&now, NULL);
// 	double time = now.tv_sec + ((double) now.tv_usec)/1000000;
// 	file = fopen("./queue.txt", "a+");
// 	//char *str;
// 	//sprintf(str,"%f",time); 
// 	//fputs(str, p);
// 	fprintf(file,"%f \t %d \n",time, TXqueue.size);	
// 	fprintf(stderr,"[queueManagement::logQueueOccupancy] -- Time: %f \t queueOccupancy: %d \n", time, TXqueue.size);
// 	fclose(file);
// }