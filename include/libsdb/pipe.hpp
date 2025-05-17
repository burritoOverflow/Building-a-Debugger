#ifndef SDB_PIPE_HPP
#define SDB_PIPE_HPP

#include <vector>

namespace sdb {
  class Pipe {
public:
    explicit Pipe(bool close_on_exec);
    ~Pipe();

    int GetReadFd() const { return pipe_fds_[read_fd_]; }
    int GetWriteFd() const { return pipe_fds_[write_fd_]; }

    int ReleaseReadFd();
    int ReleaseWriteFd();

    void CloseReadFd();
    void CloseWriteFd();

    std::vector<std::byte> Read() const;
    void Write(const std::byte *from, std::size_t bytes) const;

private:
    static constexpr unsigned read_fd_  = 0;
    static constexpr unsigned write_fd_ = 1;
    int                       pipe_fds_[2];
  };
}  // namespace sdb

#endif  // SDB_PIPE_HPP
