#ifndef CRASH_UTILS_HPP
#define CRASH_UTILS_HPP

#include <cxxabi.h>
#include <cstdlib>
#include <signal.h>
#include <string>
#include <unistd.h>

namespace crash_utils {

inline const char *signal_name(int signum) {
  switch (signum) {
  case SIGSEGV:
    return "SIGSEGV";
  case SIGABRT:
    return "SIGABRT";
  case SIGFPE:
    return "SIGFPE";
  case SIGBUS:
    return "SIGBUS";
  case SIGILL:
    return "SIGILL";
  default:
    return "UNKNOWN";
  }
}

inline std::string trim_line(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
    s.pop_back();
  }
  return s;
}

inline std::string demangle_symbol(const char *mangled) {
  int status = 0;
  char *demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
  if (status == 0 && demangled != nullptr) {
    std::string res(demangled);
    std::free(demangled);
    return res;
  }
  return mangled != nullptr ? mangled : "";
}

inline std::string base_name(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

inline std::string self_exe_path() {
  char buf[4096];
  const ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len <= 0) {
    return "";
  }
  buf[len] = '\0';
  return std::string(buf);
}

} // namespace crash_utils

#endif