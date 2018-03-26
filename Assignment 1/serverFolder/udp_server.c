#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <sys/time.h>
#include <dirent.h>

#define GET_FILE 	0x15
#define PUT_FILE 	0x25
#define	DELETE    0x35
#define LIST      0x45
#define EXIT_SERVER  0x55
#define ACK_SUCCESS	 0x65
#define NO_ACK			 0x75

#define INCOMING_FILE_TRANSFER	0x20
#define TRANSFERRING	0x29
#define	INCOMING_TRANSFER_FINISHED	0x2E

#define START_FILE_TRANSFER	0x10
#define SENDING_TRANSFER_FINISHED 0x1E

//#define MAXBUFSIZE 100
#define CIPHER 0xF2
#define DATA_SIZE 1024
#define TOTAL_COMMANDS 5

struct data_packet
{
	int sequence_id;
	unsigned char service_id;
	int checksum;
	int bytes_length;
	unsigned char enc_payload[DATA_SIZE];
};

struct request_packet
{
	int sequence_id;
	unsigned char service_id;
	unsigned char filename[50];
};

typedef struct data_packet data_packet_t;
typedef struct request_packet request_packet_t;

int sock;                           //This will be our socket
//unsigned char buffer[MAXBUFSIZE];             //a buffer to store our received message
unsigned int remote_length = 0;         //length of the sockaddr_in structure
int rx_bytes = 0, tx_bytes = 0;                        //number of bytes we receive in our message
struct sockaddr_in remote;     //"Internet socket address structure"

int global_server_sequence_id = 0;
int global_client_sequence_id = 1000;
unsigned char transfer_finished = 0;
unsigned char checksum_mismatch = 0;
unsigned char same_sequence_number = 0;

void execute_service(int, unsigned char*);
void server_exit(void);
void list(void);
void put_file(unsigned char*);
void send_data_packet_and_wait_for_ack(unsigned char, int, unsigned char*, int);
void xor_encrypt(unsigned char*);
int checksum(unsigned char*);
data_packet_t receive_data_packet_and_send_ack(void);
unsigned char receive_request_packet_and_send_ack(void);
void send_request_packet_and_wait_for_ack(unsigned char);
void put_file(unsigned char*);
void get_file(unsigned char*);
void delete(unsigned char*);

int main (int argc, unsigned char * argv[] )
{
	struct sockaddr_in sin;     //"Internet socket address structure"
	request_packet_t buffer;
	unsigned char command;

	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 5000;

	if (argc != 2)
	{
		printf ("\nUSAGE: <port>\n");
		exit(-1);
	}

//This code populates the sockaddr_in struct with the information about our socket
	bzero(&sin,sizeof(sin));                    //zero the struct
	sin.sin_family = AF_INET;                   //address family
	sin.sin_port = htons(atoi(argv[1]));        //htons() sets the port # to network byte order
	sin.sin_addr.s_addr = INADDR_ANY;           //supplies the IP address of the local machine

//Causes the system to create a generic socket of type UDP (datagram)
	if((sock = socket(sin.sin_family, SOCK_DGRAM, 0)) < 0)
	{
		printf("## ERROR ## Unable to create socket.\n");
		exit(-1);
	}

//Once we've created a socket, we must bind that socket to the local address and port we've supplied in the sockaddr_in struct
	if(bind(sock, (struct sockaddr *)&sin, sizeof(sin)) < 0)
	{
		printf("## ERROR ## Unable to bind socket.\n");
		exit(-1);
	}

	remote_length = sizeof(remote);

	while(1)
	{
			//waits for an incoming message
			bzero(&buffer, sizeof(buffer));

			printf("\n## SERVER ## Idle.\n");

			rx_bytes = recvfrom(sock, &buffer, sizeof(buffer), 0, (struct sockaddr *)&remote, &remote_length);
			command = buffer.service_id;
			buffer.service_id = ACK_SUCCESS;


			tx_bytes = sendto(sock, (void *) &buffer, sizeof(request_packet_t), 0, (struct sockaddr *)&remote, remote_length);

			printf("## SERVER ## Request received. Sequence ID: %d\n", buffer.sequence_id);
			execute_service(command, buffer.filename);
	}
}

void execute_service(int service, unsigned char* filename)
{
	switch(service)
	{
		case 0x15:
		printf("\n## SERVER ## The client has initiated a download. File: %s\n", filename);
		get_file(filename);
		break;

		case 0x25:
		printf("\n## SERVER ## The client initiated an upload. File: %s\n", filename);
		put_file(filename);
		break;

		case 0x35:
		printf("\n## SERVER ## The client has initiated deletion of file %s\n", filename);
		delete(filename);
		break;

		case 0x45:
		printf("\n## SERVER ## The client has requested the list of files.\n");
		list();
		break;

		case 0x55:
		server_exit();
		break;
	}
}

