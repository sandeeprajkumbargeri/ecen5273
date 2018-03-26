#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <stdlib.h>
#include <memory.h>
#include <errno.h>
#include <sys/time.h>

#define GET_FILE 	0x15
#define PUT_FILE 	0x25
#define	DELETE    0x35
#define LIST      0x45
#define EXIT_SERVER  0x55
#define ACK_SUCCESS	 0x65
#define NO_ACK			 0x75

#define START_FILE_TRANSFER	0x20
#define TRANSFERRING	0x29
#define	SENDING_TRANSFER_FINISHED	0x2E

#define INCOMING_FILE_TRANSFER	0x10
#define INCOMING_TRANSFER_FINISHED 0x1E

//#define MAXBUFSIZE 100
#define CIPHER 0xF2
#define DATA_SIZE 1024
#define TOTAL_COMMANDS 5

int sock;                               //this will be our socket
struct sockaddr_in remote;     //"Internet socket address structure"
int tx_bytes = 0, rx_bytes = 0;          // number of bytes send by sendto()
//unsigned char buffer[MAXBUFSIZE];
int global_client_sequence_id = 0;
int global_server_sequence_id = 1000;

unsigned char transfer_finished = 0;
unsigned char checksum_mismatch = 0;
unsigned char same_sequence_number = 0;

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

int parse_command(unsigned char *);
void execute_service(int, unsigned char *);
void put_file(unsigned char *);
void get_file(unsigned char *);
void delete(unsigned char *);
void list(void);
void server_exit(void);
void xor_encrypt(unsigned char *);
int checksum(unsigned char*);
void send_request_packet_and_wait_for_ack(unsigned char);
void send_request_packet_with_filename_and_wait_for_ack(unsigned char, unsigned char*);
data_packet_t receive_data_packet_and_send_ack(void);
void send_data_packet_and_wait_for_ack(unsigned char, int, unsigned char*, int);
unsigned char receive_request_packet_and_send_ack(void);

int main (int argc, unsigned char * argv[])
{
	unsigned char command[64];
	int service_id;
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 500000;

	if (argc != 3)
	{
		printf("\nUSAGE: <server_ip> <server_port>\n");
		exit(1);
	}

	//Here we populate a sockaddr_in struct with information regarding where we'd like to send our packet i.e the Server.
	bzero(&remote,sizeof(remote));               //zero the struct
	remote.sin_family = AF_INET;                 //address family
	remote.sin_port = htons(atoi(argv[2]));      //sets port to network byte order
	remote.sin_addr.s_addr = inet_addr(argv[1]); //sets remote IP address

	//Causes the system to create a generic socket of type UDP (datagram)
	if ((sock = socket(remote.sin_family, SOCK_DGRAM, 0)) < 0)
	{
		printf("\n## ERROR ## Unable to create socket.\n");
		exit(1);
	}

	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const unsigned char*)&tv,sizeof(struct timeval));

	//sendto() sends immediately. Tt will report an error if the message fails to leave the computer. However, with UDP, there is no error if the message is lost in the network once it leaves the computer.

	printf("\n## AVAILABLE COMMANDS ##\nget [file_name]     put [file_name]     delete [file_name]     ls     exit\n");

	while(1)
	{
		bzero(command, sizeof(command));
		printf("\n## ENTER COMMAND ##\n$ ");
		fgets(command, sizeof(command), stdin);

		unsigned char *pos;
		if ((pos = strchr(command, '\n')) != NULL)
    	*pos = '\0';

		if((service_id = parse_command(command)) < 0)
		{
			printf("\n## ERROR ## Invalid command entered. Try again!\n");
			continue;
		}
	  else
			execute_service(service_id, command);
	}

	//tx_bytes = sendto(sock, command, strlen(command), 0, (struct sockaddr *)&remote, sizeof(remote));

	// Blocks till bytes are received
	//struct sockaddr_in from_addr;
	//int addr_length = sizeof(struct sockaddr);
	//bzero(buffer,sizeof(buffer));
	//rx_bytes = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&from_addr, &addr_length);;

	//printf("Server says %s\n", buffer);

	close(sock);
}

