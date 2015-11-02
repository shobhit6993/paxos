#include "server.h"
#include "replica.h"
#include "constants.h"
#include "utilities.h"
#include "iostream"
#include "vector"
#include "string"
#include "fstream"
#include "sstream"
#include "unistd.h"
#include "signal.h"
#include "errno.h"
#include "sys/socket.h"
#include "limits.h"
using namespace std;

typedef pair<int, Proposal> SPtuple;

#define DEBUG

#ifdef DEBUG
#  define D(x) x
#else
#  define D(x)
#endif // DEBUG
#

Replica::Replica(Server* _S) {
    S = _S;
    set_slot_num(0);

    int num_servers = S->get_num_servers();
    int num_clients = S->get_num_clients();

    commander_fd_.resize(num_servers, -1);
    scout_fd_.resize(num_servers, -1);
    leader_fd_.resize(num_servers, -1);
    client_chat_fd_.resize(num_clients, -1);
}

int Replica::get_commander_fd(const int server_id) {
    return commander_fd_[server_id];
}

int Replica::get_scout_fd(const int server_id) {
    return scout_fd_[server_id];
}

int Replica::get_leader_fd(const int server_id) {
    return leader_fd_[server_id];
}

int Replica::get_client_chat_fd(const int client_id) {
    return client_chat_fd_[client_id];
}

int Replica::get_slot_num() {
    return slot_num_;
}

void Replica::set_commander_fd(const int server_id, const int fd) {
    commander_fd_[server_id] = fd;
}

void Replica::set_scout_fd(const int server_id, const int fd) {
    scout_fd_[server_id] = fd;
}

void Replica::set_leader_fd(const int server_id, const int fd) {
    leader_fd_[server_id] = fd;
}

void Replica::set_client_chat_fd(const int client_id, const int fd) {
    client_chat_fd_[client_id] = fd;
}

void Replica::set_slot_num(const int slot_num) {
    slot_num_ = slot_num;
}

/**
 * increments the value of slot_num_
 */
void Replica::IncrementSlotNum() {
    set_slot_num(get_slot_num() + 1);
}

/**
 * creates threads for receiving messages from clients
 */
void Replica::CreateReceiveThreadsForClients() {
    int num_clients = S->get_num_clients();
    std::vector<pthread_t> receive_from_client_thread(num_clients);

    ReceiveThreadArgument **rcv_thread_arg = new ReceiveThreadArgument*[num_clients];
    for (int i = 0; i < num_clients; i++) {
        rcv_thread_arg[i] = new ReceiveThreadArgument;
        rcv_thread_arg[i]->R = this;
        rcv_thread_arg[i]->client_id = i;
        CreateThread(ReceiveMessagesFromClient,
                     (void *)rcv_thread_arg[i],
                     receive_from_client_thread[i]);
    }
}

/**
 * proposes the proposal to a leader
 * @param p Proposal to be proposed
 */
void Replica::Propose(const Proposal &p) {
    for (auto it = S->decisions_.begin(); it != S->decisions_.end(); ++it ) {
        if (it->second == p)
            return;
    }

    int min_slot;
    if (S->proposals_.rbegin() == S->proposals_.rend())
        min_slot = 0;
    else
        min_slot = S->proposals_.rbegin()->first;

    while (S->decisions_.find(min_slot) != S->decisions_.end())
        min_slot++;

    S->proposals_[min_slot] = p;
    SendProposal(min_slot, p);
}

/**
 * sends proposal to a leader
 * @param s proposed slot num for this proposal
 * @param p proposal to be proposed
 */
void Replica::SendProposal(const int& s, const Proposal& p)
{
    string msg = kPropose + kInternalDelim;
    msg += to_string(s) + kInternalDelim;
    msg += proposalToString(p) + kMessageDelim;
    S->Unicast(kPropose, msg);
}

/**
 * performs the decision reached by Paxos by adding it to decisions,
 * incrementing slot num, followed by sending decision to client
 * @param slot decided slot num
 * @param p    decided proposal
 */
