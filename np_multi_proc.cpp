/* Concurrent connection-oriented + FIFO + Shared memory */

#include<iostream>
#include<cstring>
#include<sstream>
#include<string>
#include<vector>
#include<cctype>
#include<map>
#include<pwd.h>
#include<errno.h>
#include<fcntl.h>
#include<signal.h>
#include<stdlib.h>
#include<unistd.h>
#include<sys/wait.h>
#include <arpa/inet.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <sys/stat.h> // mkfifo()

#define MAX_COUNT 1001
#define	MAXLINE	15000
#define MAXUSERINDEX 31
#define USERS_KEY 1111
#define MSG_KEY 2222
#define PIPE_KEY 3333

#define PIPE_NOT_EXIST  -1   // 根本沒建立
#define PIPE_CREATED    -2   // 建立好了，但還沒open
#define PIPE_OPENED      0   // 已經open了，fd是正確的fd值

using namespace std;

enum class ParseState 
{
    Normal,
    AfterYell,
    AfterTellCmd,
    AfterTellTarget
};

struct processState 
{
    int numberedPipe[2] = {-1, -1};
    vector<pid_t> pids;
};

struct usersInfo {
    bool exist;
    pid_t pid;
    char ip4[INET_ADDRSTRLEN];
    int port;
    char name[20];
    // map<int, processState> numberedPipes;
    // map<string, string> env;
};

struct userPipe
{
    int fd[MAXUSERINDEX];
    pid_t pid[MAXUSERINDEX];
};

map<int, processState> numberedPipes;

ParseState parseState = ParseState::Normal;
int current_stdin = STDIN_FILENO;
int current_stdout = STDOUT_FILENO;
int current_stderr = STDERR_FILENO;
constexpr int BACKUP_STDIN  = 4;
constexpr int BACKUP_STDOUT = 5;
constexpr int BACKUP_STDERR = 6;

int numUsers = 1;
int waitStatus, counter, shmUsersData, shmMsg, shmUserPipe, UserId;
char *msg;

int shm_users_id = -1;
int shm_msg_id   = -1;
int shm_pipe_id  = -1;
/*
**原先使用的map會導致子行程彼此無法共享狀態，每個使用者連進來都會 fork() 出一個新行程處理對話。
**這些子行程之間若想要 共享狀態（例如誰在線上、對誰廣播、誰傳訊給誰），就無法用普通 map（存在私有記憶體中）。
**所以必須把 usersData 放在 共享記憶體區段（shared memory），這樣每個子行程才能即時讀取與修改其他人的狀態。
*/
// map<pair<int,int>,processState> userPipes;
// map<int, usersInfo> usersData;
userPipe *userPipes;
usersInfo *usersData;

void sigintHandler(int signo)
{
    if(usersData)
        shmdt(usersData);
    if(msg)
        shmdt(msg);
    if(userPipes)
        shmdt(userPipes);

    if(shm_users_id != -1) 
        shmctl(shm_users_id, IPC_RMID, nullptr);
    if(shm_msg_id   != -1)
        shmctl(shm_msg_id, IPC_RMID, nullptr);
    if(shm_pipe_id  != -1)
        shmctl(shm_pipe_id, IPC_RMID, nullptr);
    _exit(0);
}

void sigchldHandler(int signo)
{
    while (waitpid(-1, &waitStatus, WNOHANG) > 0);
}
void sigAtoBHandler(int signo)
{
    cout << msg;
}

void sigFIFOHandler(int signo)
{
    char fifoName[30];
    for(int i = 1; i < 31; i++)
    {
        sprintf(fifoName, "user_pipe/%dto%d", i, UserId);
        if(userPipes[i].fd[UserId] == PIPE_CREATED)
        {
            userPipes[i].fd[UserId] = open(fifoName, O_RDONLY | O_NONBLOCK);
        }
    }
}

