// Included only in the offline test translation unit, inside namespace iodock.
namespace mock {
struct Chunk { int delay; std::string bytes; };
struct Step { std::string command; std::vector<Chunk> chunks; };
std::mutex guard;
std::deque<Step> steps;
std::deque<std::pair<std::chrono::steady_clock::time_point, std::string>> incoming;
std::string outgoing;
std::string failure;
int writeLimit = 3;
bool disconnected = false;
void reset() {
    std::lock_guard<std::mutex> lock(guard);
    steps.clear(); incoming.clear(); outgoing.clear(); failure.clear(); disconnected = false;
}
void expect(const std::string& command, std::vector<Chunk> chunks) {
    std::lock_guard<std::mutex> lock(guard);
    steps.push_back({command, std::move(chunks)});
}
void inject(const std::string& bytes) {
    std::lock_guard<std::mutex> lock(guard);
    incoming.push_back({std::chrono::steady_clock::now(), bytes});
}
int write(const std::string& bytes) {
    std::lock_guard<std::mutex> lock(guard);
    auto n = std::min(bytes.size(), static_cast<size_t>(writeLimit));
    outgoing += bytes.substr(0, n);
    auto end = outgoing.find('\n');
    if (end != std::string::npos) {
        auto command = outgoing.substr(0, end);
        outgoing.erase(0, end + 1);
        if (steps.empty() || steps.front().command != command) {
            failure = "Unexpected command: " + command; return -1;
        }
        auto when = std::chrono::steady_clock::now();
        for (auto& chunk : steps.front().chunks) {
            when += std::chrono::milliseconds(chunk.delay);
            incoming.push_back({when, chunk.bytes});
        }
        steps.pop_front();
    }
    return static_cast<int>(n);
}
int read(void* buffer, size_t) {
    std::lock_guard<std::mutex> lock(guard);
    if (disconnected) return -1;
    if (incoming.empty() || incoming.front().first > std::chrono::steady_clock::now()) return 0;
    auto& bytes = incoming.front().second;
    *static_cast<char*>(buffer) = bytes.front();
    bytes.erase(0, 1);
    if (bytes.empty()) incoming.pop_front();
    return 1;
}
void verify() {
    std::lock_guard<std::mutex> lock(guard);
    if (!failure.empty()) throw std::runtime_error(failure);
    if (!steps.empty()) throw std::runtime_error("Expected command was not sent");
}
}