void Replica::Perform(const int& slot, const Proposal& p)
{
    for (auto it = S->decisions_.begin(); it != S->decisions_.end(); it++)
    {
        if (it->second == p && it->first < get_slot_num())
        {   //think why not increase in loop
            IncrementSlotNum();
            return;
        }
    }

    IncrementSlotNum();
    SendResponseToClient(slot, p);
}

/**
 * sends the decided proposal to client
 * @param s decided slot num
 * @param p proposal to be sent
 */
void Replica::SendResponseToClient(const int& s, const Proposal& p)
{
    string msg = kResponse + kInternalDelim;
    msg += to_string(s) + kInternalDelim;
    msg += proposalToString(p) + kMessageDelim;

    if (send(get_client_chat_fd(stoi(p.client_id)), msg.c_str(), msg.size(), 0) == -1) {
        D(cout << "SR" << S->get_pid() << ": ERROR: sending response to client C"
          << stoi(p.client_id) << endl;)
    }
    else {
        D(cout << "SR" << S->get_pid() << ": Message sent to client C"
          << stoi(p.client_id) << ": " << msg << endl;)
    }
}

/**
 * function for performing replica related job
 */
void Replica::ReplicaMode()
{
    char buf[kMaxDataSize];
    int num_bytes;

    fd_set fromset, temp_set;
    vector<int> fds;

    while (true) {  // always listen to messages from the acceptors
        int fd_max = INT_MIN, fd_temp;
        FD_ZERO(&fromset);
        for (int i = 0; i < S->get_num_clients(); i++)
        {
            fd_temp = get_client_chat_fd(i);
            fd_max = max(fd_max, fd_temp);
            fds.push_back(fd_temp);
            FD_SET(fd_temp, &fromset);
        }
        //TODO: Use local primary_id from calling function or get_primary_id()?
        fd_temp = get_leader_fd(S->get_primary_id());
        fd_max = max(fd_max, fd_temp);
        fds.push_back(fd_temp);
        FD_SET(fd_temp, &fromset);

        int rv = select(fd_max + 1, &fromset, NULL, NULL, NULL);
        if (rv == -1) { //error in select
            D(cout << "SR" << S->get_pid() << ": error in select()" << endl;)
        } else if (rv == 0) {
            D(cout << "SR" << S->get_pid() << ": ERROR Unexpected select timeout" << endl;)
        } else {
            for (int i = 0; i < fds.size(); i++) {
                if (FD_ISSET(fds[i], &fromset)) { // we got one!!
                    char buf[kMaxDataSize];
                    if ((num_bytes = recv(fds[i], buf, kMaxDataSize - 1, 0)) == -1) {
                        D(cout << "SR" << S->get_pid()
                          << ": ERROR in receiving from leader or clients" << endl;)
                    } else if (num_bytes == 0) {     //connection closed
                        D(cout << "SR" << S->get_pid() << ": Connection closed by leader or client" << endl;)
                    } else {
                        buf[num_bytes] = '\0';
                        std::vector<string> message = split(string(buf), kMessageDelim[0]);
                        for (const auto &msg : message) {
                            std::vector<string> token = split(string(msg), kInternalDelim[0]);
                            if (token[0] == kChat)
                            {
                                D(cout << "SR" << S->get_pid() << ": Received chat from client: " << buf <<  endl;)
                                Proposal p = stringToProposal(token[1]);
                                Propose(p);
                            }
                            else if (token[0] == kDecision)
                            {
                                D(cout << "SR" << S->get_pid() << ": Received decision: " << buf <<  endl;)
                                int s = stoi(token[1]);
                                Proposal p = stringToProposal(token[2]);
                                S->decisions_[s] = p;

                                Proposal currdecision;
                                int slot_num = get_slot_num();
                                while (S->decisions_.find(slot_num) != S->decisions_.end())
                                {
                                    currdecision = S->decisions_[slot_num];
                                    if (S->proposals_.find(slot_num) != S->proposals_.end())
                                    {
                                        if (!(S->proposals_[slot_num] == currdecision))
                                            Propose(S->proposals_[slot_num]);
                                    }
                                    Perform(slot_num, currdecision);
                                    //s has to slot_num. check if it is slotnum in recovery too.
                                    //if so can remove argument from perform, sendresponse functions
                                }
                            }
                            else {    //other messages
                                D(cout << "SR" << S->get_pid() << ": ERROR Unexpected message received: " << msg << endl;)
                            }
                        }
                    }
                }
            }
        }
    }
}