void ParseInput(std::vector<std::vector<std::string>>& dst, const std::string& input)
{
    /* 0. 設計的三個BuildInFunction：yell / tell / name 指令有別於一般輸入，要進行特殊處理 */
    auto starts_with = [](const std::string& line, const std::string& word)->bool
    {
        return line.rfind(word + " ", 0) == 0;
    };

    if (starts_with(input, "yell") || starts_with(input, "tell") || starts_with(input, "name"))
    {
        // 直接把整行當成唯一segment ─ 不處理pipe/redirection
        dst.clear();
        dst.resize(1);
        std::vector<std::string>& args = dst[0];

        std::string arg;
        bool inTok = false;
        for (char c : input) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (inTok) {
                    args.push_back(arg);
                    arg.clear();
                    inTok = false;
                }
            } else {
                inTok = true;
                arg.push_back(c);
            }
        }
        if (!arg.empty()) args.push_back(arg);

        /* 把 yell / tell 的剩餘字串合併成一個參數 */
        if (!args.empty() && (args[0] == "tell" || args[0] == "yell")) {
            std::size_t msg_start = (args[0] == "tell") ? 2 : 1;
            if (args.size() > msg_start) {
                std::string msg;
                for (std::size_t j = msg_start; j < args.size(); ++j) {
                    if (j != msg_start) msg += " ";
                    msg += args[j];
                }
                args.resize(msg_start);
                args.push_back(msg);
            }
        }
        return;
    }

    /* 1.1 一般輸入：依 | / > 先切 segment */
    std::vector<std::string> segments;
    int begin = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '|') {
            segments.push_back(input.substr(begin, i - begin));
            begin = i + 1;
        }
        else if (input[i] == '>') 
        {
            if (i + 1 < (int)input.size() && std::isdigit(input[i + 1]))
                continue;
            segments.push_back(input.substr(begin, i - begin));
            segments.push_back(">" + input.substr(i + 1));
            begin = input.size();
            break;
        }
    }
    if (begin < static_cast<int>(input.size()))
        segments.push_back(input.substr(begin));

    /* 1.2 把每段切成 token */
    dst.clear();
    dst.resize(segments.size());

    for (std::size_t idx = 0; idx < segments.size(); ++idx) {
        const std::string& seg = segments[idx];
        std::vector<std::string>& args = dst[idx];

        std::string tok;
        bool inTok = false;
        std::size_t startPos = 0;

        if (!seg.empty() && seg[0] == '>') {
            args.push_back(">");
            startPos = 1;
        }

        for (std::size_t i = startPos; i < seg.size(); ++i) {
            char c = seg[i];
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (inTok) {
                    args.push_back(tok);
                    tok.clear();
                    inTok = false;
                }
            } else {
                inTok = true;
                tok.push_back(c);
            }
        }
        if (!tok.empty()) args.push_back(tok);
    }
}

// execvp( const char *file, char *const argv[] ) 時，第二個參數必須是以 NULL 結尾的 char* 陣列
// C++ 的 vector<string> 與 string 物件並不符合這種 C 介面的要求，所以必須轉成裸指標陣列
char **string_to_char(vector<string> &input)    //vector<string> to char[][]
{
    int argc = input.size();
    char **argv = new char*[argc+1]; 
    for(int i = 0; i < argc; i++)
    {
        argv[i] = new char[input[i].size() + 1];    // +1 放 '\0'
        strcpy(argv[i], input[i].c_str());
    }
    argv[argc] = nullptr;
    return argv;
}

