// The RigExec worker as a child process: started through the worker
// launcher with Maya's USD environment removed, spoken to over its stdin and
// stdout in the framing scripts/rigexec_maya/protocol.py defines.
#ifndef RIGEXEC_MAYA_WORKER_PROCESS_H
#define RIGEXEC_MAYA_WORKER_PROCESS_H

#include <cstdint>
#include <string>
#include <vector>

namespace rigExecMaya {

class WorkerProcess
{
public:
    WorkerProcess() = default;
    ~WorkerProcess();
    WorkerProcess(const WorkerProcess &) = delete;
    WorkerProcess &operator=(const WorkerProcess &) = delete;

    // Starts `launcher rigRoot stage [rigPath]` and waits for its ready
    // message. False with the reason (the worker's own, when it sent one).
    bool Start(const std::string &launcher, const std::string &rigRoot,
               const std::string &stage, const std::string &rigPath,
               std::string *error);

    bool IsRunning() const;

    // One evaluate request; `frame` receives the reply bytes.
    bool Evaluate(double time, const std::vector<double> &values,
                  std::vector<uint8_t> *frame, std::string *error);

    void Stop();

private:
    bool _Write(const void *data, size_t size);
    bool _Read(void *data, size_t size);
    bool _Request(const std::vector<uint8_t> &payload,
                  std::vector<uint8_t> *reply, std::string *error);
    bool _ReadMessage(std::vector<uint8_t> *message);

#ifdef _WIN32
    void *_process = nullptr;
    void *_toChild = nullptr;
    void *_fromChild = nullptr;
#else
    int _pid = -1;
    int _toChild = -1;
    int _fromChild = -1;
#endif
    std::string _logPath;
};

}  // namespace rigExecMaya

#endif
