# NP-RWG — Concurrent Connection-Oriented（Shared Memory + FIFO 版）

> 這個版本在前一個Project（`np_simple` / `np_single_proc`）基礎上， 但改為Shared Memory + FIFO：
> - 同樣是 **connection-oriented + fork-per-client** 的Concurrent 伺服器。
> - 新增 **共享記憶體（System V SHM）** 管理多人狀態與訊息佇列。
> - 以 **Named FIFO（有名管道）** 實作跨使用者的 User Pipe。
> - 透過 **訊號**（`SIGUSR1` / `SIGUSR2`）做跨行程通知（廣播/私訊、FIFO 就緒）。
>
> 其他如 Numbered Pipe、ordinary pipe、built-in 指令、解析流程，**與上一個專案相同**。

---

## 功能
- **共享記憶體（SHM）**：
  - `USERS_KEY=1111`：`usersInfo[1..30]`，保存 `exist/pid/ip:port/name`。
  - `MSG_KEY=2222`：`char msg[MAXLINE]`，用來暫存一次性廣播/私訊內容。
  - `PIPE_KEY=3333`：`userPipe[1..30]`，保存 **UserPipe 狀態矩陣**（`fd[dst]`、`pid[dst]`）。
- **Named FIFO User Pipe**：
  - 檔名規則：`user_pipe/<src>to<dst>`（例如 `user_pipe/3to7`）。
  - 送端 `>n`：`mkfifo` → 在 SHM 標 `PIPE_CREATED` → `kill(dst, SIGUSR2)` → 開 `O_WRONLY` 並 `dup2(STDOUT)`。
  - 收端 `<n`：在 `SIGUSR2` handler 裡 `open(O_RDONLY|O_NONBLOCK)` → 執行指令時 `dup2(STDIN)`。
- **訊號（Signals）**：
  - `SIGUSR1`：**訊息顯示**。誰收到就 `printf(msg)`（`sigAtoBHandler`）。
  - `SIGUSR2`：**FIFO 準備就緒**。收端在 handler 內嘗試開啟 `user_pipe/<src>to<me>`（`sigFIFOHandler`）。
  - `SIGCHLD`：回收子程序；`SIGINT`：**收攤清理**（`shmdt` + `shmctl(IPC_RMID)`）。

---