int parse_command(unsigned char* input)
{
		int i = 0;
		unsigned char* commands[TOTAL_COMMANDS];
		unsigned char* match;

		commands[0] = "get";
		commands[1] = "put";
		commands[2] = "delete";
		commands[3] = "ls";
		commands[4] = "exit";

		for(i = 0; i < TOTAL_COMMANDS; i++)
		{
			match = strstr(input, commands[i]);

			if(match == input)
				return (i+1);
		}

		return (-1);
}

void execute_service(int service, unsigned char* command)
{
	unsigned char filename[100];

	switch(service)
	{
		case 1:
		strcpy(filename, command + 4);
		get_file(filename);
		break;

		case 2:
		strcpy(filename, command + 4);
		put_file(filename);
		break;

		case 3:
		strcpy(filename, command + 7);
		delete(filename);
		break;

		case 4:
		list();
		break;

		case 5:
		server_exit();
		break;
	}
}

void get_file(unsigned char* file)
{
	FILE *newfile;
	unsigned char service_id;
	data_packet_t rx_packet;
	unsigned char* match;
	unsigned char rx_data[DATA_SIZE];

	send_request_packet_and_wait_for_ack(LIST);

	service_id = receive_request_packet_and_send_ack();

	if(service_id == INCOMING_FILE_TRANSFER)
		printf("## CLIENT ## Making sure the server has the file.\n");

	while(transfer_finished == 0)
	{
		bzero(&rx_packet, sizeof(rx_packet));

		checksum_mismatch = 0;
		same_sequence_number = 0;

		rx_packet = receive_data_packet_and_send_ack();

		if(checksum_mismatch == 1 || same_sequence_number == 1)
			continue;
	}

	transfer_finished = 0;
	match = strstr(rx_packet.enc_payload, file);

	if(match == NULL)
	{
		printf("\n## CLIENT ## Sorry, the file you requested doesn't exist on the server.\n");
		return;
	}

	printf("## CLIENT ## File located on the server! Initiating download request.");
	send_request_packet_with_filename_and_wait_for_ack(GET_FILE, file);

	newfile = fopen(file, "wb");

	service_id = receive_request_packet_and_send_ack();

	if(service_id == INCOMING_FILE_TRANSFER)
		printf("## CLIENT ## Starting file transfer.\n");

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
	printf("\n## CLIENT ## Incoming file %s received successfully.\n", file);
	transfer_finished = 0;
	return;

}

void put_file(unsigned char* file)
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

	send_request_packet_with_filename_and_wait_for_ack(PUT_FILE, file);
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

	printf("\n## CLIENT ## File %s has been successfully uploaded.\n", file);
}

void delete(unsigned char* file)
{
	FILE *newfile;
	unsigned char service_id;
	data_packet_t rx_packet;
	unsigned char* match;
	unsigned char enc_data[DATA_SIZE];
	unsigned char dec_data[DATA_SIZE];

	send_request_packet_and_wait_for_ack(LIST);

	service_id = receive_request_packet_and_send_ack();

	if(service_id == INCOMING_FILE_TRANSFER)
		printf("## CLIENT ## Making sure the server has the file.\n");

	while(transfer_finished == 0)
	{
		bzero(&rx_packet, sizeof(rx_packet));

		checksum_mismatch = 0;
		same_sequence_number = 0;

		rx_packet = receive_data_packet_and_send_ack();

		if(checksum_mismatch == 1 || same_sequence_number == 1)
			continue;
	}

	transfer_finished = 0;
	match = strstr(rx_packet.enc_payload, file);

	if(match == NULL)
	{
		printf("\n## CLIENT ## Sorry, the file you requested doesn't exist.\n");
		return;
	}

	printf("## CLIENT ## File located on the server. Initiating download request.");
	send_request_packet_with_filename_and_wait_for_ack(DELETE, file);
}

void list(void)
{
	unsigned char service_id;
	data_packet_t rx_packet;

	send_request_packet_and_wait_for_ack(LIST);

	service_id = receive_request_packet_and_send_ack();

	if(service_id == INCOMING_FILE_TRANSFER)
		printf("## CLIENT ## Requesting file list.\n");

	while(transfer_finished == 0)
	{
		bzero(&rx_packet, sizeof(rx_packet));

		checksum_mismatch = 0;
		same_sequence_number = 0;

		rx_packet = receive_data_packet_and_send_ack();

		if(checksum_mismatch == 1 || same_sequence_number == 1)
			continue;
	}

	transfer_finished = 0;

	printf("\n## FILE LIST ##\n");
	printf("%s\n", rx_packet.enc_payload);
}

