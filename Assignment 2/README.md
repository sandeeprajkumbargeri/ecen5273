Author: Sandeep Raj Kumbargeri
Date: 10/23/2017

This program implements a HTTP Server which reads from the config file and sets itself up. It supports the following features:

1. Supports multiple clients at the same time
2. Processes multiple requests from the multiple clients at the same time
3. Supports pipelining with Keep-Alive connections 
4. Supports POST requests as well
5. Handles all standard errors mentioned in the lab manual and notifies the client

To build the .c files, perform make in the terminal.

Server Usage: ./server

Configure the ws.conf file to customize the server!
