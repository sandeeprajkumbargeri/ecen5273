Author: Sandeep Raj Kumbargeri
Date: Dec 13 2017

Description: This is a simple HTTP Web Proxy server which demonstrates the following functionality:

1. Blocks blacklisted websites (present in the blacklist.conf) (sends a "400 Bad Request" HTTP error message).
2. Discards any other method apart from GET (sends a "400 Bad Request" HTTP error message).
3. Discards HTTPS requests.
4. Caches responses to individual requests to speed up loading and network traffic.
5. Caches come with expiration timer, a daemon thread garbage-collects the expired caches.
6. Multiple connections, requests at the same time using pThreads.
7. Maintains a database file to store previously resolved DNS requests to save on generating repeated DNS queries for the a URL.

Note: This program is not robust enough, given the point in time (nearing finals). But it demonstrates all the network systems concepts that were learned.
