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

23. Added page fault detection. When oss receives a memory request, it checks whether the requested page is already loaded in that process page table.

24. If the requested page is not currently in memory, oss logs it as a page fault.

25. Added a blocked queue for processes waiting on page faults.

26. When a page fault happens, oss places the process into the blocked queue instead of immediately sending a message back to the worker.

27. Added a simulated 14ms disk delay for page fault handling.

28. When the 14ms delay is complete, oss loads the requested page into a frame.

29. After the page is loaded, oss unblocks the process and sends a message back to the worker so it can continue.

30. Added a blocked flag inside the PCB to track whether a process is currently waiting on a page fault.

31. Added output for blocked processes in the memory layout section.

32. Added statistics tracking for:
    - total memory requests
    - total reads
    - total writes
    - total page faults
    - page fault percentage

33. Added final statistics output before oss cleans up and exits.

34. Added logic to advance the simulated clock faster if all active processes are blocked. This prevents oss from sitting idle while every worker is waiting on a page fault.

35. Added cleanup for blocked queue entries when a process terminates.

36. 36. Completed FIFO page replacement when memory is full.

37. The temporary frame replacement behavior was replaced with FIFO page replacement.

38. A FIFO queue was added to keep track of the order that frames are loaded into memory.

39. When memory is full and a page fault occurs, oss now selects the oldest loaded frame from the FIFO queue for replacement.

40. Before a frame is replaced, oss clears the old process page table entry so that the process no longer points to that frame.

41. When a new page is loaded into memory, the frame is added back into the FIFO queue.

42. Frame generation tracking was added so old FIFO queue entries can be ignored if a frame was already freed or reused.

43. Page replacement logging was completed so oss records when page faults occur, when frames are cleared, which process page is swapped out, and which new page is swapped in.

44. Dirty bit replacement behavior was completed so dirty frames add extra simulated time when they are selected for replacement.

45. The blocked queue handling was completed so processes wait during page faults and continue after the simulated disk delay finishes.

46. Memory cleanup was completed so when a process terminates, all frames owned by that process are released and the page table is reset.

47. Final statistics output was completed to show total memory references, total reads, total writes, total page faults, page fault percentage, and effective memory access time.

48. Signal and resource cleanup were completed so shared memory, message queues, and child processes are cleaned up when oss exits.

49. The project was tested using make clean, make, and ./oss -n 5 -s 2 -t 3 -i 0.5 -f oss.log.

50. The log file was checked to confirm that memory requests, page faults, blocked processes, frame table output, page table output, FIFO replacement, dirty bit behavior, and final statistics were being recorded.

51. The worker output was kept separate from the log file so only oss writes to the screen and the log file.

52. The project now meets the main Assignment 6 memory management requirements, including page tables, frame table, dirty bits, page faults, blocked I/O delay, FIFO page replacement, memory layout output, final statistics, and cleanup.

AI Usage:

Used: ChatGPT

