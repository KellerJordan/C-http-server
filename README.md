# C-http-server
An implementation of a simple HTTP server in C.

To build and run the server, do
```
make
make run
```
This will run the server with default settings: on port 8000, printing errors to console and piping output to `out.log`.

The directory `docs` is the default root served by the server. It contains several example files and directories.

The implementation approximately follows Swarthmore's [CS43 Lab 2](https://www.cs.swarthmore.edu/~kwebb/cs43/f17/labs/lab2.html).