void get_file(unsigned char *file)
{
	FILE *target;
	unsigned char tx_data[DATA_SIZE];
	int bytes_count = DATA_SIZE;

	target = fopen(file, "rb");

	if(target == NULL)
	{
		printf("\n## ERROR ## Unable to open file %s\n", file);
		return;
	}

	send_request_packet_and_wait_for_ack(START_FILE_TRANSFER);

	while(bytes_count == DATA_SIZE)
	{
		bzero(tx_data, sizeof(tx_data));

		bytes_count = fread(tx_data, sizeof(unsigned char), DATA_SIZE, target);

		xor_encrypt(tx_data);

		if(bytes_count == DATA_SIZE)
			send_data_packet_and_wait_for_ack(TRANSFERRING, bytes_count, tx_data, checksum(tx_data));

		if(bytes_count != DATA_SIZE)
		{
			printf("Sending the last packet.\n");
			send_data_packet_and_wait_for_ack(SENDING_TRANSFER_FINISHED, bytes_count, tx_data, checksum(tx_data));
		}
	}

	printf("\n## SERVER ## File %s has been successfully uploaded.\n", file);
}

void put_file(unsigned char* filename)
{
	FILE *newfile;
	unsigned char service_id;
	data_packet_t rx_packet;
	unsigned char rx_data[DATA_SIZE];
	newfile = fopen(filename, "wb");

	service_id = receive_request_packet_and_send_ack();

	if(service_id == INCOMING_FILE_TRANSFER)
		printf("\n## SERVER ## Starting file transfer.\n");

	while(transfer_finished == 0)
	{
		bzero(&rx_packet, sizeof(rx_packet));
		bzero(rx_data, sizeof(rx_data));

		checksum_mismatch = 0;
		same_sequence_number = 0;

		rx_packet = receive_data_packet_and_send_ack();
		memcpy(rx_data, rx_packet.enc_payload, rx_packet.bytes_length);
		xor_encrypt(rx_data);

		if(checksum_mismatch == 1 || same_sequence_number == 1)
			continue;

		fwrite(rx_data, sizeof(unsigned char), rx_packet.bytes_length, newfile);
	}

	fclose(newfile);
	printf("\n## SERVER ## Incoming file received successfully.\n");
	transfer_finished = 0;
	return;
}

void delete(unsigned char* file)
{
	remove(file);
	printf("## SERVER ## The file %s has been successfully removed.\n", file);
}

void list(void)
{
	DIR *dir;
	struct dirent *ent;
	unsigned char files_list[1024];
	bzero(&files_list, sizeof(files_list));

	if((dir = opendir("./")) != NULL)
	{
  	while((ent = readdir(dir)) != NULL)
		{
    	strcat(files_list, ent->d_name);	//print all the files and directories within directory
			strcat(files_list,"\n");
		}
		closedir(dir);

		send_request_packet_and_wait_for_ack(START_FILE_TRANSFER);
		send_data_packet_and_wait_for_ack(SENDING_TRANSFER_FINISHED, strlen(files_list), files_list, checksum(files_list));
		printf("\n## SERVER ## The file list was sent to the client.");

		return;
	}
}

void server_exit(void)
{
	printf("\n## SERVER ## The client has requested to exit the server.");
	printf("\n## SERVER ## Closing the socket.");
	close(sock);
	printf("\n## SERVER ## Socket closed successfully. The server will now exit gracefully.\n\n");
	exit(0);
}

void send_request_packet_and_wait_for_ack(unsigned char request)
{
	request_packet_t tx_packet;
	request_packet_t rx_buffer;
	global_server_sequence_id++;

	bzero(&tx_packet, sizeof(tx_packet));
	bzero(&rx_buffer, sizeof(rx_buffer));

	printf("Sending request packet; Server Seq ID: %d\n", global_server_sequence_id);

	tx_packet.sequence_id = global_server_sequence_id;
	tx_packet.service_id = request;
	bzero(&tx_packet.filename, sizeof(tx_packet.filename));

	while(1)
	{
		tx_bytes = sendto(sock, (void *) &tx_packet, sizeof(request_packet_t), 0, (struct sockaddr *)&remote, sizeof(remote));

		struct sockaddr_in from_addr;
		int addr_length = sizeof(struct sockaddr);

		rx_bytes = recvfrom(sock, (void *) &rx_buffer, sizeof(request_packet_t), 0, (struct sockaddr *)&from_addr, &addr_length);

		if((rx_buffer.sequence_id == tx_packet.sequence_id) && (rx_buffer.service_id == ACK_SUCCESS))
				break;
		}

		return;
}