void server_exit(void)
{
	send_request_packet_and_wait_for_ack(EXIT_SERVER);
	printf("\n## CLIENT ## The server has ended gracefully.\n");
}

void xor_encrypt(unsigned char* input)
{
	unsigned char key = CIPHER;

	for(int i = 0; i < DATA_SIZE; i++)
		input[i] = input[i] ^ key;
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
	return checksum;
}

void send_request_packet_and_wait_for_ack(unsigned char request)
{
	request_packet_t tx_packet;
	request_packet_t rx_buffer;
	global_client_sequence_id++;

	bzero(&tx_packet, sizeof(tx_packet));
	bzero(&rx_buffer, sizeof(rx_buffer));

	printf("Sending data transfer request packet; Client Seq ID: %d\n", global_client_sequence_id);

	tx_packet.sequence_id = global_client_sequence_id;
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

void send_request_packet_with_filename_and_wait_for_ack(unsigned char request, unsigned char* filename)
{
	request_packet_t tx_packet;
	request_packet_t rx_buffer;
	global_client_sequence_id++;

	bzero(&tx_packet, sizeof(tx_packet));
	bzero(&rx_buffer, sizeof(rx_buffer));

	printf("Sending request packet and filename; Seq ID: %d\n", global_client_sequence_id);

	tx_packet.sequence_id = global_client_sequence_id;
	tx_packet.service_id = request;
	memcpy(tx_packet.filename, filename, strlen(filename));

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

void send_data_packet_and_wait_for_ack(unsigned char service_id, int bytes, unsigned char* enc_data, int checksum)
{
	data_packet_t tx_packet;
	request_packet_t rx_buffer;

	global_client_sequence_id++;

	bzero(&tx_packet, sizeof(tx_packet));
	bzero(&rx_buffer, sizeof(rx_buffer));

	printf("Sending data packet; Client Seq ID: %d\n", global_client_sequence_id);

	tx_packet.sequence_id = global_client_sequence_id;
	tx_packet.service_id = service_id;
	memcpy(tx_packet.enc_payload, enc_data, bytes);
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

data_packet_t receive_data_packet_and_send_ack(void)
{
	data_packet_t rx_packet;
	request_packet_t tx_buffer;
	struct sockaddr_in from_addr;
	int addr_length = sizeof(struct sockaddr);

	bzero(&rx_packet, sizeof(rx_packet));
	bzero(&tx_buffer, sizeof(tx_buffer));

	rx_bytes = recvfrom(sock, (void *) &rx_packet, sizeof(data_packet_t), 0, (struct sockaddr *)&from_addr, &addr_length);

	if(rx_packet.sequence_id == global_server_sequence_id)
	{
		printf("Packet with same sequence number received.\n");
		tx_buffer.sequence_id = rx_packet.sequence_id;
		tx_buffer.service_id = ACK_SUCCESS;

		tx_bytes = sendto(sock, (void *) &tx_buffer, sizeof(request_packet_t), 0, (struct sockaddr *)&remote, sizeof(remote));
		same_sequence_number = 1;
		return rx_packet;
	}

	if(rx_packet.sequence_id == global_server_sequence_id + 1)
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
			global_server_sequence_id ++;

			printf("Sequence ID: %d\n", global_server_sequence_id);
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

	//printf("Server Sequence_ID before: %d\n",global_server_sequence_id);

	if(rx_packet.service_id == INCOMING_FILE_TRANSFER)
		global_server_sequence_id = rx_packet.sequence_id;

	printf("Server data transfer request Seq ID: %d\n",global_server_sequence_id);

	tx_buffer.service_id = ACK_SUCCESS;
	tx_bytes = sendto(sock, (void *) &tx_buffer, sizeof(request_packet_t), 0, (struct sockaddr *)&remote, sizeof(remote));

	return(rx_packet.service_id);
}
