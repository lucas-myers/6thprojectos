#include <iostream>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>

using namespace std;

const int PAGE_SIZE = 1024;
const int NUM_PAGES = 16;

struct Message {
    long mtype;
    int index;
    int address;
    int isWrite;
    int terminate;
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cerr << "worker: missing process index\n";
        return 1;
    }

    int localIndex = atoi(argv[1]);

    srand(static_cast<unsigned int>(getpid() ^ time(nullptr)));

    key_t msgKey = ftok(".", 75);
    if (msgKey == -1) {
        perror("worker ftok");
        return 1;
    }

    int msgId = msgget(msgKey, 0666);
    if (msgId == -1) {
        perror("worker msgget");
        return 1;
    }

    while (true) {
        Message msg;

        // Wait until oss sends this process a message.
        // oss sends using mtype = child pid.
        if (msgrcv(msgId, &msg, sizeof(Message) - sizeof(long), getpid(), 0) == -1) {
            perror("worker msgrcv");
            return 1;
        }

        Message reply;
        reply.mtype = 1;          // all worker replies go back to oss
        reply.index = localIndex;
        reply.terminate = 0;
        reply.address = 0;
        reply.isWrite = 0;

        int action = rand() % 100;

        // Small chance to terminate.
        if (action < 10) {
            reply.terminate = 1;
        } else {
            int page = rand() % NUM_PAGES;
            int offset = rand() % PAGE_SIZE;

            reply.address = page * PAGE_SIZE + offset;

            // Bias toward reads.
            // 0 = read, 1 = write
            reply.isWrite = (rand() % 100 < 30) ? 1 : 0;
        }

        if (msgsnd(msgId, &reply, sizeof(Message) - sizeof(long), 0) == -1) {
            perror("worker msgsnd");
            return 1;
        }

        if (reply.terminate) {
            break;
        }
    }

    return 0;
}