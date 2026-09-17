#pragma once
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

struct MetricRecord
{
    std::string time;
    std::string process_name;
    std::string window_title;

    bool user_active = false;
};

class MetricsBuffer
{
public:
    void push(MetricRecord r)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Лимит
        if (data_.size() >= 100)
        {
            data_.pop_front();
        }

        data_.push_back(std::move(r));
    }

    std::vector<MetricRecord> drainAll()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<MetricRecord> out(data_.begin(), data_.end());

        data_.clear();

        return out;
    }

    size_t size()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        return data_.size();
    }

private:
    std::deque<MetricRecord> data_;
    std::mutex mutex_;
};

MetricRecord captureSnapshot();

bool sendBatch(const std::vector<MetricRecord>& records, const std::string& agentId);

class Agent
{
public:
    void run();
    void requestStop();

private:
    void collectorLoop();
    void senderLoop();
    void flushToBackupFile();

    std::atomic<bool> stopRequested_{ false };
    MetricsBuffer buffer_;
    std::string agentId_ = "DESKTOP-AGENT";
};