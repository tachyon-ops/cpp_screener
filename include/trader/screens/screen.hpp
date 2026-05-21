#pragma once

#include <string>
#include <vector>
#include <memory>
#include "trader/persistence/sqlite_store.hpp"

namespace trader {
namespace screens {

class Screen {
public:
    virtual ~Screen() = default;
    virtual std::string name() const = 0;
    virtual void evaluate(const std::string& date) = 0;
};

} // namespace screens
} // namespace trader