int buildInCommand(vector<vector<string>> &cmd, int userId)
{
    if (cmd.empty() || cmd[0].empty())      // ← 先檢查
        return 1;
    if (cmd[0][0] == "printenv")
    {
        // printenv後面要跟環境變數名稱！如果使用者沒有輸入變數名稱（少於兩個字串），就什麼都不做
        // getenv() 接收的是 const char*，而 cmd[0][1] 是 string，透過c_str() 轉換，將 string 轉成 C-style 字串，方便給 C 函數使用
        if (cmd[0].size() < 2 || getenv(cmd[0][1].c_str()) == NULL);
            // do nothing
        else 
            cout << getenv(cmd[0][1].c_str()) << '\n';
        return 1;
    }
    // int setenv(const char *name, const char *value, int overwrite);
    // name: 環境變數名稱 value: 環境變數的值 overwrite: 是否覆蓋原有的環境變數(1,0)
    else if (cmd[0][0] == "setenv")
    {
        // setenv後面要跟環境變數名稱！如果使用者沒有輸入變數名稱（少於兩個字串），就什麼都不做
        if (cmd[0].size() < 2);
            // do nothing
        else if (cmd[0].size() < 3)  
        {
            setenv(cmd[0][1].c_str(), "", 1);
        }
        else
        {
            setenv(cmd[0][1].c_str(), cmd[0][2].c_str(), 1);
        }
        return 1;
    }
    else if (cmd[0][0] == "exit")
    {
        while(waitpid(-1, NULL, WNOHANG) > 0);
        return -1;
    }

    else if (cmd[0][0] == "unsetenv")
    {
        // unsetenv後面要跟環境變數名稱！如果使用者沒有輸入變數名稱（少於兩個字串），就什麼都不做
        if (cmd[0].size() < 2);
            // do nothing
        else
        {
            unsetenv(cmd[0][1].c_str());
        }
        return 1;
    }

    /** 這裡是原本的 who() 函數 **/
    // 但這個函數會導致子行程彼此無法共享狀態，每個使用者連進來都會 fork() 出一個新行程處理對話。
    // 這些子行程之間若想要 共享狀態（例如誰在線上、對誰廣播、誰傳訊給誰），就無法用普通 map（存在私有記憶體中）。
    // 所以必須把 usersData 放在 共享記憶體區段（shared memory），透過 mmap() 映射進各個行程，這樣每個子行程才能 即時讀取與修改其他人的狀態。
    // void who(int userId) 
    // {
    //     cout << "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n";
    //     for (const auto& [id, info] : usersData) 
    //     {
    //         cout << id << '\t' << info.name << '\t' << info.ip4 << ':' << info.port;
    //         if (id == userId)
    //             cout << "\t<-me";
    //         cout << '\n';
    //     }
    // }
    else if (cmd[0][0] == "who")
    {
        cout << "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n";
        for (int i = 1; i < 31; ++i) 
        {
            // 存在就印出資訊
            if (!usersData[i].exist)
                continue;
            cout << i << '\t' << usersData[i].name << '\t' << usersData[i].ip4 << ':' << usersData[i].port;
            // 如果是自己就加上 <-me
            if (i == userId)
                cout << "\t<-me";
            cout << '\n';
        }
        return 1;
    }

    // void tell(int senderId, int receiverId, string msg)
    // {
    //     if (client[receiverId] == -1) 
    //     {
    //         cerr << "*** Error: user #" << receiverId << " does not exist yet. ***\n";
    //         return;
    //     }
    //     dup2(client[receiverId], STDIN_FILENO);
    //     dup2(client[receiverId], STDOUT_FILENO);
    //     dup2(client[receiverId], STDERR_FILENO);
    //     cout << "*** " << usersData[senderId].name << " told you ***: " << msg << '\n';
    //     dup2(client[senderId], STDIN_FILENO);
    //     dup2(client[senderId], STDOUT_FILENO);
    //     dup2(client[senderId], STDERR_FILENO);
    // }
    else if (cmd[0][0] == "tell")
    {
        int receiverId = 0;
        sscanf(cmd[0][1].c_str(), "%d", &receiverId);

        if (!usersData[receiverId].exist)
        {
            cerr << "*** Error: user #" << receiverId << " does not exist yet. ***" << '\n';
            return 1;
        }

        memset(msg, '\0' ,sizeof(msg));
        string AtoBmsg = "*** " + string(usersData[userId].name) + " told you ***: " + cmd[0][2] + "\n";
        strcpy(msg, AtoBmsg.c_str());

        if(usersData[receiverId].exist)
            // 用kill() 發送訊號給對方的行程，讓它知道有訊息要傳送
            kill(usersData[receiverId].pid, SIGUSR1);

        return 1;
    }

    // yell 會傳給所有的其他使用者，用for迴圈去遍歷所有的使用者
    // void yell(int userId, string msg) 
    // {
    //     for (const auto& [id, info] : usersData) 
    //     {
    //         dup2(client[id], STDIN_FILENO);
    //         dup2(client[id], STDOUT_FILENO);
    //         dup2(client[id], STDERR_FILENO);
    //         cout << "*** " << usersData[userId].name << " yelled ***: " << msg << '\n';
    //     }
    //     dup2(client[userId], STDIN_FILENO);
    //     dup2(client[userId], STDOUT_FILENO);
    //     dup2(client[userId], STDERR_FILENO);
    // }
    else if (cmd[0][0] == "yell")
    {
        memset(msg, '\0' ,sizeof(msg));
        string broadcast = "*** " + string(usersData[userId].name) + " yelled ***: " + cmd[0][1] + "\n";
        strcpy(msg, broadcast.c_str());
        for (int i = 1; i < 31; ++i) 
        {
            if (usersData[i].exist)
                kill(usersData[i].pid, SIGUSR1);
        }
        usleep(1000);
        return 1;
    }

    // void name(int userId, string newName) 
    // {
    //     for (const auto& [id, info] : usersData) 
    //     {
    //         if (info.name == newName) 
    //         {
    //             cerr << "*** User '" << newName << "' already exists. ***\n";
    //             return;
    //         }
    //     }

    //     usersData[userId].name = newName;
    //     for (const auto& [id, info] : usersData) 
    //     {
    //         dup2(client[id], STDIN_FILENO);
    //         dup2(client[id], STDOUT_FILENO);
    //         dup2(client[id], STDERR_FILENO);
    //         cout << "*** User from " << usersData[userId].ip4 << ":" << usersData[userId].port << " is named '" << newName << "'. ***\n";
    //     }
    //     dup2(client[userId], STDIN_FILENO);
    //     dup2(client[userId], STDOUT_FILENO);
    //     dup2(client[userId], STDERR_FILENO);
    // }
    else if (cmd[0][0] == "name")
    {
        // 檢查是否有重複的名稱
        for (int i = 1; i < 31; ++i) 
        {
            if (usersData[i].exist && cmd[0][1] == usersData[i].name) 
            {
                cerr << "*** User '" << cmd[0][1] << "' already exists. ***" << '\n';
                return 1;
            }
        }

        strcpy(usersData[userId].name, cmd[0][1].c_str());
        memset(msg, '\0' ,sizeof(msg));
        string broadcast = "*** User from " + string(usersData[userId].ip4) + ":" + to_string(usersData[userId].port) + " is named '" + cmd[0][1] + "'. ***\n";
        strcpy(msg, broadcast.c_str());
        for (int i = 1; i < 31; ++i) 
        {
            if (usersData[i].exist)
                kill(usersData[i].pid, SIGUSR1);
        }
        usleep(1000);
        return 1;
    }

    return 0;
}

inline void safeClose(int fd) 
{
    if (fd >= 0)
        close(fd);
}

