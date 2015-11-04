#include "master.h"
#include "constants.h"
#include "utilities.h"
#include "iostream"
#include "vector"
#include "string"
#include "fstream"
#include "sstream"
#include "cstring"
#include "unistd.h"
#include "spawn.h"
#include "sys/types.h"
#include "signal.h"
#include "errno.h"
#include "sys/socket.h"
using namespace std;

// #define DEBUG

#ifdef DEBUG
#  define D(x) x
#else
#  define D(x)
#endif // DEBUG

extern char **environ;

int Master::get_server_fd(const int server_id) {
    return server_fd_[server_id];
}

int Master::get_client_fd(const int client_id) {
    return client_fd_[client_id];
}

int Master::get_master_port() {
    return master_port_;
}

int Master::get_server_listen_port(const int server_id) {
    return server_listen_port_[server_id];
}

int Master::get_client_listen_port(const int client_id) {
    return client_listen_port_[client_id];
}

int Master::get_server_pid(const int server_id) {
    return server_pid_[server_id];
}

int Master::get_client_pid(const int client_id) {
    return client_pid_[client_id];
}

int Master::get_primary_id() {
    return primary_id_;
}

set<int> Acceptor::get_server_fd_set() {
    return server_fd_;
}

int Master::get_num_servers() {
    return num_servers_;
}

void Master::set_server_pid(const int server_id, const int pid) {
    server_pid_[server_id] = pid;
}

void Master::set_client_pid(const int client_id, const int pid) {
    client_pid_[client_id] = pid;
}

void Master::set_server_fd(const int server_id, const int fd) {
    server_fd_[server_id] = fd;
}

void Master::set_client_fd(const int client_id, const int fd) {
    client_fd_[client_id] = fd;
}

void Master::set_primary_id(const int primary_id) {
    primary_id_ = primary_id;
}

/**
 * extracts chat message from the sendMessage command
 * @param command the sendMessage command given by user
 * @param message [out] extracted chat message
 */
void Master::ExtractChatMessage(const string &command, string &message) {
    int n = command.size();
    int i = 0;
    int spaces_count = 0;
    while (i < n && spaces_count != 2) {
        if (command[i] == ' ')
            spaces_count++;
        i++;
    }
    while (i < n) {
        message.push_back(command[i]);
        i++;
    }
}

/**
 * constructs the following templated message from the chat message body
 * CHAT-<chat_message>
 * @param chat_message body of chat message
 * @param message      [out] templated message which can be sent
 */
void Master::ConstructChatMessage(const string &chat_message, string &message) {
    message = kChat + kInternalDelim + chat_message + kMessageDelim;
}

/**
 * reads ports-file and populates port related vectors/maps
 * @return true is ports-file was read successfully
 */
bool Master::ReadPortsFile() {
    ifstream fin;
    fin.exceptions ( ifstream::failbit | ifstream::badbit );
    try {
        fin.open(kPortsFile.c_str());
        fin >> master_port_;

        int port;
        for (int i = 0; i < num_clients_; i++) {
            fin >> port;
            client_listen_port_[i] = port;
            fin >> port;
        }

        for (int i = 0; i < num_servers_; i++) {
            fin >> port;
            server_listen_port_[i] = port;
            for (int j = 0; j < 7; ++j) {
                fin >> port;
            }
        }
        fin.close();
        return true;

    } catch (ifstream::failure e) {
        D(cout << e.what() << endl;)
        if (fin.is_open()) fin.close();
        return false;
    }
}

/**
 * reads test commands from stdin
 */
