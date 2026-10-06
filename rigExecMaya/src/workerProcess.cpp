#include "workerProcess.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace rigExecMaya {

namespace {

// Environment that belongs to Maya's process and must not reach the worker
// (see scripts/rigexec_maya/client.py, which applies the same rule).
const char *const kScrubbed[] = {"PYTHONHOME", "PYTHONPATH", "PYTHONSTARTUP",
                                 "PYTHONUSERBASE", "USD_LOCATION",
                                 "MATERIALX_SEARCH_PATH"};
const char *const kScrubbedPrefixes[] = {"PXR_", "MAYAUSD_", "MAYA_USD_",
                                         "MAYA_PXR_"};

std::string Upper(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return char(std::toupper(c)); });
    return text;
}

bool Scrubbed(const std::string &name)
{
    const std::string upper = Upper(name);
    for (const char *exact : kScrubbed) {
        if (upper == exact) {
            return true;
        }
    }
    for (const char *prefix : kScrubbedPrefixes) {
        if (upper.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

// PATH without Maya's own directories: the worker resolves its libraries
// from the RigExec install.
std::string FilteredPath(const std::string &value, char separator)
{
    std::string out;
    std::stringstream stream(value);
    std::string entry;
    while (std::getline(stream, entry, separator)) {
        if (Upper(entry).find("AUTODESK") != std::string::npos) {
            continue;
        }
        if (!out.empty()) {
            out += separator;
        }
        out += entry;
    }
    return out;
}

std::vector<std::string> WorkerEnvironment(char separator)
{
    std::vector<std::string> entries;
#ifdef _WIN32
    LPCH block = GetEnvironmentStringsA();
    for (LPCH cursor = block; cursor && *cursor;
         cursor += std::strlen(cursor) + 1) {
        entries.emplace_back(cursor);
    }
    if (block) {
        FreeEnvironmentStringsA(block);
    }
#else
    for (char **cursor = environ; cursor && *cursor; ++cursor) {
        entries.emplace_back(*cursor);
    }
#endif
    std::vector<std::string> out;
    for (const std::string &entry : entries) {
        const size_t equals = entry.find('=', entry.empty() ? 0 : 1);
        if (equals == std::string::npos) {
            out.push_back(entry);
            continue;
        }
        const std::string name = entry.substr(0, equals);
        if (Scrubbed(name)) {
            continue;
        }
        if (Upper(name) == "PATH") {
            out.push_back(name + "=" +
                          FilteredPath(entry.substr(equals + 1), separator));
            continue;
        }
        out.push_back(entry);
    }
    return out;
}

std::string ReadLogTail(const std::string &path)
{
    std::ifstream log(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(log, line)) {
        if (line.rfind("Warning:", 0) == 0) {
            continue;
        }
        lines.push_back(line);
        if (lines.size() > 12) {
            lines.erase(lines.begin());
        }
    }
    std::string out;
    for (const std::string &kept : lines) {
        out += "\n" + kept;
    }
    return out;
}

std::string TempDirectory()
{
#ifdef _WIN32
    char buffer[MAX_PATH + 1];
    const DWORD size = GetTempPathA(MAX_PATH, buffer);
    return size ? std::string(buffer, size) : std::string(".\\");
#else
    const char *tmp = std::getenv("TMPDIR");
    return std::string(tmp ? tmp : "/tmp") + "/";
#endif
}

}  // namespace

WorkerProcess::~WorkerProcess()
{
    Stop();
}

bool
WorkerProcess::Start(const std::string &launcher, const std::string &rigRoot,
                     const std::string &stage, const std::string &rigPath,
                     std::string *error)
{
    Stop();
    _logPath = TempDirectory() + "rigexec_maya_worker.log";
#ifdef _WIN32
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE childIn = nullptr, parentOut = nullptr;
    HANDLE parentIn = nullptr, childOut = nullptr;
    if (!CreatePipe(&childIn, &parentOut, &inherit, 0) ||
        !CreatePipe(&parentIn, &childOut, &inherit, 0)) {
        *error = "could not create the worker pipes";
        return false;
    }
    SetHandleInformation(parentOut, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentIn, HANDLE_FLAG_INHERIT, 0);
    HANDLE log = CreateFileA(_logPath.c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    std::string command = "cmd.exe /d /c \"\"" + launcher + "\" \"" +
                          rigRoot + "\" \"" + stage + "\"";
    if (!rigPath.empty()) {
        command += " \"" + rigPath + "\"";
    }
    command += "\"";
    std::string block;
    for (const std::string &entry : WorkerEnvironment(';')) {
        block += entry;
        block.push_back('\0');
    }
    block.push_back('\0');

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = childIn;
    startup.hStdOutput = childOut;
    startup.hStdError = log != INVALID_HANDLE_VALUE ? log : childOut;
    PROCESS_INFORMATION info{};
    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');
    const BOOL started = CreateProcessA(
        nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, block.data(), nullptr, &startup, &info);
    CloseHandle(childIn);
    CloseHandle(childOut);
    if (log != INVALID_HANDLE_VALUE) {
        CloseHandle(log);
    }
    if (!started) {
        CloseHandle(parentOut);
        CloseHandle(parentIn);
        *error = "could not start " + launcher;
        return false;
    }
    CloseHandle(info.hThread);
    _process = info.hProcess;
    _toChild = parentOut;
    _fromChild = parentIn;
#else
    int toChild[2], fromChild[2];
    if (pipe(toChild) != 0 || pipe(fromChild) != 0) {
        *error = "could not create the worker pipes";
        return false;
    }
    std::vector<std::string> env = WorkerEnvironment(':');
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(toChild[0], 0);
        dup2(fromChild[1], 1);
        const int log = open(_logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                             0644);
        if (log >= 0) {
            dup2(log, 2);
        }
        close(toChild[1]);
        close(fromChild[0]);
        std::vector<char *> envp;
        for (std::string &entry : env) {
            envp.push_back(entry.data());
        }
        envp.push_back(nullptr);
        std::vector<std::string> args = {"/bin/sh", launcher, rigRoot, stage};
        if (!rigPath.empty()) {
            args.push_back(rigPath);
        }
        std::vector<char *> argv;
        for (std::string &arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        execve("/bin/sh", argv.data(), envp.data());
        _exit(127);
    }
    close(toChild[0]);
    close(fromChild[1]);
    if (pid < 0) {
        close(toChild[1]);
        close(fromChild[0]);
        *error = "could not start " + launcher;
        return false;
    }
    _pid = pid;
    _toChild = toChild[1];
    _fromChild = fromChild[0];
#endif
    std::vector<uint8_t> ready;
    if (!_ReadMessage(&ready) ||
        std::string(ready.begin(), ready.end()) != "ready") {
        std::string reason;
        if (!ready.empty() && ready[0] == '!') {
            reason = std::string(ready.begin() + 1, ready.end());
        } else {
            reason = "the RigExec worker did not start" +
                     ReadLogTail(_logPath);
        }
        Stop();
        *error = reason;
        return false;
    }
    return true;
}

bool
WorkerProcess::IsRunning() const
{
#ifdef _WIN32
    return _process &&
           WaitForSingleObject(static_cast<HANDLE>(_process), 0) ==
               WAIT_TIMEOUT;
#else
    if (_pid <= 0) {
        return false;
    }
    int status = 0;
    return waitpid(_pid, &status, WNOHANG) == 0;
#endif
}

bool
WorkerProcess::Evaluate(double time, const std::vector<double> &values,
                        std::vector<uint8_t> *frame, std::string *error)
{
    const uint32_t count = uint32_t(values.size());
    std::vector<uint8_t> payload(1 + sizeof(double) + sizeof(uint32_t) +
                                 values.size() * sizeof(double));
    payload[0] = 'E';
    std::memcpy(payload.data() + 1, &time, sizeof(double));
    std::memcpy(payload.data() + 1 + sizeof(double), &count,
                sizeof(uint32_t));
    std::memcpy(payload.data() + 1 + sizeof(double) + sizeof(uint32_t),
                values.data(), values.size() * sizeof(double));
    return _Request(payload, frame, error);
}

bool
WorkerProcess::_Request(const std::vector<uint8_t> &payload,
                        std::vector<uint8_t> *reply, std::string *error)
{
    if (!IsRunning()) {
        *error = "the RigExec worker is not running" + ReadLogTail(_logPath);
        return false;
    }
    const uint32_t size = uint32_t(payload.size());
    if (!_Write(&size, sizeof(size)) ||
        !_Write(payload.data(), payload.size()) || !_ReadMessage(reply)) {
        *error = "lost the RigExec worker" + ReadLogTail(_logPath);
        Stop();
        return false;
    }
    if (!reply->empty() && (*reply)[0] == '!') {
        *error = std::string(reply->begin() + 1, reply->end());
        return false;
    }
    return true;
}

bool
WorkerProcess::_ReadMessage(std::vector<uint8_t> *message)
{
    uint32_t size = 0;
    if (!_Read(&size, sizeof(size))) {
        return false;
    }
    message->resize(size);
    return size == 0 || _Read(message->data(), size);
}

bool
WorkerProcess::_Write(const void *data, size_t size)
{
    const char *cursor = static_cast<const char *>(data);
    while (size) {
#ifdef _WIN32
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(_toChild), cursor,
                       DWORD(std::min<size_t>(size, 1u << 30)), &written,
                       nullptr) || written == 0) {
            return false;
        }
#else
        const ssize_t written = write(_toChild, cursor, size);
        if (written <= 0) {
            return false;
        }
#endif
        cursor += written;
        size -= size_t(written);
    }
    return true;
}

bool
WorkerProcess::_Read(void *data, size_t size)
{
    char *cursor = static_cast<char *>(data);
    while (size) {
#ifdef _WIN32
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(_fromChild), cursor,
                      DWORD(std::min<size_t>(size, 1u << 30)), &got,
                      nullptr) || got == 0) {
            return false;
        }
#else
        const ssize_t got = read(_fromChild, cursor, size);
        if (got <= 0) {
            return false;
        }
#endif
        cursor += got;
        size -= size_t(got);
    }
    return true;
}

