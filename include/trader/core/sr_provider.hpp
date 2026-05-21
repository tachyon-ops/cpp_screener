#pragma once

#include "trader/core/types.hpp"
#include <string>
#include <vector>
#include <memory>

namespace trader {

namespace storage {
class TimeSeriesStore;
}

namespace core {

struct SRLevel {
    double price;
    std::string source; // "swing", "ma", "fib", "round", "file"
    int touch_count = 0;
    uint64_t last_updated_ms = 0;
};

class ISupportResistanceProvider {
public:
    virtual ~ISupportResistanceProvider() = default;
    virtual std::vector<SRLevel> get_levels(const std::string& symbol) = 0;
};

// Algorithmic SR Provider
class AlgorithmicSRProvider : public ISupportResistanceProvider {
public:
    explicit AlgorithmicSRProvider(std::shared_ptr<storage::TimeSeriesStore> ts_store);
    ~AlgorithmicSRProvider() override = default;

    std::vector<SRLevel> get_levels(const std::string& symbol) override;

private:
    std::shared_ptr<storage::TimeSeriesStore> ts_store_;
};

// File-based SR Provider
class FileBasedSRProvider : public ISupportResistanceProvider {
public:
    explicit FileBasedSRProvider(const std::string& folder_path);
    ~FileBasedSRProvider() override = default;

    std::vector<SRLevel> get_levels(const std::string& symbol) override;

private:
    std::string folder_path_;
};

// Composite SR Provider
class CompositeSRProvider : public ISupportResistanceProvider {
public:
    CompositeSRProvider(std::vector<std::shared_ptr<ISupportResistanceProvider>> providers, double proximity_pct = 0.01);
    ~CompositeSRProvider() override = default;

    std::vector<SRLevel> get_levels(const std::string& symbol) override;

private:
    std::vector<std::shared_ptr<ISupportResistanceProvider>> providers_;
    double proximity_pct_;
};

} // namespace core
} // namespace trader