void Master::ReadTest() {
    string line;
    while (getline(std::cin, line)) {
        std::istringstream iss(line);
        string keyword;
        iss >> keyword;
        if (keyword == kStart) {
            iss >> num_servers_ >> num_clients_;
            Initialize();
            if (!ReadPortsFile())
                return ;
            if (!SpawnServers(num_servers_))
                return;
            if (!SpawnClients(num_clients_))
                return;
            usleep(kGeneralSleep);
        }
        if (keyword == kSendMessage) {
            int client_id;
            iss >> client_id;
            string chat_message;
            ExtractChatMessage(line, chat_message);
            string message;
            ConstructChatMessage(chat_message, message);
            SendMessageToClient(client_id, message);
        }
        if (keyword == kCrashServer) {

        }
        if (keyword == kRestartServer) {

        }
        if (keyword == kAllClear) {
            SendAllClearToServers(); //sends to primary server
            WaitForAllClearDone();
        }
        if (keyword == kTimeBombLeader) {

        }
        if (keyword == kPrintChatLog) {
            usleep(5000 * 1000);
            int client_id;
            iss >> client_id;
            string message = kChatLog + kInternalDelim;
            SendMessageToClient(client_id, message);
            string chat_log;
            ReceiveChatLogFromClient(client_id, chat_log);
            PrintChatLog(client_id, chat_log);
        }
    }
}

void Master::ConstructAllClearMessage(string &message) {
    message = kAllClear + kMessageDelim;
}

void Master::WaitForAllClearDone()
{

    fd_set server_set;
    int fd_max;
    vector<int> fd_vec;

    while (true) {  // always listen to messages from the acceptors
         GetServerFdSet(server_set, fd_vec, fd_max);
        // if (fd_max == INT_MIN) {
        //     D(cout << "M: Exiting because no servers sem to" << endl;)
        //     return NULL;
        // }
        int rv = select(fd_max + 1, &server_set, NULL, NULL, NULL);

        if (rv == -1) { //error in select
            D(cout << "SC" << C->S->get_pid() << ": ERROR in select()" << endl;)
        } else if (rv == 0) {
            D(cout << "SC" << C->S->get_pid() << ": ERROR: Unexpected timeout in select()" << endl;)
        } else {
            for (int i = 0; i < num_servers; i++) {
                if (FD_ISSET(fd_vec[i], &server_set)) { // we got one!!
                    char buf[kMaxDataSize];
                    if ((num_bytes = recv(fd_vec[i], buf, kMaxDataSize - 1, 0)) == -1) {
                        D(cout << "M" << C->S->get_pid() << ": ERROR in receiving p2b from acceptor S" << i << endl;)
                        close(C->get_acceptor_fd(i));
                        C->set_acceptor_fd(i, -1);
                    } else if (num_bytes == 0) {     //connection closed
                        D(cout << "SC" << C->S->get_pid() << ": Connection closed by acceptor S" << i << endl;)
                        close(C->get_acceptor_fd(i));
                        C->set_acceptor_fd(i, -1);
                    } else {
                        buf[num_bytes] = '\0';
                        std::vector<string> message = split(string(buf), kMessageDelim[0]);
                        for (const auto &msg : message) {
                            std::vector<string> token = split(string(msg), kInternalDelim[0]);
                            if (token[0] == kP2b) {
                                D(cout << "SC" << C->S->get_pid()
                                  << ": P2b message received from acceptor S" << i << ": " << msg <<  endl;)

                                // close connection with this acceptor
                                // because no future communication with it will happen
                                close(C->get_acceptor_fd(i));
                                C->set_acceptor_fd(i, -1);

                                Ballot recvd_ballot = stringToBallot(token[2]);
                                if (recvd_ballot == toSend.b)
                                {
                                    waitfor--;
                                    if (waitfor < (num_servers / 2))
                                    {
                                        C->SendDecision(toSend);
                                        C->CloseAllConnections();
                                        return NULL;
                                    }
                                } else {
                                    C->SendPreEmpted(recvd_ballot);
                                    C->CloseAllConnections();
                                    return NULL;
                                }
                            } else {    //other messages
                                D(cout << "SC" << C->S->get_pid() << ": Unexpected message received: " << msg << endl;)
                            }
                        }
                    }
                }
            }
        }
    }
}