void closeAllUnusedFDs(int *ordinaryPipe, int pipeCount, int currentCmdIndex) 
{
    // for (auto &user : usersData)
    // {
    //     safeClose(client[user.first]);
    //     for (auto &proc : user.second.numberedPipes) 
    //     {
    //         safeClose(proc.second.numberedPipe[0]);
    //         safeClose(proc.second.numberedPipe[1]);
    //     }
    // }

    // 同上註解，兩層for來關閉
    for (int j = 1; j < MAXUSERINDEX; j++)
    {
        for (int k = 1; k < MAXUSERINDEX; k++)
        {
            if (userPipes[j].fd[k] != -1)
            {
                safeClose(userPipes[j].fd[k]);
            }
        }
    }

    for (int j = 0; j < (currentCmdIndex + 1) * 2 && j < pipeCount * 2; ++j)
        safeClose(ordinaryPipe[j]);

    // 關閉所有 active 的 numbered pipe
    for (auto &pipe : numberedPipes)
    {
        safeClose(pipe.second.numberedPipe[0]);
        safeClose(pipe.second.numberedPipe[1]);
    }

    // for (int k=0; k<FD_SETSIZE; ++k)
    //     safeClose(client[k]);
}

// 統一定義：來源->目的 的 FIFO 名稱
string getFifoName(int src, int dst)
{
    char name[64];
    snprintf(name, sizeof(name), "user_pipe/%dto%d", src, dst);
    return string(name);
}

void childProcess(int i,
                int numOrdinaryPipe,
                int ordinaryPipe[],
                bool fileRedir,
                int &fd,
                int userId,
                vector<vector<string>> &cmd,
                bool hasUserPipeInput, bool hasUserPipeOutput,
                int UserPipeSrcId, int UserPipeDstId,
                bool userPipeExists)
{
    bool usedUserPipeIn = false, usedUserPipeOut = false;

    if(hasUserPipeInput && UserPipeSrcId != 0)
    {
        if(!usersData[UserPipeSrcId].exist)
        {
            dup2(open("/dev/null", O_RDONLY, 0), STDIN_FILENO);     // 錯誤處理 -> 開個空stdin避免阻塞
        }
        else if(userPipes[UserPipeSrcId].fd[userId] == PIPE_NOT_EXIST)
        {
            dup2(open("/dev/null", O_RDONLY, 0), STDIN_FILENO);     // 無效的 user pipe，導向 null
        }
        else
        {
            usedUserPipeIn = true;
            dup2(userPipes[UserPipeSrcId].fd[userId], STDIN_FILENO);
            userPipes[UserPipeSrcId].fd[userId] = -1;
        }
    }
    if(hasUserPipeOutput && UserPipeDstId != 0)
    {
        if(!usersData[UserPipeDstId].exist)
        {
            dup2(open("/dev/null", O_RDWR, 0), STDOUT_FILENO);
        }
        else if(userPipeExists)
        {
            dup2(open("/dev/null", O_RDWR, 0), STDOUT_FILENO);
        }
        else
        {
            usedUserPipeOut = true;
            // 有userpipeout要傳送給目標使用者，且目標使用者存在時
            // 透過fifo file來傳送訊息
            // 參考 https://man7.org/linux/man-pages/man7/fifo.7.html
            // FIFO 檔名：user_pipe/來源id-目標id
            std::string fifoName = getFifoName(userId, UserPipeDstId);

            mkfifo(fifoName.c_str(), 0666);

            // 標記 userPipes 對應 fd 位置，供接收端之後 SIGUSR2 開啟讀端
            userPipes[userId].fd[UserPipeDstId] = PIPE_CREATED;

            // 通知目標使用者準備接收
            kill(usersData[UserPipeDstId].pid, SIGUSR2);

            // 開啟 FIFO 的寫端，並導向 stdout
            int fifoFD = open(fifoName.c_str(), O_WRONLY);
            if (fifoFD == -1) 
            {
                perror("open FIFO for writing");
                dup2(open("/dev/null", O_RDWR), STDOUT_FILENO);
            }
            else
            {
                dup2(fifoFD, STDOUT_FILENO);
                // dup2(fifoFD, STDERR_FILENO);    // 錯誤訊息也導向 FIFO
                close(fifoFD);
            }
        }
    }
    if (!usedUserPipeIn)
    {
        // 第一條指令 如果沒有Number pipe -> 保持預設 STDIN
        if (i != 0) 
            dup2( ordinaryPipe[(i - 1) * 2], STDIN_FILENO );    // 從上一條一般 pipe 的 讀端 接進來
        else if ( numberedPipes.find(counter) != numberedPipes.end() )
            dup2( numberedPipes[counter].numberedPipe[0], STDIN_FILENO );
    }
    if (!usedUserPipeOut)
    {
        if (i != numOrdinaryPipe)    // 中間指令
        {
            dup2( ordinaryPipe[i * 2 + 1], STDOUT_FILENO );
        }
        else if (fileRedir)     // 最後一條且 >
        {
            fd = open( cmd.back()[1].c_str(), O_TRUNC | O_RDWR | O_CREAT, 0777);
            dup2( fd, STDOUT_FILENO );
        }
        else    // 最後一條無重導
        {
            dup2(current_stdout,STDOUT_FILENO);
            dup2(current_stderr,STDERR_FILENO);
        }
    }

    closeAllUnusedFDs(ordinaryPipe, numOrdinaryPipe, i);

    // 透過輔助函式 string_to_char(cmd[i]) 產生 argv。
    char **argv = string_to_char(cmd[i]);
    if ( execvp(argv[0], argv) == -1 ) {
        cerr << "Unknown command: [" << argv[0] << "].\n";
        exit(1);
    }
    exit(0);
}

