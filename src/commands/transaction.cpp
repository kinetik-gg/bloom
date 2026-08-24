#include <bloom/commands/transaction.hpp>

#include <utility>

namespace bloom::commands {

bool Transaction::add(std::unique_ptr<Operation> operation) {
    if (!operation) {
        return false;
    }
    operations_.push_back(std::move(operation));
    return true;
}

} // namespace bloom::commands