void Master::SendAllClearToServers()
{   
    string message;
    ConstructAllClearMessage(message);

    for(int i=0; i<get_num_servers(); i++)
    {
        if (send(get_server_fd(i), message.c_str(), message.size(), 0) == -1) {
            D(cout << "M  : ERROR: Cannot send all clear to server " <<i<<  endl;)
        } else {
            D(cout << "M  : All clear message sent to server " << i<< endl;)
        }
    }
}

void Master::GetServerFdSet(fd_set& server_fd_set, vector<int>& server_fd_vec, int& fd_max)
{
    int fd_temp;
    fd_max = INT_MIN;
    set<int> local_set = get_server_fd_set();
    FD_ZERO(&server_fd_set);
    server_fd_vec.clear();
    for (auto it = local_set.begin(); it != local_set.end(); it++)
    {
        fd_temp = *it;
        if (fd_temp != -1)
        {
            FD_SET(fd_temp, &server_fd_set);
            fd_max = max(fd_max, fd_temp);
            server_fd_vec.push_back(fd_temp);
        }
    }
}

/**
 * receives chatlog from a client
 * @param client_id id of client from which chatlog is to be received
 * @param chat_log  [out] chatlog received in concatenated string form
 */
void Master::ReceiveChatLogFromClient(const int client_id, string &chat_log) {
    char buf[kMaxDataSize];
    int num_bytes;
    num_bytes = recv(get_client_fd(client_id), buf, kMaxDataSize - 1, 0);
    if (num_bytes == -1) {
        D(cout << "M  : ERROR in receiving ChatLog from client C" << client_id << endl;)
    } else if (num_bytes == 0) {    // connection closed by master
        D(cout << "M  : Connection closed by client C" << client_id << endl;)
    } else {
        buf[num_bytes] = '\0';
        chat_log = string(buf);
        // cout<<"CHATLOG"<<chat_log<<endl;
        // TODO: DOES NOT handle multiple chatlogs sent by the client in one go
    }
}

/**
 * prints chat log received from a client in the expected format
 * @param chat_log chat log in concatenated string format
 */
void Master::PrintChatLog(const int client_id, const string &chat_log) {
    std::vector<string> chat = split(chat_log, kMessageDelim[0]);
    for (auto &c : chat) {
        std::vector<string> token = split(c, kInternalDelim[0]);
        for (int i = 0; (i + 2) < token.size(); i = i + 3) {
            fout_[client_id] << token[i] << " " << token[i + 1] << ": " << token[i + 2] << endl;
        }
    }
    fout_[client_id]<<"-------------"<<endl;
}

/**
 * initialize data members and resize vectors
 */
void Master::Initialize() {
    set_primary_id(0);
    server_pid_.resize(num_servers_, -1);
    client_pid_.resize(num_clients_, -1);
    server_fd_.resize(num_servers_, -1);
    client_fd_.resize(num_clients_, -1);
    server_listen_port_.resize(num_servers_);
    client_listen_port_.resize(num_clients_);
    fout_.resize(num_clients_);

    for (int i = 0; i < num_clients_; ++i) {
        fout_[i].open(kChatLogFile + to_string(i), ios::app);
    }
}

/**
 * spawns n servers and connects to them
 * @param n number of servers to be spawned
 * @return true if n servers were spawned and connected to successfully
 */
bool Master::SpawnServers(const int n) {
    pid_t pid;
    int status;
    for (int i = 0; i < n; ++i) {
        char server_id_arg[10];
        char num_servers_arg[10];
        char num_clients_arg[10];
        sprintf(server_id_arg, "%d", i);
        sprintf(num_servers_arg, "%d", num_servers_);
        sprintf(num_clients_arg, "%d", num_clients_);
        char *argv[] = {(char*)kServerExecutable.c_str(),
                        server_id_arg,
                        num_servers_arg,
                        num_clients_arg,
                        NULL
                       };
        status = posix_spawn(&pid,
                             (char*)kServerExecutable.c_str(),
                             NULL,
                             NULL,
                             argv,
                             environ);
        if (status == 0) {
            D(cout << "M  : Spawed server S" << i << endl;)
            set_server_pid(i, pid);
        } else {
            D(cout << "M  : ERROR: Cannot spawn server S"
              << i << " - " << strerror(status) << endl);
            return false;
        }
    }

    // sleep for some time to make sure accept threads of servers are running
    usleep(kGeneralSleep);
    for (int i = 0; i < n; ++i) {
        if (ConnectToServer(i)) {
            D(cout << "M  : Connected to server S" << i << endl;)
        } else {
            D(cout << "M  : ERROR: Cannot connect to server S" << i << endl;)
            return false;
        }
    }
    return true;
}