void parentProcess(int i,
                int numOrdPipe,
                int ordinaryPipe[],
                bool shouldRecvUP,
                int upSrc,
                int userId)
{
    /* 回收殭屍 */
    while (waitpid(-1, &waitStatus, WNOHANG) > 0);

    /* 關掉上一條一般 pipe */
    if (i != 0)
    {
        close(ordinaryPipe[(i - 1) * 2]);
        close(ordinaryPipe[(i - 1) * 2 + 1]);
    }

    /* Number pipe 到期處理 */
    if ( numberedPipes.find(counter) != numberedPipes.end() ) 
    {
        close( numberedPipes[counter].numberedPipe[0] );
        close( numberedPipes[counter].numberedPipe[1] );
        // 逐一 waitpid 該 pipe 內記錄的子行程 PID，確保完全回收
        for (auto childpid : numberedPipes[counter].pids)
            waitpid(childpid, &waitStatus, 0);
        numberedPipes.erase(counter);
    }
    if(shouldRecvUP)
    {
        pid_t &pid = userPipes[upSrc].pid[userId];
        int &fd = userPipes[upSrc].fd[userId];

        waitpid(pid, &waitStatus, 0);
        pid = 0;
    
        close(fd);

        std::string fifoPath = "user_pipe/" + std::to_string(upSrc) + "to" + std::to_string(userId);
        if (access(fifoPath.c_str(), F_OK) == 0)
        {
            unlink(fifoPath.c_str());
        }
    }
}