void
WorkerProcess::Stop()
{
#ifdef _WIN32
    if (_toChild) {
        const uint32_t size = 1;
        const char quit = 'Q';
        _Write(&size, sizeof(size));
        _Write(&quit, 1);
        CloseHandle(static_cast<HANDLE>(_toChild));
        _toChild = nullptr;
    }
    if (_process) {
        if (WaitForSingleObject(static_cast<HANDLE>(_process), 3000) !=
            WAIT_OBJECT_0) {
            TerminateProcess(static_cast<HANDLE>(_process), 1);
        }
        CloseHandle(static_cast<HANDLE>(_process));
        _process = nullptr;
    }
    if (_fromChild) {
        CloseHandle(static_cast<HANDLE>(_fromChild));
        _fromChild = nullptr;
    }
#else
    if (_toChild >= 0) {
        const uint32_t size = 1;
        const char quit = 'Q';
        _Write(&size, sizeof(size));
        _Write(&quit, 1);
        close(_toChild);
        _toChild = -1;
    }
    if (_pid > 0) {
        for (int i = 0; i < 30; ++i) {
            int status = 0;
            if (waitpid(_pid, &status, WNOHANG) != 0) {
                _pid = -1;
                break;
            }
            usleep(100000);
        }
        if (_pid > 0) {
            kill(_pid, SIGKILL);
            waitpid(_pid, nullptr, 0);
            _pid = -1;
        }
    }
    if (_fromChild >= 0) {
        close(_fromChild);
        _fromChild = -1;
    }
#endif
}

}  // namespace rigExecMaya
