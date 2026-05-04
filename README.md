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
   - a page number from 0 to 15
   - an offset from 0 to 1023
   - a full logical memory address using page * 1024 + offset
   - a read or write request, biased toward reads


7. oss receives and logs memory requests from processes.

8. Implemented PCB structure with page table placeholders, but did not add the logic yet gonna do that another day.

9. Implemented frame table structure, isn't used for allocation yet gonna do that later as well.

10. The Logging system outputs both to screen and file.

11. Added a page table to each PCB with 16 entries per process.

12. Added a frame table with 64 total frames.

13. Initialized all page table entries to -1 to show that pages are not in memory at the start.

14. Initialized all frame table entries with:
    - occupied flag
    - process number
    - page number
    - dirty bit

15. Implemented basic page  toframe allocation.

16. When a requested page is not currently in memory, oss finds a free frame and loads that page into the frame.

17. When a requested page is already in memory, oss uses the existing frame.

18.Implemented dirty bit logic. If the request is a write, the dirty bit for that frame is set to 1.

19. Implemented cleanup logic so when a worker terminates, any frames owned by that process are released.

20. Added memory layout output that prints the frame table and each active process page table.

21. Logging outputs to both the screen and the log file.

22. Added temporary frame replacement behavior when memory is full. For now, it replaces frame 0. FIFO replacement will be added later.

AI Usage:

Used: ChatGPT

