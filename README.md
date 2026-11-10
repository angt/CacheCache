# CacheCache

CacheCache is a terminal chat for processes on the same machine.
It uses the shared CPU cache instead of sockets or shared files:
the sender creates cache pressure, and the receiver measures
memory-access times to recover bits. A small protocol handles
framing, error detection, acks, and retries.

Vibcoded with OSLLMs.
