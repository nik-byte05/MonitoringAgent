#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "MonitoringAgent.h"
#include <windows.h>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <iostream>

using json = nlohmann::json;

std::string ansiToUtf8(const std::string& ansi)
{
    if (ansi.empty())
    {
        return "";
    }

    int wlen = MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), -1, nullptr, 0);

    std::wstring wide(wlen, 0);
    MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), -1, &wide[0], wlen);

    int ulen = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);

    std::string utf8(ulen, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8[0], ulen, nullptr, nullptr);

    utf8.resize(strlen(utf8.c_str()));
    return utf8;
}

MetricRecord captureSnapshot()
{
    MetricRecord r;

    // Время
    std::time_t t = std::time(nullptr);
    std::tm tmBuf{};

    localtime_s(&tmBuf, &t);

    std::ostringstream oss;

    oss << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S");
    r.time = oss.str();

    // Окно и процесс в фокусе
    HWND hwnd = GetForegroundWindow();

    if (hwnd)
    {
        char title[512] = { 0 };

        GetWindowTextA(hwnd, title, sizeof(title) - 1);
        r.window_title = ansiToUtf8(title);

        DWORD pid = 0;

        GetWindowThreadProcessId(hwnd, &pid);

        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

        if (hProc)
        {
            char path[MAX_PATH] = { 0 };

            DWORD size = MAX_PATH;

            if (QueryFullProcessImageNameA(hProc, 0, path, &size))
            {
                std::string full(path);

                size_t pos = full.find_last_of("\\/");
                r.process_name = ansiToUtf8((pos == std::string::npos) ? full : full.substr(pos + 1));
            }

            CloseHandle(hProc);
        }
    }

    // Проверка, была активность ввода за последние 5 секунд
    LASTINPUTINFO lii;

    lii.cbSize = sizeof(LASTINPUTINFO);

    GetLastInputInfo(&lii);

    DWORD idleMs = GetTickCount() - lii.dwTime;
    r.user_active = idleMs < 5000;

    return r;
}

bool sendBatch(const std::vector<MetricRecord>& records, const std::string& agentId)
{
    if (records.empty())
    {
        return true;
    }

    json j;
    j["agent_id"] = agentId;
    j["timestamp"] = static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());

    json payload = json::array();
    for (const auto& r : records)
    {
        payload.push_back({
            {"time", r.time},
            {"process_name", r.process_name},
            {"window_title", r.window_title},
            {"user_active", r.user_active}
            });
    }

    j["payload"] = payload;

    httplib::Client cli("localhost", 8080);

    cli.set_connection_timeout(3, 0);
    cli.set_write_timeout(5, 0);
    cli.set_read_timeout(5, 0);

    auto res = cli.Post("/", j.dump(), "application/json");

    return res && res->status >= 200 && res->status < 300;
}

void Agent::requestStop()
{
    stopRequested_ = true;
}

void Agent::collectorLoop()
{
    while (!stopRequested_)
    {
        MetricRecord r = captureSnapshot();
        buffer_.push(r);

        for (int i = 0; i < 50 && !stopRequested_; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void Agent::senderLoop()
{
    auto lastAttempt = std::chrono::steady_clock::now() - std::chrono::seconds(30);
    bool lastAttemptFailed = false;

    while (!stopRequested_)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - lastAttempt).count();

        int minInterval = lastAttemptFailed ? 5 : 0;

        bool timeToSend = elapsed >= 30;
        bool enoughRecords = buffer_.size() >= 10 && elapsed >= minInterval;

        if (!timeToSend && !enoughRecords)
        {
            continue;
        }

        auto batch = buffer_.drainAll();
        lastAttempt = std::chrono::steady_clock::now();

        if (batch.empty())
        {
            continue;
        }

        bool ok = false;
        try
        {
            ok = sendBatch(batch, agentId_);
        }

        catch (const std::exception& e)
        {
            std::cout << "sendBatch failed: " << e.what() << "\n";
        }

        lastAttemptFailed = !ok;

        if (!ok)
        {
            for (auto& r : batch) buffer_.push(r);
        }
    }
}

void Agent::run()
{
    std::thread collector(&Agent::collectorLoop, this);
    std::thread sender(&Agent::senderLoop, this);

    collector.join();
    sender.join();

    flushToBackupFile();
}

void Agent::flushToBackupFile()
{
    auto remaining = buffer_.drainAll();

    if (remaining.empty())
    {
        return;
    }

    json j;

    j["agent_id"] = agentId_;
    j["timestamp"] = static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());

    json payload = json::array();

    for (const auto& r : remaining)
    {
        payload.push_back({
            {"time", r.time},
            {"process_name", r.process_name},
            {"window_title", r.window_title},
            {"user_active", r.user_active}
            });
    }

    j["payload"] = payload;

    std::ofstream out("backup.json");
    out << j.dump(2);

    std::cout << "Saved " << remaining.size() << " unsent record(s) to backup.json\n";
}

namespace
{
    Agent* g_agentInstance = nullptr;

    BOOL WINAPI consoleHandler(DWORD signal)
    {
        switch (signal)
        {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (g_agentInstance)
            {
                g_agentInstance->requestStop();
            }

            Sleep(2500);

            return TRUE;

        default:
            return FALSE;
        }
    }
}

int main()
{
    Agent agent;
    g_agentInstance = &agent;
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    std::cout << "MonitoringAgent started. Press Ctrl+C to stop.\n";
    agent.run();

    std::cout << "MonitoringAgent stopped gracefully.\n";

    return 0;
}