void send_data_packet_and_wait_for_ack(unsigned char response, int bytes, unsigned char* payload, int checksum)
{
	data_packet_t tx_packet;
	request_packet_t rx_buffer;

	global_server_sequence_id++;

	bzero(&tx_packet, sizeof(tx_packet));
	bzero(&rx_buffer, sizeof(rx_buffer));

	printf("Sending data packet; Server Seq ID: %d\n", global_server_sequence_id);

	tx_packet.sequence_id = global_server_sequence_id;
	tx_packet.service_id = response;

	memcpy(tx_packet.enc_payload, payload, bytes);
	tx_packet.checksum = checksum;
	tx_packet.bytes_length = bytes;

	while(1)
	{
		tx_bytes = sendto(sock, (void *) &tx_packet, sizeof(data_packet_t), 0, (struct sockaddr *)&remote, sizeof(remote));

		struct sockaddr_in from_addr;
		int addr_length = sizeof(struct sockaddr);

		rx_bytes = recvfrom(sock, (void *) &rx_buffer, sizeof(request_packet_t), 0, (struct sockaddr *)&from_addr, &addr_length);

		if((rx_buffer.sequence_id == tx_packet.sequence_id) && (rx_buffer.service_id == ACK_SUCCESS))
				break;
		}

}

unsigned char receive_request_packet_and_send_ack(void)
{
	request_packet_t rx_packet;
	request_packet_t tx_buffer;
	struct sockaddr_in from_addr;
	int addr_length = sizeof(struct sockaddr);

	bzero(&rx_packet, sizeof(rx_packet));
	bzero(&tx_buffer, sizeof(tx_buffer));

	rx_bytes = recvfrom(sock, (void *) &rx_packet, sizeof(request_packet_t), 0, (struct sockaddr *)&from_addr, &addr_length);

	tx_buffer.sequence_id = rx_packet.sequence_id;

	//printf("Client Sequence_ID before: %d\n",global_client_sequence_id);

	if(rx_packet.service_id == INCOMING_FILE_TRANSFER)
		global_client_sequence_id = rx_packet.sequence_id;

	printf("Client data transfer request Seq ID: %d\n",global_client_sequence_id);

	tx_buffer.service_id = ACK_SUCCESS;
	tx_bytes = sendto(sock, (void *) &tx_buffer, sizeof(request_packet_t), 0, (struct sockaddr *)&remote, sizeof(remote));

	return(rx_packet.service_id);
}

data_packet_t receive_data_packet_and_send_ack(void)
{
	data_packet_t rx_packet;
	request_packet_t tx_buffer;
	struct sockaddr_in from_addr;
	int addr_length = sizeof(struct sockaddr);

	bzero(&rx_packet, sizeof(rx_packet));
	bzero(&tx_buffer, sizeof(tx_buffer));

	rx_bytes = recvfrom(sock, (void *) &rx_packet, sizeof(data_packet_t), 0, (struct sockaddr *)&from_addr, &addr_length);

	if(rx_packet.sequence_id == global_client_sequence_id)
	{
		printf("Packet with same sequence number received.\n");
		tx_buffer.sequence_id = rx_packet.sequence_id;
		tx_buffer.service_id = ACK_SUCCESS;

		tx_bytes = sendto(sock, (void *) &tx_buffer, sizeof(request_packet_t), 0, (struct sockaddr *)&remote, sizeof(remote));
		same_sequence_number = 1;
		return rx_packet;
	}

	if(rx_packet.sequence_id == global_client_sequence_id + 1)
	{
		tx_buffer.sequence_id = rx_packet.sequence_id;

		if(rx_packet.service_id == INCOMING_TRANSFER_FINISHED)
		{
			transfer_finished = 1;
			printf("Receiving Last Packet. Finishing file transfer.\n");
		}

		//printf("Original checksum = %d\n", rx_packet.checksum);
		if(rx_packet.checksum == checksum(rx_packet.enc_payload))
		{
			tx_buffer.service_id = ACK_SUCCESS;
			tx_bytes = sendto(sock, (void *) &tx_buffer, sizeof(request_packet_t), 0, (struct sockaddr *)&remote, sizeof(remote));

			//printf("Checksum Match. Returning payload\n");
			global_client_sequence_id ++;

			printf("Sequence ID: %d\n", global_client_sequence_id);
			return rx_packet;
		}

		else
		{
			tx_buffer.service_id = NO_ACK;
			printf("%s\n", rx_packet.enc_payload);
			printf("## ERROR ## Checksum didn't match. Sending NACK.\n");

			tx_bytes = sendto(sock, (void *) &tx_buffer, sizeof(request_packet_t), 0, (struct sockaddr *)&remote, sizeof(remote));
			checksum_mismatch = 1;
			return rx_packet;
		}
	}
}

int checksum(unsigned char* input)
{
	int i = 0;
	int checksum = 0x0000;

	for(i = 0; i < (sizeof(input) / 4); i++)
	{
		checksum = checksum + ~((unsigned int) *(input + (4*i)));
	}
	checksum = ~checksum;
	//printf("Calc checksum = %d\n", checksum);
	return checksum;
}

void xor_encrypt(unsigned char* input)
{
	int i = 0;
	unsigned char key = CIPHER;

	for(i = 0; i < DATA_SIZE; i++)
		input[i] = input[i] ^ key;
}
