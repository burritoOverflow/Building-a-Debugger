#include <fcntl.h>
#include <libsdb/error.hpp>
#include <libsdb/pipe.hpp>
#include <unistd.h>
#include <utility>

sdb::Pipe::Pipe(const bool close_on_exec) {
  // passing O_CLOEXEC if close_on_exec is true, so the pipe is closed when
  // we call execlp (avoiding duplicated file descriptors)
  if (pipe2(this->pipe_fds_, close_on_exec ? O_CLOEXEC : 0) == -1) {
    Error::SendErrno("Pipe creation failed");
  }
}

sdb::Pipe::~Pipe() {
  this->CloseReadFd();
  this->CloseWriteFd();
}

int sdb::Pipe::ReleaseReadFd() {
  return std::exchange(this->pipe_fds_[read_fd_], -1);
}

int sdb::Pipe::ReleaseWriteFd() {
  return std::exchange(this->pipe_fds_[write_fd_], -1);
}

void sdb::Pipe::CloseReadFd() {
  if (this->pipe_fds_[read_fd_] != -1) {
    close(this->pipe_fds_[read_fd_]);
    this->pipe_fds_[read_fd_] = -1;
  }
}

void sdb::Pipe::CloseWriteFd() {
  if (this->pipe_fds_[write_fd_] != -1) {
    close(this->pipe_fds_[write_fd_]);
    this->pipe_fds_[write_fd_] = -1;
  }
}

// Read and return the data from the pipe, as a vector of bytes
std::vector<std::byte> sdb::Pipe::Read() const {
  char buf[1024];
  int  chars_read;

  if ((chars_read = ::read(this->pipe_fds_[read_fd_], buf, sizeof(buf))) ==
      -1) {
    Error::SendErrno("Could not read from pipe");
  }

  const auto bytes = reinterpret_cast<std::byte*>(buf);
  return {bytes, bytes + chars_read};
}

void sdb::Pipe::Write(const std::byte* from, const std::size_t bytes) const {
  if (::write(this->pipe_fds_[write_fd_], from, bytes) == -1) {
    Error::SendErrno("Could not write to pipe");
  }
}
