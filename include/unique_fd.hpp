#include <unistd.h>

class UniqueFd
{
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() { if (fd_ >= 0) ::close(fd_); }

    UniqueFd(const UniqueFd&) = delete;              // can't copy an fd
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& o) noexcept : fd_(o.release()) {}
    UniqueFd& operator=(UniqueFd&& o) noexcept { reset(o.release()); return *this; }

    int  get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    int  release()  { int f = fd_; fd_ = -1; return f; }
    void reset(int f = -1) { if (fd_ >= 0) ::close(fd_); fd_ = f; }
private:
    int fd_ = -1;
};