/**
 * function for the thread receiving messages from a client with id=client_id
 * @param _rcv_thread_arg argument containing server object and client_id
 */
void* ReceiveMessagesFromClient(void* _rcv_thread_arg) {
    ReceiveThreadArgument *rcv_thread_arg = (ReceiveThreadArgument *)_rcv_thread_arg;
    Replica *R = rcv_thread_arg->R;
    int client_id = rcv_thread_arg->client_id;

    char buf[kMaxDataSize];
    int num_bytes;

    while (true) {  // always listen to messages from the client
        num_bytes = recv(R->get_client_chat_fd(client_id), buf, kMaxDataSize - 1, 0);
        if (num_bytes == -1) {
            D(cout << "SR" << R->S->get_pid() << ": ERROR in receiving message from C"
              << client_id << endl;)
            return NULL;
        } else if (num_bytes == 0) {    // connection closed by client
            D(cout << "SR" << R->S->get_pid() << ": ERROR: Connection closed by Client." << endl;)
            return NULL;
        } else {
            buf[num_bytes] = '\0';
            D(cout << "SR" << R->S->get_pid() << ": Message received from C"
              << client_id << " - " << buf << endl;)

            // extract multiple messages from the received buf
            std::vector<string> message = split(string(buf), kMessageDelim[0]);
            for (const auto &msg : message) {
                std::vector<string> token = split(string(msg), kInternalDelim[0]);
                // token[0] is the CHAT tag
                // token[1] is client id
                // token[2] is chat_id
                // token[3] is the chat message
                if (token[0] == kChat) {
                    Proposal p(token[1], token[2], token[3]);
                    R->Propose(p);
                } else {
                    D(cout << "S" << R->S->get_pid()
                      << ": ERROR Unexpected message received from C" << client_id
                      << " - " << buf << endl;)
                }
            }
        }
    }
    return NULL;
}

/**
 * thread entry function for replica
 * @param  _S pointer to server class object
 * @return    NULL
 */
void* ReplicaEntry(void *_S) {
    Replica R((Server*)_S);

    pthread_t accept_connections_thread;
    CreateThread(AcceptConnectionsReplica, (void*)&R, accept_connections_thread);

    // sleep for some time to make sure accept threads of commanders and scouts are running
    usleep(kGeneralSleep);
    int primary_id = R.S->get_primary_id();
    if (R.ConnectToCommander(primary_id)) {
        D(cout << "SR" << R.S->get_pid() << ": Connected to commander of S"
          << primary_id << endl;)
    } else {
        D(cout << "SR" << R.S->get_pid() << ": ERROR in connecting to commander of S"
          << primary_id << endl;)
        return NULL;
    }

    if (R.ConnectToScout(primary_id)) {
        D(cout << "SR" << R.S->get_pid() << ": Connected to scout of S"
          << primary_id << endl;)
    } else {
        D(cout << "SR" << R.S->get_pid() << ": ERROR in connecting to scout of S"
          << primary_id << endl;)
        return NULL;
    }

    // sleep for some time to make sure all connections are established
    usleep(kGeneralSleep);
    usleep(kGeneralSleep);
    usleep(kGeneralSleep);
    if (R.S->get_pid() == primary_id) {
        R.CreateReceiveThreadsForClients();
    }

    R.ReplicaMode();

    void *status;
    pthread_join(accept_connections_thread, &status);
    return NULL;

}