void ExecuteCommand(vector<vector<string>> &cmd, int pipeTime, string &line, int userId)
{
    int numOrdinaryPipe = cmd.size() - 1, fd = -1;
    int UserPipeSrcId = 0, UserPipeDstId = 0;
    bool shouldReceiveUserPipe = false, shouldSendUserPipe = false;
    /* 檢查最後一段是否是 > */
    bool fileRedirection = cmd.back()[0] == ">";
    if (fileRedirection)
        numOrdinaryPipe--;

    /* 建立一般 pipe 陣列 */
    int ordinaryPipe[numOrdinaryPipe * 2];
    memset( ordinaryPipe, -1, sizeof(ordinaryPipe));

    pid_t childpid;

    for (int i = 0; i < numOrdinaryPipe + 1; ++i) 
    {
        // UserPipeHandle
        bool hasUserPipeInput = false, hasUserPipeOutput = false, userPipeExists = false;
        int argc = cmd[i].size();
        
        if (argc > 1)
        {
            // 如果這一段指令的最後一個字串的第一個字元是'>' Ex: >2
            if (cmd[i][argc - 1][0] == '>')
            {
                // >3 -> 3
                sscanf(cmd[i][argc - 1].substr(1).c_str(), "%d", &UserPipeDstId);
                hasUserPipeOutput = true;
            }
            else if (cmd[i][argc - 1][0] == '<')
            {
                // <3 -> 3
                sscanf(cmd[i][argc - 1].substr(1).c_str(), "%d", &UserPipeSrcId);
                hasUserPipeInput = true;
            }
            // 處理 <2 >3 的情況
            if (argc > 2)
            {
                if (cmd[i][argc - 2][0] == '>')
                {
                    sscanf(cmd[i][argc - 2].substr(1).c_str(), "%d", &UserPipeDstId);
                    hasUserPipeOutput = true;
                }
                else if (cmd[i][argc - 2][0] == '<')
                {
                    sscanf(cmd[i][argc - 2].substr(1).c_str(), "%d", &UserPipeSrcId);
                    hasUserPipeInput = true;
                }
            }
        }

        if (hasUserPipeInput && UserPipeSrcId != 0)
        {
            cmd[i].pop_back();    // 移除 >3
            if (!usersData[UserPipeSrcId].exist)
            {
                cerr << "*** Error: user #" << UserPipeSrcId << " does not exist yet. ***" << '\n';
            }
            // 處理 <2 cat 但user #2 並沒有對使用過 >N的情況 Ex: *** Error: the pipe #<sender_id>->#<receiver_id> does not exist yet. ***
            else if (userPipes[UserPipeSrcId].fd[userId] == PIPE_NOT_EXIST)
            {
                cerr << "*** Error: the pipe #" << UserPipeSrcId << "->#" << userId << " does not exist yet. ***" << '\n';
            }
            else
            {
                // 原本是map的情況下，用for迴圈去遍歷到所有的使用者
                // for (const auto& [uid, info] : usersData)
                // {
                //     dup2(client[uid], STDIN_FILENO);
                //     dup2(client[uid], STDOUT_FILENO);
                //     dup2(client[uid], STDERR_FILENO);
                //     cout << "*** " << usersData[userId].name << " (#" << userId << ") just received from " << usersData[UserPipeSrcId].name << " (#" << UserPipeSrcId << ") by '" << line << "' ***\n";
                // }
                // dup2(client[userId], STDIN_FILENO);
                // dup2(client[userId], STDOUT_FILENO);
                // dup2(client[userId], STDERR_FILENO);
                // shouldReceiveUserPipe = true;
                memset(msg, '\0' ,sizeof(msg));
                string broadcast = "*** " + string(usersData[userId].name) + " (#" + to_string(userId) + ") just received from " + string(usersData[UserPipeSrcId].name) + " (#" + to_string(UserPipeSrcId) + ") by '" + line + "' ***\n";
                strcpy(msg, broadcast.c_str());
                for(int i = 1; i < 31; i++)
                {
                    if(usersData[i].exist)
                        kill(usersData[i].pid, SIGUSR1);
                }
                usleep(1000);

                shouldReceiveUserPipe = true;
            }
        }

        if (hasUserPipeOutput && UserPipeDstId != 0)
        {
            cmd[i].pop_back();    // 移除 <3
            if (!usersData[UserPipeDstId].exist)
            {
                cerr << "*** Error: user #" << UserPipeDstId << " does not exist yet. ***" << '\n';
            }
            else if (userPipes[userId].fd[UserPipeDstId] != PIPE_NOT_EXIST)
            {
                cerr << "*** Error: the pipe #" << userId << "->#" << UserPipeDstId << " already exists. ***" << '\n';
                userPipeExists = true;
            }
            else
            {
                // for (const auto& [uid, info] : usersData)
                // {
                //     dup2(client[uid], STDIN_FILENO);
                //     dup2(client[uid], STDOUT_FILENO);
                //     dup2(client[uid], STDERR_FILENO);
                //     cout << "*** " << usersData[userId].name << " (#" << userId << ") just piped '" << line << "' to " << usersData[UserPipeDstId].name << " (#" << UserPipeDstId << ") ***" << '\n';
                // }
                // dup2(client[userId], STDIN_FILENO);
                // dup2(client[userId], STDOUT_FILENO);
                // dup2(client[userId], STDERR_FILENO);
                // pipe(userPipes[{userId, UserPipeDstId}].numberedPipe);
                memset(msg, '\0' ,sizeof(msg));
                string broadcast = "*** " + string(usersData[userId].name) + " (#" + to_string(userId) + ") just piped '" + line + "' to " + string(usersData[UserPipeDstId].name) + " (#" + to_string(UserPipeDstId) + ") ***\n";
                strcpy(msg, broadcast.c_str());
                for(int i = 1; i < 31; i++)
                {
                    if(usersData[i].exist)
                        kill(usersData[i].pid, SIGUSR1);
                }
                usleep(1000);

                shouldSendUserPipe = true;
            }
        }

        // Handle fork may failed
        while (true)
        {
            if(i < numOrdinaryPipe)
                pipe(ordinaryPipe + i * 2);

            while(waitpid(-1, &waitStatus, WNOHANG) > 0);

            childpid = fork();
            if(childpid >= 0)
                break; // fork成功跳出
            usleep(1000);       // fork失敗就稍等再試
        }

        if (childpid == 0)
        { 
            childProcess(i, numOrdinaryPipe, ordinaryPipe, fileRedirection, fd, userId, cmd, hasUserPipeInput, hasUserPipeOutput, UserPipeSrcId, UserPipeDstId, userPipeExists);
        } 
        else
        { 
            parentProcess(i, numOrdinaryPipe, ordinaryPipe, shouldReceiveUserPipe, UserPipeSrcId, userId);
        }

    }

    // 最後一條指令的處理
    if (shouldSendUserPipe)
        userPipes[userId].pid[UserPipeDstId] = childpid;
    else if (pipeTime == -1)
        waitpid(childpid, &waitStatus, 0);
    else
        numberedPipes[pipeTime].pids.push_back(childpid);

    while ( waitpid(-1, &waitStatus, WNOHANG) > 0 );

    if (fileRedirection)
        close(fd);

    /* 重設預設輸出 */
    current_stdout = STDOUT_FILENO;
    current_stderr = STDERR_FILENO;
}