/**
 * spawns n clients and connects to them
 * @param n number of clients to be spawned
 * @return true if n client were spawned and connected to successfully
 */
bool Master::SpawnClients(const int n) {
    pid_t pid;
    int status;
    for (int i = 0; i < n; ++i) {
        char server_id_arg[10];
        char num_servers_arg[10];
        char num_clients_arg[10];
        sprintf(server_id_arg, "%d", i);
        sprintf(num_servers_arg, "%d", num_servers_);
        sprintf(num_clients_arg, "%d", num_clients_);
        char *argv[] = {(char*)kClientExecutable.c_str(),
                        server_id_arg,
                        num_servers_arg,
                        num_clients_arg,
                        NULL
                       };
        status = posix_spawn(&pid,
                             (char*)kClientExecutable.c_str(),
                             NULL,
                             NULL,
                             argv,
                             environ);
        if (status == 0) {
            D(cout << "M  : Spawed client C" << i << endl;)
            set_client_pid(i, pid);
        } else {
            D(cout << "M  : ERROR: Cannot spawn client C" << i << " - " << strerror(status) << endl;)
            return false;
        }
    }

    // sleep for some time to make sure accept threads of clients are running
    usleep(kGeneralSleep);
    for (int i = 0; i < n; ++i) {
        if (ConnectToClient(i)) {
            D(cout << "M  : Connected to client C" << i << endl;)
        } else {
            D(cout << "M  : ERROR: Cannot connect to client C" << i << endl;)
            return false;
        }
    }
    return true;
}

/**
 * kills the specified server. Set its pid and fd to -1
 * @param server_id id of server to be killed
 */
void Master::CrashServer(const int server_id) {
    int pid = get_server_pid(server_id);
    if (pid != -1) {
        // TODO: Think whether SIGKILL or SIGTERM
        // TODO: If using SIGTERM, consider signal handling in server.cpp
        kill(pid, SIGKILL);
        set_server_pid(server_id, -1);
        set_server_fd(server_id, -1);
    }
}

/**
 * kills the specified client. Set its pid and fd to -1
 * @param client_id id of client to be killed
 */
void Master::CrashClient(const int client_id) {
    int pid = get_client_pid(client_id);
    if (pid != -1) {
        // TODO: Think whether SIGKILL or SIGTERM
        // TODO: If using SIGTERM, consider signal handling in client.cpp
        kill(pid, SIGKILL);
        set_client_pid(client_id, -1);
        set_client_fd(client_id, -1);
    }
}

/**
 * kills all running servers
 */
void Master::KillAllServers() {
    for (int i = 0; i < num_servers_; ++i) {
        CrashServer(i);
    }
}

/**
 * kills all running clients
 */
void Master::KillAllClients() {
    for (int i = 0; i < num_clients_; ++i) {
        CrashClient(i);
    }
}

/**
 * sends message to a client
 * @param client_id id of client to which message needs to be sent
 * @param message   message to be sent
 */
void Master::SendMessageToClient(const int client_id, const string &message) {
    if (send(get_client_fd(client_id), message.c_str(), message.size(), 0) == -1) {
        D(cout << "M  : ERROR: Cannot send message to client C" << client_id << endl;)
    } else {
        D(cout << "M  : Message sent to client C" << client_id << ": " << message << endl;)
    }
}

int main() {
    Master M;
    M.ReadTest();

    usleep(8000 * 1000);
    M.KillAllServers();
    M.KillAllClients();
    return 0;
}