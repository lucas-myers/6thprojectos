Name: Lucas Myers
Date: April 27, 2026
Class: CS 4760 Operating Systems
Project: Assignment 6 – Memory Management

Environment:
- Visual Studio Code, Linux

How to Compile:
make

How to Run:
./oss -n 5 -s 2 -t 3 -i 0.5 -f oss.log

1. Reused core structure from Project 5 .

2. Removed resource management and deadlock detection logic.

3. Implemented shared memory for simulated system clock.

4. Implemented message queue communication between oss and worker processes.

5. Modified worker processes to generate memory requests instead of resource requests.

6. Each worker now randomly, Generates a memory address (page + offset), Chooses read or write which is biased toward a read, and Sends request to oss

7. oss receives and logs memory requests from processes.

8. Implemented PCB structure with page table placeholders, but did not add the logic yet gonna do that another day.

9. Implemented frame table structure, isn't used for allocation yet gonna do that later as well.

10. The Logging system outputs both to screen and file.