void handleNumberedPipes(string &line, int userId)
{
    int segmentStart = 0;     // 每段指令的起始位置
    int pipeNum = 0;          // Number 管線 N
    int pipeTargetLine;       // 實際對應的未來執行位置
    int i = 0;                // 掃描 index

    while (i < line.size()) 
    {
        if (line[i] == '|' || line[i] == '!') 
        {
            char pipeSymbol = line[i];
            int j = i + 1;
            pipeNum = 0;

            // 讀取數字 N（編號）
            while (j < line.size() && isdigit(line[j]))
            {
                pipeNum = pipeNum * 10 + (line[j] - '0');
                j++;
            }

            // 若 |0 或 !0，無意義，跳過
            if (pipeNum == 0)
            {
                i = j;
                continue;
            }

            // 計算 pipe 實際生效時間點
            counter = (counter + 1) % MAX_COUNT;
            pipeTargetLine = (counter + pipeNum) % MAX_COUNT;

            // 若尚未建立該 pipe，則建立
            if (numberedPipes.find(pipeTargetLine) == numberedPipes.end())
            {
                pipe(numberedPipes[pipeTargetLine].numberedPipe);
            }

            // 設定 stdout / stderr
            current_stdout = numberedPipes[pipeTargetLine].numberedPipe[1];
            if (pipeSymbol == '!')
            {
                current_stderr = numberedPipes[pipeTargetLine].numberedPipe[1];
            }

            // 執行目前區段
            vector<vector<string>> cmd;
            string subCommand = line.substr(segmentStart, i - segmentStart);
            ParseInput(cmd, subCommand);
            ExecuteCommand(cmd, pipeTargetLine, line, userId);

            // 移動 segment 起始點到下一段
            segmentStart = j;
            i = j - 1;
        }

        i++;
    }

    // 若還有剩下的指令（沒有尾巴 |N），執行最後一段
    if (segmentStart < line.size()) 
    {
        line = line.substr(segmentStart);
    }
    else 
    {
        line.clear();
    }
}

void serviceClient(int client_fd, int server_fd, struct sockaddr_in client_addr, int numUsers)
{
    int i, n;
    char buf[MAXLINE];
    std::string curLine;
    std::string broadcast;
    vector<vector<string>> cmd;

    UserId = numUsers;
    close(server_fd);
    dup2(client_fd, 0);
    dup2(client_fd, 1);
    dup2(client_fd, 2);
    close(client_fd);

    clearenv();
    setenv("PATH", "bin:.", 1);
    counter = 0;
    numberedPipes.clear();

    /* 歡迎訊息 + 廣播 */
    cout << "****************************************\n";
    cout << "** Welcome to the information server. **\n";
    cout << "****************************************\n";
    while(!usersData[numUsers].exist)
        usleep(1000);
    
    // for(auto  user: usersData)
    // {
    //     dup2(client[user.first], 0);
    //     dup2(client[user.first], 1);
    //     dup2(client[user.first], 2);
    //     cout << "*** User '" << usersData[i].name << "' entered from " << usersData[i].ip4 << ":" << usersData[i].port << ". ***" << '\n';
    // }
    memset(msg, '\0' ,sizeof(msg));
    broadcast = "*** User '" + string(usersData[numUsers].name) + "' entered from " + string(usersData[numUsers].ip4) + ":" + to_string(usersData[numUsers].port) + ". ***\n";
    strcpy(msg, broadcast.c_str());
    for(int i = 1; i < 31; i++)
    {
        if(usersData[i].exist)
            kill(usersData[i].pid, SIGUSR1);
    }
    usleep(1000);
        
    // dup2(client[i], 0);
    // dup2(client[i], 1);
    // dup2(client[i], 2);
    cout << "% ";
    // dup2(BACKUP_STDIN , STDIN_FILENO );
    // dup2(BACKUP_STDOUT, STDOUT_FILENO);
    // dup2(BACKUP_STDERR, STDERR_FILENO);
                
    // 處理read fail的錯誤
    while (true) 
    {
        n = read(STDIN_FILENO, buf, MAXLINE);
        if (n == -1)
            continue;
        if (n == 0)
            break;
        
        int execFlag = 0;

        for(int j = 0; j < n; j++)
        {
            if(buf[j] == '\r')
                continue;
            if(buf[j] != '\n')
            {
                curLine.push_back(buf[j]);
                continue;
            }
            string line = curLine;
            curLine.clear();

            handleNumberedPipes(line, numUsers);
            if (line.empty())
            {
                cout << "% ";
                continue;
            }
            ParseInput(cmd, line);
            execFlag = buildInCommand(cmd, numUsers);

            if(execFlag == 1)
            {
                cout << "% ";
                continue;
            }
            else if(execFlag == -1)
            {
                break;
            }
            if(cmd.size() <= 0)
            {
                cout << "% ";
                continue;
            }
            counter = (counter + 1) % MAX_COUNT;
            ExecuteCommand(cmd, -1, line, numUsers);
            cout << "% ";
        }
        if (execFlag == -1)
        {
            break;
        }
    }
    // 執行完以上處理指令後，清空指令
    char fifoName[30];
    for (int i = 1; i < MAXUSERINDEX; ++i) 
    {
        /* 關掉還開著的 fd */
        if (userPipes[numUsers].fd[i] >= 0)
            close(userPipes[numUsers].fd[i]);
        if (userPipes[i].fd[numUsers] >= 0)
            close(userPipes[i].fd[numUsers]);
        
        /* 把 FIFO 檔刪掉 */
        std::string fifo_ft = "user_pipe/" + std::to_string(numUsers) + "to" + std::to_string(i);
        std::string fifo_tf = "user_pipe/" + std::to_string(i)   + "to" + std::to_string(numUsers);
        unlink(fifo_ft.c_str());
        unlink(fifo_tf.c_str());

        /* 重設共享記憶體裡的狀態 */
        userPipes[numUsers].fd[i]  = PIPE_NOT_EXIST;
        userPipes[i].fd[numUsers]  = PIPE_NOT_EXIST;
        userPipes[numUsers].pid[i] = 0;
        userPipes[i].pid[numUsers] = 0;
    }
    usersData[numUsers].exist = false;

    memset(msg, '\0' ,sizeof(msg));
    broadcast = "*** User '" + string(usersData[numUsers].name) + "' left. ***\n";
    strcpy(msg, broadcast.c_str());
    for(int i = 1; i < 31; i++)
    {
        if(usersData[i].exist)
            kill(usersData[i].pid, SIGUSR1);
    }
    usleep(1000);

    usersData[numUsers].pid = 0;
    usersData[numUsers].port = 0;
    memset(usersData[numUsers].ip4, '\0', sizeof(usersData[numUsers].ip4));
    memset(usersData[numUsers].name, '\0', sizeof(usersData[numUsers].name));
    strcpy(usersData[numUsers].name, "(no name)");
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    // if (usersData)
    //     shmdt(usersData);
    // if (msg)
    //     shmdt(msg);
    // if (userPipes)
    //     shmdt(userPipes);

    exit(0);
}

