Author: Sandeep Raj Kumbargeri

Description: 
It is a distributed 'N' client server system. The total servers in the system can be set by changing the "#define TOTAL SERVERS" value in both client and the server. The system supports the following functionality:

1. N servers - Highly scalable.
2. Multiple requests from several clients at the same time using pThreads.
3. Username and password protection on both client and server.
4. Seperate client files for individual clients.
5. One single server config file to specify client username and client config file location.
6. PUT, GET, LIST, MKDIR, LOGOUT operations.
7. File parts preview before sending and receiving.
8. Highly error handling and self verifying.
9. Better user interface.
10. Provides privacy by encrypting using user passwords before sending and recceiving.
11. Faster transfer times.
12. One second timeout at client side.
13. Traffic optimization: Client optimizes the requests sent to the servers for parts.
14. Dynamic parts routing for N servers.
15. Less RAM consumption.
16. Implements dynamic database file to store file information.
17. Implements sub-folders on-the-go in PUT.

Usage instructions:

To build the files, type "make" in the terminal.
To execute client: ./client
To execute server: ./server <port number> <folder name>

Client config name: dfc.conf
Server config name: dfs.conf