int main(int argc,char* argv[])
{
    signal(SIGINT, sigintHandler);
    signal(SIGCHLD, sigchldHandler);
    signal(SIGUSR1, sigAtoBHandler);
    signal(SIGUSR2, sigFIFOHandler);

    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);
    
    if (argc != 2) 
    {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    // socket
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    bzero((char *)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(atoi(argv[1]));

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    
    if (listen(server_fd, 5) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    // socket
    
    // share memory
    int shm_users_id = shmget(USERS_KEY, MAXUSERINDEX * sizeof(usersInfo), IPC_CREAT | 0666);
    if (shm_users_id < 0)
    {
        perror("shmget USERS");
        exit(1);
    }

    int shm_msg_id   = shmget(MSG_KEY, MAXLINE, IPC_CREAT | 0666);
    if (shm_msg_id < 0)
    {
        perror("shmget MSG");
        exit(1);
    }

    int shm_pipe_id  = shmget(PIPE_KEY,MAXUSERINDEX * sizeof(userPipe), IPC_CREAT | 0666);
    if (shm_pipe_id < 0)
    {
        perror("shmget PIPE");
        exit(1);
    }

    usersData = static_cast<usersInfo*>(shmat(shm_users_id, nullptr, 0));
    if (usersData == (void *)-1)
    {
        perror("shmat USERS");
        exit(1);
    }

    msg = static_cast<char*>(shmat(shm_msg_id, nullptr, 0));
    if (msg == (void *)-1)
    {
        perror("shmat MSG");
        exit(1);
    }

    userPipes = static_cast<userPipe*>(shmat(shm_pipe_id, nullptr, 0));
    if (userPipes == (void *)-1)
    {
        perror("shmat PIPE");
        exit(1);
    }
    // share memory

    for(int i = 0; i < 31; i++)
    {
        for(int j = 0; j < 31; j++)
        {
            userPipes[i].fd[j] = -1;
            userPipes[i].pid[j] = 0;
        }
    }

	for(int i = 0; i < 31; i++)
    {
		usersData[i].exist = false;
		usersData[i].pid = 0;
		usersData[i].port = 0;
		memset(usersData[i].ip4, '\0', sizeof(usersData[i].ip4));
		memset(usersData[i].name, '\0', sizeof(usersData[i].name));
		strcpy(usersData[i].name, "(no name)");
	}
	memset(msg, '\0', sizeof(msg));

    while (true)
    {
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept");
            exit(EXIT_FAILURE);
        }

        int numUsers = 1;
        while(usersData[numUsers].exist)
        {
            numUsers++;
            if(numUsers >= 31)
                break;
        }
        
        pid_t pid = fork();
        // fork 失敗
        while (pid < 0)
        {
            usleep(1000);
            pid = fork();
            if (pid >= 0)
                break;
        }

        if (pid == 0)
        {
            serviceClient(client_fd, server_fd, client_addr, numUsers);
        }
        else
        {
			close(client_fd);

            /* 在共享記憶體註冊這位使用者 */
            usersData[numUsers].pid = pid;
            usersData[numUsers].exist = true;
            inet_ntop(AF_INET, &(client_addr.sin_addr), usersData[numUsers].ip4, INET_ADDRSTRLEN);
            usersData[numUsers].port = ntohs(client_addr.sin_port);
            strcpy(usersData[numUsers].name, "(no name)");
        }
    }
    
    return 0;
}